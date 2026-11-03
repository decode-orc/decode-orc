#!/usr/bin/env python3
#
# File:        phase9_noise.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 9 item 2: the reassignment's noise estimate
#              against the noise in the picture
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase9_noise.py measure OUT TAG... [--frames N] [--json FILE]
#       For N frames of each held run in OUT/decodes/held_<TAG>.cvbs
#       (phase7_real.py's or phase9_stack.py's layout): the noise in the
#       picture's flat areas, from the frame less the mean of the run's
#       other frames of its carrier phase, beside each ESTIMATOR's reading
#       of the frame alone.
#   phase9_noise.py decode OUT TAG... [--frames N] [--json FILE]
#       The same frames decoded by the prototype (hanover.frame_matrix,
#       as the decoder) with the reassignment's noise taken from each
#       estimator, scored against the run's reference.
#
# The decoder hands to luma the part of its chroma residual that stands
# above the noise, and reads the noise as the chroma band's r.m.s. on raster
# rows 12-31 (field lines 7-16), a single frame's only blank rows. On a
# stack those rows are not like the picture: the Stacker's still-frame
# copies and Picture Band Limit work on the active picture only, and field
# line 16 carries the LaserDisc code, which stacking does not reduce.

import argparse
import json
import os
import sys
from multiprocessing import Pool

import numpy as np
from scipy.ndimage import maximum_filter, uniform_filter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adaptive  # noqa: E402
import bench  # noqa: E402
import hanover  # noqa: E402
import phase7_real  # noqa: E402
import proto  # noqa: E402
import transform  # noqa: E402

PARTNER = 0.03
MINIMUM = 1000
ROWS = adaptive.BLANK_ROWS
SAMPLES = adaptive.BLANK_SAMPLES
LINE_16_ROWS = (30, 32)
ACTIVE = (177, 1099)


def _band(rows):
    return bench.filter_rows(rows, adaptive.CHROMA_BAND)[:, slice(*SAMPLES)]


def blank_rms(raster):
    """The decoder's: rows 12-31."""
    return adaptive.blank_noise(raster)


def blank_no_line_16(raster):
    lines = _band(raster[ROWS[0]:LINE_16_ROWS[0]])
    return float(np.sqrt((lines ** 2).mean()))


def blank_median(raster):
    """Rows 12-31, from the median absolute value (Gaussian scale)."""
    lines = _band(raster[slice(*ROWS)])
    return float(np.median(np.abs(lines)) * 1.4826)


def _flat(raster, c):
    """Picture samples flat in luma and chroma, on the raster."""
    y = raster - c
    detail = adaptive.luma_detail(y)
    low = bench.filter_rows(y, bench.band_filter(0.0, 2.0e6))
    grad = np.abs(np.gradient(low, axis=1)) + np.abs(np.roll(low, 2, 0) -
                                                     low)
    flat = (maximum_filter(grad, (5, 15)) < 0.02) & (detail < 0.015)
    mask = np.zeros_like(flat)
    mask[proto.FIRST_ACTIVE + 8:proto.END_ACTIVE - 8, slice(*ACTIVE)] = True
    return flat & mask


def picture_flat(raster, c, u, v, phi, s, correction):
    """The frame's own flat areas: the chroma residual left by the
    decoder's estimate, robustly."""
    r = c - adaptive.encode(u, v, phi, s, correction)
    m = _flat(raster, c)
    if m.sum() < 1000:
        return None
    return float(np.median(np.abs(r[m])) * 1.4826)


ESTIMATORS = ["blank_rms", "blank_no_line_16", "blank_median",
              "picture_flat", "blank_rms_x1.25", "blank_rms_x1.5"]


def _run_frames(out, tag, name, a, b, frames):
    cvbs = os.path.join(out, "decodes", f"held_{tag}.cvbs")
    index = [phase7_real.held_index(p) for p in range(a, b + 1)]
    rasters = [phase7_real.read_raster(cvbs, f) for f in index]
    run = phase7_real.Run(rasters)
    pick = [(5 * i) % len(rasters) for i in range(frames)]
    return rasters, run, pick


def _picture_noise(rasters, run, i, c):
    """The frame's own noise in its flat areas: it less the mean of the
    run's other frames of its phase, scaled for that mean's own noise."""
    q = run.classes[i]
    others = [r for k, r in enumerate(rasters)
              if run.classes[k] == q and k != i]
    n = rasters[i] - np.mean(others, axis=0)
    n = n / np.sqrt(1.0 + 1.0 / len(others))
    band = bench.filter_rows(n, adaptive.CHROMA_BAND)
    m = _flat(rasters[i], c)
    return (float(np.sqrt((band[m] ** 2).mean())),
            float(np.median(np.abs(band[m])) * 1.4826),
            float(m.mean()))


