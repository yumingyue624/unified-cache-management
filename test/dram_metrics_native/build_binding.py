"""Build the actual metrics pybind module without an accelerator runtime.

Usage: python test/dram_metrics_native/build_binding.py [clang++|g++]
The standalone test build and all its support live under test/.
"""

import os
from pathlib import Path
import subprocess
import sys
import sysconfig

import pybind11

root = Path(__file__).resolve().parents[2]
output = root / "build" / "dram_metrics_native"
output.mkdir(parents=True, exist_ok=True)
compiler = sys.argv[1] if len(sys.argv) > 1 else "c++"
metrics = root / "ucm/shared/metrics"
args = [compiler, "-std=c++17", "-shared", "-O1"]
if os.name != "nt":
    args += ["-fPIC", "-pthread"]
for path in [
    metrics / "cc/domain",
    metrics / "cc/api",
    Path(pybind11.get_include()),
    Path(sysconfig.get_path("include")),
]:
    args += ["-I", str(path)]
for key in [
    "UCM_PROJECT_NAME",
    "UCM_PROJECT_VERSION",
    "UCM_COMMIT_ID",
    "UCM_BUILD_TYPE",
]:
    args.append(f'-D{key}="test"')
args += [
    str(metrics / p)
    for p in ["cc/domain/metrics.cc", "cc/api/metrics_api.cc", "cpy/metrics.py.cc"]
]
if os.name == "nt":
    args += [
        "-L",
        str(Path(sys.base_prefix) / "libs"),
        f"-lpython{sys.version_info.major}{sys.version_info.minor}",
    ]
args += ["-o", str(output / ("ucmmetrics" + sysconfig.get_config_var("EXT_SUFFIX")))]
subprocess.run(args, check=True)
print(output)
