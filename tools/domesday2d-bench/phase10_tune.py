#!/usr/bin/env python3
#
# File:        phase10_tune.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 10 item 1: the decoder's constants tuned at
#              every noise level
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase10_tune.py sweep ROT [--json FILE] [--workers N]
#       Every still of phase7_rotation.py's renders in ROT (rot{0..3} and
#       rot{0..3}_clean) at 0, 1/2, 1, 2 and 4 times the Domesday model's
#       noise, decoded by the prototype (as the decoder: per-frame
#       rotation fit, masked averaging, vertical fit, reassignment) with
#       each set of constants, and scored against the truth. The sets are
#       two crossed grids about the current constants: the fit's strength
#       (kLambda0 x kLambdaDetail) and the reassignment (its noise scale
#       tau x kDetailGate d0), since each acts on its own part of the
#       output; --combos FILE gives a JSON list of [lam0, k, tau, d0]
#       instead. --heldout reads phase10_heldout.py's renders in ROT
#       (ho_rot{0..3} and ho_rot{0..3}_clean) and scores them as it does.
#   phase10_tune.py report FILE
#       Each level's best constants, and what they give against the
#       current ones on every still.
#   phase10_tune.py real OUT TAG... [--side SIDE] [--calibration FILE]
#                   [--frames N] [--combos FILE] [--json FILE]
#       The same constants on real held runs: N frames of each run in
#       OUT/decodes/held_<TAG>.cvbs (phase7_real.py's, phase9_sides.py's
#       or phase9_stack.py's layout), with the disc's pooled matrix from
#       OUT/calibration.json, scored against the run's reference as
#       phase9_tables.py scores them, beside transform2d's own decode.
#   phase10_tune.py real-report FILE...
#       The runs' means for every set of constants.
#
# The noise of a level is the noisy render less the clean one, scaled: the
# renders share every seed, so their difference is the model's capture and
# pressing noise (with the clean render's rounding, under 0.3 LSB r.m.s.).
# The scaled composite is clipped to the 10-bit range as a capture is.

import argparse
import itertools
import json
import os
import sys
from multiprocessing import Pool

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adaptive  # noqa: E402
import bench  # noqa: E402
import hanover  # noqa: E402
import phase7_real  # noqa: E402
import phase7_rotation  # noqa: E402
import phase10_heldout  # noqa: E402
import phase9_sides  # noqa: E402
import proto  # noqa: E402
import transform  # noqa: E402

LEVELS = [0.0, 0.5, 1.0, 2.0, 4.0]
LAMBDA0 = [0.0, 0.003, 0.01, 0.03]
LAMBDA_DETAIL = [1.0, 2.5, 5.0, 10.0, 20.0]
TAU = [0.75, 1.0, 1.5, 2.0, 3.0]
GATE = [0.0, 0.001, 0.003, 0.01]
CURRENT = (0.01, 5.0, 1.0, 0.003)
COMBOS = sorted({(lam0, k, CURRENT[2], CURRENT[3])
                 for lam0, k in itertools.product(LAMBDA0, LAMBDA_DETAIL)} |
                {(CURRENT[0], CURRENT[1], tau, d0)
                 for tau, d0 in itertools.product(TAU, GATE)})
METRICS = ("delta_e_mean", "delta_e_p99", "y_rms_pct", "y_hf_rms_pct",
           "cross_colour_rms_pct")


