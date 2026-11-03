#!/usr/bin/env python3
#
# File:        proto.py
# Module:      tools/domesday2d-bench
# Purpose:     Prototype PAL separation on the woven still frame: the
#              carrier lattice, per-row demodulation, and the 2D separation
#              kernels Phase 3 compares
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Domesday2D Phase 3. Every prototype works on the frame raster of
# bench.frame_raster(): 624 rows (field 1 line k on row 2(k-1), field 2
# line k on row 2(k-1)+1) of 1135 samples from 0H, in excursion units.
#
# The carrier. On the 4fsc grid a row's chroma is
#   C = U sin(theta) + s V cos(theta),  theta = n pi/2 + phi
# with n the sample after 0H, phi the row's U carrier phase and s its
# V-switch. The grid is locked to the subcarrier, so phi falls 90 deg a
# line and a field-2 line gains 180 deg from the 1137-sample line 312
# before it; s alternates line by line. phase3_lattice.py measures this.
# Here phi and s of each field are fitted to its bursts once (burst phase
# phi + 45 deg on an s = +1 line, phi + 135 deg on s = -1); a per-line
# burst reference is a Phase 2 no-go (3.4 deg of jitter).
#
# Demodulated on its own carrier, every row gives U = -Im(z) and
# V = s Re(z), z = 2 x exp(-j theta): the chroma is coherent at 0 Hz on
# every row of both fields, and the luma lands at -fsc with the lattice's
# vertical phase pattern. A separation kernel filters those planes; the
# chroma it keeps is remodulated and subtracted from the composite for the
# luma.

import numpy as np
from scipy.signal import fftconvolve

import bench

ROWS = 624
FIRST_ACTIVE = bench.TRUTH_ROW0  # row 44: field line 23
END_ACTIVE = bench.TRUTH_ROW0 + bench.TRUTH_ROWS  # row 620
BURST = (98, 138)
N = np.arange(bench.LINE)


def lattice(raster):
    """(phi deg, s) of every raster row, fitted per field to its bursts
    over the active lines."""
    phi = np.zeros(ROWS)
    s = np.zeros(ROWS)
    n = np.arange(*BURST)
    ref = np.exp(-0.5j * np.pi * n)
    for field in (0, 1):
        rows = np.arange(field, ROWS, 2)
        line = rows // 2  # field line - 1
        z = 2.0 * (raster[rows, BURST[0]:BURST[1]] * ref).mean(axis=1)
        active = (rows >= FIRST_ACTIVE) & (rows < END_ACTIVE)
        best = None
        for s0 in (1.0, -1.0):
            sw = s0 * np.where(line % 2 == 0, 1.0, -1.0)
            offset = np.where(sw > 0, 45.0, 135.0)
            model = -90.0 * line + offset
            resid = np.exp(1j * (np.angle(z) - np.radians(model)))[active]
            m = resid.mean()
            if best is None or abs(m) > best[0]:
                best = (abs(m), np.degrees(np.angle(m)), sw)
        phi[rows] = best[1] - 90.0 * line
        s[rows] = best[2]
    return phi, s


def carrier(phi):
    """theta (rad) of every raster sample."""
    return N[None, :] * (np.pi / 2) + np.radians(phi)[:, None]


def demodulate(raster, phi, s):
    """The U and V planes: each row demodulated on its own carrier, rows
    outside the active picture zeroed."""
    theta = carrier(phi)
    z = 2.0 * raster * np.exp(-1j * theta)
    u = -z.imag
    v = s[:, None] * z.real
    u[:FIRST_ACTIVE] = 0.0
    v[:FIRST_ACTIVE] = 0.0
    u[END_ACTIVE:] = 0.0
    v[END_ACTIVE:] = 0.0
    return u, v


def remodulate(u, v, phi, s):
    theta = carrier(phi)
    return u * np.sin(theta) + s[:, None] * v * np.cos(theta)


# ── Kernels ───────────────────────────────────────────────────────────────

# PalColour's chroma bandwidth (palcolour.cpp): 1.1 MHz / 0.93, and its
# raised-cosine radius ca = 0.5 fs / bandwidth samples. One field line is
# two of its vertical units, so the unit is one frame row.
PALCOLOUR_BANDWIDTH = 1.1e6 / 0.93


