#!/usr/bin/env python3
#
# File:        training.py
# Module:      tools/domesday2d-bench
# Purpose:     Procedural training pictures for the least-squares separation
#              filters, disjoint from the bench's evaluation stills
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Domesday2D Phase 3 step 4. Every picture is drawn here from a seed, so the
# set is licence-clean and shares no asset with pal_stills.yaml. Four kinds,
# after the Domesday content:
#   map      flat fills, roads with casings, 1-pixel contours, hatching and
#            lettering in map colours
#   text     lettering of many sizes and colours on flat or graded grounds
#   natural  1/f colour texture with soft-edged shapes (photograph-like)
#   graphic  saturated bars, stripes 1-8 rows high, checks and discs
# Text is set in DejaVu (Bitstream Vera licence), from the fonts directory
# passed in.
#
# pictures() yields 576 x 720 x 3 R'G'B' arrays; chroma is scaled down where
# needed so the composite stays inside the 10-bit range (encoder.encode
# would otherwise clip, which no linear filter can learn).

import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

import bench

KINDS = ("map", "text", "natural", "graphic")
W, H = 720, 576

MAP_FILLS = [(250, 245, 230), (220, 235, 200), (200, 225, 245),
             (245, 220, 200), (235, 235, 235), (255, 250, 200),
             (210, 240, 210), (240, 210, 230)]
MAP_INK = [(0, 0, 0), (180, 90, 30), (30, 60, 200), (200, 30, 30),
           (20, 130, 40), (120, 120, 120)]


def _font(fonts, rng, size):
    names = [n for n in sorted(os.listdir(fonts)) if n.endswith(".ttf")]
    return ImageFont.truetype(os.path.join(fonts, names[rng.integers(
        len(names))]), int(size))


def _words(rng, n):
    letters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    return " ".join("".join(rng.choice(list(letters), rng.integers(2, 9)))
                    for _ in range(n))


def _colour(rng, saturated=False):
    if saturated:
        c = rng.random(3)
        c = (c - c.min()) / max(1e-6, c.max() - c.min())
        return tuple(int(x) for x in (40 + 200 * c))
    return tuple(int(x) for x in rng.integers(0, 256, 3))


def _map(rng, fonts):
    im = Image.new("RGB", (W, H), MAP_FILLS[rng.integers(len(MAP_FILLS))])
    d = ImageDraw.Draw(im)
    for _ in range(rng.integers(8, 20)):
        pts = [tuple(rng.integers(-50, [W + 50, H + 50]))
               for _ in range(rng.integers(3, 8))]
        d.polygon(pts, fill=MAP_FILLS[rng.integers(len(MAP_FILLS))])
    for _ in range(rng.integers(3, 8)):  # hatching
        x0, y0 = rng.integers(0, [W - 100, H - 100])
        w, h = rng.integers(40, 200, 2)
        ink = MAP_INK[rng.integers(len(MAP_INK))]
        step = int(rng.integers(3, 8))
        for k in range(-h, w, step):
            d.line([(x0 + k, y0), (x0 + k + h, y0 + h)], fill=ink, width=1)
    for _ in range(rng.integers(10, 30)):  # contours
        pts = [tuple(rng.integers(0, [W, H])) for _ in range(6)]
        d.line(pts, fill=MAP_INK[1], width=1)
    for _ in range(rng.integers(4, 10)):  # roads
        pts = [tuple(rng.integers(0, [W, H])) for _ in range(rng.integers(2, 5))]
        width = int(rng.integers(3, 9))
        d.line(pts, fill=(60, 60, 60), width=width + 2)
        d.line(pts, fill=_colour(rng, True), width=width)
    for _ in range(rng.integers(10, 30)):  # labels
        d.text(tuple(rng.integers(0, [W - 60, H - 20])), _words(rng, 2),
               fill=MAP_INK[rng.integers(len(MAP_INK))],
               font=_font(fonts, rng, rng.integers(9, 22)))
    return im


