#!/usr/bin/env python3
#
# File:        picture_chroma.py
# Module:      tools/domesday2d-bench
# Purpose:     Per-line chroma of a capture's picture against each line's
#              own burst: the line-alternating (Hanover) error and the
#              alternating U/V crosstalk on flat colour
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Domesday2D Phase 2 step 4. The VITS chroma on Domesday pressings is not
# locked to the burst (its phase against the burst repeats only every six
# frames), so these are measured on the picture.
#
# Each line is demodulated on the 4fsc grid and referred to its own burst.
# The burst sits at +45 deg (V-switch +) or +135 deg (V-switch -) from the
# line's V axis in this demodulator's convention, and consecutive lines'
# bursts differ by the grid's 270 deg plus that +/-90 deg, which gives each
# line's switch state. A colour (U, V) then decodes the same on both
# states; whatever differs between two field-adjacent lines of flat colour
# is the line-alternating error, fitted as a 2x2 matrix:
#
#   (U, V) on a line of switch s  =  (U, V) + s * M (U, V)
#
# A chroma-to-burst phase error phi is M = [[0, -phi], [phi, 0]]; a fixed
# leak of the transmitted V into U (videosynth crosstalk v_to_u with
# alternate_with_switch) is M = [[0, k], [0, 0]].

import numpy as np

import vits

ACTIVE = (-1.0, 50.0)  # us from the bar edge: the active line
_SWITCH_PHASE = {1: np.radians(45.0), -1: np.radians(135.0)}


def field_uv(capture, frame, field, first, last, edge):
    """Decoded (U + jV) of field lines first..last over the active line,
    in IRE (peak), and each line's switch sign."""
    lines = capture.lines(frame, field, first - 1, last)
    bursts = []
    demods = []
    a, b = vits._span(edge, ACTIVE)
    for line in lines:
        z = vits.demod(line)
        bursts.append(vits.burst(line, edge))
        demods.append(z[a:b])
    bursts = np.array(bursts)
    step = np.degrees(np.angle(bursts[1:] / bursts[:-1])) - 270.0
    step = (step + 180.0) % 360.0 - 180.0
    switch = np.where(step < 0, 1, -1)
    out = []
    for z, burst, s in zip(demods[1:], bursts[1:], switch):
        carrier = burst / abs(burst) * np.exp(-1j * _SWITCH_PHASE[s])
        p = z * np.conj(carrier)  # V_tx - jU
        out.append(-p.imag + 1j * s * p.real)
    return np.array(out), switch


def flat_pairs(uv, switch, min_ire=4.0, flat_ire=1.5, span=8):
    """Samples of field-adjacent line pairs where the colour is flat:
    chroma of at least |min_ire|, and within |flat_ire| of itself |span|
    samples either side and of the same-switch line two lines on."""
    pairs = []
    for i in range(len(uv) - 3):
        a, b, a2 = uv[i], uv[i + 1], uv[i + 2]
        ok = (np.abs(a) > min_ire) & (np.abs(b) > min_ire)
        for d in (span, -span):
            ok &= np.abs(a - np.roll(a, d)) < flat_ire
            ok &= np.abs(b - np.roll(b, d)) < flat_ire
        ok &= np.abs(a - a2) < flat_ire
        ok[:span] = False
        ok[-span:] = False
        if ok.any():
            pairs.append((a[ok], b[ok], switch[i], switch[i + 1]))
    return pairs


def alternating_matrix(pairs):
    """Fit M (2x2) from flat pairs: half the difference of the + line and
    the - line against their mean. Returns M, the Hanover phase (deg) and
    the number of samples."""
    rows, targets = [], []
    for a, b, sa, sb in pairs:
        if sa == sb:
            continue
        plus, minus = (a, b) if sa > 0 else (b, a)
        mean = (plus + minus) / 2.0
        half = (plus - minus) / 2.0
        rows.append(np.stack([mean.real, mean.imag], 1))
        targets.append(np.stack([half.real, half.imag], 1))
    if not rows:
        return None, None, 0
    x = np.concatenate(rows)
    y = np.concatenate(targets)
    m, *_ = np.linalg.lstsq(x, y, rcond=None)
    m = m.T
    phase = np.degrees((m[1, 0] - m[0, 1]) / 2.0)
    return m, phase, len(x)
