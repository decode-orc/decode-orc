#!/usr/bin/env python3
#
# File:        equalise.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 5: the LaserVision channel equaliser, applied
#              to the composite before separation, and its parameter sources
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Phase 2 found one channel effect worth correcting in a current ld-decode
# output: the CAV edge smear, a one-pole tail (videosynth edge_smear)
#   H(z) = (1 - c) + c (1 - a) / (1 - a z^-1),  a = exp(-1 / tau).
# Its inverse is the IIR
#   1 / H(z) = (1 - a z^-1) / ((1 - c a) - (1 - c) a z^-1),
# whose pole (1 - c) a / (1 - c a) lies inside the unit circle for any
# 0 <= c < 1, so it is stable; for c 0.24, tau 6.1 it is 0.81 (a tail of
# 4.7 samples). It runs over the composite in transmission order, sync
# and all, as the smear did.
#
# The parts:
#   unsmear()          the inverse on composite() codes, frame by frame
#   unresponse()       a linear-phase inverse of a replay response
#                      (response_db breakpoints), its boost capped and
#                      held flat beyond |top_mhz|: tried only with the
#                      bench's own response, as an upper bound
#   burst_gain()       a burst-referenced chroma gain: the bench's nominal
#                      burst amplitude over the frame's (median over the
#                      active lines)
#   measure_smear()    the smear of a source, fitted once to its VITS
#                      (vits.smear_fit() on line 19 averaged over the
#                      frames given), with the guards a decoder needs
#   EqualisedCapture   a cvbs.Capture whose lines come through unsmear(),
#                      to measure what the equaliser leaves on real VITS

import numpy as np
from scipy.signal import firwin2, lfilter, lfilter_zi

import bench
import cvbs
import proto
import vits

# The Phase 2 median on Domesday CAV sides (2026 decodes).
NOMINAL = (0.24, 6.1)
# The bench's burst amplitude (excursion units, proto.BURST window): the
# clean bench and Community South DS1 agree to 0.4 %.
NOMINAL_BURST = 0.1702


def inverse_coefficients(fraction, tau_samples):
    """(b, a) of 1 / H for lfilter."""
    a = np.exp(-1.0 / tau_samples)
    return (np.array([1.0, -a]),
            np.array([1.0 - fraction * a, -(1.0 - fraction) * a]))


def unsmear_stream(x, fraction, tau_samples):
    """The inverse smear over one contiguous stream, its state primed to
    the first sample's level."""
    if fraction <= 0.0:
        return x.copy()
    b, a = inverse_coefficients(fraction, tau_samples)
    y, _ = lfilter(b, a, x, zi=lfilter_zi(b, a) * x[0])
    return y


def _lengths():
    return [1137 if line in (312, 624) else bench.LINE for line in range(625)]


def unsmear(raw, fraction, tau_samples):
    """composite() codes (625 x 1137) through the inverse smear, the
    frame's lines joined in transmission order."""
    if fraction <= 0.0:
        return raw.copy()
    lengths = _lengths()
    stream = np.concatenate([raw[i, :n] for i, n in enumerate(lengths)])
    y = unsmear_stream(stream - bench.BLACK, fraction, tau_samples) + \
        bench.BLACK
    out = raw.copy()
    at = 0
    for i, n in enumerate(lengths):
        out[i, :n] = y[at:at + n]
        out[i, n:] = y[at + n - 1]
        at += n
    return out


def unresponse(raw, response_db, top_mhz=5.0, max_boost_db=6.0, taps=129):
    """composite() codes through a linear-phase inverse of |response_db|
    up to |top_mhz| (each line on its own), the boost limited to
    |max_boost_db| and the correction held at its |top_mhz| value
    beyond."""
    f = np.linspace(0.0, bench.RATE / 2e6, 256)
    fm, db = zip(*response_db)
    gain = -np.interp(np.minimum(f, top_mhz), fm, db)
    gain = np.minimum(gain, max_boost_db)
    h = firwin2(taps, f * 1e6, 10 ** (gain / 20.0), fs=bench.RATE)
    from scipy.ndimage import convolve1d  # noqa: PLC0415
    return convolve1d(raw - bench.BLACK, h, axis=1, mode="nearest") + \
        bench.BLACK


def burst_amplitude(raster):
    """Median burst amplitude over the active rows (excursion units)."""
    n = np.arange(*proto.BURST)
    z = 2.0 * (raster[:, proto.BURST[0]:proto.BURST[1]] *
               np.exp(-0.5j * np.pi * n)).mean(axis=1)
    return float(np.median(np.abs(z[proto.FIRST_ACTIVE:proto.END_ACTIVE])))


def burst_gain(raster):
    return NOMINAL_BURST / max(burst_amplitude(raster), 0.1 * NOMINAL_BURST)


def fit_smear(capture, frames):
    """vits.smear_fit() on line 19 averaged over |frames|: (fraction, tau,
    rms, rms with no smear, frames used), or None with no usable bar."""
    try:
        avg = vits.aligned_average(capture, frames, 1, 19, -3.0, 17.0)
    except ZeroDivisionError:  # no frame had a bar edge
        return None
    if not np.all(np.isfinite(avg)):
        return None
    a, tau, res, res0 = vits.smear_fit(avg, -3.0)
    return float(a), float(tau), res, res0


# A fit is a smear only if it is clear of the parameter bounds, long enough
# not to be the generator's own edge shaping (a smear with tau under two
# samples is indistinguishable from it), and explains the tails clearly
# better than no smear at all.
MIN_TAU = 2.0
MAX_FRACTION = 0.45
MIN_GAIN = 0.8


def accept(fit):
    """(fraction, tau) of a fit_smear() result, (0, 1) for 'no smear',
    or None for 'cannot tell'."""
    if fit is None:
        return None
    a, tau, res, res0 = fit
    if a >= MAX_FRACTION or tau < MIN_TAU:
        return (0.0, 1.0) if res >= MIN_GAIN * res0 else None
    if res >= MIN_GAIN * res0:
        return (0.0, 1.0)
    return a, tau


def measure_smear(capture, frames, fallback=None):
    """The source's smear from its VITS over |frames| (one fit on their
    average), or |fallback| when the VITS cannot tell."""
    got = accept(fit_smear(capture, frames))
    return fallback if got is None else got


class EqualisedCapture(cvbs.Capture):
    """A capture whose lines are read through the inverse smear. Each line
    is filtered with |context| samples of the stream before it, enough
    for the inverse's tail to settle."""

    def __init__(self, path, fraction, tau_samples, context=400):
        super().__init__(path)
        self.fraction = fraction
        self.tau = tau_samples
        self.context = context

    def line(self, frame, field, field_line):
        base = self._offset(frame, field, field_line)
        start = max(0, base - self.context)
        raw = np.asarray(self.data[start:base + cvbs.LINE],
                         dtype=np.float64)
        ire = (raw - self.black) * 100.0 / (self.white - self.black)
        return unsmear_stream(ire, self.fraction, self.tau)[base - start:]
