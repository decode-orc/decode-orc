#!/usr/bin/env python3
#
# File:        phase2_picture.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 2 steps 4 and 5 on the picture: the
#              line-alternating chroma error and inter-field registration
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage: phase2_picture.py [--frames N] [--json out.json] capture...
#
# N frames spread evenly over each capture. Each frame's two fields are
# registered (registration.py) and its flat colour is checked for a
# line-alternating error (picture_chroma.py). A frame counts for
# registration if it has horizontal detail of at least 1.0 IRE per sample.
# Registration assumes a still: run it on CAV sides, whose frames are
# stills, not on a CLV side of moving video.

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cvbs  # noqa: E402
import picture_chroma as pc  # noqa: E402
import registration as rg  # noqa: E402
import vits  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+")
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--json")
    args = parser.parse_args()
    results = {}
    for path in args.captures:
        capture = cvbs.Capture(path)
        frames = np.linspace(500, capture.frames - 500,
                             args.frames).astype(int)
        reg = []
        pairs = []
        for frame in frames:
            edge = vits.bar_edge(capture.line(frame, 1, 19))
            if edge is None:
                continue
            f1 = rg.field_luma(capture, frame, 1, edge)
            f2 = rg.field_luma(capture, frame, 2, edge)
            d = rg.detail(f1)
            if d >= 1.0:
                dx, dy, rms, rms0 = rg.register(f1, f2)
                reg.append((int(frame), dx, dy, rms, rms0, d))
            for field in (1, 2):
                uv, switch = pc.field_uv(capture, frame, field, 24, 308,
                                         edge)
                pairs += pc.flat_pairs(uv, switch)
        out = {"frames": len(frames), "still_frames": len(reg)}
        if reg:
            r = np.array([x[1:] for x in reg])
            out["dx_samples"] = {"median": float(np.median(r[:, 0])),
                                 "p10": float(np.percentile(r[:, 0], 10)),
                                 "p90": float(np.percentile(r[:, 0], 90))}
            out["dy_field_lines"] = {"median": float(np.median(r[:, 1])),
                                     "p10": float(np.percentile(r[:, 1], 10)),
                                     "p90": float(np.percentile(r[:, 1], 90))}
            out["registration"] = reg
        m, phase, n = pc.alternating_matrix(pairs)
        if m is not None:
            out["alternating_matrix"] = m.round(4).tolist()
            out["hanover_deg"] = float(phase)
            out["flat_samples"] = int(n)
        results[path] = out
        print(f"{capture.name}: " + json.dumps(
            {k: v for k, v in out.items() if k != "registration"}),
            flush=True)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1)


if __name__ == "__main__":
    sys.exit(main())
