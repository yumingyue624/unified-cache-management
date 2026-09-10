#
# MIT License
#
# Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#

"""CPU-only tests against the real C++ metrics binding (build_binding.py)."""

import json
import logging
import os
from pathlib import Path
import sys
import subprocess
import threading
import time
from types import ModuleType

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(
    0, os.environ.get("UCM_METRICS_NATIVE_DIR", str(ROOT / "build/dram_metrics_native"))
)
native = pytest.importorskip("ucmmetrics")
# The native logger is unrelated to metrics and needs the full UCM runtime.
logger_module = ModuleType("ucm.logger")
logger_module.init_logger = logging.getLogger
sys.modules["ucm.logger"] = logger_module
package = ModuleType("ucm")
package.__path__ = [str(ROOT / "ucm")]
sys.modules["ucm"] = package
sys.modules["ucm.shared.metrics.ucmmetrics"] = native
import ucm.shared.metrics

ucm.shared.metrics.ucmmetrics = native
from ucm.metrics_config import MetricDefinition
from ucm.store.dram import resource_reporter as reporter

NAME = "drampool_load_duration_us"
COUNTER = "drampool_load_requests_total"
GAUGE = "drampool_used_bytes"
DEFINITIONS = [
    MetricDefinition(NAME, "histogram", buckets=(100, 500, 1000)),
    MetricDefinition(COUNTER, "counter"),
    MetricDefinition(GAUGE, "gauge"),
]
BOUNDS = [100, 500, 1000]


@pytest.fixture(autouse=True)
def metrics():
    native.set_up()
    for d in DEFINITIONS:
        native.create_stats(d.name, d.metric_type, list(d.buckets))
    native.get_all_stats_and_clear()
    yield
    native.get_all_stats_and_clear()


def record(tick=41, counts=None, total=12000, counter=36):
    counts = [10, 20, 5, 1] if counts is None else counts
    return {
        "event": "drampool_metrics_snapshot",
        "schema_version": "v1",
        "source_id": "127.0.0.1:12345",
        "timestamp": 1788825600 + tick,
        "counters": {COUNTER: counter},
        "gauges": {GAUGE: 4096},
        "histograms": {
            NAME: {
                "upper_bounds": [100, 500, 1000],
                "bucket_counts": counts,
                "count": sum(counts),
                "sum": total,
                "unit": "us",
            }
        },
    }


def parse(value):
    return reporter.parse_drampool_resource_snapshot(json.dumps(value))


def test_difference_and_real_binding():
    previous = parse(record())
    current = parse(record(43, counts=[12, 23, 6, 1], total=13900, counter=42))
    counters, gauges, histograms = reporter.snapshot_deltas(current, previous)
    assert histograms == {NAME: (BOUNDS, [2, 3, 1, 0], 1900)}
    native.merge_histogram_stats(histograms)
    native.update_stats(counters | gauges)
    got_counter, got_gauge, got_hist = native.get_all_stats_and_clear()
    assert got_counter[COUNTER] == 6
    assert got_gauge[GAUGE] == 4096
    assert got_hist[NAME] == ([2, 3, 1, 0], 1900)
    assert reporter.snapshot_deltas(current, current)[2][NAME] == (
        BOUNDS,
        [0, 0, 0, 0],
        0,
    )
    assert reporter.snapshot_deltas(previous, current)[2][NAME] == (
        BOUNDS,
        [10, 20, 5, 1],
        12000,
    )


def test_baseline_restart_and_large_integer_counter():
    previous = parse(record(counter=2**60))
    assert reporter.snapshot_deltas(previous, None)[2][NAME] == (
        BOUNDS,
        [0, 0, 0, 0],
        0,
    )
    current = parse(record(42, counter=2**60 + 1))
    assert reporter.snapshot_deltas(current, previous)[0][COUNTER] == 1
    reset = parse(record(1, counts=[1, 0, 0, 0], total=50, counter=2))
    assert reporter.snapshot_deltas(reset, previous)[2][NAME] == (
        BOUNDS,
        [1, 0, 0, 0],
        50,
    )
    assert reporter.snapshot_deltas(reset, previous)[0][COUNTER] == 2
    # A reset that has already exceeded the old totals cannot be detected.
    assert (
        reporter.snapshot_deltas(parse(record(counter=2**60 + 100)), previous)[0][
            COUNTER
        ]
        == 100
    )


