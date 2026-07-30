#!/usr/bin/env python3
"""Runtime YAML boundary matrix for the real DramPool binary and DramStore simulator."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time


ROOT = Path("/home/codex/ucm-dramstore-sim-codex-20260728")
POOL_BIN = ROOT / "build-dramstore-sim-cann851/ucm/store/dram/drampool"
SIM_BIN = ROOT / "ucm/store/dram/build/dramstore_sim"
POOL_CONTROL = "110.138.0.3:50000"
STORE_CONTROL = "110.138.0.3:50001"
POOL_ONE_SIDED = "110.138.0.3:50100"
STORE_ONE_SIDED = "110.138.0.3:50101"


DEFAULTS = {
    "device_ids": "[4]",
    "request_depth": "65536",
    "completion_depth": "65536",
    "idle_wait_us": "100",
    "pending_depth": "64",
    "capacity_mb": "64",
    "slot_size_bytes": "64",
    "gc_enabled": "true",
    "gc_interval_ms": "1000",
    "periodic_policy": "TTL",
    "deep_policy": "POSITION",
    "lease_time_ms": "10000",
    "evict_ratio": "0.2",
    "evict_period_ms": "31536000000",
    "timeout_ms": "10000",
    "log_level": "info",
    "log_dir": None,
    "log_max_files": "10",
    "log_max_size_mb": "5",
}


def build_yaml(out_dir: Path, overrides: dict[str, str] | None = None,
               endpoints: str | None = None) -> str:
    values = dict(DEFAULTS)
    values["log_dir"] = str(out_dir / "runtime-logs")
    if overrides:
        values.update(overrides)
    endpoint_text = endpoints
    if endpoint_text is None:
        endpoint_text = (
            f'    - two_sided: "{POOL_CONTROL}"\n'
            f'      one_sided: "{POOL_ONE_SIDED}"\n'
            f'    - two_sided: "{STORE_CONTROL}"\n'
            f'      one_sided: "{STORE_ONE_SIDED}"'
        )
    return f"""transport:
  device_ids: {values["device_ids"]}
  endpoints:
{endpoint_text}
queue:
  request_depth: {values["request_depth"]}
  completion_depth: {values["completion_depth"]}
request_receiver:
  idle_wait_us: {values["idle_wait_us"]}
poller:
  pending_depth: {values["pending_depth"]}
flag_buffer:
  capacity_mb: {values["capacity_mb"]}
  slot_size_bytes: {values["slot_size_bytes"]}
gc:
  enabled: {values["gc_enabled"]}
  interval_ms: {values["gc_interval_ms"]}
metadata:
  periodic_eviction_policy: {values["periodic_policy"]}
  deep_eviction_policy: {values["deep_policy"]}
  lease_time_ms: {values["lease_time_ms"]}
  default_evict_ratio: {values["evict_ratio"]}
  evict_period_ms: {values["evict_period_ms"]}
operation:
  timeout_ms: {values["timeout_ms"]}
logger:
  level: {values["log_level"]}
  dir: "{values["log_dir"]}"
  max_files: {values["log_max_files"]}
  max_size_mb: {values["log_max_size_mb"]}