class Scorer:
    """The parts of bench.measure() the tuning reads, with the truth's
    share computed once."""

    def __init__(self, features, mask):
        m = mask.copy()
        m[:, :bench.EDGE_EXCLUSION] = False
        m[:, -bench.EDGE_EXCLUSION:] = False
        self.m = m
        self.n = m.sum()
        self.t = features.t
        self.lab = tuple(x[m] for x in features.lab)
        self.xm = m & (features.chroma_nearby < bench.CHROMA_ABSENT) & \
            features.luma_detail

    def __call__(self, d):
        dy, du, dv = d
        m, n = self.m, self.n
        ey = dy - self.t[0]
        de = bench.ciede2000(self.lab, bench.yuv_to_lab(dy[m], du[m],
                                                        dv[m]))
        k = min(de.size - 1, int(0.99 * n))
        hf = bench.filter_rows(ey, bench.LUMA_BAND)[m]
        out = [float(de.mean()), float(np.partition(de, k)[k]),
               float(np.sqrt((ey[m] ** 2).sum() / n) * 100),
               float(np.sqrt((hf ** 2).sum() / n) * 100), None]
        if self.xm.any():
            dc2 = du[self.xm] ** 2 + dv[self.xm] ** 2
            out[4] = float(np.sqrt(dc2.mean()) * 100)
        return out


def level_composite(rot, r, frame, level, prefix="rot"):
    """Frame |frame| of rotation |r| at |level| times the model's noise."""
    saved = dict(bench.CASES)
    bench.CASES["clean"] = (rot, f"{prefix}{r}_clean")
    bench.CASES["noisy"] = (rot, f"{prefix}{r}")
    try:
        clean = bench.composite("clean", frame)
        if level == 0.0:
            return clean
        noisy = bench.composite("noisy", frame)
    finally:
        bench.CASES.clear()
        bench.CASES.update(saved)
    return np.clip(clean + level * (noisy - clean), 0.0, 1023.0)


def decode_combos(raster, known, combos, scorer):
    """The frame decoded as the decoder does, with each of |combos|
    [(lam0, k, tau, d0[, kn[, leak[, beta[, gate_high]]]])], scored by |scorer|. (scores,
    noise, detail). kn adds kn x the blank-row noise squared to lam0;
    leak and beta are adaptive.luma_detail()'s, gate_high
    adaptive.gate_detail()'s."""
    phi, s = proto.lattice(raster)
    c = transform.chroma_frame(raster, ytile=32)
    u0, v0 = adaptive.demodulate_rows(c, phi, s)
    m = hanover.frame_matrix(u0, v0, s, known, partner=0.03, minimum=1000)
    correction = hanover.corrector(m)
    p = dict(adaptive.DEFAULTS, prefilter=hanover.masked_average(),
             correction=correction)
    noise = adaptive.blank_noise(raster)
    detail = adaptive.luma_detail(raster - c)
    scores = {}
    fits = {}
    for combo in combos:
        lam0, k, tau, d0 = combo[:4]
        floor = lam0 + (combo[4] if len(combo) > 4 else 0.0) * noise ** 2
        leak = combo[5] if len(combo) > 5 else None
        beta = combo[6] if len(combo) > 6 else None
        gate_high = bool(combo[7]) if len(combo) > 7 else False
        if (floor, k, leak, beta) not in fits:
            u, v, _ = adaptive.chroma(raster, c, phi, s,
                                      **dict(p, lam0=floor, k=k, kn=0.0,
                                             leak=leak, beta=beta))
            r_ = c - adaptive.encode(u, v, phi, s, correction)
            env = np.sqrt(np.maximum(adaptive.uniform_filter(
                r_ ** 2, size=adaptive.ENVELOPE_SIZE), 1e-12))
            fits = {(floor, k, leak, beta): (u, v, r_, env)}
        u, v, r_, env = fits[(floor, k, leak, beta)]
        g = np.maximum(0.0, 1.0 - tau * noise / env)
        if d0:
            gate = adaptive.gate_detail(raster - c, leak, c, beta, gate_high)
            g = g * gate / (gate + d0)
        d = proto.Decoded(raster - c + g * r_, u, v).truth_grid()
        scores[_key(combo)] = scorer(d)
    return scores, noise, detail


