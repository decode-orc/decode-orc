#!/usr/bin/env python3
#
# File:        phase9_reference.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 9 item 7: checking Phase 7's held-run
#              reference
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase9_reference.py card OUT [--png DIR] [--json FILE]
#       Test Card F's held reference (OUT as phase7_real.py's) registered
#       against the card artwork videosynth bundles (the still bench's
#       truth of it), with a fitted horizontal and vertical scale and
#       offset, luma level and gamma, and chroma gain; what remains, over
#       the picture and by region.
#   phase9_reference.py filters OUT GAP [--json FILE]
#       The held runs' references rebuilt with a narrower and a wider
#       horizontal chroma filter than PalColour's, and the current
#       decoders' decodes of the real frames (phase9_gap.py's "real"
#       variant in GAP) scored against each.
#
# The reference demodulates its exact chroma with PalColour's horizontal
# filter, which Domesday2D also uses, so it cannot show whether that filter
# is too narrow for the picture.

import argparse
import json
import os
import sys

import numpy as np
from scipy.ndimage import convolve1d, map_coordinates
from scipy.optimize import least_squares, minimize

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adaptive  # noqa: E402
import bench  # noqa: E402
import encoder  # noqa: E402
import phase7_real  # noqa: E402
import phase9_gap  # noqa: E402
import proto  # noqa: E402

DECODERS = phase7_real.DECODERS
CARD_FRAME = 4  # the bench's Test Card F


# ── The card ──────────────────────────────────────────────────────────────

def _warp(plane, p, shape):
    """|plane| sampled at row c r + d, sample a x + b for the |shape| grid
    (p = a, b, c, d)."""
    a, b, c, d = p
    r, x = np.mgrid[0:shape[0], 0:shape[1]].astype(np.float64)
    return map_coordinates(plane, [c * r + d, a * x + b], order=3,
                           mode="nearest")


def _held(out, disc, name):
    _, a, b = next(h for h in phase7_real.HELD if h[0] == name)
    cvbs = os.path.join(out, "decodes", f"held_{disc}.cvbs")
    index = [phase7_real.held_index(p) for p in range(a, b + 1)]
    return phase7_real.Run([phase7_real.read_raster(cvbs, f)
                            for f in index])


def register(ref, art, interior):
    """The warp, luma level (offset, gain, gamma) and chroma gain mapping
    |art| onto |ref|, fitted over |interior|."""
    ry = ref[0]

    def luma_cost(p):
        w = _warp(art[0], p, ry.shape)[interior]
        g, o = np.linalg.lstsq(np.stack([w, np.ones_like(w)], 1),
                               ry[interior], rcond=None)[0]
        return float(((ry[interior] - g * w - o) ** 2).mean())

    # Coarse: the offsets alone on a grid, then the scales with them.
    best = None
    for b in np.arange(-24, 25, 2.0):
        for d in np.arange(-12, 13, 1.0):
            c = luma_cost((1.0, b, 1.0, d))
            if best is None or c < best[0]:
                best = (c, (1.0, b, 1.0, d))
    fit = minimize(luma_cost, best[1], method="Powell",
                   options={"xtol": 1e-4, "ftol": 1e-9})
    p = fit.x
    wy = _warp(art[0], p, ry.shape)

    def level(q):
        o, g, gamma = q
        return (o + g * np.clip(wy[interior], 0, None) ** gamma -
                ry[interior])

    o, g, gamma = least_squares(level, (0.0, 1.0, 1.0)).x
    wu = _warp(art[1], p, ry.shape)
    wv = _warp(art[2], p, ry.shape)
    wa = (ref[1] + 1j * ref[2])[interior]
    wb = (wu + 1j * wv)[interior]
    k = (np.conj(wb) * wa).sum() / (np.abs(wb) ** 2).sum()
    y = o + g * np.clip(wy, 0, None) ** gamma
    w = k * (wu + 1j * wv)
    return {"warp": [float(x) for x in p], "offset": float(o),
            "gain": float(g), "gamma": float(gamma),
            "chroma_gain": float(abs(k)),
            "chroma_phase_deg": float(np.degrees(np.angle(k)))}, \
        (y, w.real, w.imag)