def radial_kernel(geometry, bandwidth=PALCOLOUR_BANDWIDTH, row_scale=1.0):
    """PalColour's radial raised cosine, 1 + cos(pi r / ca) for r < ca, in
    (frame row, sample) units, sampled on |geometry|'s rows: 'field' keeps
    the even row offsets (the same field), 'frame' every row. |row_scale|
    stretches the vertical unit. Normalised to unit DC gain."""
    ca = 0.5 * bench.RATE / bandwidth
    reach = int(np.floor(ca))
    dr = np.arange(-reach, reach + 1)[:, None] * row_scale
    dx = np.arange(-reach, reach + 1)[None, :]
    r = np.minimum(ca, np.hypot(dr, dx))
    k = 1.0 + np.cos(np.pi * r / ca)
    if geometry == "field":
        k[(np.arange(-reach, reach + 1) % 2) == 1, :] = 0.0
    return k / k.sum()


def rows_kernel(weights, bandwidth=PALCOLOUR_BANDWIDTH):
    """A separable kernel: PalColour's horizontal raised cosine on each of
    the rows |weights| ({row offset: weight}) names. Unit DC gain."""
    ca = 0.5 * bench.RATE / bandwidth
    reach = int(np.floor(ca))
    dx = np.arange(-reach, reach + 1)
    h = 1.0 + np.cos(np.pi * np.minimum(ca, np.abs(dx)) / ca)
    k = np.zeros((2 * reach + 1, dx.size))
    for dr, w in weights.items():
        k[dr + reach] = w * h
    return k / k.sum()


def luma_kernels(geometry, bandwidth=PALCOLOUR_BANDWIDTH, side=0.2,
                 partner=None):
    """PalColour's luma-path kernels (the chroma it subtracts for luma):
    the radial raised cosine on row offset 0 and on the rows whose carrier
    is the row's own inverted (same field +/-2 lines: frame rows +/-4),
    those weighted by |side|. In 'frame' geometry the 312-line partner is
    added with weight |partner| (default |side|): the row above on a field
    1 row, below on a field 2 row. Returns the kernels for even (field 1)
    and odd (field 2) rows."""
    ca = 0.5 * bench.RATE / bandwidth
    reach = int(np.floor(ca))
    rows = np.arange(-reach, reach + 1)
    dx = np.arange(-reach, reach + 1)[None, :]
    out = []
    for parity in (0, 1):
        k = np.zeros((rows.size, dx.size))
        weights = {0: 1.0, 4: side, -4: side}
        if geometry == "frame":
            weights[-1 if parity == 0 else 1] = side if partner is None \
                else partner
        for dr, w in weights.items():
            r = np.minimum(ca, np.hypot(dr, dx[0]))
            k[dr + reach] = w * (1.0 + np.cos(np.pi * r / ca))
        out.append(k / k.sum())
    return out


def filter_planes(u, v, kernel):
    """|u| and |v| filtered by |kernel|, whose element [i, j] weights the
    sample i - reach rows and j - reach samples away (correlation, so a
    one-sided kernel reaches the side it is written on)."""
    k = kernel[::-1, ::-1]
    return (fftconvolve(u, k, mode="same"), fftconvolve(v, k, mode="same"))


def filter_by_parity(u, v, kernels):
    """Planes filtered with kernels[0] on even rows, kernels[1] on odd."""
    ue, ve = filter_planes(u, v, kernels[0])
    uo, vo = filter_planes(u, v, kernels[1])
    ue[1::2] = uo[1::2]
    ve[1::2] = vo[1::2]
    return ue, ve


class Decoded:
    """A prototype's output on the frame raster, cut to the truth grid on
    request."""

    def __init__(self, y, u, v):
        self.y, self.u, self.v = y, u, v

    def truth_grid(self):
        return bench.from_raster(self.y, self.u, self.v)


def decode_kernel(raw, kernel, luma=None):
    """Separate with one kernel on the demodulated planes. Luma is the
    composite less the remodulated chroma: of |kernel|'s, or of the
    per-parity |luma| kernels' (luma_kernels())."""
    raster = bench.frame_raster(raw)
    phi, s = lattice(raster)
    u0, v0 = demodulate(raster, phi, s)
    u, v = filter_planes(u0, v0, kernel)
    if luma is None:
        y = raster - remodulate(u, v, phi, s)
    else:
        yu, yv = filter_by_parity(u0, v0, luma)
        y = raster - remodulate(yu, yv, phi, s)
    return Decoded(y, u, v)
