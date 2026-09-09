"""Build/run CPU-only client metrics tests: compiler and fmt include path required."""

import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
output = root / "build/dram_metrics_native/client_metrics.exe"
output.parent.mkdir(parents=True, exist_ok=True)
args = [sys.argv[1], "-std=c++17", "-O1", "-DFMT_HEADER_ONLY"]
if os.name != "nt":
    args.append("-pthread")
for path in [
    root / "test/dram_metrics_native/include",
    Path(sys.argv[2]),
    root / "ucm/store/dram/cc",
    root / "ucm/store/detail",
    root / "ucm/shared",
    root / "ucm/shared/infra",
    root / "ucm/shared/metrics/cc/api",
    root / "ucm/shared/metrics/cc/domain",
]:
    args += ["-I", str(path)]
args += [
    str(root / p)
    for p in [
        "test/dram_metrics_native/client_metrics.cc",
        "ucm/store/dram/cc/task_manager.cc",
        "ucm/store/dram/cc/node_actor.cc",
        "ucm/store/dram/cc/kv_protocol.cc",
        "ucm/shared/router/router.cc",
        "ucm/shared/metrics/cc/api/metrics_api.cc",
        "ucm/shared/metrics/cc/domain/metrics.cc",
    ]
]
args += ["-o", str(output)]
subprocess.run(args, check=True)
subprocess.run([str(output)], check=True)