@pytest.mark.parametrize(
    "field,value",
    [
        ("bucket_counts", [-1, 20, 5, 1]),
        ("bucket_counts", [10.5, 20, 5, 1]),
        ("bucket_counts", [2**64, 20, 5, 1]),
        ("count", 37),
        ("upper_bounds", [100, 100, 1000]),
        ("unit", "seconds"),
        ("sum", float("nan")),
        ("sum", -1),
    ],
)
def test_invalid_histogram(field, value):
    data = record()
    data["histograms"][NAME][field] = value
    with pytest.raises(ValueError):
        parse(data)


def test_histogram_decrease_resets_entire_distribution():
    current = parse(record(counts=[9, 22, 5, 1], total=13000))
    assert reporter.snapshot_deltas(current, parse(record()))[2][NAME] == (
        BOUNDS,
        [9, 22, 5, 1],
        13000,
    )


@pytest.mark.parametrize(
    "counts,total",
    [
        ([-1, 0, 0, 0], 0),
        ([0.1, 0, 0, 0], 0),
        ([True, 0, 0, 0], 0),
        ([2**64, 0, 0, 0], 0),
        ([0, 0, 0], 0),
        ([0, 0, 0, 0], 1),
        ([1, 0, 0, 0], float("inf")),
    ],
)
def test_native_binding_rejects_invalid_counts(counts, total):
    with pytest.raises((ValueError, TypeError, RuntimeError, OverflowError)):
        native.merge_histogram_stats({NAME: (BOUNDS, counts, total)})
    assert native.get_all_stats_and_clear()[2] == {}


def test_native_invalid_batch_does_not_partially_publish():
    native.merge_histogram_stats({NAME: (BOUNDS, [1, 0, 0, 0], 50)})
    with pytest.raises(ValueError):
        native.merge_histogram_stats(
            {NAME: (BOUNDS, [2, 0, 0, 0], 100), GAUGE: ([], [1], 1)}
        )
    assert native.get_all_stats_and_clear()[2][NAME] == ([1, 0, 0, 0], 50)


def test_native_uninitialized_import_is_ignored():
    # A fresh process exercises the production lifecycle without reset hooks.
    subprocess.run(
        [
            sys.executable,
            "-c",
            """
import sys
sys.path.insert(0, sys.argv[1])
import ucmmetrics as metrics
metrics.merge_histogram_stats({'unregistered': ([100], [1, 0], 50)})
metrics.set_up()
metrics.create_stats('unregistered', 'histogram', [100])
assert metrics.get_all_stats_and_clear() == ({}, {}, {})
""",
            str(Path(native.__file__).parent),
        ],
        check=True,
    )


def test_native_unregistered_import_does_not_register_or_reject_batch():
    name = "drampool_unregistered_duration_us"
    native.merge_histogram_stats({name: ([], [1], 1), NAME: (BOUNDS, [1, 0, 0, 0], 50)})
    assert native.get_all_stats_and_clear()[2] == {NAME: ([1, 0, 0, 0], 50)}
    native.create_stats(name, "histogram", [10])
    assert native.get_all_stats_and_clear()[2] == {}
    native.merge_histogram_stats({name: ([10], [1, 0], 5)})
    assert native.get_all_stats_and_clear()[2] == {name: ([1, 0], 5)}


