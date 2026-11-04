#!/usr/bin/env python3
#
# File:        adaptive.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 4: adaptive vertical chroma estimation
#              after the frame-raster transform, and reassignment of the
#              residual to luma
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# The frame-raster Transform PAL of Phase 3 separates the chroma well; what
# it left on the bench came after separation, from the fixed U/V kernel
# (PalColour's radial raised cosine over +/-7 frame rows): it smears
# horizontal colour edges, and without it the luma's leak into the chroma
# shows as cross-colour. So here U and V are estimated from the separated
# chroma c by
#
#   1. per-row demodulation and PalColour's horizontal raised cosine only;
#   2. an edge-preserving vertical fit along each column of the frame
#      raster: minimise |W - W0|^2 + sum lambda_r |W(r+1) - W(r)|_eps over
#      W = (U, V), a vector Charbonnier (smoothed total variation) term,
#      solved by reweighted least squares (one tridiagonal solve per
#      column and iteration). lambda grows with the local luma detail
#      (the 3.5-5 MHz luma the transform left), so oscillating leakage
#      where the luma is busy is flattened while steps where it is quiet
#      are kept. Phase 10: the detail leaves out chroma the transform left
#      in luma at colour edges (luma_detail() with leak and beta), and
#      lambda's floor rises with the square of the blanked lines' noise
#      (kn);
#   3. optionally, re-encoding: the chroma the fit does not explain,
#      c - M(U, V), is demodulated again and the fit repeated from
#      W + that (a proximal-gradient step);
#   4. reassignment: the residual c - M(U, V) is luma the transform
#      mistook for chroma, or chroma noise. It is handed back to luma
#      after soft shrinkage against the chroma-band noise of the blanked
#      lines (a noise-weighted data term), and only where the luma has
#      detail (where leakage comes from), gated on the full 3.5-5 MHz
#      detail (gate_high).

import numpy as np
from scipy.ndimage import uniform_filter

import bench
import proto
import transform

# Raster rows of field lines 7-16, blanked on the bench and on a capture
# without VBI data there.
BLANK_ROWS = (12, 32)
BLANK_SAMPLES = (200, 1100)
CHROMA_BAND = bench.band_filter(3.0e6, 5.9e6)
DETAIL_SIZE = (5, 15)
ENVELOPE_SIZE = (3, 9)

# Phase 10: the luma below the chroma band, which the chroma the transform
# left in luma at colour edges does not reach.
LOW_BAND = bench.band_filter(2.5e6, 3.2e6)

DEFAULTS = dict(lam0=0.01, k=5.0, eps=0.002, irls=6, passes=1,
                bandwidth=proto.PALCOLOUR_BANDWIDTH, reassign=True, tau=1.0,
                d0=0.003, leak=2.5, beta=0.25, gate_high=True, kn=10.0)


def _band_rms(y, taps):
    band = bench.filter_rows(y, taps)
    return np.sqrt(np.maximum(uniform_filter(band ** 2, size=DETAIL_SIZE),
                              0.0))


def low_detail(y):
    """Local r.m.s. of the luma's 2.5-3.2 MHz band."""
    return _band_rms(y, LOW_BAND)


def luma_detail(y, leak=None, c=None, beta=None):
    """Local r.m.s. of the luma's 3.5-5 MHz band, H. With |leak|, only what
    is not chroma the transform left in luma at colour edges: that has no
    2.5-3.2 MHz band beside it and is a fraction of the chroma |c| there,
    so H counts as far as it is |leak| x low_detail() or exceeds |beta| x
    the chroma's local r.m.s."""
    detail = _band_rms(y, bench.LUMA_BAND)
    if leak:
        held = np.minimum(detail, leak * low_detail(y))
        if beta is not None and c is not None:
            chroma = np.sqrt(np.maximum(uniform_filter(c ** 2,
                                                       size=DETAIL_SIZE), 0.0))
            held = np.maximum(held, detail - beta * chroma)
        detail = held
    return detail


def gate_detail(y, leak=None, c=None, beta=None, gate_high=False):
    """The detail the reassignment is gated by: with |gate_high| or no
    |leak|, the 3.5-5 MHz band's; with |beta|, luma_detail()'s; with |leak|
    alone, low_detail()."""
    if gate_high or not leak:
        return luma_detail(y)
    if beta is None:
        return low_detail(y)
    return luma_detail(y, leak, c, beta)


def blank_noise(raster):
    """R.m.s. of the chroma band on the blanked lines."""
    lines = bench.filter_rows(raster[slice(*BLANK_ROWS)], CHROMA_BAND)
    return float(np.sqrt((lines[:, slice(*BLANK_SAMPLES)] ** 2).mean()))


