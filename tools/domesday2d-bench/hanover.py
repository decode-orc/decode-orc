#!/usr/bin/env python3
#
# File:        hanover.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 4 step 2: the Phase 2 per-line chroma
#              errors on the bench, and the anti-Hanover measures
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# The impairment. Phase 2 measured, on Domesday CAV pictures, a constant
# chroma-to-burst phase error of -5 to +3 deg, an alternating V<->U leak of
# 0.04-0.09 beyond it, and 3.4 deg r.m.s. of line-to-line burst phase
# jitter. impair() puts those on a bench composite: the truth's chroma
# (encoder.py) is taken out and put back with the carrier phase
# theta + e + delta_line and
#   (U, V) -> (I + s X) (U, V),  X = [[0, v_to_u], [u_to_v, 0]].
# A line's jitter has two parts: a timing error that turns its burst and
# its picture together (|jitter_shared_deg|), and burst-reading noise that
# turns only the burst (|jitter_burst_deg|). phase4_jitter.py splits the
# Phase 2 figure on Community South: about 2.0 deg shared, 2.6 deg burst
# only (PHASE2); ALL_SHARED is the worst case for the field-fitted
# lattice.
#
# Decoded against a burst that does not carry e, a line of switch s reads
#   W_s = (I + s M) W,  M = [[0, -e], [e, 0]] + X
# (picture_chroma.py's model). In a field the next line has the other
# switch, so PAL averaging over field lines cancels M to first order. In
# the frame raster the 312-line partner has the same switch, and rows
# r, r+1 | r+2, r+3 alternate in pairs; the Phase 3 kernel's reach of +/-7
# rows still averaged it out, but adaptive.py's edge-preserving fit keeps
# what it reads as structure. The measures:
#   flat_pairs(),      M fitted from the flat colour of field-adjacent
#   fit_matrix()       rows, pooled over many frames: Phase 2 found it
#                      constant per capture side, and one frame's fit is
#                      fooled by fine vertical colour structure
#   corrector(M)       removes it: W = (I + s M)^-1 W_s, on each row
#   line_lattice()     each line's phase from its own burst (per-line
#                      burst referencing; a Phase 2 no-go, rechecked here)
#   masked_average()   dp111-style masked suppression: a [1/4, 1/2, 1/4]
#                      field-line average where the field lines either
#                      side agree (not at a vertical chroma step) and the
#                      312-line partner row, which has the same switch,
#                      deviates the same way (a Hanover error does; a
#                      stripe one field line high in one field does not)

import numpy as np
from scipy.ndimage import uniform_filter1d

import bench
import encoder
import proto

PHASE2 = dict(phase_deg=-4.0, v_to_u=0.06, u_to_v=-0.02,
              jitter_shared_deg=2.0, jitter_burst_deg=2.6)
ALL_SHARED = dict(PHASE2, jitter_shared_deg=3.4, jitter_burst_deg=0.0)
BURST_SPAN = (90, 146)


def _rotate_burst(raster, delta):
    """Each row's burst turned by delta (rad): on the 4fsc grid the
    quadrature of b(n) is b(n - 1)."""
    a, b = BURST_SPAN
    seg = raster[:, a:b]
    prev = raster[:, a - 1:b - 1]
    out = raster.copy()
    out[:, a:b] = seg * np.cos(delta)[:, None] - prev * \
        np.sin(delta)[:, None]
    return out


def impair(raster, truth, phi, s, phase_deg=0.0, v_to_u=0.0, u_to_v=0.0,
           jitter_shared_deg=0.0, jitter_burst_deg=0.0, seed=1):
    """|raster| (carrying |truth| on the lattice phi, s) with the truth's
    chroma replaced by the impaired chroma."""
    ty, tu, tv = truth
    zero = np.zeros_like(ty)
    clean = encoder.encode((zero, tu, tv), phi, s, quantise=False)
    rng = np.random.default_rng(seed)
    shared = rng.normal(0.0, jitter_shared_deg, proto.ROWS)
    burst = shared + rng.normal(0.0, jitter_burst_deg, proto.ROWS)
    rows = slice(bench.TRUTH_ROW0, bench.TRUTH_ROW0 + bench.TRUTH_ROWS)
    sw = s[rows, None]
    u2 = tu + sw * v_to_u * tv
    v2 = tv + sw * u_to_v * tu
    out = raster - clean + encoder.encode((zero, u2, v2),
                                          phi + phase_deg + shared, s,
                                          quantise=False)
    return _rotate_burst(out, np.radians(burst))


def line_lattice(raster, phi, s):
    """phi of each row from its own burst (s from the field fit). Rows
    without a burst (blanked by the burst-blanking sequence) keep the
    field fit."""
    n = np.arange(*proto.BURST)
    z = (raster[:, proto.BURST[0]:proto.BURST[1]] *
         np.exp(-0.5j * np.pi * n)).mean(axis=1)
    offset = np.where(s > 0, 45.0, 135.0)
    measured = np.degrees(np.angle(z)) - offset
    # Keep the field fit's turn count: the nearest equivalent angle.
    out = phi + ((measured - phi + 180.0) % 360.0 - 180.0)
    present = np.abs(z) > 0.5 * np.median(np.abs(z))
    return np.where(present, out, phi)