def _text(rng, fonts):
    if rng.random() < 0.5:
        im = Image.new("RGB", (W, H), _colour(rng))
    else:
        a, b = np.array(_colour(rng)), np.array(_colour(rng))
        t = np.linspace(0, 1, H)[:, None, None]
        im = Image.fromarray(((1 - t) * a + t * b).repeat(W, 1).astype(
            np.uint8))
    d = ImageDraw.Draw(im)
    y = int(rng.integers(0, 20))
    while y < H - 10:
        size = int(rng.choice([10, 14, 18, 24, 32, 44]))
        d.text((int(rng.integers(0, 60)), y), _words(rng, 8),
               fill=_colour(rng, rng.random() < 0.6),
               font=_font(fonts, rng, size))
        y += int(size * rng.uniform(1.1, 1.6))
    return im


def _natural(rng, fonts):
    fy = np.fft.fftfreq(H)[:, None]
    fx = np.fft.rfftfreq(W)[None, :]
    f = np.hypot(fx, fy)
    f[0, 0] = 1.0
    planes = []
    alpha = rng.uniform(0.9, 1.3)
    for _ in range(3):
        spec = (rng.normal(size=f.shape) + 1j * rng.normal(size=f.shape)) / \
            f ** alpha
        spec[0, 0] = 0
        p = np.fft.irfft2(spec, s=(H, W))
        planes.append(p / p.std())
    mix = rng.normal(size=(3, 3)) * rng.uniform(0.05, 0.25) + np.eye(3) * 0.3
    rgb = np.einsum("ij,jhw->hwi", mix, np.stack(planes)) * 0.5 + 0.45
    im = Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8))
    d = ImageDraw.Draw(im)
    for _ in range(rng.integers(3, 10)):
        x0, y0 = rng.integers(0, [W, H])
        r = int(rng.integers(20, 150))
        d.ellipse([x0 - r, y0 - r, x0 + r, y0 + r], fill=_colour(rng))
    return im.filter(ImageFilter.GaussianBlur(rng.uniform(0.5, 2.0)))


def _graphic(rng, fonts):
    im = Image.new("RGB", (W, H), _colour(rng, True))
    d = ImageDraw.Draw(im)
    x = 0
    while x < W:  # bars
        w = int(rng.integers(20, 120))
        d.rectangle([x, 0, x + w, H // 3], fill=_colour(rng, True))
        x += w
    y = H // 3
    a, b = _colour(rng, True), _colour(rng, True)
    while y < 2 * H // 3:  # stripes of 1-8 rows
        h = int(rng.integers(1, 9))
        d.rectangle([0, y, W // 2, y + h - 1], fill=a)
        y += h
        h = int(rng.integers(1, 9))
        d.rectangle([0, y, W // 2, y + h - 1], fill=b)
        y += h
    c = int(rng.integers(4, 24))
    for i in range(W // 2, W, c):  # checks
        for j in range(H // 3, 2 * H // 3, c):
            if ((i // c) + (j // c)) % 2:
                d.rectangle([i, j, i + c - 1, j + c - 1],
                            fill=_colour(rng, True))
    for _ in range(rng.integers(5, 15)):  # discs
        x0, y0 = rng.integers(0, W), rng.integers(2 * H // 3, H)
        r = int(rng.integers(5, 60))
        d.ellipse([x0 - r, y0 - r, x0 + r, y0 + r], fill=_colour(rng, True))
    return im


DRAW = {"map": _map, "text": _text, "natural": _natural,
        "graphic": _graphic}


def pictures(fonts, per_kind=6, seed=1):
    """(kind, index, rgb) for |per_kind| pictures of each kind."""
    rng = np.random.default_rng(seed)
    for kind in KINDS:
        for i in range(per_kind):
            im = DRAW[kind](rng, fonts)
            yield kind, i, np.asarray(im, dtype=np.float64) / 255.0


def limit_chroma(rgb, ceiling=1.28, floor=-0.30):
    """|rgb| with its chroma scaled down so Y +/- |C| stays inside the
    composite range."""
    y = (0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] +
         0.114 * rgb[..., 2])[..., None]
    c = rgb - y
    u = bench.KB * (c[..., 2])
    v = bench.KR * (c[..., 0])
    mag = np.hypot(u, v)
    y0 = y[..., 0]
    with np.errstate(divide="ignore", invalid="ignore"):
        k_hi = np.where(y0 + mag > ceiling, (ceiling - y0) / mag, 1.0)
        k_lo = np.where(y0 - mag < floor, (y0 - floor) / mag, 1.0)
    k = float(np.clip(np.nanmin(np.minimum(k_hi, k_lo)), 0.0, 1.0))
    return y + c * k
