#!/usr/bin/env python3
#
# File:        encoder.py
# Module:      tools/domesday2d-bench
# Purpose:     A PAL composite encoder equivalent to videosynth's on the
#              truth grid, for training data and residual refinement
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# videosynth's composite is its picture truth modulated on the carrier
# lattice (DecoderStillTruth.PAL_TruthMatchesYc: within 1 LSB in luma,
# 1 LSB + 0.25 % of the line's chroma peak in chroma), so
#   x = Y + U sin(theta) + s V cos(theta)
# on the frame raster (proto.py) encodes a truth exactly as videosynth
# would, quantised to the 10-bit codes. phase3_lattice.py and
# check_encoder() confirm it on the bench.
#
# picture_truth() maps a 720 x 576 R'G'B' picture to the truth grid as
# videosynth does in outline: a band-limited resample of each row to the
# 922 active samples, and U and V through a 33-tap Hamming low-pass at
# 1.3 MHz (videosynth's chroma FIR).

import numpy as np
from scipy.signal import firwin, resample

import bench
import proto

CHROMA_FIR = firwin(33, 1.3e6, window="hamming", fs=bench.RATE)


def picture_truth(rgb):
    """(Y, U, V) on the truth grid (576 x 922) from a 576 x 720 x 3 R'G'B'
    picture (0 black, 1 white)."""
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    u = bench.KB * (b - y)
    v = bench.KR * (r - y)
    out = []
    for plane in (y, u, v):
        out.append(resample(plane, bench.TRUTH_WIDTH, axis=1))
    y, u, v = out
    pad = len(CHROMA_FIR)
    for plane in (u, v):
        ext = np.pad(plane, ((0, 0), (pad, pad)), mode="edge")
        plane[:] = np.apply_along_axis(
            lambda row: np.convolve(row, CHROMA_FIR, mode="same"), 1,
            ext)[:, pad:-pad]
    return y, u, v


def encode(truth, phi, s, quantise=True):
    """The frame raster (624 x 1135, excursion units) carrying |truth| on
    the lattice |phi|, |s| (proto.lattice()); blanking outside the
    picture."""
    ty, tu, tv = truth
    raster = np.zeros((proto.ROWS, bench.LINE))
    rows = slice(bench.TRUTH_ROW0, bench.TRUTH_ROW0 + bench.TRUTH_ROWS)
    cols = slice(bench.TRUTH_FIRST_SAMPLE,
                 bench.TRUTH_FIRST_SAMPLE + bench.TRUTH_WIDTH)
    theta = proto.carrier(phi)[rows, cols]
    raster[rows, cols] = (ty + tu * np.sin(theta) +
                          s[rows, None] * tv * np.cos(theta))
    if quantise:
        span = bench.WHITE - bench.BLACK
        codes = np.clip(np.round(raster * span + bench.BLACK),
                        bench.SYNC_TIP, bench.PEAK)
        raster = (codes - bench.BLACK) / span
    return raster


def check_encoder(frames=(5, 7, 11)):
    """The largest difference (10-bit codes) between encode() of the bench
    truth and the clean composite over the picture, per frame."""
    out = {}
    mask = bench.picture_mask()
    for f in frames:
        raster = bench.frame_raster(bench.composite("clean", f))
        phi, s = proto.lattice(raster)
        mine = encode(bench.truth("clean", f), phi, s)
        a = bench.from_raster(raster, raster, raster)[0]
        b = bench.from_raster(mine, mine, mine)[0]
        out[f] = float(np.abs(a - b)[mask].max() * (bench.WHITE - bench.BLACK))
    return out
