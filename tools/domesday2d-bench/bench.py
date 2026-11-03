#!/usr/bin/env python3
#
# File:        bench.py
# Module:      tools/domesday2d-bench
# Purpose:     The Phase 1 still bench in Python: videosynth's PAL stills,
#              their picture truth, decode-orc's decodes of them, and the
#              still metrics of orc-tests/videosynth/support/still_metrics
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Domesday2D Phase 3. The prototypes decode the stills of
# orc-tests/videosynth/projects/still_truth/pal_stills.yaml as rendered by
# the videosynth suite (build/videosynth-output/pal_stills{,_clean}/) and
# are scored here exactly as DecoderStillTruth scores decode-orc's
# decoders. measure() ports measure_still(); masks() the test's masks
# (picture, line-sign regions on the stress card, clipped composite).
#
# Units are those of ComponentPicture: Y 0 at black and 1 at white, U and V
# the peak subcarrier amplitude on the same scale, the V-switch removed.
#
# The truth grid is 922 samples by 576 rows: column x is sample 177 + x
# after 0H, and row r is field 1 line 23 + r/2 (r even) or field 2 line
# 23 + (r-1)/2 (r odd). The frame raster (frame_raster()) interleaves all
# 312 lines of each field the same way, so truth row r is raster row
# 44 + r.

import os

import numpy as np
from scipy.ndimage import convolve1d, maximum_filter

FSC = 4433618.75
RATE = 4 * FSC
LINE = 1135
CVBS_FRAME = 709379
BLACK = 256.0
WHITE = 844.0
SYNC_TIP = 4
PEAK = 1019

TRUTH_WIDTH = 922
TRUTH_ROWS = 576
TRUTH_FIRST_SAMPLE = 177
TRUTH_ROW0 = 44  # frame raster row of truth row 0
DECODE_FIRST_SAMPLE = 157  # kPalActiveVideoStart
DECODE_WIDTH = 948
FRAMES = 14
HALF_LINE_ROW = 2 * (623 - 336) + 1  # field 2 line 310 (frame line 623)
HALF_LINE_END = 514 - TRUTH_FIRST_SAMPLE

STILLS = [(1, "stress"), (2, "luma_zone_plate"), (3, "chroma_zone_plate"),
          (4, "test_card_f"), (5, "bars"), (6, "tartan"), (7, "osm_town"),
          (8, "osm_rural"), (9, "text_card"), (10, "green_magenta_edges"),
          (11, "photo_butterfly"), (12, "photo_galtstroms")]
STRESS = 1
BARS = 5

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUTPUT = os.path.join(ROOT, "build", "videosynth-output")
CASES = {"clean": ("pal_stills_clean", "videosynth_pal_stills_clean"),
         "noisy": ("pal_stills", "videosynth_pal_stills")}

# The colour-difference weights the video sink decodes with.
KB = 0.49211104112248356308804691718185
KR = 0.87728321993817866838972487283129


# ── The bench's files ─────────────────────────────────────────────────────


def _base(case):
    directory, stem = CASES[case]
    return os.path.join(OUTPUT, directory, stem)


# The Phase 2 replay response (inner radius, smear removed; videosynth#14),
# extended to the band edge, for the 'channel' case: the clean composite
# through it.
CHANNEL_RESPONSE_DB = [(0.0, 0.0), (0.5, 0.6), (1.0, 1.0), (2.0, 1.4),
                       (4.0, 2.1), (4.8, 1.0), (5.8, -9.8), (6.5, -20.0),
                       (RATE / 2e6, -40.0)]


def _channel_fir():
    from scipy.signal import firwin2  # noqa: PLC0415
    f, db = zip(*CHANNEL_RESPONSE_DB)
    return firwin2(129, np.array(f) * 1e6, 10 ** (np.array(db) / 20.0),
                   fs=RATE)


