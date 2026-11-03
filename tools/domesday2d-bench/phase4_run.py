#!/usr/bin/env python3
#
# File:        phase4_run.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 4 steps 1, 3 and 4: score the adaptive
#              chroma estimate, residual reassignment and chroma bandwidth
#              variants on the still bench, clean and noisy
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage: phase4_run.py [--json out.json] [--only NAME...]
#                      [--fonts DIR --training out.json]
#
# Every variant decodes the twelve stills (as phase3_run.py) and its
# prototype time per frame is recorded. With $DOMESDAY2D_LS_DIR set
# (phase3_lslcd.py's output), the oracle selection between the Phase 3
# candidate and the least-squares frame filter is scored too: the better
# of the two per 8 x 8 block, chosen against the truth, an upper bound on
# any local selection between them.
#
# With --fonts, the Phase 3 candidate and the Phase 4 defaults are also
# scored on the procedural training pictures (training.py), which none of
# the Phase 4 parameters were tuned on.

import argparse
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adaptive  # noqa: E402
import bench  # noqa: E402
import lslcd  # noqa: E402
import proto  # noqa: E402
import transform  # noqa: E402

FRAME_UV = proto.radial_kernel("frame")
LS_DIR = os.environ.get("DOMESDAY2D_LS_DIR")


def _kernel_after_transform(kernel):
    """The Phase 3 separation with a fixed U/V kernel."""
    def run(raster, c, phi, s):
        u, v = proto.filter_planes(*proto.demodulate(c, phi, s), kernel)
        return proto.Decoded(raster - c, u, v)
    return run


def _adaptive(**kw):
    def run(raster, c, phi, s):
        return adaptive.decode_separated(raster, c, phi, s, **kw)
    return run


NO_REASSIGN = dict(reassign=False)
VARIANTS = {
    # The Phase 3 candidate: a fixed U/V kernel over +/-7 frame rows.
    "phase3": _kernel_after_transform(FRAME_UV),
    # Step 4, fixed low-pass: the same kernel at other reaches and
    # bandwidths; and none (the horizontal filter only).
    "fixed_rows3": _kernel_after_transform(
        proto.radial_kernel("frame", row_scale=2.0)),
    "fixed_bw1.5": _kernel_after_transform(
        proto.radial_kernel("frame", bandwidth=1.5e6)),
    "none": _kernel_after_transform(proto.rows_kernel({0: 1.0})),
    "none_bw1.5": _kernel_after_transform(
        proto.rows_kernel({0: 1.0}, bandwidth=1.5e6)),
    # Step 1, local selection: a constant strength (no feature), then
    # steered by the luma detail.
    "fit_const0.01": _adaptive(k=0.0, passes=1, **NO_REASSIGN),
    "fit_const0.05": _adaptive(k=0.0, lam0=0.05, passes=1, **NO_REASSIGN),
    "fit_const0.1": _adaptive(k=0.0, lam0=0.1, passes=1, **NO_REASSIGN),
    "fit_detail": _adaptive(passes=1, **NO_REASSIGN),
    # Step 3, refinement: re-encode passes and residual reassignment.
    "fit_detail_p2": _adaptive(passes=2, **NO_REASSIGN),
    "fit_detail_p4": _adaptive(passes=4, **NO_REASSIGN),
    "reassign_all": _adaptive(tau=0.0, d0=0.0),
    "reassign_noise": _adaptive(d0=0.0),
    "phase4": _adaptive(),
    "phase4_p2": _adaptive(passes=2),
    "phase4_p4": _adaptive(passes=4),
    "phase4_bw1.5": _adaptive(bandwidth=1.5e6),
    "phase4_irls3": _adaptive(irls=3),
}