def flat_pairs(u, v, s, min_chroma=0.03, flat=0.02, span=8):
    """(plus, minus): W of field-adjacent rows (r, r + 2) of flat colour,
    the s = +1 row's and the s = -1 row's: both rows' chroma above
    |min_chroma|, each within |flat| of itself |span| samples either side
    and of the row 4 on (the same switch)."""
    lo, hi = proto.FIRST_ACTIVE, proto.END_ACTIVE - 4
    w = u + 1j * v
    a, b, a2 = w[lo:hi], w[lo + 2:hi + 2], w[lo + 4:hi + 4]
    ok = (np.abs(a) > min_chroma) & (np.abs(b) > min_chroma)
    for d in (span, -span):
        ok &= np.abs(a - np.roll(a, d, axis=1)) < flat
        ok &= np.abs(b - np.roll(b, d, axis=1)) < flat
    ok &= np.abs(a - a2) < flat
    ok[:, :bench.TRUTH_FIRST_SAMPLE + span] = False
    ok[:, bench.TRUTH_FIRST_SAMPLE + bench.TRUTH_WIDTH - span:] = False
    sa = np.broadcast_to(s[lo:hi, None], a.shape)
    sb = np.broadcast_to(s[lo + 2:hi + 2, None], a.shape)
    ok &= sa != sb
    return np.where(sa > 0, a, b)[ok], np.where(sa > 0, b, a)[ok]


def fit_matrix(pairs, minimum=1000, max_ratio=0.3):
    """M of W_s = (I + s M) W from a list of flat_pairs() results (zero
    with fewer than |minimum| samples). Pairs whose alternating half is
    more than |max_ratio| of their mean are picture, not error (Phase 2's
    largest error is about 0.17: fine colour stripes one field line high
    otherwise swamp the fit). Returns (M, samples)."""
    plus = np.concatenate([p for p, _ in pairs])
    minus = np.concatenate([m for _, m in pairs])
    mean = (plus + minus) / 2.0
    half = (plus - minus) / 2.0
    keep = np.abs(half) <= max_ratio * np.abs(mean)
    mean, half = mean[keep], half[keep]
    if mean.size < minimum:
        return np.zeros((2, 2)), int(mean.size)
    x = np.stack([mean.real, mean.imag], 1)
    y = np.stack([half.real, half.imag], 1)
    m, *_ = np.linalg.lstsq(x, y, rcond=None)
    return m.T, int(plus.size)


def _per_switch(mats):
    def run(u, v, s):
        out_u, out_v = u.copy(), v.copy()
        for sign in (1, -1):
            rows = s == sign
            k = mats[sign]
            out_u[rows] = k[0, 0] * u[rows] + k[0, 1] * v[rows]
            out_v[rows] = k[1, 0] * u[rows] + k[1, 1] * v[rows]
        return out_u, out_v
    return run


def corrector(m):
    """(u, v, s) -> (u, v) undoing W_s = (I + s M) W on each row. Its
    .inverse puts the error back: re-encoding a corrected estimate must
    go through it, or the residual carries the error again."""
    forward = {1: np.eye(2) + m, -1: np.eye(2) - m}
    run = _per_switch({k: np.linalg.inv(a) for k, a in forward.items()})
    run.inverse = _per_switch(forward)
    return run


def masked_average(threshold=0.04, soft=0.01):
    """(u, v, s) -> (u, v): [1/4, 1/2, 1/4] over field lines (raster rows
    r - 2, r, r + 2), weighted by a mask that is 1 where rows r - 2 and
    r + 2 agree and where the partner row's deviation from its own field
    neighbours matches row r's (each within |threshold| of vector
    difference, falling to 0 over |soft|). Differences are smoothed along
    the row over 9 samples."""
    def ramp(diff):
        diff = uniform_filter1d(diff, 9, axis=1)
        return np.clip(1.0 - (diff - threshold) / soft, 0.0, 1.0)

    def run(u, v, s):
        w = u + 1j * v
        avg = 0.5 * w + 0.25 * (np.roll(w, 2, axis=0) +
                                np.roll(w, -2, axis=0))
        dev = w - avg
        # The partner: the row above a field 1 (even) row, below a field 2
        # row.
        partner = np.empty_like(dev)
        partner[0::2] = np.roll(dev, 1, axis=0)[0::2]
        partner[1::2] = np.roll(dev, -1, axis=0)[1::2]
        m = ramp(np.abs(np.roll(w, 2, axis=0) - np.roll(w, -2, axis=0)))
        m = m * ramp(np.abs(dev - partner))
        out = w - m * dev
        return out.real, out.imag
    return run
