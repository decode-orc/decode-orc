#!/usr/bin/env python3
#
# File:        vits.py
# Module:      tools/domesday2d-bench
# Purpose:     Measure a PAL LaserVision capture's chain from its VITS
#              (CCIR 17, 18, 330 and 331 on lines 19 and 20 of each field)
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Domesday2D Phase 2, steps 1 to 4 (docs-tech/domesday2d-decoder-
# investigation.md). The insertion test signals, as found on BBC Domesday
# pressings:
#
#   field 1 line 19  CCIR 17:  white bar, 2T pulse, 20T pulse, staircase
#   field 1 line 20  CCIR 18:  white flag, multiburst 0.5/1/2/4/4.8/5.8 MHz
#   field 2 line 19  CCIR 330: bar, 2T pulse, staircase with subcarrier
#   field 2 line 20  CCIR 331: chroma bars at 20/60/100 % on a pedestal
#
# Every element is located relative to the 50 % point of the white bar's
# rising edge on field 1 line 19, so a .tbc (whose line origin is not 0H)
# and a .cvbs are measured alike. Chroma is demodulated on the 4fsc grid
# (exactly 4 samples a cycle) and every phase is taken against the burst
# of its own line, so the carrier's phase at the line start never matters.

import numpy as np
from scipy.optimize import least_squares
from scipy.signal import filtfilt, firwin

from cvbs import FSC, RATE

US = RATE / 1e6  # samples per microsecond

# Element windows in microseconds from the bar edge, measured on a
# Community South capture (bar edge 11.86 us after 0H).
BURST = (-5.8, -4.3)
BAR_PLATEAU = (2.0, 9.0)
BAR_FALL = 10.1
TWO_T = (14.0, 1.0)            # centre, half-width
TWENTY_T_SEARCH = (16.0, 24.0)
TWENTY_T_HALF = 2.8
MULTIBURST = [(0.5, 14.1, 1.5), (1.0, 20.3, 1.5), (2.0, 26.3, 1.5),
              (4.0, 32.3, 1.5), (4.8, 38.3, 1.5), (5.8, 45.2, 2.5)]
FLAG_HIGH = (1.0, 3.3)
FLAG_LOW = (5.0, 7.5)
STAIRCASE_330 = [(0, (19.5, 27.5)), (20, (29.3, 31.8)), (40, (33.3, 35.8)),
                 (60, (37.3, 39.8)), (80, (41.3, 43.8)), (100, (45.3, 47.8))]
BARS_331 = [(20, (2.8, 5.5)), (60, (6.8, 9.5)), (100, (11.0, 15.3)),
            ("long", (23.5, 47.5))]

# The IEC 60856 9.1.6 nominal pre-distortion relative to 0.5 MHz (videosynth
# group_delay preset iec60856_nominal): MHz, ns. Negative arrives early.
IEC_GD = [(0.5, 0.0), (2.0, -10.0), (3.0, -35.0), (4.0, -85.0),
          (4.43, -135.0), (4.8, -200.0)]

_LP2 = firwin(81, 2.0e6, fs=RATE)
_LP1 = firwin(81, 1.0e6, fs=RATE)


def _span(edge, window):
    return int(round(edge + window[0] * US)), int(round(edge + window[1] * US))


def lowpass(x, taps=_LP2):
    return filtfilt(taps, 1.0, x)


def demod(x, taps=_LP1):
    """Complex chroma envelope at fsc (zero-phase), peak amplitude units."""
    n = np.arange(len(x))
    z = x * np.exp(-0.5j * np.pi * n) * 2.0
    return filtfilt(taps, 1.0, z.real) + 1j * filtfilt(taps, 1.0, z.imag)


def bar_edge(line):
    """Sub-sample position of the white bar's 50 % rising edge, or None."""
    smooth = lowpass(line)
    width = int(US)
    for n in range(width, len(line) - width):
        if smooth[n - 1] < 50.0 <= smooth[n]:
            if smooth[n - width:n - width // 2].max() < 15.0 and \
                    smooth[n + width // 2:n + width].min() > 85.0:
                a, b = smooth[n - 1], smooth[n]
                return n - 1 + (50.0 - a) / (b - a)
    return None


def burst(line, edge):
    a, b = _span(edge, BURST)
    return demod(line)[a:b].mean()


def _pulse(t, amplitude, centre, base, had):
    """A sin^2 pulse of half-amplitude duration |had| (us), t in us."""
    x = np.clip((t - centre) / (2.0 * had), -0.5, 0.5)
    return base + amplitude * np.cos(np.pi * x) ** 2


def twenty_t(line, edge):
    """20T pulse: chroma/luma gain ratio and chroma-minus-luma delay (ns).

    The luma part and the chroma envelope are each fitted with the 2 us
    sin^2 shape after the same zero-phase low-pass, so the filter neither
    biases the delay nor the ratio."""
    # Found by its envelope: the pulse sits 20.3 us after the bar edge on
    # Domesday pressings and 17.8 us in videosynth's uk-national.
    s0, s1 = _span(edge, TWENTY_T_SEARCH)
    peak = s0 + int(np.argmax(np.abs(demod(line, _LP2)[s0:s1])))
    half = TWENTY_T_HALF * US
    a, b = int(round(peak - half)), int(round(peak + half))
    t = (np.arange(a, b) - edge) / US
    luma = lowpass(line)[a:b]
    envelope = np.abs(demod(line, _LP2)[a:b])

    def fit(y):
        p0 = [y.max() - np.median(y[:10]), t[np.argmax(y)], np.median(y[:10])]
        r = least_squares(lambda p: _pulse(t, p[0], p[1], p[2], 2.0) - y, p0)
        return r.x

    lum = fit(luma)
    chr_ = fit(envelope)
    return {"gain": chr_[0] / lum[0],
            "delay_ns": (chr_[1] - lum[1]) * 1000.0,
            "luma_ire": lum[0]}


def multiburst(line, edge):
    """Each packet's amplitude against the flag (60 IRE p-p), in dB."""
    hi = line[slice(*_span(edge, FLAG_HIGH))].mean()
    lo = line[slice(*_span(edge, FLAG_LOW))].mean()
    flag = (hi - lo) / 2.0
    out = {}
    for mhz, centre, half in MULTIBURST:
        a, b = _span(edge, (centre - half, centre + half))
        t = np.arange(a, b) / RATE
        y = line[a:b]
        basis = np.stack([np.sin(2 * np.pi * mhz * 1e6 * t),
                          np.cos(2 * np.pi * mhz * 1e6 * t),
                          np.ones_like(t)], 1)
        # The packet's exact frequency is the generator's; search near it.
        best = None
        for f in np.linspace(0.97, 1.03, 25) * mhz * 1e6:
            basis[:, 0] = np.sin(2 * np.pi * f * t)
            basis[:, 1] = np.cos(2 * np.pi * f * t)
            k, res, *_ = np.linalg.lstsq(basis, y, rcond=None)
            r = float(np.sum((basis @ k - y) ** 2))
            if best is None or r < best[0]:
                best = (r, np.hypot(k[0], k[1]))
        out[mhz] = 20.0 * np.log10(best[1] / flag)
    return out


def chroma_steps(line, edge, steps):
    """Each step's chroma amplitude (IRE) and phase against the burst."""
    z = demod(line)
    ref = burst(line, edge)
    out = {}
    for level, window in steps:
        c = z[slice(*_span(edge, window))].mean()
        out[level] = (abs(c), float(np.degrees(np.angle(c / ref))))
    return out, abs(ref)


def burst_phases(lines, edge):
    """Burst phase (deg) of each line of one field, demodulated on the
    line's own sample grid."""
    a, b = _span(edge, BURST)
    return np.array([np.degrees(np.angle(demod(x)[a:b].mean()))
                     for x in lines])


def burst_jitter(phases):
    """Line-to-line burst phase residual of one field (deg r.m.s.).

    On a 1135-sample grid the carrier advances 270 deg a line plus the
    0.576 deg of the 283.7516-cycle line; the burst alternates +/-45 deg
    with the V-switch. A line, a constant and the alternation are fitted
    (either starting sign), and the residual is the jitter."""
    n = np.arange(len(phases))
    p = np.unwrap(np.radians(phases - 270.0 * n))
    alt = np.where(n % 2 == 0, 1.0, -1.0)
    basis = np.stack([np.ones_like(p), n.astype(float), alt], 1)
    k, *_ = np.linalg.lstsq(basis, p, rcond=None)
    residual = p - basis @ k
    return (float(np.degrees(np.sqrt(np.mean(residual ** 2)))),
            float(np.degrees(abs(k[2]))))


# --- Channel models for the edge fits (Phase 2 step 2) ----------------------

def gd_allpass(n, curve, scale):
    """Frequency response of an all-pass whose group delay follows |curve|
    (MHz, ns) times |scale|, on an n-point FFT grid at 4fsc."""
    f = np.fft.rfftfreq(n, 1.0 / RATE)
    fm, tn = zip(*curve)
    tau = np.interp(f / 1e6, fm, tn) * 1e-9 * scale
    phase = -2.0 * np.pi * np.cumsum(tau) * (f[1] - f[0])
    return np.exp(1j * phase)


def one_pole(n, fraction, tau_samples):
    """videosynth edge_smear: (1 - a) x + a * onepole(x)."""
    f = np.fft.rfftfreq(n, 1.0 / RATE)
    w = 2 * np.pi * f / RATE
    pole = np.exp(-1.0 / max(tau_samples, 1e-6))
    h = (1 - pole) / (1 - pole * np.exp(-1j * w))
    return (1 - fraction) + fraction * h


# --- The chain's transfer function from the 2T pulse (Phase 2 steps 1, 2) ---

def aligned_average(capture, frames, field, field_line, start_us, stop_us):
    """|field_line| of |field| averaged over |frames|, each shifted (by a
    band-limited fractional delay) so its bar edge lands on the same
    sample. Returns the segment from start_us to stop_us after the edge.
    Only for chroma-free parts of the line: the subcarrier's phase turns
    from frame to frame."""
    n = int(round((stop_us - start_us) * US))
    acc = np.zeros(n)
    used = 0
    for frame in frames:
        edge = bar_edge(capture.line(frame, 1, 19))
        if edge is None:  # a dropout on the bar
            continue
        used += 1
        line = capture.line(frame, field, field_line)
        start = edge + start_us * US
        whole = int(np.floor(start))
        frac = start - whole
        pad = 64
        seg = line[whole - pad:whole + n + pad]
        k = np.fft.rfftfreq(len(seg))
        seg = np.fft.irfft(np.fft.rfft(seg) * np.exp(2j * np.pi * k * frac),
                           len(seg))
        acc += seg[pad:pad + n]
    return acc / used


def ideal_pulse(n, centre, had_ns):
    t = (np.arange(n) - centre) / RATE * 1e9
    x = np.clip(t / (2.0 * had_ns), -0.5, 0.5)
    return np.cos(np.pi * x) ** 2


def transfer(y, x, nfft=4096):
    """H = Y / X on an nfft grid, baselines removed from both."""
    y = y - np.median(np.concatenate([y[:8], y[-8:]]))
    x = x - np.median(np.concatenate([x[:8], x[-8:]]))
    f = np.fft.rfftfreq(nfft, 1.0 / RATE)
    return f, np.fft.rfft(y, nfft) / np.fft.rfft(x, nfft), np.abs(
        np.fft.rfft(x, nfft))


def phase_models():
    """Each Phase 2 step 2 model: name, parameter start, bounds, and its
    phase response on a frequency grid (Hz)."""
    def gd(f, p):
        n = 2 * (len(f) - 1)
        return np.angle(gd_allpass(n, IEC_GD, p[0]))

    def smear(f, p):
        n = 2 * (len(f) - 1)
        return np.angle(one_pole(n, p[0], p[1]))

    def both(f, p):
        return gd(f, p[:1]) + smear(f, p[1:])

    return {"group_delay": (gd, [0.5], ([-2.0], [2.0])),
            "smear": (smear, [0.1, 5.0], ([0.0, 0.1], [0.5, 100.0])),
            "both": (both, [0.5, 0.1, 5.0], ([-2.0, 0.0, 0.1],
                                             [2.0, 0.5, 100.0]))}


def fit_phase(f, h, weight, band=(0.3e6, 4.6e6)):
    """Fit each model's phase, plus a pure delay, to the measured phase of
    |h| over |band|. Returns {model: (params, delay_ns, rms_deg)}."""
    sel = (f >= band[0]) & (f <= band[1])
    measured = np.unwrap(np.angle(h))
    w = weight[sel] / weight[sel].max()
    out = {}
    for name, (model, p0, bounds) in phase_models().items():
        def residual(p):
            # p[-1] is the delay in ns
            ph = model(f, p[:-1]) - 2 * np.pi * f * p[-1] * 1e-9
            d = np.angle(np.exp(1j * (measured - ph)))
            return (d[sel] * w)
        lo = list(bounds[0]) + [-2000.0]
        hi = list(bounds[1]) + [2000.0]
        best = None
        for d0 in np.linspace(-500, 500, 11):
            r = least_squares(residual, list(p0) + [d0], bounds=(lo, hi))
            if best is None or r.cost < best.cost:
                best = r
        rms = np.degrees(np.sqrt(np.mean(
            (best.fun / np.maximum(w, 1e-9)) ** 2 * w ** 2) / np.mean(w ** 2)))
        out[name] = (best.x[:-1], best.x[-1], rms)
    return out


def group_delay_ns(f, h, at_mhz, ref_mhz=0.5, step_mhz=0.25):
    """Group delay of |h| at each of |at_mhz| relative to |ref_mhz|."""
    ph = np.unwrap(np.angle(h))

    def gd(mhz):
        a = np.interp((mhz - step_mhz / 2) * 1e6, f, ph)
        b = np.interp((mhz + step_mhz / 2) * 1e6, f, ph)
        return -(b - a) / (2 * np.pi * step_mhz * 1e6) * 1e9

    ref = gd(ref_mhz)
    return {m: gd(m) - ref for m in at_mhz}


# --- Edge tails and the smear (Phase 2 step 2) ------------------------------

def _onepole_time(x, tau_samples):
    pole = np.exp(-1.0 / tau_samples)
    y = np.empty_like(x)
    acc = x[0]
    for i, v in enumerate(x):
        acc = pole * acc + (1 - pole) * v
        y[i] = acc
    return y


def smear_fit(avg, start_us):
    """Fit videosynth's edge_smear (fraction, tau) to the long tails of
    field 1 line 19 (white bar, then the 2T pulse), averaged.

    The ideal is the bar as a rectangle and the 2T pulse as an impulse at
    their measured times: only samples 0.4 us or more from either edge or
    the pulse are fitted, so the generator's own edge shaping (100-200 ns)
    and any group delay confined to the chroma band drop out, and a tail
    lasting microseconds can only be a smear. Returns (fraction, tau
    samples, rms residual IRE, rms residual with no smear)."""
    t = start_us + np.arange(len(avg)) / US
    smooth = np.convolve(avg, np.ones(5) / 5, mode="same")
    rise = 0.0
    fall = t[np.argmax((t > 5) & (smooth < 50))]
    pulse = t[np.argmax(np.where((t > fall + 2) & (t < fall + 6), smooth,
                                 -1e9))]
    guard = 0.4
    fit = ((t > -2.5) & (t < rise - 0.2)) | \
          ((t > rise + guard) & (t < fall - 0.2)) | \
          ((t > fall + guard) & (t < pulse - 0.3)) | \
          ((t > pulse + guard) & (t < pulse + 3.0))

    def model(p):
        a, tau, height, area, base = p
        x = np.where((t >= rise) & (t < fall), height, 0.0)
        x[np.argmin(np.abs(t - pulse))] += area * US
        x = x + base
        return (1 - a) * x + a * _onepole_time(x, tau)

    p0 = [0.1, 8.0, 100.0, 0.2, 0.0]
    r = least_squares(lambda p: (model(p) - avg)[fit], p0,
                      bounds=([0.0, 0.5, 50, 0.0, -10], [0.5, 100, 150, 2.0, 10]))
    none = least_squares(
        lambda p: (model([0.0, 1.0, p[0], 0.0, p[1]]) - avg)[fit],
        [100.0, 0.0])
    return (r.x[0], r.x[1], float(np.sqrt(np.mean(r.fun ** 2))),
            float(np.sqrt(np.mean(none.fun ** 2))))


def synthetic_20t(fraction=0.0, tau_samples=1.0, gd_scale=0.0, n=1135):
    """An ideal 20T pulse (50 IRE luma + 50 IRE chroma envelope, HAD 2 us)
    with a bar edge before it, through a smear and an IEC group delay, laid
    out as on a Domesday line 19. For separating the smear's part of a
    measured 20T delay from the group delay's."""
    edge = 210.0
    t = (np.arange(n) - edge) / US
    x = np.where((t >= 0) & (t < 10.1), 100.0, 0.0)
    centre = 20.3
    env = 50.0 * np.cos(np.pi * np.clip((t - centre) / 4.0, -0.5, 0.5)) ** 2
    x = x + env + env * np.cos(0.5 * np.pi * np.arange(n) + 0.3)
    if gd_scale:
        x = np.fft.irfft(np.fft.rfft(x) * gd_allpass(n, IEC_GD, gd_scale), n)
    if fraction:
        x = (1 - fraction) * x + fraction * _onepole_time(x, tau_samples)
    return x, edge


def separate_20t(measured_delay_ns, fraction, tau_samples):
    """The IEC group-delay scale that, with the fitted smear, reproduces a
    measured 20T delay; and the smear's own share of that delay."""
    def delay(scale):
        x, edge = synthetic_20t(fraction, tau_samples, scale)
        return twenty_t(x, edge)["delay_ns"]

    smear_only = delay(0.0)
    per_unit = delay(1.0) - smear_only
    return (measured_delay_ns - smear_only) / per_unit, smear_only


# --- Chroma sideband symmetry (Phase 2 step 3) ------------------------------

SIDEBAND_OFFSETS_MHZ = (0.2, 0.4, 0.6, 0.8)


def sideband_spectrum(line, edge, nfft=8192):
    """|spectrum| of the 20T pulse, whose chroma is a 2 us sin^2 envelope
    and so symmetric about fsc as transmitted. Magnitudes only, so frames
    may be averaged whatever their subcarrier phase."""
    s0, s1 = _span(edge, TWENTY_T_SEARCH)
    peak = s0 + int(np.argmax(np.abs(demod(line, _LP2)[s0:s1])))
    half = int(3.0 * US)
    seg = line[peak - half:peak + half]
    seg = seg - lowpass(line)[peak - half:peak + half]  # drop the luma half
    f = np.fft.rfftfreq(nfft, 1.0 / RATE)
    return f, np.abs(np.fft.rfft(seg, nfft))


def sideband_ratio_db(f, mag, offsets=SIDEBAND_OFFSETS_MHZ):
    """Upper over lower sideband at each offset from fsc, in dB."""
    return {d: 20.0 * np.log10(np.interp(FSC + d * 1e6, f, mag) /
                               np.interp(FSC - d * 1e6, f, mag))
            for d in offsets}