def _known(tag, out):
    with open(os.path.join(out, "calibration.json")) as f:
        cal = json.load(f)[tag]
    return np.array([[cal["hanover_uu"], cal["hanover_uv"]],
                     [cal["hanover_vu"], cal["hanover_vv"]]])


def _job(args):
    mode, out, tag, name, a, b, frames = args
    rasters, run, pick = _run_frames(out, tag, name, a, b, frames)
    known = _known(tag, out)
    rows = []
    mask = bench.picture_mask() & run.static
    for i in pick:
        raster = rasters[i]
        phi, s = proto.lattice(raster)
        c = transform.chroma_frame(raster, ytile=32)
        u0, v0 = adaptive.demodulate_rows(c, phi, s)
        m = hanover.frame_matrix(u0, v0, s, known, partner=PARTNER,
                                 minimum=MINIMUM)
        correction = hanover.corrector(m)
        kw = dict(prefilter=hanover.masked_average(), correction=correction)
        p = dict(adaptive.DEFAULTS, **kw)
        u, v, _ = adaptive.chroma(raster, c, phi, s, **p)
        est = {"blank_rms": blank_rms(raster),
               "blank_no_line_16": blank_no_line_16(raster),
               "blank_median": blank_median(raster),
               "picture_flat": picture_flat(raster, c, u, v, phi, s,
                                            correction)}
        # The decoder's reading scaled, to show which way the best lies.
        est["blank_rms_x1.25"] = 1.25 * est["blank_rms"]
        est["blank_rms_x1.5"] = 1.5 * est["blank_rms"]
        rms, med, share = _picture_noise(rasters, run, i, c)
        row = {"picture_rms": rms, "picture_median": med,
               "flat_share": share, "estimates": est}
        if mode == "decode":
            features = bench.TruthFeatures(run.reference(i))
            row["scores"] = {}
            for e in ESTIMATORS:
                level = est[e] if est[e] is not None else est["blank_rms"]
                saved = adaptive.blank_noise
                adaptive.blank_noise = lambda _r, level=level: level
                try:
                    y = raster - c + adaptive.reassign(
                        raster, c, u, v, phi, s, p["tau"], p["d0"],
                        correction)
                finally:
                    adaptive.blank_noise = saved
                d = proto.Decoded(y, u, v).truth_grid()
                sc = bench.measure(d, features, mask)
                row["scores"][e] = {k: sc[k] for k in
                                    ("delta_e_mean", "y_rms_pct",
                                     "y_hf_rms_pct")}
        rows.append(row)
    return tag, name, rows


def run(mode, out, tags, frames, json_path):
    jobs = [(mode, out, tag, name, a, b, frames)
            for tag in tags for name, a, b in phase7_real.HELD]
    with Pool(min(len(jobs), 9)) as pool:
        done = pool.map(_job, jobs)
    results = {}
    for tag, name, rows in done:
        results[f"{tag} {name}"] = rows
        pr = np.mean([r["picture_rms"] for r in rows]) * 100
        line = f"{tag} {name:14s} picture {pr:.3f}% (median " \
               f"{np.mean([r['picture_median'] for r in rows]) * 100:.3f})"
        for e in ESTIMATORS:
            vals = [r["estimates"][e] for r in rows
                    if r["estimates"][e] is not None]
            if vals:
                line += f"  {e} {np.mean(vals) * 100:.3f}"
        print(line, flush=True)
        if mode == "decode":
            print("    " + "  ".join(
                f"{e} dE {np.mean([r['scores'][e]['delta_e_mean'] for r in rows]):.3f}"
                f" Y {np.mean([r['scores'][e]['y_rms_pct'] for r in rows]):.3f}"
                for e in ESTIMATORS), flush=True)
    for tag in tags:
        rows = [r for k, v in results.items() if k.startswith(tag + " ")
                for r in v]
        pic = np.nanmean([r["picture_rms"] for r in rows])
        line = f"{tag}: estimate / picture noise:"
        for e in ESTIMATORS:
            vals = [r["estimates"][e] / r["picture_rms"] for r in rows
                    if r["estimates"][e] is not None and
                    np.isfinite(r["picture_rms"])]
            line += f"  {e} {np.mean(vals):.2f}"
        print(line + f"  (picture {pic * 100:.3f}%)")
        if mode == "decode":
            print(f"{tag}: mean " + "  ".join(
                f"{e} dE {np.mean([r['scores'][e]['delta_e_mean'] for r in rows]):.4f}"
                f" Y {np.mean([r['scores'][e]['y_rms_pct'] for r in rows]):.4f}"
                for e in ESTIMATORS))
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("measure", "decode"):
        s = sub.add_parser(name)
        s.add_argument("out")
        s.add_argument("tags", nargs="+")
        s.add_argument("--frames", type=int, default=4)
        s.add_argument("--json")
    args = ap.parse_args()
    run(args.cmd, args.out, args.tags, args.frames, args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
