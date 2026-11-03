#!/usr/bin/env python3
#
# File:        phase5_upstream.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 5 step 3: the equaliser behind the stages
#              that change the composite upstream of the sink
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage: DOMESDAY2D_P5_DIR=DIR phase5_upstream.py [--json out.json]
#
# DIR holds phase5_bench.py's renders and these, made from them with
# orc-cli (see README.md):
#   chan_noisy2       a second capture of chan_noisy's pressing (every
#                     capture-layer noise seed + 1000)
#   chan_beat_noisy   chan_noisy with an FM beat on the master (7.088 MHz,
#                     -24 dB)
#   st_pbl            chan_noisy through picture_band_limit (defaults)
#   st_fbc            chan_beat_noisy through fm_beat_cancel at 7.088 MHz
#   st_stack          chan_noisy and chan_noisy2 through the stacker
#                     (defaults: Weighted, chroma and luma level match)
# Each stage's input and output is decoded by the Phase 4 decoder without
# the equaliser, and with it measured from that file's own VITS (as the
# decoder would measure whatever reaches it).

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import cvbs  # noqa: E402
import equalise  # noqa: E402
import phase5_run  # noqa: E402

FILES = ("chan_noisy", "chan_noisy2", "chan_beat_noisy", "st_pbl", "st_fbc",
         "st_stack")
FRAMES = list(range(bench.FRAMES))
KEYS = [("photo_butterfly", "delta_e_mean"), ("osm_town", "delta_e_mean"),
        ("osm_rural", "delta_e_mean"), ("text_card", "cross_colour_rms_pct"),
        ("test_card_f", "cross_colour_rms_pct"),
        ("green_magenta_edges", "delta_e_mean"), ("bars", "delta_e_mean"),
        ("osm_town", "y_rms_pct")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json")
    args = parser.parse_args()
    for name in FILES:
        bench.CASES[name] = (bench.P5_DIR, name)
    b = bench.Bench()
    results = {}
    for name in FILES:
        capture = cvbs.Capture(bench._base(name) + ".cvbs")
        fit = equalise.fit_smear(capture, FRAMES)
        smear = equalise.measure_smear(capture, FRAMES, (0.0, 1.0))
        r = {"fit": fit, "smear": smear}
        for variant, (s, gain) in {"none": (None, False),
                                   "measured": (smear, True)}.items():
            out = {}
            for f, still in b.stills:
                grid = phase5_run.decode(bench.composite(name, f), s, gain,
                                         False)
                out[still] = bench.measure(grid, b.features[f], b.masks[f])
            r[variant] = out
            row = " ".join(f"{out[st][k]:.2f}" for st, k in KEYS)
            print(f"{name:16s} {variant:9s} smear {np.round(smear, 3)} "
                  f"{row}", flush=True)
        results[name] = r
        if args.json:
            with open(args.json, "w") as f:
                json.dump(results, f, indent=1, default=float)


if __name__ == "__main__":
    sys.exit(main())
