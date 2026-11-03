#!/usr/bin/env python3
#
# File:        phase9_sides.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 9 item 3: Phase 7's steps 1 and 2 on
#              Community North, National A and National B
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase9_sides.py setup SIDE STACK.orcprj OUT [--orc-cli PATH]
#   phase9_sides.py held SIDE OUT [--json FILE]
#   phase9_sides.py cross SIDE OUT [--json FILE]
#       phase7_real.py's setup, held and cross for SIDE (a key of SIDES),
#       with that side's held runs, single-frame pictures, discs and
#       pressing groups. Pictures are numbered as the side's stacked video
#       is: a disc that Source Align moves on by k frames is read k frames
#       earlier in its own disc map.
#
# The runs and pictures were found on the stacked videos (thumbnails every
# frame): a held run is 22 or more frames whose thumbnails do not change;
# a single-frame still is a frame unlike both its neighbours and unlike
# their average (a cut between motion frames is like the average). National
# B is a CLV side of motion and film: its held runs are the graphic panels,
# and its "pictures" are frames of motion at cuts, which cross-disc
# agreement can use as well as stills.

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import phase7_real  # noqa: E402


def _runs(*spans):
    """Held runs (name, first, last): 24 frames from 2 into each span."""
    return [(name, a + 2, a + 25) for name, a in spans]


SIDES = {
    "community-north": {
        "held": _runs(("bars", 3050), ("uk_map", 44407),
                      ("british_isles_map", 1562), ("menu", 44862),
                      ("os_map", 47091), ("captioned_photograph", 51904),
                      ("harbour_photograph", 52086), ("map_key", 49470),
                      ("village_photograph", 51667)),
        "held_discs": ["DS1", "DS6"],
        "stills": [822, 12196, 12538, 13170, 13960, 14015, 14608, 15459,
                   16522, 16837, 18098, 18328, 20914, 22317, 24681, 26250,
                   29407, 30500, 30649, 33383, 33423, 34332, 34622, 34637],
        "groups": {"DS1": "A", "DS4": "A", "DS6": "B", "DS12": "B",
                   "DS3": "C"},
        "offsets": {"DS2": 1},
    },
    "national-a": {
        "held": _runs(("bars", 1550), ("gallery_title", 52851),
                      ("data_map", 48926), ("gallery", 53743),
                      ("plant_photograph", 44960),
                      ("portrait_photograph", 41475),
                      ("captioned_house_photograph", 43213),
                      ("menu", 52296), ("diamond_graphic", 53349)),
        # DS1's capture ends at frame 37482.
        "held_discs": ["DS2", "DS8"],
        "stills": [1030, 2087, 4890, 5942, 8301, 9716, 12312, 14053, 15669,
                   16771, 17292, 18109, 18516, 19891, 22610, 23861, 29644,
                   31564, 32162, 32795, 35651, 36151, 38628, 39746],
        "groups": {"DS7": "A", "DS2": "A", "DS9": "B", "DS3": "B",
                   "DS4": "C", "DS8": "C"},
        "offsets": {"DS3": 1, "DS11": 1},
    },
    "national-b": {
        "held": _runs(("panel_1", 51880), ("panel_2", 22009),
                      ("panel_3", 38623), ("panel_4", 78121),
                      ("panel_5", 269), ("panel_6", 64149)),
        "held_discs": ["DS10", "DS9"],
        # DS3's and DS9's captures end before 89312, the last cut found.
        "stills": [2936, 4604, 7598, 11734, 13404, 16007, 18264, 21919,
                   24544, 28259, 30798, 35238, 37369, 40747, 44320, 46010,
                   50414, 51018, 56822, 59316, 60978, 64744, 65998, 70077,
                   74250, 75230, 78029, 83262, 86614],
        "groups": {"DS10": "A", "DS2": "A", "DS9": "B", "DS3": "B",
                   "DS4": "C", "DS5": "C", "DS11": "D"},
        "offsets": {"DS11": 1, "DS3": 1, "DS5": 1, "DS12": 1, "DS4": 1,
                    "DS8": 1, "DS1": 1, "DS6": 1},
    },
}


def _use(side):
    """Points phase7_real's tables at |side|."""
    cfg = SIDES[side]
    phase7_real.HELD = cfg["held"]
    phase7_real.HELD_DISCS = cfg["held_discs"]
    phase7_real.STILLS = cfg["stills"]
    phase7_real.GROUPS = cfg["groups"]
    phase7_real.PANEL_DISC = cfg["held_discs"][0]
    return cfg


def _ranges(spans, k):
    """|spans| [(first, last)] in the stacked numbering, in a disc's own
    when Source Align moves it on by |k|."""
    return ",".join(str(a - k) if a == b else f"{a - k}-{b - k}"
                    for a, b in spans)


def setup(side, stack, out, orc_cli):
    cfg = _use(side)
    out = os.path.abspath(out)
    os.makedirs(os.path.join(out, "decodes"), exist_ok=True)
    maps = phase7_real._disc_maps(stack)
    discs = list(dict.fromkeys(cfg["held_discs"] + list(cfg["groups"])))
    cal = {}
    for disc in discs:
        capture, disc_map = maps[disc]
        cal[disc] = phase7_real._calibrate(out, disc, capture, disc_map,
                                           orc_cli)
        # A CLV side has no edge smear (Phase 2): Channel Calibration
        # finds none to apply, and the equaliser is left off.
        if "smear_tau_samples" not in cal[disc]:
            cal[disc].update(smear_fraction=0.0, smear_tau_samples=6.0)
        print(disc, " ".join(f"{k} {v:.4f}" for k, v in cal[disc].items()),
              flush=True)
    import json
    with open(os.path.join(out, "calibration.json"), "w") as f:
        json.dump(cal, f, indent=1)
    held = [(a, b) for _, a, b in cfg["held"]]
    project = phase7_real.Project(f"Domesday2D Phase 9 {side} held stills")
    for row, disc in enumerate(cfg["held_discs"]):
        k = cfg["offsets"].get(disc, 0)
        phase7_real._chain(project, out, disc, *maps[disc],
                           _ranges(held, k), cal[disc], row, "held",
                           reference=True)
    project.write(os.path.join(out, "held.orcprj"))
    stills = [(p, p) for p in cfg["stills"]]
    project = phase7_real.Project(
        f"Domesday2D Phase 9 {side} single-frame pictures")
    for row, disc in enumerate(cfg["groups"]):
        k = cfg["offsets"].get(disc, 0)
        phase7_real._chain(project, out, disc, *maps[disc],
                           _ranges(stills, k), cal[disc], row, "cross")
    project.write(os.path.join(out, "cross.orcprj"))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("setup")
    s.add_argument("side", choices=SIDES)
    s.add_argument("stack")
    s.add_argument("out")
    s.add_argument("--orc-cli", default=os.path.join(
        bench.ROOT, "build", "bin", "orc-cli"))
    for name in ("held", "cross"):
        s = sub.add_parser(name)
        s.add_argument("side", choices=SIDES)
        s.add_argument("out")
        s.add_argument("--json")
    args = ap.parse_args()
    if args.cmd == "setup":
        setup(args.side, args.stack, args.out, args.orc_cli)
    elif args.cmd == "held":
        _use(args.side)
        phase7_real.held(args.out, args.json)
    else:
        _use(args.side)
        phase7_real.cross(args.out, args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