def composite(case, frame):
    """Frame |frame| of |case| as 625 lines of 1137 samples, raw 10-bit
    codes, in transmission order (field 1's 313 lines first). Lines of 1135
    samples are padded with their last sample. Case 'channel' is the clean
    composite through CHANNEL_RESPONSE_DB."""
    if case == "channel":
        raw = composite("clean", frame) - BLACK
        return convolve1d(raw, _channel_fir(), axis=1, mode="nearest") + \
            BLACK
    data = np.memmap(_base(case) + ".cvbs", dtype="<u2", mode="r")
    raw = np.asarray(data[frame * CVBS_FRAME:(frame + 1) * CVBS_FRAME],
                     dtype=np.float64)
    out = np.empty((625, 1137))
    at = 0
    for line in range(625):
        n = 1137 if line in (312, 624) else LINE
        out[line, :n] = raw[at:at + n]
        out[line, n:] = raw[at + n - 1]
        at += n
    return out


def field_lines(raw, field):
    """Field |field|'s lines 1..312 of composite(), in excursion units
    (black 0, white 1)."""
    first = 0 if field == 1 else 313
    return (raw[first:first + 312, :LINE] - BLACK) / (WHITE - BLACK)


def frame_raster(raw):
    """The woven frame: field 1 line k on row 2(k-1), field 2 line k on row
    2(k-1)+1, 624 rows of 1135 samples in excursion units."""
    out = np.empty((624, LINE))
    out[0::2] = field_lines(raw, 1)
    out[1::2] = field_lines(raw, 2)
    return out


def truth(case, frame):
    """(Y, U, V), each 576 x 922, of frame |frame|."""
    plane = TRUTH_WIDTH * TRUTH_ROWS
    data = np.memmap(_base(case) + ".truth", dtype="<f4", mode="r")
    block = np.asarray(data[frame * 3 * plane:(frame + 1) * 3 * plane],
                       dtype=np.float64).reshape(3, TRUTH_ROWS, TRUTH_WIDTH)
    return block[0] / 700.0, block[1] / 700.0, block[2] / 700.0


def decoded(path, frame):
    """Frame |frame| of a video sink YUV444P16 decode (output_padding 1)
    on the truth grid, as (Y, U, V)."""
    plane = DECODE_WIDTH * TRUTH_ROWS
    data = np.memmap(path, dtype="<u2", mode="r")
    block = np.asarray(data[frame * 3 * plane:(frame + 1) * 3 * plane],
                       dtype=np.float64).reshape(3, TRUTH_ROWS, DECODE_WIDTH)
    a = TRUTH_FIRST_SAMPLE - DECODE_FIRST_SAMPLE
    block = block[:, :, a:a + TRUTH_WIDTH]
    y = (block[0] - 16.0 * 256.0) / (219.0 * 256.0)
    u = (block[1] - 128.0 * 256.0) * ((1.0 - 0.114) * KB / (112.0 * 256.0))
    v = (block[2] - 128.0 * 256.0) * ((1.0 - 0.299) * KR / (112.0 * 256.0))
    return y, u, v


def from_raster(y, u, v):
    """Planes on the frame raster (624 rows, 1135 samples from 0H) cut to
    the truth grid."""
    rows = slice(TRUTH_ROW0, TRUTH_ROW0 + TRUTH_ROWS)
    cols = slice(TRUTH_FIRST_SAMPLE, TRUTH_FIRST_SAMPLE + TRUTH_WIDTH)
    return y[rows, cols], u[rows, cols], v[rows, cols]


# ── Masks ─────────────────────────────────────────────────────────────────

LINE_SIGN_REGIONS = [(26, 16, 38, 544), (656, 16, 38, 544), (0, 100, 24, 40)]
LINE_SIGN_MARGIN = 8
CLIP_MARGIN_SAMPLES = 16
CLIP_MARGIN_ROWS = 4


def source_columns(pixel_x, pixel_width):
    first = pixel_x * TRUTH_WIDTH // 720
    end = (pixel_x + pixel_width) * TRUTH_WIDTH // 720
    return first, end


def picture_mask():
    mask = np.ones((TRUTH_ROWS, TRUTH_WIDTH), dtype=bool)
    mask[HALF_LINE_ROW, HALF_LINE_END:] = False
    return mask


