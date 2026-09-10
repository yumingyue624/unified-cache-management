# MIT License
# Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
"""Import a local DramPool's cumulative JSONL snapshots into UCM metrics.

Like YuanRong's reporter, this is a Scheduler-side background file reader.
Histograms stay as disjoint interval buckets throughout parsing and differencing.
Only the Prometheus exporter converts them to cumulative ``le`` buckets.
"""

import atexit
import hashlib
import json
import math
import os
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

from ucm.logger import init_logger
from ucm.metrics_config import (
    VLLM_CONNECTOR_CONSUMER,
    MetricDefinition,
    consumer_enabled,
    get_metric_definitions,
)
from ucm.metrics_dispatcher import get_initialized_metrics_dispatcher
from ucm.shared.metrics import ucmmetrics

logger = init_logger(__name__)
UINT64_MAX = (1 << 64) - 1
MAX_RECORD_BYTES = 1024 * 1024
POLL_SECONDS = 10
STALE_SECONDS = 30
_REPORTER: "DramPoolResourceReporter | None" = None
_REPORTER_LOCK = threading.Lock()


def _number(value: Any) -> int | float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError("Expected a numeric metric")
    if value < 0 or not math.isfinite(value):
        raise ValueError("Expected a finite nonnegative metric")
    return value


def _count(value: Any) -> int:
    if type(value) is not int or not 0 <= value <= UINT64_MAX:
        raise ValueError("Expected a uint64 count")
    return value


def _identity(value: Any) -> str:
    if not isinstance(value, str) or not value or len(value) > 256:
        raise ValueError("Missing or invalid source identity")
    return value


@dataclass(frozen=True)
class HistogramSnapshot:
    upper_bounds: tuple[float, ...]
    bucket_counts: tuple[int, ...]
    sum: float
    unit: str

    @property
    def count(self) -> int:
        return sum(self.bucket_counts)


@dataclass(frozen=True)
class DramPoolResourceSnapshot:
    source_id: str
    timestamp: float
    counters: dict[str, int | float]
    gauges: dict[str, int | float]
    histograms: dict[str, HistogramSnapshot]


def parse_drampool_resource_snapshot(
    line: str, definitions: dict[str, MetricDefinition]
) -> DramPoolResourceSnapshot:
    record = json.loads(line)
    if (
        record.get("event") != "drampool_metrics_snapshot"
        or record.get("schema_version") != "v1"
    ):
        raise ValueError("Unsupported DramPool snapshot event/schema")
    timestamp = record["timestamp"]
    if isinstance(timestamp, str):
        parsed = datetime.fromisoformat(timestamp.replace("Z", "+00:00"))
        if parsed.tzinfo is None:
            raise ValueError("Snapshot timestamp must include a timezone")
        timestamp = parsed.timestamp()
    counters, gauges, histograms = {}, {}, {}
    for section, metric_type, destination in (
        ("counters", "counter", counters),
        ("gauges", "gauge", gauges),
        ("histograms", "histogram", histograms),
    ):
        values = record[section]
        if not isinstance(values, dict):
            raise ValueError(f"Expected {section} mapping")
        for name, value in values.items():
            definition = definitions.get(name)
            if definition is None:
                continue  # Explicit configured whitelist; never register wire names.
            if definition.metric_type != metric_type:
                raise ValueError(f"Wrong metric type for {name}")
            if metric_type != "histogram":
                destination[name] = _number(value)
                continue
            bounds = tuple(float(_number(v)) for v in value["upper_bounds"])
            expected = tuple(b for b in definition.buckets if math.isfinite(b))
            if bounds != expected or any(a >= b for a, b in zip(bounds, bounds[1:])):
                raise ValueError(f"Histogram boundaries do not match {name}")
            counts = tuple(_count(v) for v in value["bucket_counts"])
            count = _count(value["count"])
            total = float(_number(value["sum"]))
            # This importer currently accepts duration histograms in microseconds.
            if value["unit"] != "us" or not name.endswith("_us"):
                raise ValueError(f"Unsupported histogram unit for {name}")
            if len(counts) != len(bounds) + 1 or sum(counts) != count:
                raise ValueError(f"Histogram count/shape mismatch for {name}")
            if count == 0 and total != 0:
                raise ValueError(f"Nonzero sum for empty histogram {name}")
            destination[name] = HistogramSnapshot(bounds, counts, total, "us")
    return DramPoolResourceSnapshot(
        _identity(record["source_id"]),
        float(_number(timestamp)),
        counters,
        gauges,
        histograms,
    )


