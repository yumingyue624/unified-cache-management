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

import atexit
import hashlib
import json
import os
import tempfile
import threading
from pathlib import Path
from typing import Any

from ucm.logger import init_logger

logger = init_logger(__name__)

MAX_RECORD_BYTES = 1024 * 1024


def counter_deltas(
    current: dict[str, int | float], previous: dict[str, int | float] | None
) -> dict[str, int | float]:
    if previous is None:
        return {name: 0 for name in current}
    return {
        name: value - previous.get(name, 0) if value >= previous.get(name, 0) else value
        for name, value in current.items()
    }


class FileResourceMetricsReporter:
    """Poll a snapshot log from one elected process on each host."""

    def __init__(
        self,
        log_path: str,
        reporter_name: str,
        identity: str,
        interval_sec: float = 15.0,
        shared_memory_dir: str = "/dev/shm",
    ):
        self.log_path = Path(log_path)
        self.reporter_name = reporter_name
        self.interval_sec = max(float(interval_sec), 1.0)
        shared_dir = Path(shared_memory_dir)
        if not shared_dir.is_dir():
            shared_dir = Path(tempfile.gettempdir())
        digest = hashlib.sha256(identity.encode()).hexdigest()[:24]
        self.lock_path = shared_dir / f"ucm_{reporter_name}_metrics_{digest}.lock"
        self.state_path = shared_dir / f"ucm_{reporter_name}_metrics_{digest}.json"
        self._lock_file = None
        self._stop_event = threading.Event()
        self._thread = threading.Thread(
            target=self._run,
            name=f"{reporter_name}-resource-reporter",
            daemon=True,
        )
        atexit.register(self.stop)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        if self._thread.is_alive() and threading.current_thread() is not self._thread:
            self._thread.join(timeout=min(self.interval_sec + 1.0, 5.0))
        if not self._thread.is_alive():
            self._release_leadership()

    def _run(self) -> None:
        try:
            if self._stop_event.is_set():
                return
            try:
                if not self._try_become_leader():
                    return
            except Exception as error:
                self._handle_error("elect", error)
                return
            while not self._stop_event.is_set():
                try:
                    self._collect_once()
                except Exception as error:
                    self._handle_error("collect", error)
                self._stop_event.wait(self.interval_sec)
        finally:
            self._release_leadership()

    def _collect_once(self) -> None:
        raise NotImplementedError

    def _handle_error(self, action: str, error: Exception) -> None:
        logger.warning(
            f"Failed to {action} {self.reporter_name} resource metrics: {error}"
        )

    def _try_become_leader(self) -> bool:
        if self._lock_file is not None:
            return True
        try:
            import fcntl
        except ImportError:
            logger.warning(
                f"{self.reporter_name} resource reporter requires fcntl for host election"
            )
            self._stop_event.set()
            return False

        lock_file = self.lock_path.open("a+")
        try:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            lock_file.close()
            return False
        except Exception:
            lock_file.close()
            raise
        self._lock_file = lock_file
        logger.info(
            f"Became {self.reporter_name} resource metrics reporter for {self.log_path}"
        )
        return True

    def _release_leadership(self) -> None:
        if self._lock_file is None:
            return
        try:
            import fcntl

            fcntl.flock(self._lock_file.fileno(), fcntl.LOCK_UN)
        except (ImportError, OSError):
            pass
        self._lock_file.close()
        self._lock_file = None

    def _read_latest_complete_line(self) -> str:
        with self.log_path.open("rb") as stream:
            stream.seek(0, os.SEEK_END)
            end = stream.tell()
            start = max(0, end - 2 * MAX_RECORD_BYTES)
            stream.seek(start)
            lines = stream.read(end - start).split(b"\n")
        lines.pop()
        if start:
            lines = lines[1:]
        for line in reversed(lines):
            if not line.strip():
                continue
            if len(line) > MAX_RECORD_BYTES:
                raise ValueError("Resource snapshot exceeds 1 MiB")
            return line.decode("utf-8")
        raise ValueError("Resource log has no complete record")

    def _read_state_json(self) -> dict[str, Any] | None:
        try:
            with self.state_path.open("rb") as stream:
                data = stream.read(MAX_RECORD_BYTES + 1)
            if len(data) > MAX_RECORD_BYTES:
                raise ValueError("Resource reporter state exceeds 1 MiB")
            state = json.loads(data)
            if not isinstance(state, dict):
                raise ValueError("Resource reporter state must be an object")
            return state
        except FileNotFoundError:
            return None

    def _write_state_json(self, state: dict[str, Any]) -> None:
        temporary = self.state_path.with_suffix(f".{os.getpid()}.tmp")
        try:
            with temporary.open("w", encoding="utf-8") as stream:
                json.dump(state, stream, allow_nan=False, separators=(",", ":"))
            os.replace(temporary, self.state_path)
        finally:
            temporary.unlink(missing_ok=True)