def masks(frame):
    """The samples DecoderStillTruth measures on |frame|, and the count of
    clipped composite samples under the picture."""
    mask = picture_mask()
    if frame == STRESS:
        for x, y, w, h in LINE_SIGN_REGIONS:
            x0 = max(0, x - LINE_SIGN_MARGIN)
            y0 = max(0, y - LINE_SIGN_MARGIN)
            a, b = source_columns(x0, x + w + LINE_SIGN_MARGIN - x0)
            mask[y0:y + h + LINE_SIGN_MARGIN, a:b] = False
    raster = composite("clean", frame)
    rows = np.empty((TRUTH_ROWS, TRUTH_WIDTH))
    rows[0::2] = raster[22:22 + 288, TRUTH_FIRST_SAMPLE:
                        TRUTH_FIRST_SAMPLE + TRUTH_WIDTH]
    rows[1::2] = raster[335:335 + 288, TRUTH_FIRST_SAMPLE:
                        TRUTH_FIRST_SAMPLE + TRUTH_WIDTH]
    clipped = ((rows <= SYNC_TIP) | (rows >= PEAK)) & picture_mask()
    spread = maximum_filter(clipped.astype(np.uint8),
                            size=(2 * CLIP_MARGIN_ROWS + 1,
                                  2 * CLIP_MARGIN_SAMPLES + 1),
                            mode="constant") > 0
    return mask & ~spread, int(clipped.sum())


# ── Metrics (still_metrics.cpp) ───────────────────────────────────────────

CHROMA_PRESENT = 0.05
CHROMA_ABSENT = 0.005
LUMA_DETAIL = 0.02
CHROMA_EDGE = 0.05
FILTER_TAPS = 129
EDGE_EXCLUSION = 16


def band_filter(low_hz, high_hz):
    n = np.arange(FILTER_TAPS)
    m = n - FILTER_TAPS // 2
    lo = min(max(low_hz, 0.0), RATE / 2) / RATE
    hi = min(max(high_hz, 0.0), RATE / 2) / RATE

    def sinc_lp(fc):
        with np.errstate(invalid="ignore", divide="ignore"):
            out = np.sin(2 * np.pi * fc * m) / (np.pi * m)
        out[m == 0] = 2 * fc
        return out

    window = (0.42 - 0.5 * np.cos(2 * np.pi * n / (FILTER_TAPS - 1)) +
              0.08 * np.cos(4 * np.pi * n / (FILTER_TAPS - 1)))
    return (sinc_lp(hi) - sinc_lp(lo)) * window


def filter_rows(plane, taps):
    return convolve1d(plane, taps[::-1], axis=1, mode="nearest")


def yuv_to_rgb(y, u, v):
    r = y + v / KR
    b = y + u / KB
    g = (y - 0.299 * r - 0.114 * b) / 0.587
    return r, g, b


def _rgb_to_xyz(xy):
    p = np.array([[x / y for x, y in xy], [1.0] * 3,
                  [(1 - x - y) / y for x, y in xy]])
    w = np.array([0.3127 / 0.3290, 1.0, (1 - 0.3127 - 0.3290) / 0.3290])
    return p * np.linalg.solve(p, w)


EBU = _rgb_to_xyz([(0.64, 0.33), (0.29, 0.60), (0.15, 0.06)])


def yuv_to_lab(y, u, v):
    r, g, b = yuv_to_rgb(y, u, v)
    rgb = np.stack([np.clip(c, 0.0, 1.0) ** 2.4 for c in (r, g, b)])
    xyz = np.tensordot(EBU, rgb, axes=1)
    white = EBU.sum(axis=1)
    d = 6.0 / 29.0

    def f(t):
        return np.where(t > d ** 3, np.cbrt(t), t / (3 * d * d) + 4.0 / 29.0)

    fx, fy, fz = f(xyz[0] / white[0]), f(xyz[1]), f(xyz[2] / white[2])
    return 116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)