@pytest.mark.parametrize(
    "bounds", [[100, 600, 1000], [100, 500], [100, 500, float("inf")], [100, 100, 1000]]
)
def test_native_registered_bucket_schema_is_checked(bounds):
    native.merge_histogram_stats({NAME: (BOUNDS, [1, 0, 0, 0], 50)})
    with pytest.raises(ValueError):
        native.merge_histogram_stats({NAME: (bounds, [2, 0, 0, 0], 100)})
    assert native.get_all_stats_and_clear()[2][NAME] == ([1, 0, 0, 0], 50)


def test_reporter_leaves_registered_schema_validation_to_native():
    data = record()
    data["histograms"][NAME]["upper_bounds"] = [100, 600, 1000]
    snapshot = parse(data)  # Valid wire data; Python needs no registration config.
    with pytest.raises(ValueError):
        native.merge_histogram_stats(reporter.snapshot_deltas(snapshot, None)[2])
    assert native.get_all_stats_and_clear()[2] == {}


def test_reporter_preserves_unregistered_source_metrics():
    data = record()
    data["histograms"]["drampool_disabled_duration_us"] = data["histograms"][NAME]
    snapshot = parse(data)
    delta = reporter.snapshot_deltas(snapshot, None)[2]
    assert "drampool_disabled_duration_us" in delta
    native.merge_histogram_stats(delta)
    assert set(native.get_all_stats_and_clear()[2]) == {NAME}


def test_native_observations_and_concurrent_imports():
    failures = []

    def write():
        try:
            for _ in range(300):
                native.merge_histogram_stats({NAME: (BOUNDS, [2, 3, 1, 0], 1900)})
                native.update_stats(NAME, 50)
        except Exception as error:
            failures.append(error)

    threads = [threading.Thread(target=write) for _ in range(4)]
    for thread in threads:
        thread.start()
    buckets, total = [0, 0, 0, 0], 0

    def drain():
        nonlocal total
        h = native.get_all_stats_and_clear()[2].get(NAME)
        if h:
            for i, value in enumerate(h[0]):
                buckets[i] += value
            total += h[1]

    while any(t.is_alive() for t in threads):
        drain()
    for thread in threads:
        thread.join()
    drain()
    assert not failures
    assert buckets == [3600, 3600, 1200, 0]
    assert total == 1200 * 1950


def test_native_merge_total_overflow_is_atomic():
    native.merge_histogram_stats({NAME: (BOUNDS, [2**64 - 1, 0, 0, 0], 0)})
    with pytest.raises(OverflowError):
        native.merge_histogram_stats({NAME: (BOUNDS, [0, 1, 0, 0], 100)})
    assert native.get_all_stats_and_clear()[2][NAME] == ([2**64 - 1, 0, 0, 0], 0)


def make_reporter(tmp_path, monkeypatch):
    reader = reporter.DramPoolResourceReporter(
        str(tmp_path / "metrics.log"), str(tmp_path)
    )
    reader.source_id = "127.0.0.1:12345"
    reader._state_path = tmp_path / "state.json"
    monkeypatch.setattr(reader, "_try_become_leader", lambda: True)
    return reader


def write_record(reader, data, tail=b""):
    reader.log_path.write_bytes(json.dumps(data).encode() + b"\n" + tail)


def test_file_baseline_partial_tail_rotation_and_state(tmp_path, monkeypatch):
    reader = make_reporter(tmp_path, monkeypatch)
    write_record(reader, record(), b'{"incomplete":')
    reader._collect_once()
    native.get_all_stats_and_clear()
    reader.log_path.rename(tmp_path / "old.log")
    write_record(reader, record(43, counts=[12, 23, 6, 1], total=13900, counter=42))
    reader._collect_once()
    assert native.get_all_stats_and_clear()[2][NAME] == ([2, 3, 1, 0], 1900)
    reader._collect_once()
    assert native.get_all_stats_and_clear()[2][NAME] == ([0, 0, 0, 0], 0)
    assert reader._read_state() == parse(
        record(43, counts=[12, 23, 6, 1], total=13900, counter=42)
    )


