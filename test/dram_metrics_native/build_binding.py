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