def ciede2000(lab1, lab2):
    l1, a1, b1 = lab1
    l2, a2, b2 = lab2
    c1 = np.hypot(a1, b1)
    c2 = np.hypot(a2, b2)
    c7 = ((c1 + c2) / 2) ** 7
    g = 0.5 * (1 - np.sqrt(c7 / (c7 + 25.0 ** 7)))
    ap1 = (1 + g) * a1
    ap2 = (1 + g) * a2
    cp1 = np.hypot(ap1, b1)
    cp2 = np.hypot(ap2, b2)

    def hue(b, a):
        h = np.degrees(np.arctan2(b, a))
        h = np.where(h < 0, h + 360, h)
        return np.where((a == 0) & (b == 0), 0.0, h)

    h1 = hue(b1, ap1)
    h2 = hue(b2, ap2)
    dl = l2 - l1
    dc = cp2 - cp1
    both = cp1 * cp2 != 0
    dh = h2 - h1
    dh = np.where(dh > 180, dh - 360, dh)
    dh = np.where(dh < -180, dh + 360, dh)
    dh = np.where(both, dh, 0.0)
    dbh = 2 * np.sqrt(cp1 * cp2) * np.sin(np.radians(dh / 2))
    lm = (l1 + l2) / 2
    cpm = (cp1 + cp2) / 2
    hm = np.where(np.abs(h1 - h2) <= 180, (h1 + h2) / 2,
                  np.where(h1 + h2 < 360, (h1 + h2 + 360) / 2,
                           (h1 + h2 - 360) / 2))
    hm = np.where(both, hm, h1 + h2)
    t = (1 - 0.17 * np.cos(np.radians(hm - 30)) +
         0.24 * np.cos(np.radians(2 * hm)) +
         0.32 * np.cos(np.radians(3 * hm + 6)) -
         0.20 * np.cos(np.radians(4 * hm - 63)))
    dtheta = 30 * np.exp(-((hm - 275) / 25) ** 2)
    cp7 = cpm ** 7
    rc = 2 * np.sqrt(cp7 / (cp7 + 25.0 ** 7))
    l50 = (lm - 50) ** 2
    sl = 1 + 0.015 * l50 / np.sqrt(20 + l50)
    sc = 1 + 0.045 * cpm
    sh = 1 + 0.015 * cpm * t
    rt = -np.sin(np.radians(2 * dtheta)) * rc
    tl, tc, th = dl / sl, dc / sc, dbh / sh
    return np.sqrt(tl * tl + tc * tc + th * th + rt * tc * th)


class TruthFeatures:
    """The truth-only parts of measure(), computed once per still."""

    def __init__(self, t):
        ty, tu, tv = t
        self.t = t
        self.c = np.hypot(tu, tv)
        self.chroma_nearby = maximum_filter(self.c, size=(3, 5),
                                            mode="nearest")
        step = np.zeros_like(self.c)
        step[:, 1:-1] = np.hypot(tu[:, 2:] - tu[:, :-2],
                                 tv[:, 2:] - tv[:, :-2])
        vstep = np.zeros_like(self.c)
        vstep[1:-1] = np.hypot(tu[2:] - tu[:-2], tv[2:] - tv[:-2])
        self.chroma_edge = maximum_filter(np.maximum(step, vstep),
                                          size=(3, 3), mode="nearest")
        high = filter_rows(ty, band_filter(3.0e6, RATE))
        energy = filter_rows(high * high, np.full(9, 1.0 / 9.0))
        self.luma_detail = np.sqrt(energy) >= LUMA_DETAIL
        self.lab = yuv_to_lab(ty, tu, tv)


LUMA_BAND = band_filter(3.5e6, 5.0e6)