def test_state_write_failure_reuses_persisted_baseline(tmp_path, monkeypatch):
    reader = make_reporter(tmp_path, monkeypatch)
    write_record(reader, record())
    reader._collect_once()
    native.get_all_stats_and_clear()
    write_record(reader, record(43, counts=[12, 23, 6, 1], total=13900, counter=42))

    def fail(snapshot):
        raise OSError("disk full")

    monkeypatch.setattr(reader, "_write_state", fail)
    reader._collect_once()
    assert native.get_all_stats_and_clear()[2][NAME][0] == [2, 3, 1, 0]
    reader._collect_once()
    assert native.get_all_stats_and_clear()[2][NAME][0] == [2, 3, 1, 0]
    assert reader._read_state() == parse(record())


def test_empty_and_oversized_file(tmp_path, monkeypatch):
    reader = make_reporter(tmp_path, monkeypatch)
    for content in [b"", b"x" * (2 * reporter.MAX_RECORD_BYTES + 1)]:
        reader.log_path.write_bytes(content)
        with pytest.raises(ValueError):
            reader._read_latest_snapshot()


def test_scalar_failure_leaves_state_for_next_round(tmp_path, monkeypatch):
    reader = make_reporter(tmp_path, monkeypatch)
    write_record(reader, record())
    reader._collect_once()
    native.get_all_stats_and_clear()
    write_record(reader, record(43, counts=[12, 23, 6, 1], total=13900, counter=42))
    update = native.update_stats

    def fail(values):
        if COUNTER in values:
            raise RuntimeError("temporary scalar failure")
        update(values)

    monkeypatch.setattr(native, "update_stats", fail)
    reader._collect_once()
    assert native.get_all_stats_and_clear()[2][NAME][0] == [2, 3, 1, 0]
    monkeypatch.setattr(native, "update_stats", update)
    reader._collect_once()
    c, _, h = native.get_all_stats_and_clear()
    assert c[COUNTER] == 6 and h[NAME][0] == [2, 3, 1, 0]
    assert reader._read_state().timestamp == record(43)["timestamp"]


def test_real_prometheus_export(monkeypatch):
    """Real reporter delta -> C++ -> dispatcher -> connector -> Prometheus text."""
    from dataclasses import dataclass, field
    from functools import partial
    from types import SimpleNamespace

    prometheus = pytest.importorskip("prometheus_client")
    registry = prometheus.CollectorRegistry()
    vllm = ModuleType("vllm")
    vllm.__path__ = []
    config_module = ModuleType("vllm.config")
    config_module.VllmConfig = object
    bridge = ModuleType("vllm.distributed.kv_transfer.kv_connector.v1.metrics")

    @dataclass
    class Stats:
        data: dict = field(default_factory=dict)

    class PromMetrics:
        def __init__(self, config, metric_types, labelnames, per_engine_labelvalues):
            self._counter_cls = partial(prometheus.Counter, registry=registry)
            self._gauge_cls = partial(prometheus.Gauge, registry=registry)
            self._histogram_cls = partial(prometheus.Histogram, registry=registry)
            self.per_engine_labelvalues = per_engine_labelvalues

    bridge.KVConnectorStats = Stats
    bridge.KVConnectorPromMetrics = PromMetrics
    bridge.PromMetric = bridge.PromMetricT = object
    for name, module in [
        ("vllm", vllm),
        ("vllm.config", config_module),
        (bridge.__name__, bridge),
    ]:
        monkeypatch.setitem(sys.modules, name, module)
    from ucm.integration.vllm.metrics import UCMConnectorStats, UCMPromMetrics
    from ucm.metrics_config import get_metric_definitions
    from ucm.metrics_dispatcher import MetricsDispatcher

    config = {
        "consumers": {"vllm_connector": True},
        "histogram": [
            {
                "name": NAME,
                "buckets": [100, 500, 1000],
                "vllm_connector_name": "drampool_load_duration_seconds",
                "vllm_connector_value_scale": 1e-6,
            }
        ],
    }
    delta = reporter.snapshot_deltas(
        parse(record(43, counts=[12, 23, 6, 1], total=13900)), parse(record())
    )
    native.merge_histogram_stats(delta[2])
    dispatcher = MetricsDispatcher(config)
    dispatcher.drain_to_consumers()
    c, g, h = dispatcher.get_stats_and_clear("vllm_connector")
    stats = UCMConnectorStats.from_ucm_snapshot(
        c,
        g,
        h,
        "scheduler",
        get_metric_definitions(config),
    )
    vllm_config = SimpleNamespace(
        kv_transfer_config=SimpleNamespace(launch_config={"metrics_config": config})
    )
    prom = UCMPromMetrics(vllm_config, {}, ["model_name"], {0: ["test-model"]})
    prom.observe(stats.data)
    samples = [s for metric in registry.collect() for s in metric.samples]
    buckets = [s for s in samples if s.name.endswith("_bucket")]
    assert [s.value for s in buckets] == [2, 5, 6, 6]
    assert next(s.value for s in samples if s.name.endswith("_sum")) == pytest.approx(
        0.0019
    )
    assert next(s.value for s in samples if s.name.endswith("_count")) == 6