def _job(args):
    rot, r, f, level, combos, heldout = args
    at = phase7_rotation.rotated_frame(f, r)
    prefix = "ho_rot" if heldout else "rot"
    features = bench.TruthFeatures(phase7_rotation._truth(
        os.path.join(rot, f"{prefix}{r}_clean.truth"), at))
    if heldout:
        mask = phase10_heldout._mask(rot, r, at)
        name = phase10_heldout.STILLS[f - 1][0]
    else:
        mask = phase7_rotation._mask(rot, r, f)
        name = dict(bench.STILLS)[f]
    raster = bench.frame_raster(level_composite(rot, r, at, level, prefix))
    scores, noise, detail = decode_combos(raster, np.zeros((2, 2)), combos,
                                          Scorer(features, mask))
    return {"rotation": r, "still": f, "name": name,
            "level": level, "noise": noise,
            "detail_median": float(np.median(detail[60:600, 180:1100])),
            "scores": scores}


def _real_job(args):
    out, tag, name, a, b, held, known, frames, combos = args
    phase7_real.HELD = held
    cvbs = os.path.join(out, "decodes", f"held_{tag}.cvbs")
    index = [phase7_real.held_index(q) for q in range(a, b + 1)]
    rasters = [phase7_real.read_raster(cvbs, f) for f in index]
    run = phase7_real.Run(rasters)
    mask = bench.picture_mask() & run.static
    rows = []
    for i in [(5 * j) % len(rasters) for j in range(frames)]:
        scorer = Scorer(bench.TruthFeatures(run.reference(i)), mask)
        scores, noise, _ = decode_combos(rasters[i], known, combos, scorer)
        path = os.path.join(out, "decodes", f"held_{tag}_transform2d.yuv")
        scores["transform2d"] = scorer(bench.decoded(path, index[i]))
        rows.append({"frame": index[i], "noise": noise, "scores": scores})
    return tag, name, rows


def real(out, tags, side, calibration, frames, combos, json_path,
         workers):
    """|combos| on the held runs of OUT/decodes/held_<TAG>.cvbs, scored as
    phase9_tables.py scores them, beside transform2d's own decodes."""
    if side:
        phase9_sides._use(side)
    with open(calibration or os.path.join(out, "calibration.json")) as f:
        cal = json.load(f)
    jobs = []
    for tag in tags:
        known = np.array([[cal[tag]["hanover_uu"], cal[tag]["hanover_uv"]],
                          [cal[tag]["hanover_vu"], cal[tag]["hanover_vv"]]])
        jobs += [(out, tag, name, a, b, phase7_real.HELD, known, frames,
                  combos) for name, a, b in phase7_real.HELD]
    results = {}
    with Pool(min(len(jobs), workers)) as pool:
        for tag, name, rows in pool.imap_unordered(_real_job, jobs):
            results[f"{tag} {name}"] = rows
            print(f"{tag} {name}", flush=True)
    with open(json_path, "w") as f:
        json.dump({"metrics": METRICS, "runs": results}, f)


def real_report(json_paths):
    runs = {}
    for path in json_paths:
        with open(path) as f:
            runs.update({f"{os.path.basename(path)} {k}": v
                         for k, v in json.load(f)["runs"].items()})
    keys = list(next(iter(runs.values()))[0]["scores"])

    def run_mean(rows, k, i=0):
        return float(np.mean([r["scores"][k][i] for r in rows]))

    mean = {k: np.mean([run_mean(v, k) for v in runs.values()])
            for k in keys}
    print("over the runs: " + "; ".join(
        f"{k} {mean[k]:.3f}" for k in sorted(keys, key=mean.get)[:8]))
    cur = _key(CURRENT) if _key(CURRENT) in keys else keys[0]
    best = min((k for k in keys if k != "transform2d"), key=mean.get)
    print(f"current {cur} {mean[cur]:.3f}  best {best} {mean[best]:.3f}  "
          f"transform2d {mean['transform2d']:.3f}")
    for name, rows in sorted(runs.items()):
        print(f"  {name:40s} t2d {run_mean(rows, 'transform2d'):.3f}  "
              f"current {run_mean(rows, cur):.3f}  best "
              f"{run_mean(rows, best):.3f}  Y {run_mean(rows, cur, 2):.2f}"
              f" -> {run_mean(rows, best, 2):.2f} (t2d "
              f"{run_mean(rows, 'transform2d', 2):.2f})")


