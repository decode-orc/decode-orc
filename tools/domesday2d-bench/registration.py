#!/usr/bin/env python3
#
# File:        registration.py
# Module:      tools/domesday2d-bench
# Purpose:     Inter-field registration of a still frame: field 2's
#              horizontal and vertical offset against field 1
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Domesday2D Phase 2 step 5. On a still frame, field 2's lines sit half a
# field line below field 1's. The luma below 2.5 MHz of field 2 is fitted
# as field 1 shifted right by dx samples and interpolated vertically (cubic,
# Keys a = -0.5) to a position alpha below each of its lines (0.5 nominal).
# dy = 0.5 - alpha field lines is how far field 2's picture sits below
# where it should (videosynth field2_offset.y_lines / 2), and dx how far
# right (field2_offset.x_samples). The picture is used only where it has
# horizontal detail, which sets dx.

import numpy as np
from scipy.signal import filtfilt, firwin

import vits

_LP = firwin(81, 2.5e6, fs=vits.RATE)
ACTIVE = (1.0, 49.0)
FIRST, LAST = 24, 308  # field lines


def field_luma(capture, frame, field, edge):
    a, b = vits._span(edge, ACTIVE)
    lines = capture.lines(frame, field, FIRST, LAST)
    return filtfilt(_LP, 1.0, lines, axis=1)[:, a:b]


def _shift(rows, dx):
    k = np.fft.rfftfreq(rows.shape[1])
    return np.fft.irfft(np.fft.rfft(rows, axis=1) *
                        np.exp(-2j * np.pi * k * dx), rows.shape[1], axis=1)


def _keys(a):
    """Keys cubic weights (a = -0.5) for lines k-1, k, k+1, k+2 at k+a."""
    return np.array([((-0.5 * a + 1.0) * a - 0.5) * a,
                     (1.5 * a - 2.5) * a * a + 1.0,
                     ((-1.5 * a + 2.0) * a + 0.5) * a,
                     (0.5 * a - 0.5) * a * a])


def register(f1, f2, search=2.0, step=0.05, margin=24):
    """(dx samples, dy field lines, rms residual IRE, rms at the nominal
    registration) for one frame's two fields."""
    alphas = np.arange(0.2, 0.801, 0.01)

    def fit(dx):
        s = _shift(f1, dx)[:, margin:-margin]
        target = f2[1:-2, margin:-margin]
        best = None
        for alpha in alphas:
            w = _keys(alpha)
            est = (w[0] * s[:-3] + w[1] * s[1:-2] + w[2] * s[2:-1] +
                   w[3] * s[3:])
            r = float(np.sqrt(np.mean((target - est) ** 2)))
            if best is None or r < best[0]:
                best = (r, alpha)
        return best

    # Coarse, then fine around the best coarse shift.
    coarse = np.arange(-search, search + 0.125, 0.25)
    centre = coarse[int(np.argmin([fit(dx)[0] for dx in coarse]))]
    shifts = centre + np.arange(-0.25, 0.25 + step / 2, step)
    scores = [fit(dx)[0] for dx in shifts]
    i = int(np.argmin(scores))
    dx = shifts[i]
    if 0 < i < len(shifts) - 1:
        a, b, c = scores[i - 1], scores[i], scores[i + 1]
        dx += step * 0.5 * (a - c) / (a - 2 * b + c)
    rms, alpha = fit(dx)
    w = _keys(0.5)
    s = f1[:, margin:-margin]
    nominal = (w[0] * s[:-3] + w[1] * s[1:-2] + w[2] * s[2:-1] +
               w[3] * s[3:])
    rms0 = float(np.sqrt(np.mean((f2[1:-2, margin:-margin] - nominal) ** 2)))
    return dx, 0.5 - alpha, rms, rms0


def detail(f):
    """Horizontal luma detail of a field (rms of the first difference)."""
    return float(np.sqrt(np.mean(np.diff(f, axis=1) ** 2)))