@pytest.mark.skipif(os.name != "posix", reason="real flock election requires POSIX")
def test_real_flock_excludes_other_processes(tmp_path):
    readers = [
        reporter.DramPoolResourceReporter(
            str(tmp_path / "metrics.log"),
            str(tmp_path),
        )
        for _ in range(2)
    ]
    try:
        assert readers[0]._try_become_leader()
        # An independent process must also be excluded by the actual OS lock.
        probe = """
import fcntl, sys
with open(sys.argv[1], 'a+') as lock:
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        sys.exit(7)
"""
        lock_path = readers[0]._lock_file.name
        assert subprocess.run([sys.executable, "-c", probe, lock_path]).returncode == 7
        assert not readers[1]._try_become_leader()
        readers[0]._lock_file.close()
        readers[0]._lock_file = None
        assert subprocess.run([sys.executable, "-c", probe, lock_path]).returncode == 0
        assert readers[1]._try_become_leader()
    finally:
        for reader in readers:
            if reader._lock_file:
                reader._lock_file.close()


@pytest.mark.parametrize(
    "overrides",
    [
        {"drampool_resource_log_path": ""},
        {"device_id": 0},
        {"drampool_resource_metrics_enable": False},
    ],
)
def test_reporter_role_and_enable_guards(overrides):
    assert (
        reporter.start_drampool_resource_reporter(
            {"drampool_resource_log_path": "/not-opened/metrics.log", **overrides}
        )
        is None
    )


@pytest.mark.skipif(os.name != "posix", reason="reporter lifecycle uses POSIX flock")
def test_reporter_thread_elects_once_and_loser_exits(tmp_path, monkeypatch):
    monkeypatch.setattr(reporter, "POLL_SECONDS", 0.01)
    readers = [
        reporter.DramPoolResourceReporter(
            str(tmp_path / "metrics.log"),
            str(tmp_path),
        )
        for _ in range(2)
    ]

    def wait_until(predicate):
        deadline = time.monotonic() + 5
        while not predicate() and time.monotonic() < deadline:
            time.sleep(0.01)
        assert predicate()

    try:
        readers[0].start()
        wait_until(lambda: readers[0]._lock_file is not None)
        write_record(readers[0], record())
        wait_until(lambda: readers[0]._state_path and readers[0]._state_path.exists())
        readers[1].start()
        readers[1]._thread.join(timeout=5)
        assert not readers[1]._thread.is_alive()
        assert readers[1]._lock_file is None
        readers[0].stop()
        assert not readers[0]._thread.is_alive()
        assert readers[0]._lock_file is None
        # Stopping the leader does not restart the losing reporter.
        assert not readers[1]._thread.is_alive()
        assert readers[1]._lock_file is None
    finally:
        for reader in readers:
            reader.stop()