def _tridiagonal(a, b, c, d):
    """Solve every column's tridiagonal system (sub a, diagonal b, super
    c) for right-hand side d, rows on axis 0."""
    n = b.shape[0]
    cp = np.empty_like(b)
    dp = np.empty_like(d)
    cp[0] = c[0] / b[0]
    dp[0] = d[0] / b[0]
    for i in range(1, n):
        m = b[i] - a[i] * cp[i - 1]
        cp[i] = c[i] / m
        dp[i] = (d[i] - a[i] * dp[i - 1]) / m
    x = np.empty_like(d)
    x[-1] = dp[-1]
    for i in range(n - 2, -1, -1):
        x[i] = dp[i] - cp[i] * x[i + 1]
    return x


def vertical_fit(u0, v0, lam, eps, irls):
    """The vector-Charbonnier vertical fit of step 2 over the active rows.
    |lam| is a scalar or a raster plane (each row difference takes the
    mean of its two rows')."""
    lo, hi = proto.FIRST_ACTIVE, proto.END_ACTIVE
    a0, b0 = u0[lo:hi], v0[lo:hi]
    lam = lam[lo:hi] if np.ndim(lam) else np.full(a0.shape, lam)
    lam = 0.5 * (lam[1:] + lam[:-1])
    a, b = a0, b0
    for _ in range(irls):
        g = np.sqrt(eps ** 2 + np.diff(a, axis=0) ** 2 +
                    np.diff(b, axis=0) ** 2)
        w = lam / g
        diag = np.ones_like(a0)
        diag[:-1] += w
        diag[1:] += w
        sub = np.zeros_like(a0)
        sub[1:] = -w
        sup = np.zeros_like(a0)
        sup[:-1] = -w
        a = _tridiagonal(sub, diag, sup, a0)
        b = _tridiagonal(sub, diag, sup, b0)
    u, v = u0.copy(), v0.copy()
    u[lo:hi], v[lo:hi] = a, b
    return u, v


def demodulate_rows(c, phi, s, correction=None,
                    bandwidth=proto.PALCOLOUR_BANDWIDTH):
    """U and V of each row with PalColour's horizontal filter only (at
    |bandwidth|). |correction| (u, v, s) -> (u, v), if given, is applied
    to the result (hanover.py's line-alternating correction)."""
    u, v = proto.filter_planes(*proto.demodulate(c, phi, s),
                               proto.rows_kernel({0: 1.0}, bandwidth))
    if correction is not None:
        u, v = correction(u, v, s)
    return u, v


def chroma(raster, c, phi, s, lam0, k, eps, irls, passes, bandwidth,
           correction=None, prefilter=None, leak=None, beta=None, kn=0.0,
           **_):
    """(U, V, lam): the adaptive estimate of steps 1-3. |prefilter|
    (u, v, s) -> (u, v) runs before the vertical fit (hanover.py's masked
    suppression). |kn| raises lambda's floor by kn x blank_noise()^2."""
    floor = lam0 + (kn * blank_noise(raster) ** 2 if kn else 0.0)
    lam = floor + k * luma_detail(raster - c, leak, c, beta)
    u = v = None
    for _ in range(passes):
        r = c if u is None else c - encode(u, v, phi, s, correction)
        du, dv = demodulate_rows(r, phi, s, correction, bandwidth)
        if u is not None:
            du, dv = u + du, v + dv
        if prefilter is not None:
            du, dv = prefilter(du, dv, s)
        u, v = vertical_fit(du, dv, lam, eps, irls)
    return u, v, lam


def encode(u, v, phi, s, correction=None):
    """The chroma that would carry (U, V): through the inverse of
    |correction|, so the residual is formed against what was sent."""
    if correction is not None:
        u, v = correction.inverse(u, v, s)
    return proto.remodulate(u, v, phi, s)


def reassign(raster, c, u, v, phi, s, tau, d0, correction=None,
             leak=None, beta=None, gate_high=False):
    """The part of the residual c - M(U, V) handed to luma (step 4)."""
    r = c - encode(u, v, phi, s, correction)
    env = np.sqrt(np.maximum(uniform_filter(r ** 2, size=ENVELOPE_SIZE),
                             1e-12))
    g = np.maximum(0.0, 1.0 - tau * blank_noise(raster) / env)
    if d0:
        d = gate_detail(raster - c, leak, c, beta, gate_high)
        g = g * d / (d + d0)
    return g * r


def decode_separated(raster, c, phi, s, **kw):
    """Steps 1-4 on a separated frame. Keyword arguments override
    DEFAULTS; 'correction' and 'prefilter' are passed through."""
    p = dict(DEFAULTS, **kw)
    u, v, _ = chroma(raster, c, phi, s, **p)
    y = raster - c
    if p["reassign"]:
        y = y + reassign(raster, c, u, v, phi, s, p["tau"], p["d0"],
                         p.get("correction"), p["leak"], p["beta"],
                         p["gate_high"])
    return proto.Decoded(y, u, v)


def decode(raw, **kw):
    """The Phase 4 candidate: frame-raster Transform PAL (32 x 32 tiles,
    threshold 0.4), then decode_separated()."""
    raster, c, phi, s = transform.separate(raw, "frame", ytile=32)
    return decode_separated(raster, c, phi, s, **kw)
