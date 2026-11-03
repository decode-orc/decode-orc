#!/usr/bin/env python3
#
# File:        phase3_lslcd.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 3 step 4: train the least-squares
#              separators on procedural pictures and report the
#              generalisation gap
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage: phase3_lslcd.py --fonts DIR --out DIR [--per-kind N]
#
# The pictures of training.py are encoded (encoder.py) on the lattices of
# the bench's first four frames (the four frame phases), each once clean
# and once with the Domesday noise model's noise added: the noisy bench's
# composite less the clean one's on the unmeasured lead and tail frames
# (0 and 13). The last picture of each kind is held out. Writes one .npz
# per separator and train.json: the Y/U/V r.m.s. error (percent) on the
# training pictures, the held-out pictures and the evaluation stills.

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import encoder  # noqa: E402
import lslcd  # noqa: E402
import proto  # noqa: E402
import training  # noqa: E402

SEPARATORS = {
    "ls_frame_r4t8": dict(rows=4, taps=8, geometry="frame"),
    "ls_field_r4t8": dict(rows=4, taps=8, geometry="field"),
    "ls_frame_r6t10": dict(rows=6, taps=10, geometry="frame"),
    "ls_field_r6t10": dict(rows=6, taps=10, geometry="field"),
}


def lattices():
    out = []
    for f in range(4):
        raster = bench.frame_raster(bench.composite("clean", f))
        out.append(proto.lattice(raster))
    return out


def noise_fields():
    out = []
    for f in (0, 13):
        clean = bench.frame_raster(bench.composite("clean", f))
        noisy = bench.frame_raster(bench.composite("noisy", f))
        out.append(noisy - clean)
    return out


def rms(est, truth, mask):
    return [float(np.sqrt(((a - b)[mask] ** 2).mean()) * 100)
            for a, b in zip(est, truth)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fonts", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--per-kind", type=int, default=6)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    lats = lattices()
    noises = noise_fields()
    seps = {name: {"clean": lslcd.Separator(**kw),
                   "noisy": lslcd.Separator(**kw)}
            for name, kw in SEPARATORS.items()}
    train, held = [], []
    for n, (kind, i, rgb) in enumerate(training.pictures(
            args.fonts, args.per_kind)):
        rgb = training.limit_chroma(rgb)
        truth = encoder.picture_truth(rgb)
        phi, s = lats[n % 4]
        raster = encoder.encode(truth, phi, s)
        noisy = raster + noises[n % 2]
        item = (kind, i, truth, raster, noisy, phi, s)
        if i == args.per_kind - 1:
            held.append(item)
            continue
        train.append(item)
        for name in seps:
            seps[name]["clean"].add(raster, truth, phi, s)
            seps[name]["noisy"].add(noisy, truth, phi, s)
        print(f"trained on {kind} {i}", flush=True)
    mask = bench.picture_mask()
    mask[:, :bench.EDGE_EXCLUSION] = False
    mask[:, -bench.EDGE_EXCLUSION:] = False
    report = {}
    b = bench.Bench()
    for name in seps:
        for cond in ("clean", "noisy"):
            sep = seps[name][cond]
            sep.solve()
            sep.save(os.path.join(args.out, f"{name}_{cond}.npz"))
            r = {}
            for label, items in (("train", train), ("held", held)):
                errs = []
                for kind, i, truth, raster, noisy, phi, s in items:
                    x = raster if cond == "clean" else noisy
                    errs.append(rms(sep.apply(x, phi, s), truth, mask))
                r[label] = np.mean(errs, axis=0).tolist()
            errs = []
            case = "clean" if cond == "clean" else "noisy"
            for f, still in b.stills:
                raster = bench.frame_raster(bench.composite(case, f))
                phi, s = proto.lattice(raster)
                est = sep.apply(raster, phi, s)
                errs.append(rms(est, b.truth[f], b.masks[f]))
            r["eval"] = np.mean(errs, axis=0).tolist()
            report[f"{name}_{cond}"] = r
            print(name, cond, json.dumps(r), flush=True)
    with open(os.path.join(args.out, "train.json"), "w") as f:
        json.dump(report, f, indent=1)


if __name__ == "__main__":
    sys.exit(main())