def snapshot_deltas(
    current: DramPoolResourceSnapshot, previous: DramPoolResourceSnapshot | None
):
    """YuanRong-style cumulative differences, with whole-Histogram resets."""
    if previous is not None and current.source_id != previous.source_id:
        raise ValueError("DramPool source changed")
    counters, histograms = {}, {}
    for name, value in current.counters.items():
        old = previous.counters.get(name, 0) if previous is not None else None
        counters[name] = 0 if old is None else value - old if value >= old else value
    for name, value in current.histograms.items():
        old = previous.histograms.get(name) if previous is not None else None
        if previous is None:
            counts, total = [0] * len(value.bucket_counts), 0.0
        elif old is None:
            counts, total = list(value.bucket_counts), value.sum
        else:
            if old.upper_bounds != value.upper_bounds or old.unit != value.unit:
                raise ValueError(f"Histogram schema changed: {name}")
            counts = [a - b for a, b in zip(value.bucket_counts, old.bucket_counts)]
            total = value.sum - old.sum
            if any(v < 0 for v in counts) or total < 0:
                # Like a counter decrease, infer a reset. Reset the whole
                # distribution rather than mixing reset and differenced buckets.
                counts, total = list(value.bucket_counts), value.sum
            if sum(counts) == 0 and total != 0:
                raise ValueError(f"Histogram sum changed without samples: {name}")
        histograms[name] = (counts, total)
    return counters, current.gauges, histograms


def _snapshot_record(snapshot: DramPoolResourceSnapshot) -> dict:
    return {
        "event": "drampool_metrics_snapshot",
        "schema_version": "v1",
        "source_id": snapshot.source_id,
        "timestamp": snapshot.timestamp,
        "counters": snapshot.counters,
        "gauges": snapshot.gauges,
        "histograms": {
            name: {
                "upper_bounds": h.upper_bounds,
                "bucket_counts": h.bucket_counts,
                "count": h.count,
                "sum": h.sum,
                "unit": h.unit,
            }
            for name, h in snapshot.histograms.items()
        },
    }


class DramPoolResourceReporter:
    def __init__(
        self,
        log_path: str,
        shared_dir: str,
        endpoints: list[str],
        definitions: list[MetricDefinition],
    ):
        self.log_path = Path(log_path)
        self.shared_dir = Path(shared_dir)
        self.endpoints = frozenset(endpoints)
        self.definitions = {
            d.name: d
            for d in definitions
            if d.name.startswith("drampool_")
            and not d.name.startswith("drampool_resource_")
            and d.vllm_connector_enabled
        }
        self.source_id = ""
        self._snapshot_timestamp = 0.0
        self._lock_file = None
        self._state_path: Path | None = None
        self._stop_event = threading.Event()
        self._thread = threading.Thread(
            target=self._run, name="drampool-resource-reporter", daemon=True
        )
        self._last_warning: dict[str, float] = {}

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop_event.set()
        if self._thread.is_alive() and threading.current_thread() is not self._thread:
            self._thread.join(timeout=5)
        # Only _run's finally releases leadership. A slow file operation must not
        # allow another leader while this thread could still import a snapshot.

    def _error(self, stage: str, error: Exception):
        try:
            ucmmetrics.update_stats({f"drampool_resource_{stage}_errors_total": 1.0})
        except Exception:
            pass
        now = time.monotonic()
        if now - self._last_warning.get(stage, -math.inf) >= 60:
            logger.warning(f"DramPool resource {stage} failed: {error}")
            self._last_warning[stage] = now

    def _read_latest_snapshot(self):
        with self.log_path.open("rb") as stream:
            stream.seek(0, os.SEEK_END)
            end = stream.tell()
            # Two maximum-sized records allow skipping a partial final record.
            start = max(0, end - 2 * MAX_RECORD_BYTES)
            stream.seek(start)
            lines = stream.read(end - start).split(b"\n")
        lines.pop()  # EOF fragment, including an empty fragment after a newline.
        if start:
            lines = lines[1:]  # May begin in the middle of a record.
        for line in reversed(lines):
            if not line.strip():
                continue
            try:
                if len(line) > MAX_RECORD_BYTES:
                    raise ValueError("Snapshot exceeds 1 MiB")
                return parse_drampool_resource_snapshot(
                    line.decode("utf-8"), self.definitions
                )
            except (
                ValueError,
                KeyError,
                TypeError,
                OverflowError,
                AttributeError,
            ) as error:
                self._error("parse", error)
        raise ValueError("No complete valid DramPool snapshot")

    def _try_become_leader(self, snapshot):
        if self.source_id and snapshot.source_id != self.source_id:
            raise ValueError("The configured local file changed source identity")
        if snapshot.source_id not in self.endpoints:
            raise ValueError("Snapshot source is not a configured DramPool endpoint")
        self.source_id = snapshot.source_id
        if self._lock_file is not None:
            return True
        import fcntl

        # All containers must share this directory; do not silently fall back to
        # a container-private temporary directory.
        identity = hashlib.sha256(self.source_id.encode()).hexdigest()[:24]
        lock_file = (self.shared_dir / f"ucm_drampool_metrics_{identity}.lock").open(
            "a+"
        )
        try:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            lock_file.close()
            return False
        except Exception:
            lock_file.close()
            raise
        self._lock_file = lock_file
        self._state_path = self.shared_dir / f"ucm_drampool_metrics_{identity}.json"
        return True

    def _read_state(self):
        try:
            with self._state_path.open("rb") as stream:
                data = stream.read(MAX_RECORD_BYTES + 1)
            if len(data) > MAX_RECORD_BYTES:
                raise ValueError("Oversized reporter state")
            state = json.loads(data)
            if state["state_version"] != 1:
                raise ValueError("Unsupported reporter state")
            previous = parse_drampool_resource_snapshot(
                json.dumps(state["snapshot"]), self.definitions
            )
            if previous.source_id != self.source_id:
                raise ValueError("Reporter state belongs to another source")
            return previous
        except FileNotFoundError:
            return None
        except (
            OSError,
            ValueError,
            KeyError,
            TypeError,
            AttributeError,
            OverflowError,
        ) as error:
            self._error("state_read", error)
            return None

    def _write_state(self, snapshot):
        temporary = self._state_path.with_suffix(f".{os.getpid()}.tmp")
        try:
            with temporary.open("w", encoding="utf-8") as stream:
                json.dump(
                    {
                        "state_version": 1,
                        "snapshot": _snapshot_record(snapshot),
                    },
                    stream,
                    allow_nan=False,
                    separators=(",", ":"),
                )
            os.replace(temporary, self._state_path)
        finally:
            temporary.unlink(missing_ok=True)

    def _collect_once(self):
        if self._stop_event.is_set():
            return
        snapshot = self._read_latest_snapshot()
        if snapshot.source_id != self.source_id:
            raise ValueError("Snapshot source changed")
        previous = self._read_state()
        counters, gauges, histograms = snapshot_deltas(snapshot, previous)
        try:
            ucmmetrics.merge_histogram_stats(histograms)
            ucmmetrics.update_stats(counters | gauges)
        except Exception as error:
            self._error("import", error)
            return
        self._snapshot_timestamp = snapshot.timestamp
        try:
            self._write_state(snapshot)
        except OSError as error:
            self._error("state_write", error)
        # As in YuanRong, the next round reads the persisted baseline again.
        # Import and state replacement are not a transaction; failures can replay.

    def _health(self):
        leader = self._lock_file is not None
        updates = {"drampool_resource_reporter_leader": float(leader)}
        if leader:
            timestamp = self._snapshot_timestamp
            age = max(0.0, time.time() - timestamp)
            updates |= {
                "drampool_resource_snapshot_timestamp_seconds": timestamp,
                "drampool_resource_snapshot_age_seconds": age,
                "drampool_resource_snapshot_fresh": float(
                    bool(timestamp) and age <= STALE_SECONDS
                ),
            }
        ucmmetrics.update_stats(updates)

    def _run(self):
        try:
            if self._stop_event.is_set():
                return
            try:
                if not self._try_become_leader(self._read_latest_snapshot()):
                    return
            except Exception as error:
                self._error("read", error)
                return
            while not self._stop_event.is_set():
                try:
                    self._collect_once()
                except Exception as error:
                    self._error("read", error)
                try:
                    self._health()
                except Exception as error:
                    self._error("import", error)
                self._stop_event.wait(POLL_SECONDS)
        finally:
            if self._lock_file is not None:
                self._lock_file.close()
                self._lock_file = None


