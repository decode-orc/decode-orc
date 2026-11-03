#!/usr/bin/env python3
#
# File:        phase5_decoders.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 5: the equaliser as a stage of its own, in
#              front of each decoder, with its chroma gain applied to the
#              composite
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage: DOMESDAY2D_P5_DIR=DIR phase5_decoders.py [--json out.json]
#
# A stage before the sink cannot scale a decoder's U and V. It can scale
# the composite's subcarrier band, as the Stacker's chroma_gain_match does:
#   x + (g - 1) h * x,  h the 5-tap (1 - cos wT)^2 shelf (0 at DC, 1 at the
# subcarrier, T one 4fsc sample),
# with g the nominal burst over the frame's burst after the inverse smear.
# Each decoder (the prototype pal2d, transform2d and the Phase 4 decoder)
# is scored without the equaliser, with it and the gain on U and V (as
# phase5_run.py), and with it as a stage (the gain on the composite).

import argparse
import json
import os
import sys

import numpy as np
from scipy.ndimage import convolve1d

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adaptive  # noqa: E402
import bench  # noqa: E402
import cvbs  # noqa: E402
import equalise  # noqa: E402
import hanover  # noqa: E402
import phase4_hanover  # noqa: E402
import proto  # noqa: E402
import transform  # noqa: E402

CASES = ("vits_clean", "vits_noisy", "chan_clean", "chan_noisy")
SHELF = np.array([0.25, -1.0, 1.5, -1.0, 0.25])
KEYS = [("photo_butterfly", "delta_e_mean"), ("photo_galtstroms",
                                               "delta_e_mean"),
        ("osm_town", "delta_e_mean"), ("osm_rural", "delta_e_mean"),
        ("text_card", "cross_colour_rms_pct"),
        ("luma_zone_plate", "cross_colour_rms_pct"),
        ("test_card_f", "cross_colour_rms_pct"),
        ("green_magenta_edges", "delta_e_mean"), ("osm_town", "y_rms_pct"),
        ("bars", "chroma_gain_error_pct")]


def stage(raw, smear):
    """The equaliser as a stage: inverse smear, then the burst gain on the
    composite's subcarrier band."""
    if smear[0] > 0.0:
        raw = equalise.unsmear(raw, *smear)
    g = equalise.burst_gain(bench.frame_raster(raw))
    x = raw - bench.BLACK
    return bench.BLACK + x + (g - 1.0) * convolve1d(x, SHELF, axis=1,
                                                   mode="nearest")


def pal2d(raw):
    raster = bench.frame_raster(raw)
    phi, s = proto.lattice(raster)
    return phase4_hanover.pal2d(raster, None, phi, s, None)


def transform2d(raw):
    return transform.decode(raw, "field")


def phase4(raw):
    raster, c, phi, s = transform.separate(raw, "frame", ytile=32)
    return adaptive.decode_separated(raster, c, phi, s,
                                     prefilter=hanover.masked_average())


DECODERS = {"pal2d": pal2d, "transform2d": transform2d, "phase4": phase4}


def run(decoder, raw, smear, mode):
    if mode == "stage":
        return decoder(stage(raw, smear)).truth_grid()
    if mode == "uv" and smear[0] > 0.0:
        raw = equalise.unsmear(raw, *smear)
    d = decoder(raw)
    if mode == "uv":
        g = equalise.burst_gain(bench.frame_raster(raw))
        d.u, d.v = d.u * g, d.v * g
    return d.truth_grid()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json")
    args = parser.parse_args()
    b = bench.Bench()
    results = {}
    for case in CASES:
        capture = cvbs.Capture(bench._base(case) + ".cvbs")
        smear = equalise.measure_smear(capture, list(range(bench.FRAMES)),
                                       (0.0, 1.0))
        results[case] = {"smear": smear}
        for name, decoder in DECODERS.items():
            for mode in ("none", "uv", "stage"):
                out = {}
                for f, still in b.stills:
                    grid = run(decoder, bench.composite(case, f), smear, mode)
                    out[still] = bench.measure(grid, b.features[f],
                                               b.masks[f])
                results[case][f"{name}/{mode}"] = out
                row = " ".join(f"{out[s][k]:5.2f}" for s, k in KEYS)
                print(f"{case:10s} {name:11s} {mode:5s} {row}", flush=True)
                if args.json:
                    with open(args.json, "w") as f:
                        json.dump(results, f, indent=1, default=float)


if __name__ == "__main__":
    sys.exit(main())
