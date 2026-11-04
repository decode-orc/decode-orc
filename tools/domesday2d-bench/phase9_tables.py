#!/usr/bin/env python3
#
# File:        phase9_tables.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 9 item 6: Phase 7's tables with the 99th
#              percentile of ΔE, the luma error and its detail band, and
#              bootstrap confidence intervals
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase9_tables.py held OUT TAG... [--side SIDE] [--json FILE]
#       phase7_real.py's step 1 for OUT/decodes/held_<TAG>[_<decoder>]:
#       every frame of every held run scored against the run's reference
#       without it; per run and decoder the mean of each metric with a 95 %
#       interval by bootstrap over the run's frames, and over the runs the
#       mean with an interval by bootstrap over the runs.
#   phase9_tables.py cross OUT [--side SIDE] [--groups TAG=GROUP,...]
#                    [--json FILE]
#       phase7_real.py's step 2, with each picture's disagreement kept so
#       the means over pictures carry a bootstrap interval. --groups names
#       the decodes compared (cross_<TAG>_<decoder>) and their pressing
#       groups; the default is the side's discs.
#
# SIDE is a phase9_sides.py side; without it, Community South as Phase 7
# measured it.

import argparse
import itertools
import json
import os
import sys
from multiprocessing import Pool

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import phase7_real  # noqa: E402
import phase9_sides  # noqa: E402

DECODERS = phase7_real.DECODERS
METRICS = ("delta_e_mean", "delta_e_p99", "y_rms_pct", "y_hf_rms_pct")
SHORT = {"delta_e_mean": "ΔE", "delta_e_p99": "p99", "y_rms_pct": "Y",
         "y_hf_rms_pct": "Y 3.5-5"}
BOOT = 2000


def interval(values, seed=0):
    """(mean, low, high): the mean of |values| with a 95 % interval by
    bootstrap."""
    x = np.asarray(values, dtype=np.float64)
    rng = np.random.default_rng(seed)
    means = x[rng.integers(0, len(x), (BOOT, len(x)))].mean(axis=1)
    return (float(x.mean()), float(np.percentile(means, 2.5)),
            float(np.percentile(means, 97.5)))


def paired(a, b, seed=0):
    """The mean of a - b with its interval, resampled together."""
    return interval(np.asarray(a) - np.asarray(b), seed)


def _held_job(job):
    out, tag, name, a, b, held = job
    phase7_real.HELD = held
    cvbs = os.path.join(out, "decodes", f"held_{tag}.cvbs")
    frames = [phase7_real.held_index(p) for p in range(a, b + 1)]
    run = phase7_real.Run([phase7_real.read_raster(cvbs, f)
                           for f in frames])
    mask = bench.picture_mask() & run.static
    per = {d: {m: [] for m in METRICS} for d in DECODERS}
    for i, frame in enumerate(frames):
        features = bench.TruthFeatures(run.reference(i))
        for d in DECODERS:
            path = os.path.join(out, "decodes", f"held_{tag}_{d}.yuv")
            s = bench.measure(bench.decoded(path, frame), features, mask)
            for m in METRICS:
                per[d][m].append(s[m])
    return tag, name, per


def held(out, tags, json_path):
    jobs = [(out, tag, name, a, b, phase7_real.HELD)
            for tag in tags for name, a, b in phase7_real.HELD]
    with Pool(min(len(jobs), 9)) as pool:
        done = pool.map(_held_job, jobs)
    results = {}
    dropped = 0
    for tag, name, per in done:
        # A frame every decoder scores at over three times the run's median
        # is not the run's picture (a skip in that capture): left out.
        de = np.array([per[d]["delta_e_mean"] for d in DECODERS])
        keep = ~(de > 3 * np.median(de, axis=1, keepdims=True)).all(axis=0)
        dropped += int((~keep).sum())
        per = {d: {m: [x for x, k in zip(v, keep) if k]
                   for m, v in per[d].items()} for d in DECODERS}
        results[f"{tag} {name}"] = per
        print(f"{tag} {name:24s} " + "  ".join(
            f"{d} " + " ".join(
                "{0:.2f} [{1:.2f}, {2:.2f}]".format(
                    *interval(per[d][m])) if m == "delta_e_mean" else
                f"{np.mean(per[d][m]):.2f}" for m in METRICS)
            for d in DECODERS), flush=True)
    print(f"over the runs (mean [95 % interval]); {dropped} frames left "
          "out as not the run's picture:")
    for d in DECODERS:
        print(f"  {d:12s} " + "  ".join(
            "{0} {1:.3f} [{2:.3f}, {3:.3f}]".format(
                SHORT[m], *interval([np.mean(r[d][m])
                                     for r in results.values()]))
            for m in METRICS))
    for other in ("pal2d", "transform2d"):
        print(f"  domesday2d - {other}: " + "  ".join(
            "{0} {1:+.3f} [{2:+.3f}, {3:+.3f}]".format(
                SHORT[m], *paired([np.mean(r["palstill"][m])
                                   for r in results.values()],
                                  [np.mean(r[other][m])
                                   for r in results.values()]))
            for m in METRICS))
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f)


