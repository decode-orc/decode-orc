#!/usr/bin/env python3
#
# File:        phase5_vits.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 5 steps 1 and 2 on captures: the smear's
#              parameter sources, and what the equaliser leaves on the VITS
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage: phase5_vits.py [--frames N] [--per-frame K] [--json out.json]
#                       capture...
#
# For each capture, at the inner, middle and outer radius (as
# phase2_vits.py), N frames each:
#   - the smear fitted once over the N frames (equalise.fit_smear()) and
#     what equalise.accept() makes of it;
#   - K of those frames fitted alone: how many fits are accepted, and
#     their spread;
#   - the source's value: the median of the accepted radius fits, 'no
#     smear' if none is accepted;
#   - the VITS read through the inverse smear at the source's value
#     (equalise.EqualisedCapture), measured as in Phase 2: the smear fit
#     left, the 20T delay and gain, the multiburst;
#   - the burst amplitude against the bench's (the burst-referenced
#     chroma gain).

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cvbs  # noqa: E402
import equalise  # noqa: E402
import phase2_vits  # noqa: E402
import proto  # noqa: E402


def burst_ratio(capture, frames):
    n = np.arange(*proto.BURST)
    ref = np.exp(-0.5j * np.pi * n)
    out = []
    for f in frames:
        lines = capture.lines(f, 1, 23, 309) / 100.0
        z = 2.0 * (lines[:, proto.BURST[0]:proto.BURST[1]] * ref).mean(1)
        out.append(np.median(np.abs(z)))
    return float(np.median(out) / equalise.NOMINAL_BURST)


def short(m):
    return {k: m[k] for k in ("frames", "twenty_t_gain", "twenty_t_delay_ns",
                              "multiburst_db", "smear_fraction",
                              "smear_tau_samples", "smear_rms_ire",
                              "gd_scale")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+")
    parser.add_argument("--frames", type=int, default=48)
    parser.add_argument("--per-frame", type=int, default=16)
    parser.add_argument("--json")
    args = parser.parse_args()
    results = {}
    for path in args.captures:
        capture = cvbs.Capture(path)
        positions = {"inner": 1000, "middle": capture.frames // 2,
                     "outer": capture.frames - 1000}
        r = {"radius": {}}
        accepted = []
        for name, start in positions.items():
            frames = list(range(start, start + args.frames))
            fit = equalise.fit_smear(capture, frames)
            got = equalise.accept(fit)
            singles = [equalise.accept(equalise.fit_smear(capture, [f]))
                       for f in frames[:args.per_frame]]
            smears = np.array([s for s in singles if s and s[0] > 0])
            r["radius"][name] = {
                "fit": fit, "accepted": got,
                "single_accepted": int(len(smears)),
                "single_none": int(sum(1 for s in singles
                                       if s and s[0] == 0)),
                "single_rejected": int(sum(1 for s in singles if s is None)),
                "single_fraction_p10_p50_p90":
                    np.percentile(smears[:, 0], [10, 50, 90]).tolist()
                    if len(smears) else None,
                "single_tau_p10_p50_p90":
                    np.percentile(smears[:, 1], [10, 50, 90]).tolist()
                    if len(smears) else None,
                "burst_ratio": burst_ratio(capture, frames[:8]),
            }
            if got is not None and got[0] > 0:
                accepted.append(got)
        source = tuple(np.median(accepted, axis=0)) if accepted \
            else (0.0, 1.0)
        r["source"] = source
        eq = equalise.EqualisedCapture(path, *source)
        for name, start in positions.items():
            frames = list(range(start, start + args.frames))
            r["radius"][name]["before"] = short(
                phase2_vits.measure(capture, frames))
            r["radius"][name]["after"] = short(phase2_vits.measure(eq, frames))
            refit = equalise.fit_smear(eq, frames)
            r["radius"][name]["after_accepted"] = equalise.accept(refit)
        results[path] = r
        print(capture.name, json.dumps(r, default=float), flush=True)
        if args.json:
            with open(args.json, "w") as f:
                json.dump(results, f, indent=1, default=float)


if __name__ == "__main__":
    sys.exit(main())
