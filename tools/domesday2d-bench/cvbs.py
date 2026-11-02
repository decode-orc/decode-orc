#!/usr/bin/env python3
#
# File:        cvbs.py
# Module:      tools/domesday2d-bench
# Purpose:     Read PAL captures (CVBS_U10_4FSC .cvbs, or an ld-decode .tbc)
#              line by line without loading them
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# A .cvbs frame is 709379 little-endian 16-bit samples: 625 lines in
# transmission order, field 1 (313 lines) first, 0H at sample 0 of each
# line. Lines 312 and 624 (0-based) carry 1137 samples, every other line
# 1135 (EBU Tech. 3280-E 1.2). Levels are 10-bit: black 256, white 844.
#
# An ld-decode .tbc stores fields one after another, each 313 lines of 1135
# samples, the isFirstField field first. TBC line n is field line n + 1.
# Levels are 16-bit, black and white as its .tbc.json states. Its line
# origin is not 0H, so the measurements anchor on the VITS bar instead.

import json
import os

import numpy as np

FSC = 4433618.75
RATE = 4 * FSC
LINE = 1135
CVBS_FRAME = 709379


class Capture:
    """One PAL capture. Field lines are 1-based; field is 1 or 2."""

    def __init__(self, path):
        self.path = path
        if path.endswith(".tbc"):
            self.kind = "tbc"
            self.data = np.memmap(path, dtype="<u2", mode="r")
            with open(path + ".json") as f:
                video = json.load(f)["videoParameters"]
            self.black = float(video["black16bIre"])
            self.white = float(video["white16bIre"])
            self.frames = self.data.size // (2 * 313 * LINE)
        else:
            self.kind = "cvbs"
            self.data = np.memmap(path, dtype="<u2", mode="r")
            self.black = 256.0
            self.white = 844.0
            self.frames = self.data.size // CVBS_FRAME
        self.name = os.path.basename(path)

    def _offset(self, frame, field, field_line):
        if self.kind == "tbc":
            return ((2 * frame + field - 1) * 313 + field_line - 1) * LINE
        line = field_line - 1 + (313 if field == 2 else 0)
        return frame * CVBS_FRAME + line * LINE + (2 if line > 312 else 0)

    def line(self, frame, field, field_line):
        """One line in IRE (black 0, white 100), as float64."""
        base = self._offset(frame, field, field_line)
        raw = np.asarray(self.data[base:base + LINE], dtype=np.float64)
        return (raw - self.black) * 100.0 / (self.white - self.black)

    def lines(self, frame, field, first, last):
        """Field lines first..last (inclusive) as a 2-D array in IRE."""
        return np.stack([self.line(frame, field, n)
                         for n in range(first, last + 1)])
