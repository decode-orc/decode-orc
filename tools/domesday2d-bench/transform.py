#!/usr/bin/env python3
#
# File:        transform.py
# Module:      tools/domesday2d-bench
# Purpose:     Transform PAL prototypes: decode-orc's transform2d on a
#              field, and the Phase 3 variants on the woven frame
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Domesday2D Phase 3 step 3. transform_chroma() is transformpal2d.cpp
# vectorised: Hann-windowed tiles of 32 samples, 50 % overlap-add, a 2D FFT,
# and each bin of 0.5-1.5 fsc kept as chroma only when its magnitude is
# within |threshold| of its reflection's. With geometry 'field' and 16 rows
# it is transform2d (checked against decode-orc's output by phase3_transform).
#
# The reflection. In one field the U carrier sits at (fsc, -1/4 cycle per
# field line) and V at (fsc, +1/4); reflecting about either is the same
# map, y -> H/2 - y (mod H). In the woven frame (rows = frame lines) the
# carrier is not one point: on a row r the U phase is
#   -pi r / 4 + (0 on one field's rows, 3 pi / 4 on the other's)
# so U has a lobe at -1/8 cycle per row (weight cos(3 pi / 8) = 0.38) and
# one at +3/8 (weight 0.92), and V the mirror image (+1/8 and -3/8). Each
# lobe is symmetric about its own centre, so U's lobes share the
# reflection y -> 3H/4 - y and V's y -> H/4 - y, and they differ. The frame
# test therefore asks each bin both questions: is it symmetric as U, or as
# V? Low vertical luma near fsc lands between the two weak lobes; only luma
# of high vertical frequency (near +/-3/8) meets the strong ones.
#
# Variants (keyword arguments):
#   mode       'binary' (keep or discard the pair) or 'match' (EP1175102's
#              amplitude matching: the larger of the pair scaled to the
#              smaller, phase kept)
#   band_bins  test only bins within this many of fsc (the Phase 2
#              symmetric band is +/-0.5 MHz, one bin of 0.55 MHz); bins
#              outside take the decision of the in-band bin at the same
#              vertical frequency
#   fallback   a chroma signal on the raster (e.g. a comb's remodulated
#              chroma) whose bins replace the rejected ones, instead of
#              sending them to luma

import numpy as np

import bench
import proto

XTILE = 32
HALF_X = XTILE // 2
FIRST_SAMPLE = bench.DECODE_FIRST_SAMPLE  # 157
END_SAMPLE = FIRST_SAMPLE + bench.DECODE_WIDTH  # 1105


def _window(n):
    return 0.5 - 0.5 * np.cos(2 * np.pi * (np.arange(n) + 0.5) / n)


