#!/usr/bin/env python3
#
# File:        phase2_vits.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 2 steps 1, 2 and 4: each capture's chain
#              measured from its VITS at three disc positions
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage: phase2_vits.py [--frames N] [--json out.json] capture...
#
# A capture is a PAL .cvbs (CVBS_U10_4FSC) or an ld-decode .tbc. Each is
# measured at file frames near the start, middle and end of a CAV side
# (inner, middle and outer radius), N frames at each. See vits.py for the
# signals and methods; the figures are written up in
# docs-tech/domesday2d-decoder-investigation.md, Phase 2.

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cvbs  # noqa: E402
import vits  # noqa: E402


def wrap(deg):
    return (deg + 180.0) % 360.0 - 180.0


def measure(capture, frames):
    out = {}
    twenty = [], []
    mb = []
    bars = []
    stairs = []
    sidebands = []
    edges = []
    for frame in frames:
        l19 = capture.line(frame, 1, 19)
        edge = vits.bar_edge(l19)
        if edge is None:
            continue
        edges.append(edge)
        t = vits.twenty_t(l19, edge)
        twenty[0].append(t["gain"])
        twenty[1].append(t["delay_ns"])
        mb.append([v for _, v in sorted(vits.multiburst(
            capture.line(frame, 1, 20), edge).items())])

        l333 = capture.line(frame, 2, 20)
        steps, burst_amp = vits.chroma_steps(l333, edge, vits.BARS_331)
        bars.append([steps[20][0], steps[60][0], steps[100][0],
                     steps[100][0] / burst_amp, steps[20][1], steps[60][1],
                     steps[100][1]])

        l332 = capture.line(frame, 2, 19)
        steps, _ = vits.chroma_steps(l332, edge, vits.STAIRCASE_330)
        stairs.append([steps[k][0] for k in (0, 20, 40, 60, 80, 100)] +
                      [steps[k][1] for k in (0, 20, 40, 60, 80, 100)])
        f, mag = vits.sideband_spectrum(l19, edge)
        sidebands.append(mag)

    out["frames"] = len(edges)
    out["edge_jitter_ns"] = float(np.std(edges) / vits.RATE * 1e9)
    out["twenty_t_gain"] = float(np.mean(twenty[0]))
    out["twenty_t_delay_ns"] = float(np.mean(twenty[1]))
    out["twenty_t_delay_sd_ns"] = float(np.std(twenty[1]))
    out["multiburst_db"] = dict(zip(
        ["0.5", "1.0", "2.0", "4.0", "4.8", "5.8"],
        np.round(np.median(mb, 0), 2).tolist()))

    bars = np.array(bars)
    out["bars_331_ratio_20_60"] = [
        float(np.median(bars[:, 0] / bars[:, 2])),
        float(np.median(bars[:, 1] / bars[:, 2]))]
    out["bars_331_100_over_burst"] = float(np.median(bars[:, 3]))
    out["bars_331_phase_20_60_deg"] = [
        float(np.median(wrap(bars[:, 4] - bars[:, 6]))),
        float(np.median(wrap(bars[:, 5] - bars[:, 6])))]

    stairs = np.array(stairs)
    amp = stairs[:, :6] / stairs[:, :1]
    phase = wrap(stairs[:, 6:] - stairs[:, 6:7])
    out["dg_pct"] = (np.median(amp, 0) * 100.0 - 100.0).round(2).tolist()
    out["dp_deg"] = np.median(phase, 0).round(2).tolist()

    out["sideband_upper_over_lower_db"] = {
        str(k): round(float(v), 2) for k, v in
        vits.sideband_ratio_db(f, np.mean(sidebands, 0)).items()}

    # Burst jitter over the active lines of field 1, eight frames.
    jitter = []
    for frame in frames[:8]:
        edge = vits.bar_edge(capture.line(frame, 1, 19))
        if edge is None:
            continue
        for field in (1, 2):
            ph = vits.burst_phases(capture.lines(frame, field, 23, 309),
                                   edge)
            jitter.append(vits.burst_jitter(ph)[0])
    out["burst_jitter_deg"] = float(np.median(jitter))

    avg = vits.aligned_average(capture, frames, 1, 19, -3.0, 17.0)
    a, tau, res, res0 = vits.smear_fit(avg, -3.0)
    out["smear_fraction"] = float(a)
    out["smear_tau_samples"] = float(tau)
    out["smear_rms_ire"] = [res, res0]
    scale, smear_share = vits.separate_20t(out["twenty_t_delay_ns"], a, tau)
    out["gd_scale"] = float(scale)
    out["twenty_t_smear_share_ns"] = float(smear_share)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+")
    parser.add_argument("--frames", type=int, default=32)
    parser.add_argument("--json")
    args = parser.parse_args()
    results = {}
    for path in args.captures:
        capture = cvbs.Capture(path)
        positions = {"inner": 1000, "middle": capture.frames // 2,
                     "outer": capture.frames - 1000}
        results[path] = {}
        for name, start in positions.items():
            frames = list(range(start, start + args.frames))
            r = measure(capture, frames)
            results[path][name] = r
            print(f"{capture.name} {name}: " + json.dumps(r), flush=True)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1)


if __name__ == "__main__":
    sys.exit(main())