def card(out, png_dir, json_path):
    art = bench.truth("clean", CARD_FRAME)
    results = {}
    for disc in phase7_real.HELD_DISCS:
        run = _held(out, disc, "test_card_f")
        ref = run.reference()
        interior = np.zeros(ref[0].shape, dtype=bool)
        interior[24:-24, 48:-48] = True
        fit, mapped = register(ref, art, interior)
        features = bench.TruthFeatures(mapped)
        m = bench.measure(ref, features, interior)
        # Regions of the card: the outer castellations and grid, and the
        # central picture.
        centre = np.zeros_like(interior)
        h, w = interior.shape
        centre[h // 4:3 * h // 4, w // 4:3 * w // 4] = True
        regions = {"picture": interior, "outside the centre":
                   interior & ~centre, "centre": centre}
        row = {"fit": fit}
        for name, mask in regions.items():
            mm = bench.measure(ref, features, mask)
            row[name] = {k: mm[k] for k in ("delta_e_mean", "delta_e_p99",
                                            "y_rms_pct", "u_rms_pct",
                                            "v_rms_pct")}
        results[disc] = row
        print(disc, json.dumps(fit))
        for name in regions:
            print(f"  {name:20s} " + " ".join(
                f"{k} {v:.3f}" for k, v in row[name].items()))
        if png_dir:
            os.makedirs(png_dir, exist_ok=True)
            de = bench.ciede2000(features.lab, bench.yuv_to_lab(*ref))
            phase7_real._sheet([("held reference", ref),
                                ("card artwork, registered", mapped),
                                ("|difference| x4 (luma)",
                                 (np.abs(ref[0] - mapped[0]) * 4,
                                  np.zeros_like(de), np.zeros_like(de)))],
                               os.path.join(png_dir, f"card_{disc}.png"))
        del m
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f, indent=1)


# ── The reference's chroma filter ─────────────────────────────────────────

def _demod_palcolour(bandwidth):
    def run(c, phi, s, correction):
        return adaptive.demodulate_rows(c, phi, s, correction=correction,
                                        bandwidth=bandwidth)
    return run


def _demod_fir(c, phi, s, correction):
    u, v = proto.demodulate(c, phi, s)
    u = convolve1d(u, encoder.CHROMA_FIR, axis=1, mode="nearest")
    v = convolve1d(v, encoder.CHROMA_FIR, axis=1, mode="nearest")
    return correction(u, v, s)


FILTERS = {"narrow (0.8 MHz raised cosine)": _demod_palcolour(0.8e6 / 0.93),
           "PalColour (1.1 MHz)": _demod_palcolour(proto.PALCOLOUR_BANDWIDTH),
           "wide (1.3 MHz videosynth FIR)": _demod_fir}


def reference_with(run, leave_out, demod):
    means = run._means(leave_out)
    ys, us, vs = [], [], []
    for q in (0, 1):
        a, b = means[q], means[q + 2]
        ys.append((a + b) / 2)
        c = (a - b) / 2
        phi, s = proto.lattice(c)
        u, v = demod(c, phi, s, run.correction)
        us.append(u)
        vs.append(v)
    return bench.from_raster(np.mean(ys, axis=0), np.mean(us, axis=0),
                             np.mean(vs, axis=0))


def filters(out, gap, json_path):
    with open(os.path.join(gap, "layout.json")) as f:
        per_run = json.load(f)["per_run"]
    results = {}
    for disc in phase7_real.HELD_DISCS:
        slot = 0
        for name, a, b in phase7_real.HELD:
            run = _held(out, disc, name)
            mask = bench.picture_mask() & run.static
            row = {f: {d: [] for d in DECODERS} for f in FILTERS}
            for i in phase9_gap.chosen(b - a + 1, per_run):
                for fname, demod in FILTERS.items():
                    features = bench.TruthFeatures(
                        reference_with(run, i, demod))
                    for d in DECODERS:
                        path = os.path.join(gap, f"real_{disc}_{d}.yuv")
                        row[fname][d].append(bench.measure(
                            bench.decoded(path, slot), features, mask))
                slot += 1
            summary = {f: {d: {k: float(np.mean([x[k] for x in row[f][d]]))
                               for k in ("delta_e_mean", "u_rms_pct",
                                         "v_rms_pct")}
                           for d in DECODERS} for f in FILTERS}
            results[f"{disc} {name}"] = summary
            print(f"{disc} {name:14s} " + "  |  ".join(
                f"{f.split()[0]}: " + " ".join(
                    f"{summary[f][d]['delta_e_mean']:.2f}" for d in DECODERS)
                for f in FILTERS), flush=True)
    for f in FILTERS:
        print(f"mean {f:32s} " + "  ".join(
            f"{d} {np.mean([r[f][d]['delta_e_mean'] for r in results.values()]):.3f}"
            for d in DECODERS))
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("card")
    s.add_argument("out")
    s.add_argument("--png")
    s.add_argument("--json")
    s = sub.add_parser("filters")
    s.add_argument("out")
    s.add_argument("gap")
    s.add_argument("--json")
    args = ap.parse_args()
    if args.cmd == "card":
        card(args.out, args.png, args.json)
    else:
        filters(args.out, args.gap, args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