def _oracle(case):
    """The better of the Phase 3 candidate and the least-squares frame
    filter per 8 x 8 block, against the truth."""
    cond = "clean" if case == "clean" else "noisy"
    sep = lslcd.Separator.load(os.path.join(LS_DIR,
                                            f"ls_frame_r6t10_{cond}.npz"))

    def blocks(e, n=8):
        h, w = e.shape
        return e[:h // n * n, :w // n * n].reshape(
            h // n, n, w // n, n).sum((1, 3))

    def run(raster, c, phi, s, truth):
        a = VARIANTS["phase3"](raster, c, phi, s).truth_grid()
        b = sep.apply(raster, phi, s)
        ea = blocks(sum((x - t) ** 2 for x, t in zip(a, truth)))
        eb = blocks(sum((x - t) ** 2 for x, t in zip(b, truth)))
        pick = np.zeros(a[0].shape, dtype=bool)
        k = np.kron(eb < ea, np.ones((8, 8), dtype=bool))
        pick[:k.shape[0], :k.shape[1]] = k
        return tuple(np.where(pick, y, x) for x, y in zip(a, b))
    return run


def score(b, name, decode, case):
    out = {}
    seconds = []
    for frame, still in b.stills:
        raster, c, phi, s = transform.separate(bench.composite(case, frame),
                                               "frame", ytile=32)
        start = time.process_time()
        if name == "oracle_phase3_ls":
            grid = decode(raster, c, phi, s, b.truth[frame])
        else:
            grid = decode(raster, c, phi, s).truth_grid()
        seconds.append(time.process_time() - start)
        out[still] = bench.measure(grid, b.features[frame], b.masks[frame])
        if frame == bench.GREEN_EDGES:
            out[still]["edge_dots_codes"] = bench.edge_dots(
                grid[0], b.truth[frame][0])
    out["_seconds_per_frame"] = float(np.mean(seconds))
    return out


def training_check(fonts, per_kind):
    """Y r.m.s., RGB PSNR and mean delta E of the Phase 3 candidate and
    the Phase 4 defaults on the procedural pictures, clean and with the
    Domesday model's noise, per kind of picture."""
    import encoder  # noqa: PLC0415
    import training  # noqa: PLC0415
    lattices = [proto.lattice(bench.frame_raster(bench.composite("clean", f)))
                for f in range(4)]
    noises = [bench.frame_raster(bench.composite("noisy", f)) -
              bench.frame_raster(bench.composite("clean", f)) for f in (0, 13)]
    mask = bench.picture_mask()
    out = {}
    for n, (kind, i, rgb) in enumerate(training.pictures(fonts, per_kind)):
        truth = encoder.picture_truth(training.limit_chroma(rgb))
        features = bench.TruthFeatures(truth)
        phi, s = lattices[n % 4]
        clean = encoder.encode(truth, phi, s)
        for case, raster in (("clean", clean),
                             ("noisy", clean + noises[n % 2])):
            c = transform.chroma_frame(raster, ytile=32)
            for name in ("phase3", "phase4"):
                grid = VARIANTS[name](raster, c, phi, s).truth_grid()
                m = bench.measure(grid, features, mask)
                row = out.setdefault(case, {}).setdefault(
                    name, {}).setdefault(kind, [])
                row.append([m["y_rms_pct"], m["rgb_psnr_db"],
                            m["delta_e_mean"]])
        print("training", kind, i, flush=True)
    for case in out:
        for name in out[case]:
            for kind in out[case][name]:
                out[case][name][kind] = np.mean(out[case][name][kind],
                                                axis=0).tolist()
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json")
    parser.add_argument("--only", nargs="*")
    parser.add_argument("--fonts")
    parser.add_argument("--per-kind", type=int, default=3)
    parser.add_argument("--training")
    args = parser.parse_args()
    b = bench.Bench()
    results = {}
    if args.json and os.path.exists(args.json):
        with open(args.json) as f:
            results = json.load(f)
    for case in ("clean", "noisy"):
        results.setdefault(case, {})
        variants = dict(VARIANTS)
        if LS_DIR:
            variants["oracle_phase3_ls"] = _oracle(case)
        for name, decode in variants.items():
            if args.only and name not in args.only:
                continue
            r = score(b, name, decode, case)
            results[case][name] = r
            print(f"{case:5s} {name:20s} town dE "
                  f"{r['osm_town']['delta_e_mean']:5.2f}  text cc "
                  f"{r['text_card'].get('cross_colour_rms_pct', 0):5.2f}  "
                  f"edge dE {r['green_magenta_edges']['delta_e_mean']:5.2f}"
                  f"  {r['_seconds_per_frame']:.2f}s", flush=True)
            if args.json:
                with open(args.json, "w") as f:
                    json.dump(results, f, indent=1)
    if args.fonts:
        check = training_check(args.fonts, args.per_kind)
        with open(args.training, "w") as f:
            json.dump(check, f, indent=1)


if __name__ == "__main__":
    sys.exit(main())
