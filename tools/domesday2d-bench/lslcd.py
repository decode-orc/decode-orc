#!/usr/bin/env python3
#
# File:        lslcd.py
# Module:      tools/domesday2d-bench
# Purpose:     Least-squares-trained PAL separation filters on the woven
#              frame (after Leung, Jeon & Dubois, IEEE TIP 20(7) 2011)
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Domesday2D Phase 3 step 4. Y, U and V at each sample are linear in the
# composite around it. The carrier lattice repeats, so a sample's filter
# depends only on its class: the raster row's parity (which field; the
# 312-line partner is above on one, below on the other), the carrier's
# quadrant at the sample, and the row's V-switch. That is 16 classes, and
# within one the best linear filter is a fixed kernel: solved here by
# least squares over the training pictures (training.py), with a small
# ridge.
#
# The window is +/-rows by +/-taps. 'field' windows see only the sample's
# own field (even row offsets), 'frame' windows both: the like-for-like
# test of H1 for the optimal linear filter.

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view

import bench
import proto

CLASSES = 16


def classes(phi, s):
    """The class of every raster sample (624 x 1135)."""
    q = (np.round(phi / 90.0).astype(int)[:, None] +
         np.arange(bench.LINE)[None, :]) % 4
    p = (np.arange(proto.ROWS) % 2)[:, None]
    sw = (s > 0).astype(int)[:, None]
    return p * 8 + q * 2 + sw


class Separator:
    def __init__(self, rows=4, taps=8, geometry="frame", ridge=1e-6):
        self.rows, self.taps, self.geometry = rows, taps, geometry
        self.ridge = ridge
        n = self.inputs()
        self.ata = np.zeros((CLASSES, n, n))
        self.atb = np.zeros((CLASSES, n, 3))
        self.count = np.zeros(CLASSES)
        self.coef = None

    def inputs(self):
        r = 2 * self.rows + 1
        if self.geometry == "field":
            r = 2 * (self.rows // 2) + 1  # the even offsets only
        return r * (2 * self.taps + 1) + 1

    def _windows(self, raster):
        """A view of every raster sample's window, the active rows only
        (rows outside read as blanking, as the decoders do)."""
        active = np.zeros_like(raster)
        active[proto.FIRST_ACTIVE:proto.END_ACTIVE] = \
            raster[proto.FIRST_ACTIVE:proto.END_ACTIVE]
        padded = np.pad(active, ((self.rows, self.rows),
                                 (self.taps, self.taps)))
        view = sliding_window_view(padded, (2 * self.rows + 1,
                                            2 * self.taps + 1))
        if self.geometry == "field":
            view = view[:, :, self.rows % 2::2, :]
        return view

    def _design(self, view, rr, cc):
        a = view[rr, cc].reshape(rr.size, -1)
        return np.concatenate([a, np.ones((rr.size, 1))], axis=1)

    def add(self, raster, truth, phi, s):
        """Accumulate one training picture: its raster and truth."""
        view = self._windows(raster)
        cls = classes(phi, s)
        ty, tu, tv = truth
        rows = np.arange(bench.TRUTH_ROWS) + bench.TRUTH_ROW0
        cols = np.arange(bench.TRUTH_WIDTH) + bench.TRUTH_FIRST_SAMPLE
        rr, cc = np.meshgrid(rows, cols, indexing="ij")
        target = np.stack([ty, tu, tv], -1).reshape(-1, 3)
        rr, cc = rr.ravel(), cc.ravel()
        k = cls[rr, cc]
        for c in range(CLASSES):
            sel = k == c
            if not sel.any():
                continue
            a = self._design(view, rr[sel], cc[sel])
            self.ata[c] += a.T @ a
            self.atb[c] += a.T @ target[sel]
            self.count[c] += sel.sum()

    def solve(self):
        n = self.ata.shape[1]
        self.coef = np.zeros((CLASSES, n, 3))
        for c in range(CLASSES):
            if self.count[c] == 0:
                continue
            reg = self.ridge * np.trace(self.ata[c]) / n
            self.coef[c] = np.linalg.solve(self.ata[c] + reg * np.eye(n),
                                           self.atb[c])

    def apply(self, raster, phi, s):
        """(Y, U, V) on the truth grid."""
        view = self._windows(raster)
        cls = classes(phi, s)
        rows = np.arange(bench.TRUTH_ROWS) + bench.TRUTH_ROW0
        cols = np.arange(bench.TRUTH_WIDTH) + bench.TRUTH_FIRST_SAMPLE
        rr, cc = np.meshgrid(rows, cols, indexing="ij")
        rr, cc = rr.ravel(), cc.ravel()
        k = cls[rr, cc]
        out = np.zeros((rr.size, 3))
        for c in range(CLASSES):
            sel = k == c
            if sel.any():
                out[sel] = self._design(view, rr[sel], cc[sel]) @ self.coef[c]
        out = out.reshape(bench.TRUTH_ROWS, bench.TRUTH_WIDTH, 3)
        return out[..., 0], out[..., 1], out[..., 2]

    def save(self, path):
        np.savez(path, coef=self.coef, rows=self.rows, taps=self.taps,
                 geometry=self.geometry)

    @classmethod
    def load(cls, path):
        z = np.load(path)
        sep = cls(int(z["rows"]), int(z["taps"]), str(z["geometry"]))
        sep.coef = z["coef"]
        return sep
