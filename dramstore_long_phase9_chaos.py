#!/usr/bin/env python3
"""Incremental DramPool chaos/recovery soak against the real DramStore simulator."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time


ROOT = Path("/home/codex/ucm-dramstore-sim-codex-20260728")
POOL_BIN = ROOT / "build-dramstore-sim-cann851/ucm/store/dram/drampool"
SIM_BIN = ROOT / "ucm/store/dram/build/dramstore_sim"
POOL_CONTROL = "110.138.0.3:50200"
STORE_CONTROL = "110.138.0.3:50201"
POOL_ONE_SIDED = "110.138.0.3:50300"
STORE_ONE_SIDED = "110.138.0.3:50301"


def config_text(log_dir: Path) -> str:
    return f"""transport:
  device_ids: [4]
  endpoints:
    - two_sided: "{POOL_CONTROL}"
      one_sided: "{POOL_ONE_SIDED}"
    - two_sided: "{STORE_CONTROL}"
      one_sided: "{STORE_ONE_SIDED}"
queue:
  request_depth: 32
  completion_depth: 32
request_receiver:
  idle_wait_us: 50
poller:
  pending_depth: 8
flag_buffer:
  capacity_mb: 1
  slot_size_bytes: 64
gc:
  enabled: true
  interval_ms: 10
metadata:
  periodic_eviction_policy: TTL
  deep_eviction_policy: POSITION
  lease_time_ms: 3000
  default_evict_ratio: 0.2
  evict_period_ms: 100
operation:
  timeout_ms: 3000
logger:
  level: info
  dir: "{log_dir}"
  max_files: 20
  max_size_mb: 10
