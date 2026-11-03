#!/usr/bin/env python3
#
# File:        phase7_rotation.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 7: the still bench with its stills rotated
#              through the four frames of the PAL sequence
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase7_rotation.py write OUT
#       OUT/rot{0,1,2,3}.yaml and rot{0,1,2,3}_clean.yaml: pal_stills.yaml
#       with its twelve stills (frames 1-12) moved on by 0-3 frames, the last
#       ones wrapping to the front. Each still keeps its own noise seeds, so
#       only its place in the 4-frame (8-field) PAL sequence changes: twelve
#       is a multiple of four, so a still moved by r sits r frames later in
#       the sequence whether it wrapped or not. rot0 is the bench as it is.
#       Then: videosynth --project OUT/<name>.yaml --output-root OUT, and
#       decode each with the video sink (raw yuv, output_padding 1) to
#       OUT/<name>_<decoder>.yuv.
#   phase7_rotation.py score OUT [--json FILE]
#       DecoderStillTruth's metrics for every still at every rotation.
#
# The bench has always put each still on the same frame, so every still
# has only ever been decoded at one carrier phase and V-switch alignment. A
# decoder tuned on it could fit a still and its phase together; the spread
# of a still's score over the rotations shows whether any did.

import argparse
import copy
import json
import os
import sys

import numpy as np
import yaml
from scipy.ndimage import maximum_filter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402

PROJECT = os.path.join(bench.ROOT, "orc-tests", "videosynth", "projects",
                       "still_truth", "pal_stills.yaml")
DECODERS = ["pal2d", "transform2d", "domesday2d"]
ROTATIONS = range(4)
STILL_COUNT = 12  # sections 1-12; section 0 (Lead) and 13 (Tail) stay


def rotated_frame(still_frame, r):
    """Where the still on bench frame |still_frame| (1-12) lands."""
    return (still_frame - 1 + r) % STILL_COUNT + 1


def write(out):
    os.makedirs(out, exist_ok=True)
    with open(PROJECT) as f:
        doc = yaml.safe_load(f)
    for r in ROTATIONS:
        for clean in (False, True):
            d = copy.deepcopy(doc)
            stills = d["sections"][1:1 + STILL_COUNT]
            moved = [None] * STILL_COUNT
            for i, section in enumerate(stills):
                moved[rotated_frame(i + 1, r) - 1] = section
            d["sections"][1:1 + STILL_COUNT] = moved
            name = f"rot{r}" + ("_clean" if clean else "")
            if clean:
                for section in d["sections"]:
                    section.pop("noise", None)
            d["project"]["name"] = f"PalStillsRot{r}" + ("Clean" if clean
                                                         else "")
            d["output"]["video_path"] = f"{{output}}/{name}.cvbs"
            # The project's {bundled} pictures resolve against its own
            # folder in videosynth; keep them absolute to the original's.
            with open(os.path.join(out, f"{name}.yaml"), "w") as f:
                yaml.safe_dump(d, f, sort_keys=False)
            print(name)


def _truth(path, frame):
    plane = bench.TRUTH_WIDTH * bench.TRUTH_ROWS
    data = np.memmap(path, dtype="<f4", mode="r")
    block = np.asarray(data[frame * 3 * plane:(frame + 1) * 3 * plane],
                       dtype=np.float64).reshape(3, bench.TRUTH_ROWS,
                                                 bench.TRUTH_WIDTH)
    return block[0] / 700.0, block[1] / 700.0, block[2] / 700.0


def _mask(out, r, f):
    """bench.masks() for the still on bench frame |f| at rotation |r|:
    the stress card's line-sign strips wherever it lands, and the clipped
    samples read from the rotated clean composite."""
    saved = bench.CASES["clean"]
    bench.CASES["clean"] = (os.path.abspath(out), f"rot{r}_clean")
    try:
        raster = bench.composite("clean", rotated_frame(f, r))
    finally:
        bench.CASES["clean"] = saved
    mask = bench.picture_mask()
    if f == bench.STRESS:
        for x, y, w, h in bench.LINE_SIGN_REGIONS:
            x0 = max(0, x - bench.LINE_SIGN_MARGIN)
            y0 = max(0, y - bench.LINE_SIGN_MARGIN)
            a, b = bench.source_columns(x0, x + w + bench.LINE_SIGN_MARGIN -
                                        x0)
            mask[y0:y + h + bench.LINE_SIGN_MARGIN, a:b] = False
    first, width = bench.TRUTH_FIRST_SAMPLE, bench.TRUTH_WIDTH
    rows = np.empty((bench.TRUTH_ROWS, width))
    rows[0::2] = raster[22:22 + 288, first:first + width]
    rows[1::2] = raster[335:335 + 288, first:first + width]
    clipped = ((rows <= bench.SYNC_TIP) | (rows >= bench.PEAK)) & \
        bench.picture_mask()
    spread = maximum_filter(clipped.astype(np.uint8),
                            size=(2 * bench.CLIP_MARGIN_ROWS + 1,
                                  2 * bench.CLIP_MARGIN_SAMPLES + 1),
                            mode="constant") > 0
    return mask & ~spread


def score(out, json_path):
    b = bench.Bench()
    results = {}
    for case in ("clean", "noisy"):
        for f, still in b.stills:
            row = {}
            masks = {r: _mask(out, r, f) for r in ROTATIONS}
            for r in ROTATIONS:
                name = f"rot{r}" + ("_clean" if case == "clean" else "")
                at = rotated_frame(f, r)
                truth = _truth(os.path.join(out, f"{name}.truth"), at)
                # The truth moves with the still: check the rotation did.
                if np.abs(truth[0] - b.truth[f][0]).max() > 1e-6:
                    raise SystemExit(f"{name}: frame {at} is not {still}")
                for d in DECODERS:
                    got = bench.decoded(
                        os.path.join(out, f"{name}_{d}.yuv"), at)
                    m = bench.measure(got, b.features[f], masks[r])
                    row.setdefault(d, []).append(m)
            results[f"{case} {still}"] = row
            line = []
            for d in DECODERS:
                key = ("cross_colour_rms_pct"
                       if "cross_colour_rms_pct" in row[d][0]
                       else "delta_e_mean")
                vals = [m[key] for m in row[d]]
                line.append(f"{d} " + "/".join(f"{v:.2f}" for v in vals))
            print(f"{case:5s} {still:20s} {key[:5]}  " + "  ".join(line),
                  flush=True)
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("write")
    s.add_argument("out")
    s = sub.add_parser("score")
    s.add_argument("out")
    s.add_argument("--json")
    args = ap.parse_args()
    if args.cmd == "write":
        write(args.out)
    else:
        score(args.out, args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