"""


def base_env() -> dict[str, str]:
    env = os.environ.copy()
    cann = "/usr/local/Ascend/cann-8.5.1"
    env.update({
        "ASCEND_HOME_PATH": cann,
        "ASCEND_TOOLKIT_HOME": cann,
        "HIXL_HOME": f"{cann}/aarch64-linux",
        "LD_LIBRARY_PATH": (
            f"{cann}/lib64:{cann}/aarch64-linux/lib:"
            f"{cann}/runtime/lib64:{env.get('LD_LIBRARY_PATH', '')}"
        ),
    })
    return env


def pool_command(config: Path) -> list[str]:
    return [
        str(POOL_BIN),
        "--addr", POOL_CONTROL,
        "--nics", "mlx5_0",
        "--pool-size-gb", "1",
        "--kvcache-block-sizes", "4096", "65536",
        "--kvcache-block-proportions", "1", "1",
        "--ttl-minutes", "120",
        "--config", str(config),
    ]


def tail(text: str, lines: int = 8) -> str:
    return "\n".join(text.splitlines()[-lines:])


def terminate_process(process: subprocess.Popen[str], grace: float = 12.0) -> tuple[int, bool]:
    if process.poll() is not None:
        return process.returncode, False
    os.killpg(process.pid, signal.SIGTERM)
    try:
        return process.wait(timeout=grace), False
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        return process.wait(timeout=5), True


def invalid_cases(out_dir: Path) -> list[tuple[str, str]]:
    base = build_yaml(out_dir)
    duplicate_endpoints = (
        f'    - two_sided: "{POOL_CONTROL}"\n'
        f'      one_sided: "{POOL_ONE_SIDED}"\n'
        f'    - two_sided: "{POOL_CONTROL}"\n'
        f'      one_sided: "{STORE_ONE_SIDED}"'
    )
    duplicate_one_sided = (
        f'    - two_sided: "{POOL_CONTROL}"\n'
        f'      one_sided: "{POOL_ONE_SIDED}"\n'
        f'    - two_sided: "{STORE_CONTROL}"\n'
        f'      one_sided: "{POOL_ONE_SIDED}"'
    )
    overlap_endpoint = (
        f'    - two_sided: "{POOL_CONTROL}"\n'
        f'      one_sided: "{STORE_CONTROL}"\n'
        f'    - two_sided: "{STORE_CONTROL}"\n'
        f'      one_sided: "{STORE_ONE_SIDED}"'
    )
    missing_local = (
        f'    - two_sided: "{STORE_CONTROL}"\n'
        f'      one_sided: "{STORE_ONE_SIDED}"'
    )
    overrides = [
        ("device_empty", {"device_ids": "[]"}),
        ("device_duplicate", {"device_ids": "[4, 4]"}),
        ("device_negative", {"device_ids": "[-1]"}),
        ("request_depth_1", {"request_depth": "1"}),
        ("completion_depth_1", {"completion_depth": "1"}),
        ("idle_wait_zero", {"idle_wait_us": "0"}),
        ("pending_zero", {"pending_depth": "0"}),
        ("flag_capacity_zero", {"capacity_mb": "0"}),
        ("flag_slot_zero", {"slot_size_bytes": "0"}),
        ("flag_slot_below_minimum_response", {"slot_size_bytes": "1"}),
        ("flag_too_few_slots", {
            "capacity_mb": "1", "slot_size_bytes": "1048576", "pending_depth": "2"
        }),
        ("flag_capacity_overflow", {"capacity_mb": "17592186044416"}),
        ("flag_slot_overflow", {"slot_size_bytes": "18446744073709551615"}),
        ("gc_enabled_interval_zero", {"gc_enabled": "true", "gc_interval_ms": "0"}),
        ("lease_zero", {"lease_time_ms": "0"}),
        ("lease_over_int64", {"lease_time_ms": "9223372036854775808"}),
        ("ratio_negative", {"evict_ratio": "-0.01"}),
        ("ratio_over_one", {"evict_ratio": "1.01"}),
        ("ratio_nan", {"evict_ratio": "nan"}),
        ("ratio_inf", {"evict_ratio": "inf"}),
        ("evict_period_zero", {"evict_period_ms": "0"}),
        ("evict_period_over_int64", {"evict_period_ms": "9223372036854775808"}),
        ("timeout_zero", {"timeout_ms": "0"}),
        ("logger_bad_level", {"log_level": "verbose"}),
        ("logger_empty_dir", {"log_dir": ""}),
        ("logger_files_zero", {"log_max_files": "0"}),
        ("logger_size_zero", {"log_max_size_mb": "0"}),
        ("logger_files_over_int", {"log_max_files": "2147483648"}),
        ("policy_periodic_lru", {"periodic_policy": "LRU"}),
        ("policy_deep_unknown", {"deep_policy": "RANDOM"}),
    ]
    cases = [(name, build_yaml(out_dir, values)) for name, values in overrides]
    cases.extend([
        ("endpoint_duplicate_control", build_yaml(out_dir, endpoints=duplicate_endpoints)),
        ("endpoint_duplicate_one_sided", build_yaml(out_dir, endpoints=duplicate_one_sided)),
        ("endpoint_role_overlap", build_yaml(out_dir, endpoints=overlap_endpoint)),
        ("endpoint_missing_local", build_yaml(out_dir, endpoints=missing_local)),
        ("endpoint_missing_one_sided", build_yaml(
            out_dir, endpoints=f'    - two_sided: "{POOL_CONTROL}"'
        )),
        ("endpoint_bad_port", base.replace(POOL_ONE_SIDED, "110.138.0.3:70000", 1)),
        ("missing_required_key", base.replace("  completion_depth: 65536\n", "")),
        ("unknown_scalar_key", base.replace(
            "  completion_depth: 65536\n",
            "  completion_depth: 65536\n  mystery: 1\n",
        )),
        ("duplicate_scalar_key", base.replace(
            "  completion_depth: 65536\n",
            "  completion_depth: 65536\n  completion_depth: 8\n",
        )),
        ("tab_indentation", base.replace("  request_depth", "\trequest_depth", 1)),
        ("unmatched_quote", base.replace('  level: info', '  level: "info', 1)),
        ("device_list_malformed", base.replace("  device_ids: [4]", "  device_ids: 4", 1)),
        ("bad_boolean", base.replace("  enabled: true", "  enabled: maybe", 1)),
        ("uint_negative", base.replace("  timeout_ms: 10000", "  timeout_ms: -1", 1)),
        ("uint_overflow", base.replace(
            "  timeout_ms: 10000", "  timeout_ms: 4294967296", 1
        )),
    ])
    return cases


VALID_CASES = [
    ("baseline", {}),
    ("queue_minimum", {
        "request_depth": "2", "completion_depth": "2", "pending_depth": "1"
    }),
    ("queue_small_balanced", {
        "request_depth": "8", "completion_depth": "8", "pending_depth": "4"
    }),
    ("queue_asymmetric", {
        "request_depth": "2", "completion_depth": "65536", "pending_depth": "1"
    }),
    ("idle_wait_1us", {"idle_wait_us": "1"}),
    ("idle_wait_500ms", {"idle_wait_us": "500000"}),
    ("flag_minimum", {"capacity_mb": "1", "slot_size_bytes": "64", "pending_depth": "1"}),
    ("flag_slot_2bytes", {"capacity_mb": "1", "slot_size_bytes": "2"}),
    ("flag_slot_4096bytes", {"capacity_mb": "1", "slot_size_bytes": "4096"}),
    ("gc_disabled_interval_zero", {"gc_enabled": "false", "gc_interval_ms": "0"}),
    ("gc_interval_1ms", {"gc_enabled": "true", "gc_interval_ms": "1"}),
    ("policies_position_ttl", {"periodic_policy": "POSITION", "deep_policy": "TTL"}),
    ("metadata_boundary_fast", {
        "lease_time_ms": "1", "evict_ratio": "1.0", "evict_period_ms": "1"
    }),
    ("operation_timeout_100ms", {"timeout_ms": "100"}),
    ("logger_trace_min_rotation", {
        "log_level": "trace", "log_max_files": "1", "log_max_size_mb": "1"
    }),
    ("logger_critical", {"log_level": "critical"}),
    ("two_transport_devices", {"device_ids": "[4, 6]"}),
]


def run_invalid(out_dir: Path) -> dict:
    rows = []
    for index, (name, yaml_text) in enumerate(invalid_cases(out_dir), 1):
        config = out_dir / f"invalid-{index:02d}-{name}.yaml"
        log = out_dir / f"invalid-{index:02d}-{name}.log"
        config.write_text(yaml_text, encoding="utf-8")
        start = time.monotonic()
        timed_out = False
        try:
            result = subprocess.run(
                pool_command(config),
                env=base_env(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=8,
            )
            status = result.returncode
            output = result.stdout
        except subprocess.TimeoutExpired as error:
            timed_out = True
            status = 124
            output = (error.stdout or "") + "\nPROCESS_ACCEPTED_INVALID_CONFIG_OR_HUNG"
        elapsed = time.monotonic() - start
        log.write_text(output, encoding="utf-8")
        passed = status != 0 and not timed_out
        rows.append({
            "name": name,
            "passed": passed,
            "status": status,
            "elapsed_seconds": round(elapsed, 3),
            "diagnostic": tail(output).replace("\n", " | "),
            "config": str(config),
            "log": str(log),
        })
        print(f"INVALID {index:02d} {name}: {'PASS' if passed else 'FAIL'} "
              f"status={status} elapsed={elapsed:.3f}s", flush=True)
    return {"kind": "invalid", "rows": rows}


def wait_ready(process: subprocess.Popen[str], log: Path, timeout: float = 30.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return False
        if log.exists() and "DramPool service ready" in log.read_text(
                encoding="utf-8", errors="replace"
        ):
            return True
        time.sleep(0.1)
    return False


def run_valid(out_dir: Path) -> dict:
    rows = []
    env = base_env()
    for index, (name, overrides) in enumerate(VALID_CASES, 1):
        case_dir = out_dir / f"valid-{index:02d}-{name}"
        case_dir.mkdir(parents=True, exist_ok=True)
        config = case_dir / "drampool.yaml"
        pool_log = case_dir / "pool.log"
        sim_log = case_dir / "sim.log"
        config.write_text(build_yaml(case_dir, overrides), encoding="utf-8")
        started = time.time()
        with pool_log.open("w", encoding="utf-8") as pool_output:
            pool = subprocess.Popen(
                pool_command(config),
                env=env,
                text=True,
                stdout=pool_output,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        if name == "logger_critical":
            # A healthy critical-only logger intentionally suppresses the ready INFO line.
            time.sleep(3)
            ready = pool.poll() is None
        else:
            ready = wait_ready(pool, pool_log)
        sim_status = None
        sim_timed_out = False
        if ready:
            sim_command = [
                str(SIM_BIN),
                "--config", str(config),
                "--pool-control", POOL_CONTROL,
                "--store-control", STORE_CONTROL,
                "--devices", "5",
                "--store-index", "0",
                "--key-seed", str(202607290000 + index),
                "--block-size", "4096",
                "--block-num", "2",
                "--rounds", "1",
                "--put", "20",
                "--get", "20",
                "--lookup-exist", "20",
                "--lookup-miss", "20",
                "--eviction-aware", "1",
            ]
            try:
                result = subprocess.run(
                    sim_command,
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    timeout=90,
                )
                sim_status = result.returncode
                sim_output = result.stdout
            except subprocess.TimeoutExpired as error:
                sim_status = 124
                sim_timed_out = True
                sim_output = (error.stdout or "") + "\nSIMULATOR_TIMEOUT"
            sim_log.write_text(sim_output, encoding="utf-8")
        else:
            sim_output = ""
            sim_log.write_text("POOL_NOT_READY\n", encoding="utf-8")
        pool_status, forced = terminate_process(pool)
        pool_text = pool_log.read_text(encoding="utf-8", errors="replace")
        expected_timeout = name == "operation_timeout_100ms"
        if expected_timeout:
            passed = (
                ready and sim_status != 0 and not sim_timed_out and not forced
                and "timed out" in sim_output
                and "recovered peer after transfer failure" in pool_text
                and "Segmentation fault" not in pool_text
            )
        else:
            passed = (
                ready and sim_status == 0 and not sim_timed_out and not forced
                and "dramstore simulation passed" in sim_output
                and "AddressSanitizer" not in sim_output
                and "Segmentation fault" not in pool_text
                and "[UC][E]" not in pool_text
                and "TransportManager shutdown failed" not in pool_text
            )
        rows.append({
            "name": name,
            "overrides": overrides,
            "passed": passed,
            "ready": ready,
            "sim_status": sim_status,
            "pool_status": pool_status,
            "forced_cleanup": forced,
            "expected_timeout": expected_timeout,
            "elapsed_seconds": round(time.time() - started, 3),
            "pool_tail": tail(pool_text).replace("\n", " | "),
            "sim_tail": tail(sim_output).replace("\n", " | "),
            "dir": str(case_dir),
        })
        print(f"VALID {index:02d} {name}: {'PASS' if passed else 'FAIL'} "
              f"ready={ready} sim={sim_status} pool={pool_status} forced={forced}",
              flush=True)
        time.sleep(0.5)
    return {"kind": "valid", "rows": rows}


def write_results(out_dir: Path, result: dict, started: str) -> int:
    result["started"] = started
    result["ended"] = time.strftime("%Y-%m-%d %H:%M:%S %z")
    result["passes"] = sum(row["passed"] for row in result["rows"])
    result["failures"] = len(result["rows"]) - result["passes"]
    (out_dir / f'{result["kind"]}-summary.json').write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps({
        "kind": result["kind"],
        "started": result["started"],
        "ended": result["ended"],
        "passes": result["passes"],
        "failures": result["failures"],
        "out_dir": str(out_dir),
    }, ensure_ascii=False), flush=True)
    return 0 if result["failures"] == 0 else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("invalid", "valid"))
    parser.add_argument("out_dir", type=Path)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    started = time.strftime("%Y-%m-%d %H:%M:%S %z")
    result = run_invalid(args.out_dir) if args.mode == "invalid" else run_valid(args.out_dir)
    return write_results(args.out_dir, result, started)


if __name__ == "__main__":
    raise SystemExit(main())