def _reflections(geometry, ytile):
    y = np.arange(ytile)
    if geometry == "field":
        r = (ytile // 2 - y) % ytile
        return [r]
    return [(3 * ytile // 4 - y) % ytile, (ytile // 4 - y) % ytile]


def _tiles(plane, first_row, end_row, ytile):
    """Tile origins (row, sample) covering [first_row, end_row) x the
    active samples, and the windowed tile stack. Rows outside the range
    read as blanking (0)."""
    rows = np.arange(first_row - ytile // 2, end_row, ytile // 2)
    cols = np.arange(FIRST_SAMPLE - HALF_X, END_SAMPLE, HALF_X)
    padded = np.zeros((plane.shape[0] + 2 * ytile, plane.shape[1] + XTILE))
    active = np.zeros_like(plane)
    active[first_row:end_row] = plane[first_row:end_row]
    padded[ytile:ytile + plane.shape[0], :plane.shape[1]] = active
    w = np.outer(_window(ytile), _window(XTILE))
    stack = np.empty((rows.size, cols.size, ytile, XTILE))
    for i, r in enumerate(rows):
        for j, c in enumerate(cols):
            stack[i, j] = padded[ytile + r:ytile + r + ytile, c:c + XTILE]
    return rows, cols, stack * w


def _overlap_add(shape, rows, cols, tiles, first_row, end_row):
    out = np.zeros((shape[0] + 2 * tiles.shape[2], shape[1] + XTILE))
    ytile = tiles.shape[2]
    for i, r in enumerate(rows):
        for j, c in enumerate(cols):
            out[ytile + r:ytile + r + ytile, c:c + XTILE] += tiles[i, j]
    out = out[ytile:ytile + shape[0], :shape[1]]
    keep = np.zeros(shape, dtype=bool)
    keep[first_row:end_row, FIRST_SAMPLE:END_SAMPLE] = True
    return np.where(keep, out, 0.0)


def _filter(spec, geometry, threshold, mode, band_bins, fallback_spec):
    """The chroma bins of a stack of tile spectra (rfft2 over the last two
    axes)."""
    ytile = spec.shape[-2]
    xs = np.arange(XTILE // 8, XTILE // 4 + 1)  # 4..8: 0.5 fsc to fsc
    xr = XTILE // 2 - xs  # 12..8
    out = np.zeros_like(spec)
    t2 = threshold * threshold
    mag = np.abs(spec) ** 2
    # Each bin is paired under the reflection whose magnitudes agree best
    # (in the frame, the other one would pair a strong lobe with a weak
    # one), and kept if that pair is within the threshold.
    reflections = _reflections(geometry, ytile)
    y = np.arange(ytile)
    agree = []
    for refl in reflections:
        a = mag[..., :, xs]  # (..., y, x)
        b = mag[..., refl, :][..., xr]
        with np.errstate(invalid="ignore", divide="ignore"):
            q = np.where(np.maximum(a, b) > 0,
                         np.minimum(a, b) / np.maximum(a, b), 1.0)
        # A carrier bin (its own reflection) is always kept.
        own = (refl == y)[:, None] & (xs == xr)[None, :]
        agree.append(np.where(own, 2.0, q))
    agree = np.stack(agree)
    best = np.argmax(agree, axis=0)
    best_pairs = []
    for k, refl in enumerate(reflections):
        ok = (best == k) & (agree[k] >= t2)
        if band_bins is not None:
            # Decide on the in-band bins; the others take the decision of
            # the nearest in-band bin of the same row.
            first = int(np.argmax(np.abs(xs - XTILE // 4) <= band_bins))
            ok = ok[..., np.maximum(np.arange(xs.size), first)]
        best_pairs.append((refl, ok))
    for refl, ok in best_pairs:
        a = spec[..., :, xs]
        b = spec[..., refl, :][..., xr]
        if mode == "match":
            ma = np.abs(a)
            mb = np.abs(b)
            small = np.minimum(ma, mb)
            with np.errstate(invalid="ignore", divide="ignore"):
                a = np.where(ma > 0, a * small / ma, 0)
                b = np.where(mb > 0, b * small / mb, 0)
        cur = out[..., :, xs]
        out[..., :, xs] = np.where(ok, a, cur)
        tmp = out[..., refl, :]
        cur_r = tmp[..., xr]
        tmp[..., xr] = np.where(ok, b, cur_r)
        out[..., refl, :] = tmp
    if fallback_spec is not None:
        # Rejected bins (neither member of any kept pair) take the
        # fallback's value.
        kept = np.zeros(spec.shape, dtype=bool)
        for refl, ok in best_pairs:
            k = kept[..., :, xs] | ok
            kept[..., :, xs] = k
            tmp = kept[..., refl, :]
            tmp[..., xr] |= ok
            kept[..., refl, :] = tmp
        region = np.zeros(spec.shape, dtype=bool)
        region[..., :, XTILE // 8:3 * XTILE // 8 + 1] = True
        out = np.where(region & ~kept, fallback_spec, out)
    return out


def transform_chroma(plane, geometry="field", ytile=16, threshold=0.4,
                     mode="binary", band_bins=None, fallback=None,
                     first_row=None, end_row=None):
    """The chroma Transform PAL finds in |plane| (rows of one geometry: a
    field's lines or the frame raster's rows) over [first_row, end_row)."""
    rows, cols, stack = _tiles(plane, first_row, end_row, ytile)
    spec = np.fft.rfft2(stack)
    fb = None
    if fallback is not None:
        _, _, fstack = _tiles(fallback, first_row, end_row, ytile)
        fb = np.fft.rfft2(fstack)
    kept = _filter(spec, geometry, threshold, mode, band_bins, fb)
    tiles = np.fft.irfft2(kept, s=(ytile, XTILE))
    return _overlap_add(plane.shape, rows, cols, tiles, first_row, end_row)


def chroma_field(raster, **kw):
    """Transform PAL on each field of the frame raster separately."""
    out = np.zeros_like(raster)
    first = proto.FIRST_ACTIVE // 2
    end = proto.END_ACTIVE // 2
    fb = kw.pop("fallback", None)
    for p in (0, 1):
        out[p::2] = transform_chroma(
            raster[p::2], "field", first_row=first, end_row=end,
            fallback=None if fb is None else fb[p::2], **kw)
    return out


def chroma_frame(raster, **kw):
    return transform_chroma(raster, "frame", first_row=proto.FIRST_ACTIVE,
                            end_row=proto.END_ACTIVE, **kw)


def separate(raw, geometry="field", **kw):
    """(raster, chroma, phi, s): the frame raster, the chroma Transform PAL
    finds in it, and the lattice."""
    raster = bench.frame_raster(raw)
    phi, s = proto.lattice(raster)
    if kw.get("fallback") == "comb":
        u0, v0 = proto.demodulate(raster, phi, s)
        u, v = proto.filter_planes(u0, v0, proto.radial_kernel("frame"))
        kw["fallback"] = proto.remodulate(u, v, phi, s)
    c = chroma_field(raster, **kw) if geometry == "field" \
        else chroma_frame(raster, **kw)
    return raster, c, phi, s


def decode(raw, geometry="field", uv_kernel=None, **kw):
    """A Transform PAL decode: luma is the composite less the transform's
    chroma, and U and V are the chroma demodulated per row and filtered by
    |uv_kernel| (PalColour's field kernel by default, as transform2d)."""
    raster, c, phi, s = separate(raw, geometry, **kw)
    y = raster - c
    u, v = proto.demodulate(c, phi, s)
    k = uv_kernel if uv_kernel is not None else proto.radial_kernel("field")
    u, v = proto.filter_planes(u, v, k)
    return proto.Decoded(y, u, v)