def _matched(da, db, m):
    """phase7_real._matched(), leaving the chroma alone where the second
    decode has none to fit (a picture without colour)."""
    ya, yb = da[0][m], db[0][m]
    g, o = np.linalg.lstsq(np.stack([yb, np.ones_like(yb)], 1), ya,
                           rcond=None)[0]
    wa = da[1][m] + 1j * da[2][m]
    wb = db[1][m] + 1j * db[2][m]
    power = (np.abs(wb) ** 2).sum()
    k = (np.conj(wb) * wa).sum() / power if power > 1e-9 * wb.size else 1.0
    w = k * (db[1] + 1j * db[2])
    return g * db[0] + o, w.real, w.imag


def _disagreement(da, db, mask):
    m = mask.copy()
    m[:, :bench.EDGE_EXCLUSION] = False
    m[:, -bench.EDGE_EXCLUSION:] = False
    db = _matched(da, db, m)
    out = {}
    for name, x, y in zip("YUV", da, db):
        out[name] = float(np.sqrt(((x - y)[m] ** 2).mean()) * 100)
    de = bench.ciede2000(bench.yuv_to_lab(*(x[m] for x in da)),
                         bench.yuv_to_lab(*(x[m] for x in db)))
    out["dE"] = float(de.mean())
    k = int(0.99 * de.size)
    out["dE_p99"] = float(np.partition(de, k)[k])
    band = [bench.filter_rows(x, bench.LUMA_BAND) for x in (da[0], db[0])]
    out["Y_hf"] = float(np.sqrt(((band[0] - band[1])[m] ** 2).mean()) * 100)
    return out


def _cross_job(job):
    out, k, groups = job
    mask = bench.picture_mask()
    tags = list(groups)
    decodes = {(t, d): bench.decoded(
        os.path.join(out, "decodes", f"cross_{t}_{d}.yuv"), k)
        for t in tags for d in DECODERS}
    row = {}
    for d in DECODERS:
        per = {"within": [], "across": []}
        for a, b in itertools.combinations(tags, 2):
            kind = "within" if groups[a] == groups[b] else "across"
            per[kind].append(_disagreement(decodes[(a, d)], decodes[(b, d)],
                                           mask))
        row[d] = {kind: {m: float(np.mean([x[m] for x in v]))
                         for m in ("Y", "U", "V", "dE", "dE_p99", "Y_hf")}
                  for kind, v in per.items() if v}
    return k, row


def cross(out, groups, json_path):
    stills = phase7_real.STILLS
    with Pool(min(len(stills), 12)) as pool:
        done = dict(pool.map(_cross_job, [(out, k, groups)
                                          for k in range(len(stills))]))
    rows = [done[k] for k in range(len(stills))]
    results = {"pictures": {str(p): rows[k] for k, p in enumerate(stills)}}
    for kind in ("within", "across"):
        if kind not in rows[0][DECODERS[0]]:
            continue
        print(f"{kind} groups (mean over pictures [95 % interval]):")
        for d in DECODERS:
            print(f"  {d:12s} " + "  ".join(
                "{0} {1:.3f} [{2:.3f}, {3:.3f}]".format(
                    m, *interval([r[d][kind][m] for r in rows]))
                for m in ("dE", "dE_p99", "Y", "Y_hf", "U", "V")))
        for other in ("pal2d", "transform2d"):
            print(f"  domesday2d - {other}: " + "  ".join(
                "{0} {1:+.3f} [{2:+.3f}, {3:+.3f}]".format(
                    m, *paired([r["palstill"][kind][m] for r in rows],
                               [r[other][kind][m] for r in rows]))
                for m in ("dE", "dE_p99", "Y", "U", "V")))
        best = sum(1 for r in rows if r["palstill"][kind]["dE"] ==
                   min(r[d][kind]["dE"] for d in DECODERS))
        print(f"  domesday2d lowest ΔE on {best} of {len(rows)} pictures")
    if json_path:
        with open(json_path, "w") as f:
            json.dump(results, f, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("held")
    s.add_argument("out")
    s.add_argument("tags", nargs="+")
    s.add_argument("--side", choices=phase9_sides.SIDES)
    s.add_argument("--json")
    s = sub.add_parser("cross")
    s.add_argument("out")
    s.add_argument("--side", choices=phase9_sides.SIDES)
    s.add_argument("--groups")
    s.add_argument("--json")
    args = ap.parse_args()
    if args.side:
        phase9_sides._use(args.side)
    if args.cmd == "held":
        held(args.out, args.tags, args.json)
    else:
        groups = dict(phase7_real.GROUPS)
        if args.groups:
            groups = dict(x.split("=") for x in args.groups.split(","))
        cross(args.out, groups, args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
