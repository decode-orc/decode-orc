#!/usr/bin/env python3
#
# File:        phase8_rotation.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 8: the per-frame Hanover rotation fitted as
#              its only unknown, against Phase 7's full fit
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase8_rotation.py synthetic
#       Frames of one hue (a whole-picture fill, a caption box on grey, a
#       pale map fill ruled with black lines) and one of two hues, put on
#       the noisy bench's composite in place of its still and impaired with
#       hanover.PHASE2 (true rotation 0.110, symmetric part 0.02). Each
#       frame's rotation from Phase 7's full fit, and from fit_rotation()
#       with the true symmetric part and with none.
#   phase8_rotation.py held OUT [--frames N] [--json FILE]
#       Phase 7's held runs (OUT as phase7_real.py's): each run's rotation
#       from its exact chroma, and each of N frames' rotation from the
#       transform's chroma, by both fits; and the same fits on the run's
#       average through the transform, which has no noise, to tell noise
#       from the separation.
#   phase8_rotation.py decode OUT [--frames N] [--json FILE]
#       N frames of each held run decoded by the prototype with each way of
#       fitting the frame's rotation (VARIANTS), scored against the run's
#       reference as phase7_real.py held scores the decoders.
#
# Phase 7 found a single frame reads about 0.8 of its run's rotation. The
# full fit solves M's four terms from the 2 x 2 Gram matrix of the pairs'
# means, which one hue leaves singular; the rotation alone is one unknown.

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adaptive  # noqa: E402
import bench  # noqa: E402
import encoder  # noqa: E402
import hanover  # noqa: E402
import phase7_real  # noqa: E402
import proto  # noqa: E402
import transform  # noqa: E402

PARTNER = 0.03
MINIMUM = 1000
# hanover.PHASE2 as M: [[0, -e], [e, 0]] + [[0, v_to_u], [u_to_v, 0]].
_E = np.radians(hanover.PHASE2["phase_deg"])
TRUE_M = np.array([[0.0, -_E + hanover.PHASE2["v_to_u"]],
                   [_E + hanover.PHASE2["u_to_v"], 0.0]])
# The bench still whose noise and timing the synthetic frames borrow.
CARRIER_FRAME = 5
# Each disc's hanover_* from Phase 7's calibration.
DISC_MATRIX = {"DS1": [[-0.009, 0.051], [-0.106, -0.024]],
               "DS6": [[-0.007, 0.044], [-0.098, -0.026]]}


