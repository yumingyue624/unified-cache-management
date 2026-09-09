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
