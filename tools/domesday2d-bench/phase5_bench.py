#!/usr/bin/env python3
#
# File:        phase5_bench.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 5: write the videosynth projects for the
#              channel-impaired still bench
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage: phase5_bench.py OUT_DIR   (needs PyYAML)
#        then: videosynth --project OUT_DIR/<name>.yaml, for each name
#
# Four copies of orc-tests/videosynth/projects/still_truth/pal_stills.yaml,
# each with the LaserDisc VITS on lines 19, 20, 332 and 333 (CCIR 17, 18,
# 330, 331) so the equaliser can be measured from them:
#
#   vits_clean, vits_noisy   no channel (the noise-free copy has every
#                            section's noise block removed)
#   chan_clean, chan_noisy   the Phase 2 Domesday CAV channel: edge smear
#                            0.24, tau 6.1 samples, and the inner-radius
#                            replay response (videosynth#14)
#
# The picture truth is that of the bench (videosynth's truth excludes the
# channel). Point $DOMESDAY2D_P5_DIR at OUT_DIR for bench.py to read them.

import os
import sys

import yaml

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SOURCE = os.path.join(ROOT, "orc-tests", "videosynth", "projects",
                      "still_truth", "pal_stills.yaml")

VITS = {"placement": "laserdisc", "vits": [
    {"vits_type": "uk-national", "target_lines": [19]},
    {"vits_type": "vits20", "target_lines": [20]},
    {"vits_type": "itu-composite", "target_lines": [332]},
    {"vits_type": "itu-combination", "target_lines": [333]}]}
SMEAR = {"edge_smear": {"fraction": 0.24, "tau_samples": 6.1}}
RESPONSE = {"channel": {"response_db": [
    [0, 0], [0.5, 0.6], [1.0, 1.0], [2.0, 1.4], [4.0, 2.1], [4.8, 1.0],
    [5.8, -9.8]]}}


def main():
    out_dir = sys.argv[1]
    with open(SOURCE) as f:
        source = yaml.safe_load(f)
    for name in ("vits_clean", "vits_noisy", "chan_clean", "chan_noisy"):
        p = yaml.safe_load(yaml.safe_dump(source))
        p["line_injections"] = VITS
        if name.startswith("chan"):
            p["channel"] = SMEAR
            p["laserdisc"] = RESPONSE
        if name.endswith("clean"):
            for section in p["sections"]:
                section.pop("noise", None)
        p["output"]["video_path"] = "{output}/" + name + ".cvbs"
        with open(os.path.join(out_dir, name + ".yaml"), "w") as f:
            yaml.safe_dump(p, f, sort_keys=False)


if __name__ == "__main__":
    sys.exit(main())
