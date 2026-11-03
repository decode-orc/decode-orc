#!/usr/bin/env python3
#
# File:        phase3_run.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 3 steps 2, 3 and 5: score separation
#              prototypes on the still bench, clean and noisy
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage: phase3_run.py [--yuv-dir DIR] [--json out.json] [--only NAME...]
#
# Every prototype in VARIANTS decodes the twelve stills of the clean and
# the noisy (Domesday noise model) bench, and is scored by bench.measure()
# as DecoderStillTruth scores decode-orc's decoders, plus the "||||" dot
# amplitude (bench.edge_dots). With --yuv-dir, decode-orc's own pal2d and
# transform2d decodes (<dir>/pal_stills{_clean,}_<decoder>.yuv, written by
# the video sink with output_mode raw, raw_format yuv, output_padding 1)
# are scored beside them.

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import lslcd  # noqa: E402
import proto  # noqa: E402
import transform  # noqa: E402


def _kernel(name, **kw):
    def run(raw):
        g = name
        return proto.decode_kernel(raw, proto.radial_kernel(g, **kw),
                                   proto.luma_kernels(g))
    return run


def _hybrid(raw):
    """Frame-raster chroma kernel for U and V; PalColour's in-field luma
    path."""
    return proto.decode_kernel(raw, proto.radial_kernel("frame"),
                               proto.luma_kernels("field"))


def _transform(geometry, **kw):
    def run(raw):
        return transform.decode(raw, geometry, **kw)
    return run


FRAME_UV = proto.radial_kernel("frame")
LS_DIR = os.environ.get("DOMESDAY2D_LS_DIR")


def _ls(name):
    """A trained separator (phase3_lslcd.py) from $DOMESDAY2D_LS_DIR, the
    clean- or noise-trained one by the case decoded."""
    cache = {}

    def run(raw, case):
        if case not in cache:
            cond = "clean" if case == "clean" else "noisy"
            cache[case] = lslcd.Separator.load(
                os.path.join(LS_DIR, f"{name}_{cond}.npz"))
        raster = bench.frame_raster(raw)
        phi, s = proto.lattice(raster)
        return cache[case].apply(raster, phi, s)
    run.wants_case = True
    return run

VARIANTS = {
    # Step 2: one design, field against frame geometry.
    "pal2d_field": _kernel("field"),
    "pal2d_frame": _kernel("frame"),
    "pal2d_frame_chroma": _hybrid,
    # Step 3: Transform PAL.
    "t_field16": _transform("field"),
    "t_field32": _transform("field", ytile=32),
    "t_frame16": _transform("frame", ytile=16),
    "t_frame32": _transform("frame", ytile=32),
    "t_frame32_uvframe": _transform("frame", ytile=32, uv_kernel=FRAME_UV),
    "t_field16_match": _transform("field", mode="match"),
    "t_frame32_match": _transform("frame", ytile=32, mode="match"),
    "t_field16_band": _transform("field", band_bins=1),
    "t_frame32_band": _transform("frame", ytile=32, band_bins=1),
    "t_field16_comb": _transform("field", fallback="comb"),
    "t_frame32_comb": _transform("frame", ytile=32, fallback="comb"),
    "t_frame32_comb_uvframe": _transform("frame", ytile=32, fallback="comb",
                                         uv_kernel=FRAME_UV),
}
if LS_DIR:
    for _name in ("ls_frame_r4t8", "ls_field_r4t8", "ls_frame_r6t10",
                  "ls_field_r6t10"):
        VARIANTS[_name] = _ls(_name)


def score(b, decode, case):
    out = {}
    for frame, name in b.stills:
        raw = bench.composite(case, frame)
        d = decode(raw, case) if getattr(decode, "wants_case", False) \
            else decode(raw)
        grid = d.truth_grid() if hasattr(d, "truth_grid") else d
        out[name] = bench.measure(grid, b.features[frame], b.masks[frame])
        if frame == bench.GREEN_EDGES:
            out[name]["edge_dots_codes"] = bench.edge_dots(
                grid[0], b.truth[frame][0])
    return out


def score_yuv(b, path):
    out = {}
    for frame, name in b.stills:
        grid = bench.decoded(path, frame)
        out[name] = bench.measure(grid, b.features[frame], b.masks[frame])
        if frame == bench.GREEN_EDGES:
            out[name]["edge_dots_codes"] = bench.edge_dots(
                grid[0], b.truth[frame][0])
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--yuv-dir")
    parser.add_argument("--json")
    parser.add_argument("--only", nargs="*")
    args = parser.parse_args()
    b = bench.Bench()
    results = {}
    if args.json and os.path.exists(args.json):
        with open(args.json) as f:
            results = json.load(f)
    for case in ("clean", "noisy"):
        results.setdefault(case, {})
        if args.yuv_dir:
            stem = "pal_stills_clean" if case == "clean" else "pal_stills"
            for dec in ("pal2d", "transform2d"):
                path = os.path.join(args.yuv_dir, f"{stem}_{dec}.yuv")
                results[case][dec] = score_yuv(b, path)
        for name, decode in VARIANTS.items():
            if args.only and name not in args.only:
                continue
            results[case][name] = score(b, decode, case)
            r = results[case][name]
            print(f"{case:5s} {name:24s} dots "
                  f"{r['green_magenta_edges']['edge_dots_codes']:5.1f}  "
                  f"lzpY {r['luma_zone_plate']['y_rms_pct']:5.2f}  "
                  f"tcfY {r['test_card_f']['y_rms_pct']:5.2f}", flush=True)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1)


if __name__ == "__main__":
    sys.exit(main())