def get_drampool_resource_source() -> str:
    return _REPORTER.source_id if _REPORTER is not None else ""


def stop_drampool_resource_reporter():
    if _REPORTER is not None:
        _REPORTER.stop()


def start_drampool_resource_reporter(config: dict) -> DramPoolResourceReporter | None:
    global _REPORTER
    path = str(config.get("drampool_resource_log_path", ""))
    enabled = config.get("drampool_resource_metrics_enable", bool(path))
    if isinstance(enabled, str):
        enabled = enabled.lower() in {"true", "1", "yes", "on"}
    if not enabled or not path or int(config.get("device_id", -1)) >= 0:
        return None
    dispatcher = get_initialized_metrics_dispatcher()
    if dispatcher is None:
        return None
    active_config = dispatcher.config
    if not consumer_enabled(active_config, VLLM_CONNECTOR_CONSUMER):
        logger.warning(
            "DramPool resource reporter requires the vllm_connector consumer"
        )
        return None
    shared_dir = str(config.get("drampool_resource_shared_dir", ""))
    if not shared_dir or not Path(shared_dir).is_dir():
        logger.warning(
            "DramPool reporter requires an existing host-shared state directory"
        )
        return None
    if os.name != "posix":
        logger.warning("DramPool resource election requires POSIX flock")
        return None
    definitions = get_metric_definitions(active_config)
    if not any(d.name.startswith("drampool_") for d in definitions):
        logger.warning("No DramPool metrics are configured")
        return None
    if not hasattr(ucmmetrics, "merge_histogram_stats"):
        logger.warning(
            "DramPool reporter requires the Histogram import metrics binding"
        )
        return None
    with _REPORTER_LOCK:
        if _REPORTER is not None:
            if _REPORTER.log_path.resolve() != Path(path).resolve():
                logger.warning(
                    "Only one local DramPool resource source is supported per process"
                )
                return None
            return _REPORTER
        reporter = DramPoolResourceReporter(
            path,
            shared_dir,
            list(config.get("node_control_endpoints", [])),
            definitions,
        )
        if not reporter.definitions:
            logger.warning("No DramPool source metrics are enabled for vllm_connector")
            return None
        reporter.start()
        _REPORTER = reporter
        atexit.register(stop_drampool_resource_reporter)
        return reporter
