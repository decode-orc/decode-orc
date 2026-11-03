#!/usr/bin/env python3
#
# File:        phase4_jitter.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 4 step 2: does a capture's picture chroma
#              share its bursts' line-to-line phase jitter?
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage: phase4_jitter.py [--frames N] [--json out.json] capture...
#
# Phase 2 measured 3.4 deg r.m.s. of line-to-line burst phase jitter but
# could not say whether it is a timing error that moves the picture's
# chroma with the burst (then each line should be decoded against its own
# burst) or noise in reading the burst (then against the field's fitted
# lattice, as proto.lattice() does). On the bench the two need opposite
# decoders (phase4_hanover.py), so it is measured here, on flat colour:
# lines l and l + 2 of one field (the same V-switch, so no Hanover term)
# are decoded both ways, and the r.m.s. hue difference between them is
# compared. The reference that follows the picture gives the smaller one.
# The flat-colour mask does not depend on the reference: chroma above
# 4 IRE, flat along the line, and the same magnitude on both lines.

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cvbs  # noqa: E402
import picture_chroma as pc  # noqa: E402
import vits  # noqa: E402

FIRST, LAST = 24, 308


def field_references(capture, frame, field, edge):
    """Each line's demodulated active line and two unit carriers: from its
    own burst, and from the field's bursts fitted as one constant on the
    grid's 270 deg a line."""
    lines = capture.lines(frame, field, FIRST - 1, LAST)
    a, b = vits._span(edge, pc.ACTIVE)
    demods, bursts = [], []
    for line in lines:
        demods.append(vits.demod(line)[a:b])
        bursts.append(vits.burst(line, edge))
    bursts = np.array(bursts)
    step = np.degrees(np.angle(bursts[1:] / bursts[:-1])) - 270.0
    step = (step + 180.0) % 360.0 - 180.0
    switch = np.where(step < 0, 1, -1)
    own = np.array([bu / abs(bu) * np.exp(-1j * pc._SWITCH_PHASE[s])
                    for bu, s in zip(bursts[1:], switch)])
    n = np.arange(own.size)
    turn = np.exp(1j * np.radians(270.0) * n)
    fitted = np.angle((own / turn).mean())
    field_fit = np.exp(1j * fitted) * turn
    return np.array(demods[1:]), switch, own, field_fit


def decode(demods, switch, carriers):
    out = []
    for z, s, c in zip(demods, switch, carriers):
        p = z * np.conj(c)
        out.append(-p.imag + 1j * s * p.real)
    return np.array(out)


def hue_differences(own, fit, min_ire=4.0, flat_ire=1.5, span=8):
    """Hue difference (deg) of lines l and l + 2 at the flat samples, for
    both references."""
    d_own, d_fit = [], []
    for i in range(len(own) - 2):
        a, b = own[i], own[i + 2]
        ok = (np.abs(a) > min_ire) & (np.abs(b) > min_ire)
        ok &= np.abs(np.abs(a) - np.abs(b)) < 1.0
        for d in (span, -span):
            ok &= np.abs(a - np.roll(a, d)) < flat_ire
            ok &= np.abs(b - np.roll(b, d)) < flat_ire
        ok[:span] = False
        ok[-span:] = False
        if ok.sum() < 20:
            continue
        # One hue difference per line pair: the mean over its samples.
        d_own.append(np.degrees(np.angle((b[ok] * np.conj(a[ok])).mean())))
        fa, fb = fit[i], fit[i + 2]
        d_fit.append(np.degrees(np.angle((fb[ok] * np.conj(fa[ok])).mean())))
    return d_own, d_fit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+")
    parser.add_argument("--frames", type=int, default=40)
    parser.add_argument("--json")
    args = parser.parse_args()
    results = {}
    for path in args.captures:
        capture = cvbs.Capture(path)
        frames = np.linspace(500, capture.frames - 500,
                             args.frames).astype(int)
        own_all, fit_all, burst_steps = [], [], []
        for frame in frames:
            edge = vits.bar_edge(capture.line(frame, 1, 19))
            if edge is None:
                continue
            for field in (1, 2):
                demods, switch, own, fit = field_references(
                    capture, frame, field, edge)
                d_own, d_fit = hue_differences(decode(demods, switch, own),
                                               decode(demods, switch, fit))
                own_all += d_own
                fit_all += d_fit
                burst_steps += list(np.degrees(np.angle(own / fit)))
        own_all, fit_all = np.array(own_all), np.array(fit_all)

        def rms(x):
            return float(np.sqrt((x ** 2).mean())) if x.size else None

        def robust(x):
            return float(1.4826 * np.median(np.abs(x))) if x.size else None
        results[capture.name] = {
            "pairs": int(own_all.size),
            "own_burst_rms_deg": rms(own_all),
            "field_fit_rms_deg": rms(fit_all),
            "own_burst_robust_deg": robust(own_all),
            "field_fit_robust_deg": robust(fit_all),
            "burst_vs_fit_rms_deg": rms(np.array(burst_steps)),
        }
        print(capture.name, json.dumps(results[capture.name]), flush=True)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1)


if __name__ == "__main__":
    sys.exit(main())
