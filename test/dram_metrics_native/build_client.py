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