def _pictures():
    shape = (bench.TRUTH_ROWS, bench.TRUTH_WIDTH)
    rows, cols = np.mgrid[0:shape[0], 0:shape[1]]
    out = {}
    out["fill"] = (np.full(shape, 0.4), np.full(shape, 0.15),
                   np.full(shape, -0.10))
    y = np.full(shape, 0.3)
    u = np.zeros(shape)
    v = np.zeros(shape)
    box = (rows >= 150) & (rows < 430) & (cols >= 150) & (cols < 770)
    y[box], u[box], v[box] = 0.45, -0.12, 0.20
    # Caption text: white strokes one to three rows high inside the box.
    text = box & ((rows // 6) % 5 == 2) & ((cols // 9) % 3 != 0) & \
        (rows % 6 < (cols // 40) % 3 + 1)
    y[text] = 0.9
    out["caption"] = (y, u, v)
    y = np.full(shape, 0.7)
    u = np.full(shape, -0.08)
    v = np.full(shape, 0.03)
    ruled = (rows % 37 < 2) | (cols % 53 < 3)
    y[ruled], u[ruled], v[ruled] = 0.05, 0.0, 0.0
    out["map_fill"] = (y, u, v)
    y = np.full(shape, 0.5)
    u = np.where(cols < shape[1] // 2, 0.15, -0.10)
    v = np.where(cols < shape[1] // 2, -0.05, 0.15)
    out["two_hues"] = (y, u, v)
    return out


def _fits(c, phi, s, known):
    u, v = adaptive.demodulate_rows(c, phi, s)
    pairs = [hanover.flat_pairs(u, v, s, partner=PARTNER)]
    full, n = hanover.fit_matrix(pairs, MINIMUM)
    full_theta = (full[0, 1] - full[1, 0]) / 2.0 if n >= MINIMUM else None
    alone, _ = hanover.fit_rotation(pairs, known, MINIMUM)
    alone0, _ = hanover.fit_rotation(pairs, np.zeros((2, 2)), MINIMUM)
    return {"pairs": n, "full": full_theta, "full_m": full.tolist(),
            "alone": alone, "alone_no_symmetric": alone0}


def _rotation_matrix(u, v, s, known, variant):
    """The frame's matrix by |variant|: Phase 7's (the full fit's
    rotation), or the rotation fitted alone with the cut on the
    alternating half at 0.3 or 0.6 of the mean, or centred on a first
    estimate (hanover.frame_matrix(), the decoder's)."""
    if variant == "full":
        return hanover.frame_matrix_full(u, v, s, known, partner=PARTNER,
                                         minimum=MINIMUM)
    if variant == "alone_centred":
        return hanover.frame_matrix(u, v, s, known, partner=PARTNER,
                                    minimum=MINIMUM)
    pairs = [hanover.flat_pairs(u, v, s, partner=PARTNER)]
    ratio = 0.3 if variant == "alone" else 0.6
    theta, _ = hanover.fit_rotation(pairs, known, MINIMUM, max_ratio=ratio)
    if theta is None:
        return np.asarray(known, dtype=np.float64)
    return hanover.with_rotation(np.asarray(known, dtype=np.float64), theta)


VARIANTS = ["full", "alone", "alone_wide", "alone_centred"]


def decode(out, frames, json_path):
    results = {}
    for disc in phase7_real.HELD_DISCS:
        known = np.asarray(DISC_MATRIX[disc])
        cvbs = os.path.join(out, "decodes", f"held_{disc}.cvbs")
        for name, a, b in phase7_real.HELD:
            index = [phase7_real.held_index(p) for p in range(a, b + 1)]
            rasters = [phase7_real.read_raster(cvbs, f) for f in index]
            run = phase7_real.Run(rasters)
            mask = bench.picture_mask() & run.static
            step = max(1, len(rasters) // frames)
            scores = {v: [] for v in VARIANTS}
            thetas = {v: [] for v in VARIANTS}
            for i in range(0, len(rasters), step)[:frames]:
                raster = rasters[i]
                phi, s = proto.lattice(raster)
                c = transform.chroma_frame(raster, ytile=32)
                u, v = adaptive.demodulate_rows(c, phi, s)
                features = bench.TruthFeatures(run.reference(i))
                for variant in VARIANTS:
                    m = _rotation_matrix(u, v, s, known, variant)
                    d = adaptive.decode_separated(
                        raster, c, phi, s, prefilter=hanover.masked_average(),
                        correction=hanover.corrector(m))
                    scores[variant].append(bench.measure(d.truth_grid(),
                                                         features, mask))
                    thetas[variant].append(float((m[0, 1] - m[1, 0]) / 2))
            row = {}
            for variant in VARIANTS:
                keys = scores[variant][0].keys()
                row[variant] = {k: float(np.mean([x[k] for x in scores[variant]
                                                  if k in x])) for k in keys}
                row[variant]["rotation"] = float(np.mean(thetas[variant]))
            results[f"{disc} {name}"] = row
            print(f"{disc} {name:14s} " + "  ".join(
                f"{v} {row[v]['rotation']:.3f} dE {row[v]['delta_e_mean']:.3f}"
                f" Y {row[v]['y_rms_pct']:.3f}" for v in VARIANTS), flush=True)
    for variant in VARIANTS:
        print(f"mean {variant:14s} dE "
              f"{np.mean([r[variant]['delta_e_mean'] for r in results.values()]):.4f}"
              f" Y {np.mean([r[variant]['y_rms_pct'] for r in results.values()]):.4f}")
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f, indent=1)


def _fmt(x):
    return "   -  " if x is None else f"{x:6.3f}"


def synthetic():
    raw = bench.composite("noisy", CARRIER_FRAME)
    raster = bench.frame_raster(raw)
    phi, s = proto.lattice(raster)
    old = encoder.encode(bench.truth("noisy", CARRIER_FRAME), phi, s,
                         quantise=False)
    true_theta = (TRUE_M[0, 1] - TRUE_M[1, 0]) / 2.0
    print(f"true rotation {true_theta:.3f}")
    for name, pic in _pictures().items():
        r = raster - old + encoder.encode(pic, phi, s, quantise=False)
        r = hanover.impair(r, pic, phi, s, **hanover.PHASE2)
        c = transform.chroma_frame(r, ytile=32)
        f = _fits(c, phi, s, TRUE_M)
        u, v = adaptive.demodulate_rows(c, phi, s)
        centred = _rotation_matrix(u, v, s, TRUE_M, "alone_centred")
        print(f"{name:10s} pairs {f['pairs']:7d}  full {_fmt(f['full'])}  "
              f"alone {_fmt(f['alone'])}  alone, S = 0 "
              f"{_fmt(f['alone_no_symmetric'])}  centred "
              f"{_fmt((centred[0, 1] - centred[1, 0]) / 2)}  full M "
              f"{np.round(f['full_m'], 3).tolist()}", flush=True)


def held(out, frames, json_path):
    results = {}
    for disc in phase7_real.HELD_DISCS:
        known = np.asarray(DISC_MATRIX[disc])
        cvbs = os.path.join(out, "decodes", f"held_{disc}.cvbs")
        for name, a, b in phase7_real.HELD:
            index = [phase7_real.held_index(p) for p in range(a, b + 1)]
            rasters = [phase7_real.read_raster(cvbs, f) for f in index]
            run = phase7_real.Run(rasters)
            means = run._means(-1)
            # The run's own rotation, on its exact chroma.
            pairs = []
            for q in (0, 1):
                c = (means[q] - means[q + 2]) / 2
                phi, s = proto.lattice(c)
                pairs.append(hanover.flat_pairs(
                    *adaptive.demodulate_rows(c, phi, s), s))
            run_full = run.matrix
            run_alone, _ = hanover.fit_rotation(pairs, known)
            # The run's average frame through the transform: no noise.
            q = run.classes[0]
            mean_raster = means[q]
            phi, s = proto.lattice(mean_raster)
            quiet = _fits(transform.chroma_frame(mean_raster, ytile=32),
                          phi, s, known)
            step = max(1, len(rasters) // frames)
            single = []
            for raster in rasters[::step][:frames]:
                phi, s = proto.lattice(raster)
                single.append(_fits(transform.chroma_frame(raster, ytile=32),
                                    phi, s, known))
            row = {"run_full": float((run_full[0, 1] - run_full[1, 0]) / 2),
                   "run_alone": run_alone, "quiet": quiet,
                   "frames": single}
            results[f"{disc} {name}"] = row

            def mean_of(key):
                vals = [f[key] for f in single if f[key] is not None]
                return float(np.mean(vals)) if vals else None
            print(f"{disc} {name:14s} run full {row['run_full']:6.3f} alone "
                  f"{_fmt(run_alone)} | quiet full {_fmt(quiet['full'])} "
                  f"alone {_fmt(quiet['alone'])} | frames full "
                  f"{_fmt(mean_of('full'))} alone {_fmt(mean_of('alone'))} "
                  f"(pairs {int(np.mean([f['pairs'] for f in single]))})",
                  flush=True)
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("synthetic")
    s = sub.add_parser("held")
    s.add_argument("out")
    s.add_argument("--frames", type=int, default=4)
    s.add_argument("--json")
    s = sub.add_parser("decode")
    s.add_argument("out")
    s.add_argument("--frames", type=int, default=4)
    s.add_argument("--json")
    args = ap.parse_args()
    if args.cmd == "synthetic":
        synthetic()
    elif args.cmd == "held":
        held(args.out, args.frames, args.json)
    else:
        decode(args.out, args.frames, args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
