#!/usr/bin/env python3
#
# File:        phase5_run.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 5 steps 1 and 2 on the bench: the Phase 4
#              decoder with each equaliser variant, without and with the
#              Phase 2 channel
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage: DOMESDAY2D_P5_DIR=DIR phase5_run.py [--json out.json]
#
# DIR holds phase5_bench.py's renders. Each case's smear is measured once
# from its own VITS over all fourteen frames ('measured'), as a decoder
# would measure it over a source; 'per_frame' fits each frame's VITS alone,
# falling back to no correction when it cannot tell. The decoder is the
# Phase 4 one (frame-raster transform, partner-checked masked averaging,
# luma-steered vertical fit, residual reassignment); there is no chroma
# error on these renders, so no line-alternating correction.

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adaptive  # noqa: E402
import bench  # noqa: E402
import cvbs  # noqa: E402
import equalise  # noqa: E402
import hanover  # noqa: E402
import transform  # noqa: E402

CASES = ("vits_clean", "vits_noisy", "chan_clean", "chan_noisy")
RESPONSE = [(0, 0), (0.5, 0.6), (1.0, 1.0), (2.0, 1.4), (4.0, 2.1),
            (4.8, 1.0), (5.8, -9.8)]
FRAMES = list(range(bench.FRAMES))

# name: (smear source, burst gain, inverse response)
VARIANTS = {
    "none": (None, False, False),
    "gain": (None, True, False),
    "measured_nogain": ("measured", False, False),
    "measured": ("measured", True, False),
    "per_frame": ("per_frame", True, False),
    "nominal": ("nominal", True, False),
    "true": ("true", True, False),
    "measured_response": ("measured", True, True),
}


def decode(raw, smear, gain, response):
    if smear is not None and smear[0] > 0.0:
        raw = equalise.unsmear(raw, *smear)
    if response:
        raw = equalise.unresponse(raw, RESPONSE)
    raster, c, phi, s = transform.separate(raw, "frame", ytile=32)
    d = adaptive.decode_separated(raster, c, phi, s,
                                  prefilter=hanover.masked_average())
    if gain:
        g = equalise.burst_gain(raster)
        d.u, d.v = d.u * g, d.v * g
    return d.truth_grid()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json")
    parser.add_argument("--only", nargs="*")
    args = parser.parse_args()
    b = bench.Bench()
    results = {"smear": {}}
    for case in CASES:
        capture = cvbs.Capture(bench._base(case) + ".cvbs")
        fit = equalise.fit_smear(capture, FRAMES)
        measured = equalise.measure_smear(capture, FRAMES, (0.0, 1.0))
        per_frame = {f: equalise.measure_smear(capture, [f], (0.0, 1.0))
                     for f, _ in b.stills}
        results["smear"][case] = {"fit": fit, "measured": measured,
                                  "per_frame": per_frame}
        print(case, "fit", np.round(fit, 3), "used", measured, flush=True)
        results[case] = {}
        for name, (source, gain, response) in VARIANTS.items():
            if args.only and name not in args.only:
                continue
            out = {}
            for f, still in b.stills:
                smear = {None: None, "measured": measured,
                         "per_frame": per_frame[f],
                         "nominal": equalise.NOMINAL,
                         "true": equalise.NOMINAL if case.startswith("chan")
                         else (0.0, 1.0)}[source]
                grid = decode(bench.composite(case, f), smear, gain,
                              response)
                out[still] = bench.measure(grid, b.features[f], b.masks[f])
                if f == bench.GREEN_EDGES:
                    out[still]["edge_dots_codes"] = bench.edge_dots(
                        grid[0], b.truth[f][0])
            results[case][name] = out
            print(f"{case:10s} {name:18s} town dE "
                  f"{out['osm_town']['delta_e_mean']:5.2f}  text cc "
                  f"{out['text_card'].get('cross_colour_rms_pct', 0):5.2f}"
                  f"  butterfly dE "
                  f"{out['photo_butterfly']['delta_e_mean']:5.2f}  edge dE "
                  f"{out['green_magenta_edges']['delta_e_mean']:5.2f}",
                  flush=True)
            if args.json:
                with open(args.json, "w") as f:
                    json.dump(results, f, indent=1)


if __name__ == "__main__":
    sys.exit(main())