"""


def pool_command(config: Path) -> list[str]:
    return [
        str(POOL_BIN), "--addr", POOL_CONTROL, "--nics", "mlx5_0",
        "--pool-size-gb", "1",
        "--kvcache-block-sizes", "4096", "65536",
        "--kvcache-block-proportions", "3", "1",
        "--ttl-minutes", "120", "--config", str(config),
    ]


def sim_command(config: Path, seed: int, high: bool) -> list[str]:
    if high:
        workload = [
            "--block-size", "4096", "--block-num", "2", "--rounds", "2",
            "--put", "10000", "--get", "10000",
            "--lookup-exist", "1000", "--lookup-miss", "1000",
        ]
    else:
        workload = [
            "--block-size", "4096", "--block-num", "2", "--rounds", "1",
            "--put", "100", "--get", "100",
            "--lookup-exist", "20", "--lookup-miss", "20",
        ]
    return [
        str(SIM_BIN), "--config", str(config),
        "--pool-control", POOL_CONTROL, "--store-control", STORE_CONTROL,
        "--devices", "5", "--store-index", "0", "--key-seed", str(seed),
        "--eviction-aware", "1", *workload,
    ]


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def wait_pool_ready(process: subprocess.Popen[str], log: Path, timeout: float = 30) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return False
        if "DramPool service ready" in read_text(log):
            return True
        time.sleep(0.1)
    return False


def wait_process(process: subprocess.Popen[str], timeout: float,
                 terminate: bool = True) -> tuple[int | None, bool]:
    try:
        return process.wait(timeout=timeout), False
    except subprocess.TimeoutExpired:
        if not terminate:
            return None, False
        os.killpg(process.pid, signal.SIGTERM)
        try:
            return process.wait(timeout=10), True
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            return process.wait(timeout=5), True


def start_logged(command: list[str], log: Path) -> subprocess.Popen[str]:
    output = log.open("w", encoding="utf-8")
    process = subprocess.Popen(
        command, text=True, stdout=output, stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    output.close()
    return process


def process_metrics(pid: int) -> dict[str, int]:
    metrics = {"rss_kb": 0, "threads": 0, "fds": 0}
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                metrics["rss_kb"] = int(line.split()[1])
            elif line.startswith("Threads:"):
                metrics["threads"] = int(line.split()[1])
        metrics["fds"] = len(list(Path(f"/proc/{pid}/fd").iterdir()))
    except (FileNotFoundError, ProcessLookupError):
        pass
    return metrics


class ChaosRun:
    def __init__(self, out_dir: Path, duration_seconds: int):
        self.out_dir = out_dir
        self.duration_seconds = duration_seconds
        self.config = out_dir / "chaos.yaml"
        self.pool: subprocess.Popen[str] | None = None
        self.pool_log: Path | None = None
        self.rows: list[dict] = []
        self.sequence = 0
        out_dir.mkdir(parents=True, exist_ok=True)
        self.config.write_text(config_text(out_dir / "runtime-logs"), encoding="utf-8")

    def start_pool(self, case_dir: Path, suffix: str = "") -> bool:
        self.pool_log = case_dir / f"pool{suffix}.log"
        self.pool = start_logged(pool_command(self.config), self.pool_log)
        return wait_pool_ready(self.pool, self.pool_log)

    def stop_pool(self, grace: float = 20) -> tuple[int | None, bool]:
        if self.pool is None:
            return None, False
        if self.pool.poll() is None:
            os.killpg(self.pool.pid, signal.SIGTERM)
        result = wait_process(self.pool, grace)
        self.pool = None
        return result

    def run_recovery(self, case_dir: Path, seed: int) -> tuple[bool, int, str]:
        log = case_dir / "recovery.log"
        process = start_logged(sim_command(self.config, seed, False), log)
        status, forced = wait_process(process, 90)
        text = read_text(log)
        passed = (
            status == 0 and not forced and "dramstore simulation passed" in text
            and "response validation failed" not in text
        )
        return passed, status if status is not None else 125, str(log)

    def execute(self, scenario: str, cycle: int) -> dict:
        self.sequence += 1
        case_dir = self.out_dir / f"{self.sequence:03d}-{scenario}"
        case_dir.mkdir(parents=True, exist_ok=True)
        row = {
            "sequence": self.sequence,
            "cycle": cycle,
            "scenario": scenario,
            "start": time.strftime("%Y-%m-%d %H:%M:%S %z"),
            "primary_status": None,
            "primary_forced": False,
            "recovery_passed": False,
            "pool_forced": False,
            "passed": False,
            "dir": str(case_dir),
        }
        if not self.start_pool(case_dir):
            row["error"] = "pool_not_ready"
            self.stop_pool()
            return row
        row["metrics_before"] = process_metrics(self.pool.pid)
        seed = 202607300000 + self.sequence * 10
        primary_log = case_dir / "primary.log"
        primary = None
        sockets: list[socket.socket] = []
        pool_restarted = False
        try:
            if scenario == "store_sigkill_midflight":
                primary = start_logged(sim_command(self.config, seed, True), primary_log)
                time.sleep(2)
                os.killpg(primary.pid, signal.SIGKILL)
                row["primary_status"], row["primary_forced"] = wait_process(primary, 10)

            elif scenario == "store_sigstop_resume":
                primary = start_logged(sim_command(self.config, seed, True), primary_log)
                time.sleep(2)
                os.killpg(primary.pid, signal.SIGSTOP)
                time.sleep(5)
                os.killpg(primary.pid, signal.SIGCONT)
                row["primary_status"], row["primary_forced"] = wait_process(primary, 90)

            elif scenario == "pool_sigstop_resume":
                primary = start_logged(sim_command(self.config, seed, True), primary_log)
                time.sleep(2)
                os.killpg(self.pool.pid, signal.SIGSTOP)
                time.sleep(5)
                os.killpg(self.pool.pid, signal.SIGCONT)
                row["primary_status"], row["primary_forced"] = wait_process(primary, 90)

            elif scenario == "pool_sigterm_midflight":
                primary = start_logged(sim_command(self.config, seed, True), primary_log)
                time.sleep(2)
                pool_status, pool_forced = self.stop_pool(30)
                row["midflight_pool_status"] = pool_status
                row["pool_forced"] = pool_forced
                row["primary_status"], row["primary_forced"] = wait_process(primary, 30)
                pool_restarted = self.start_pool(case_dir, "-restarted")

            elif scenario == "pool_sigkill_midflight":
                primary = start_logged(sim_command(self.config, seed, True), primary_log)
                time.sleep(2)
                os.killpg(self.pool.pid, signal.SIGKILL)
                row["midflight_pool_status"], _ = wait_process(self.pool, 10)
                self.pool = None
                row["primary_status"], row["primary_forced"] = wait_process(primary, 30)
                pool_restarted = self.start_pool(case_dir, "-restarted")

            elif scenario == "tcp_malformed_fanout":
                host, port_text = POOL_CONTROL.rsplit(":", 1)
                for index in range(128):
                    sock = socket.create_connection((host, int(port_text)), timeout=2)
                    if index % 3 == 0:
                        sock.sendall(bytes([0xFF, 0x00, index & 0xFF, 0x7F]))
                    sockets.append(sock)
                primary = start_logged(sim_command(self.config, seed, False), primary_log)
                time.sleep(5)
                for sock in sockets:
                    sock.close()
                sockets.clear()
                row["primary_status"], row["primary_forced"] = wait_process(primary, 90)

            elif scenario == "duplicate_store_identity":
                primary = start_logged(sim_command(self.config, seed, True), primary_log)
                time.sleep(1)
                duplicate_log = case_dir / "duplicate.log"
                duplicate = start_logged(sim_command(self.config, seed + 1, False), duplicate_log)
                duplicate_status, duplicate_forced = wait_process(duplicate, 30)
                row["duplicate_status"] = duplicate_status
                row["duplicate_forced"] = duplicate_forced
                row["primary_status"], row["primary_forced"] = wait_process(primary, 90)
            else:
                raise ValueError(scenario)

            if scenario in {"pool_sigterm_midflight", "pool_sigkill_midflight"}:
                if not pool_restarted:
                    row["error"] = "pool_restart_failed"
                    return row
            if self.pool is not None:
                row["metrics_after_primary"] = process_metrics(self.pool.pid)
                recovery, status, recovery_log = self.run_recovery(
                    case_dir, seed + 9
                )
                row["recovery_passed"] = recovery
                row["recovery_status"] = status
                row["recovery_log"] = recovery_log
            primary_text = read_text(primary_log)
            pool_text = "\n".join(read_text(path) for path in case_dir.glob("pool*.log"))
            row["fatal_markers"] = sum(
                marker in primary_text or marker in pool_text
                for marker in (
                    "Segmentation fault", "AddressSanitizer", "double free",
                    "use-after-free", "response validation failed",
                )
            )
            row["passed"] = row["recovery_passed"] and row["fatal_markers"] == 0
            return row
        finally:
            for sock in sockets:
                sock.close()
            if primary is not None and primary.poll() is None:
                wait_process(primary, 1)
            _, forced = self.stop_pool()
            row["pool_forced"] = row["pool_forced"] or forced
            row["end"] = time.strftime("%Y-%m-%d %H:%M:%S %z")
            row["passed"] = row["passed"] and not row["pool_forced"]

    def run(self) -> int:
        started = time.strftime("%Y-%m-%d %H:%M:%S %z")
        deadline = time.monotonic() + self.duration_seconds
        scenarios = [
            "store_sigkill_midflight",
            "store_sigstop_resume",
            "pool_sigstop_resume",
            "pool_sigterm_midflight",
            "pool_sigkill_midflight",
            "tcp_malformed_fanout",
            "duplicate_store_identity",
        ]
        cycle = 0
        while time.monotonic() < deadline or not self.rows:
            cycle += 1
            for scenario in scenarios:
                row = self.execute(scenario, cycle)
                self.rows.append(row)
                with (self.out_dir / "checkpoints.jsonl").open("a", encoding="utf-8") as output:
                    output.write(json.dumps(row, ensure_ascii=False) + "\n")
                print(
                    f"CHAOS {row['sequence']:03d} {scenario}: "
                    f"{'PASS' if row['passed'] else 'FAIL'} "
                    f"primary={row['primary_status']} recovery={row['recovery_passed']} "
                    f"pool_forced={row['pool_forced']}",
                    flush=True,
                )
                if not row["passed"] or time.monotonic() >= deadline:
                    break
            if not self.rows[-1]["passed"]:
                break
        summary = {
            "started": started,
            "ended": time.strftime("%Y-%m-%d %H:%M:%S %z"),
            "requested_duration_seconds": self.duration_seconds,
            "scenarios": len(self.rows),
            "passes": sum(row["passed"] for row in self.rows),
            "failures": sum(not row["passed"] for row in self.rows),
            "rows": self.rows,
        }
        (self.out_dir / "summary.json").write_text(
            json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        print(json.dumps({
            key: summary[key] for key in (
                "started", "ended", "requested_duration_seconds",
                "scenarios", "passes", "failures",
            )
        }, ensure_ascii=False), flush=True)
        return 0 if summary["failures"] == 0 else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("out_dir", type=Path)
    parser.add_argument("--duration-seconds", type=int, default=1800)
    args = parser.parse_args()
    return ChaosRun(args.out_dir, args.duration_seconds).run()


if __name__ == "__main__":
    raise SystemExit(main())