def sweep(rot, json_path, workers, combos, heldout=False):
    jobs = [(os.path.abspath(rot), r, f, level, combos, heldout)
            for level in LEVELS for r in range(4)
            for f, _ in bench.STILLS]
    rows = []
    with Pool(workers) as pool:
        for i, row in enumerate(pool.imap_unordered(_job, jobs)):
            rows.append(row)
            print(f"{i + 1}/{len(jobs)} level {row['level']} rot "
                  f"{row['rotation']} {row['name']}", flush=True)
    with open(json_path, "w") as f:
        json.dump({"metrics": METRICS, "rows": rows}, f)


def _key(t):
    return ",".join(str(x) for x in t)


def report(json_path):
    with open(json_path) as f:
        rows = json.load(f)["rows"]
    keys = list(rows[0]["scores"])
    current = _key(CURRENT) if _key(CURRENT) in keys else keys[0]
    for level in LEVELS:
        at = [x for x in rows if x["level"] == level]
        mean = {k: np.mean([x["scores"][k][0] for x in at]) for k in keys}
        best = min(keys, key=mean.get)
        print(f"\nlevel {level}: blank noise "
              f"{np.mean([x['noise'] for x in at]):.4f}, detail median "
              f"{np.mean([x['detail_median'] for x in at]):.4f}")
        for name, k in (("current", current), ("best", best)):
            m = np.mean([x["scores"][k][:4] for x in at], axis=0)
            print(f"  {name:8s} {k:22s} dE {m[0]:.3f} p99 {m[1]:.2f} "
                  f"Y {m[2]:.3f} Yhf {m[3]:.3f}")
        ranked = sorted(keys, key=mean.get)[:5]
        print("  top 5: " + "; ".join(f"{k} {mean[k]:.3f}" for k in ranked))
        for f, name in bench.STILLS:
            s = [x for x in at if x["still"] == f]
            a = np.mean([x["scores"][current][:4] for x in s], axis=0)
            b = np.mean([x["scores"][best][:4] for x in s], axis=0)
            print(f"    {name:20s} dE {a[0]:.3f} -> {b[0]:.3f}  "
                  f"Y {a[2]:.3f} -> {b[2]:.3f}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="mode", required=True)
    a = sub.add_parser("sweep")
    a.add_argument("rot")
    a.add_argument("--json", default="phase10_tune.json")
    a.add_argument("--workers", type=int, default=os.cpu_count())
    a.add_argument("--combos")
    a.add_argument("--heldout", action="store_true")
    b = sub.add_parser("report")
    b.add_argument("json")
    c = sub.add_parser("real")
    c.add_argument("out")
    c.add_argument("tags", nargs="+")
    c.add_argument("--side")
    c.add_argument("--calibration")
    c.add_argument("--frames", type=int, default=4)
    c.add_argument("--json", default="phase10_real.json")
    c.add_argument("--workers", type=int, default=os.cpu_count())
    c.add_argument("--combos")
    d = sub.add_parser("real-report")
    d.add_argument("json", nargs="+")
    args = ap.parse_args()
    combos = COMBOS
    if getattr(args, "combos", None):
        with open(args.combos) as f:
            combos = sorted(tuple(x) for x in json.load(f))
    if args.mode == "sweep":
        sweep(args.rot, args.json, args.workers, combos, args.heldout)
    elif args.mode == "real":
        real(args.out, args.tags, args.side, args.calibration, args.frames,
             combos, args.json, args.workers)
    elif args.mode == "real-report":
        real_report(args.json)
    else:
        report(args.json)


if __name__ == "__main__":
    main()
