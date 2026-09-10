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

"""Regenerate the DramStore/DramPool block of default_metrics_config.py from YAML."""

from pathlib import Path
import pprint

import yaml

root = Path(__file__).resolve().parents[2]
config = yaml.safe_load(
    (root / "examples/metrics/metrics_configs.yaml").read_text(encoding="utf-8")
)
selected = {
    kind: [
        item
        for item in config[kind]
        if item["name"].startswith(("dramstore_", "drampool_"))
    ]
    for kind in ("counter", "gauge", "histogram")
}
target = root / "ucm/default_metrics_config.py"
source = target.read_text(encoding="utf-8")
marker = "# Generated DramStore/DramPool definitions from examples/metrics/metrics_configs.yaml."
begin = source.index(marker)
end = source.index("\ndef get_default_metrics_config()", begin)
block = (
    marker
    + "\n_DRAM_METRICS_CONFIG = "
    + pprint.pformat(selected, sort_dicts=False, width=95)
)
block += "\nfor _kind, _definitions in _DRAM_METRICS_CONFIG.items():\n    DEFAULT_METRICS_CONFIG[_kind][0:0] = _definitions\n\n"
target.write_text(source[:begin] + block + source[end:], encoding="utf-8")