def measure(d, features, mask):
    """measure_still(): |d| = (Y, U, V) against the truth in |features|,
    over |mask| less the edge exclusion. A dict of the test's metrics."""
    ty, tu, tv = features.t
    dy, du, dv = d
    m = mask.copy()
    m[:, :EDGE_EXCLUSION] = False
    m[:, -EDGE_EXCLUSION:] = False
    n = m.sum()
    out = {"samples": int(n)}

    def pct(e, count):
        return float(np.sqrt(e / count) * 100) if count else 0.0

    ey, eu, ev = dy - ty, du - tu, dv - tv
    out["y_rms_pct"] = pct((ey[m] ** 2).sum(), n)
    out["u_rms_pct"] = pct((eu[m] ** 2).sum(), n)
    out["v_rms_pct"] = pct((ev[m] ** 2).sum(), n)
    ra = yuv_to_rgb(dy[m], du[m], dv[m])
    rb = yuv_to_rgb(ty[m], tu[m], tv[m])
    mse = sum(((a - b) ** 2).sum() for a, b in zip(ra, rb)) / (3 * n)
    out["rgb_psnr_db"] = float(min(120.0, -10 * np.log10(mse))) \
        if mse > 0 else 120.0
    de = ciede2000(tuple(x[m] for x in features.lab),
                   yuv_to_lab(dy[m], du[m], dv[m]))
    out["delta_e_mean"] = float(de.mean())
    k = min(de.size - 1, int(0.95 * n))
    out["delta_e_p95"] = float(np.partition(de, k)[k])

    dc = np.hypot(du, dv)
    cm = m & (features.c >= CHROMA_PRESENT)
    nc = cm.sum()
    if nc:
        out["chroma_magnitude_rms_pct"] = pct(
            ((dc[cm] - features.c[cm]) ** 2).sum(), nc)
        dp = np.arctan2(dv[cm], du[cm]) - np.arctan2(tv[cm], tu[cm])
        dp = np.remainder(dp + np.pi, 2 * np.pi) - np.pi
        out["chroma_phase_rms_deg"] = float(
            np.sqrt((np.degrees(dp) ** 2).mean()))
        gain = (dc[cm] * features.c[cm]).sum() / (features.c[cm] ** 2).sum()
        out["chroma_gain_error_pct"] = float(abs(gain - 1) * 100)
    xm = m & (features.chroma_nearby < CHROMA_ABSENT) & features.luma_detail
    if xm.sum():
        out["cross_colour_rms_pct"] = pct((dc[xm] ** 2).sum(), xm.sum())
    lm = m & (features.chroma_edge >= CHROMA_EDGE)
    if lm.sum():
        band = filter_rows(ey, LUMA_BAND)
        out["cross_luma_rms_pct"] = pct((band[lm] ** 2).sum(), lm.sum())
    return out


class Bench:
    """Truth, masks and truth features for every still, loaded once."""

    def __init__(self, stills=STILLS):
        self.stills = stills
        self.truth = {f: truth("clean", f) for f, _ in stills}
        self.features = {f: TruthFeatures(self.truth[f]) for f, _ in stills}
        self.masks = {f: masks(f)[0] for f, _ in stills}

    def score(self, decode, frames=None):
        """{still name: metrics} for |decode|(frame) -> (Y, U, V) on the
        truth grid."""
        out = {}
        for f, name in self.stills:
            if frames is not None and f not in frames:
                continue
            out[name] = measure(decode(f), self.features[f], self.masks[f])
        return out


# ── The "||||" dots ───────────────────────────────────────────────────────

GREEN_EDGES = 10  # the green/magenta edge card's frame
GREEN_EDGE_ROWS = (32, 64)  # its two single green/magenta horizontal edges
EDGE_DOT_REACH = 8  # rows either side measured


def edge_dots(y, truth_y):
    """The "||||" dots at the edge card's single green/magenta edges: the
    luma error in 3.5-5 MHz, as the largest row's sine amplitude (r.m.s.
    times root 2) in 8-bit codes (219 from black to white)."""
    band = filter_rows(y - truth_y, LUMA_BAND)
    worst = 0.0
    for edge in GREEN_EDGE_ROWS:
        rows = band[edge - EDGE_DOT_REACH:edge + EDGE_DOT_REACH,
                    EDGE_EXCLUSION:-EDGE_EXCLUSION]
        amp = np.sqrt(2.0 * (rows ** 2).mean(axis=1)).max() * 219.0
        worst = max(worst, float(amp))
    return worst
