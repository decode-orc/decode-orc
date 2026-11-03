#!/usr/bin/env python3
#
# File:        phase3_lattice.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 3 step 1: the PAL carrier lattice of a
#              woven still frame, measured on videosynth output and checked
#              on a real capture's bursts
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage: phase3_lattice.py [capture.cvbs ...]
#
# On the bench (bench.py), each picture line's chroma is the clean composite
# less the truth's luma; it is fitted against the truth's U and V as
#   C = g (U sin(theta) + s V cos(theta)),  theta = n pi/2 + phi
# (n the sample after 0H), which gives the line's U carrier phase phi and
# its V-switch sign s. The U and V carrier shifts between two lines d lines
# apart are then phi_b - phi_a and that plus 180 deg when s differs. Frame
# lines 2 frames apart are compared across two stills.
#
# On a real capture only the burst is known: lines an even distance apart
# share the V-switch, so their burst phases differ by the carrier shift.

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import cvbs  # noqa: E402
import vits  # noqa: E402

CHROMA_STILLS = (5, 6, 7)  # bars, tartan, town map


def line_carriers(frame):
    """{frame line (0-based, transmission order): (phi deg, s)} for every
    truth row of |frame| that carries chroma."""
    raw = bench.composite("clean", frame)
    ty, tu, tv = bench.truth("clean", frame)
    n = bench.TRUTH_FIRST_SAMPLE + np.arange(bench.TRUTH_WIDTH)
    c4 = np.cos(n * np.pi / 2)
    s4 = np.sin(n * np.pi / 2)
    out = {}
    for row in range(bench.TRUTH_ROWS):
        line = (22 + row // 2) if row % 2 == 0 else (335 + row // 2)
        if row == bench.HALF_LINE_ROW:
            continue  # its tail is blanking
        if np.hypot(tu[row], tv[row]).max() < 0.05:
            continue
        x = raw[line, n] - bench.BLACK
        c = x / (bench.WHITE - bench.BLACK) - ty[row]
        best = None
        for s in (1.0, -1.0):
            b1 = tu[row] * c4 - s * tv[row] * s4
            b2 = tu[row] * s4 + s * tv[row] * c4
            basis = np.stack([b1, b2, np.ones_like(b1)], 1)
            k, *_ = np.linalg.lstsq(basis, c, rcond=None)
            r = np.abs(c - basis @ k).max()
            if best is None or r < best[0]:
                best = (r, np.degrees(np.arctan2(k[0], k[1])), s)
        out[line] = (best[1], best[2])
    return out


def wrap(deg):
    return (np.asarray(deg) + 180.0) % 360.0 - 180.0


def shifts(a, b):
    """U and V carrier shifts (deg, 0..360) from line carriers a to b."""
    u = (b[0] - a[0]) % 360.0
    v = (u + (180.0 if a[1] != b[1] else 0.0)) % 360.0
    return u, v


def summarise(pairs):
    u = np.array([p[0] for p in pairs])
    v = np.array([p[1] for p in pairs])
    um = np.degrees(np.angle(np.exp(1j * np.radians(u)).mean())) % 360.0
    vm = np.degrees(np.angle(np.exp(1j * np.radians(v)).mean())) % 360.0
    return um, vm, float(np.abs(wrap(u - um)).max()), \
        float(np.abs(wrap(v - vm)).max()), len(u)


def bench_lattice():
    carriers = {f: line_carriers(f) for f in CHROMA_STILLS}
    print("Bench (videosynth, clean): carrier shift from a line to the line "
          "d lines later")
    print(f"{'d':>6} {'U deg':>8} {'V deg':>8} {'U spread':>9} "
          f"{'V spread':>9} {'pairs':>6}")
    for d in (1, 2, 312, 313):
        pairs = []
        for lines in carriers.values():
            for a, ca in lines.items():
                if a + d in lines:
                    pairs.append(shifts(ca, lines[a + d]))
        print(f"{d:>6} " + "{:8.2f} {:8.2f} {:9.3f} {:9.3f} {:6d}".format(
            *summarise(pairs)))
    # Two frames apart: frames 5 and 7 (bars and town map).
    pairs = [shifts(ca, carriers[7][a])
             for a, ca in carriers[5].items() if a in carriers[7]]
    print(f"{1250:>6} " + "{:8.2f} {:8.2f} {:9.3f} {:9.3f} {:6d}".format(
        *summarise(pairs)))

    # Which raster neighbour is the 312-line partner: raster row 2k is
    # field 1 line k+1 (frame line k), row 2k+1 field 2 line k+1 (frame
    # line 313 + k).
    print("\nIn the woven frame (truth row r):")
    print("  field 1 row r (line L): row r-1 is field 2 line L+312 -> "
          "312 lines later, the partner ABOVE")
    print("  field 2 row r (line L): row r+1 is field 1 line L-312 -> "
          "312 lines earlier, the partner BELOW")
    print("  the other neighbour of each is 313 lines away")
    return carriers


def capture_lattice(path, frames=24):
    capture = cvbs.Capture(path)
    picks = np.linspace(500, capture.frames - 500, frames).astype(int)
    d312, d2 = [], []
    for frame in picks:
        edge = vits.bar_edge(capture.line(frame, 1, 19))
        if edge is None:
            continue
        b1 = vits.burst_phases(capture.lines(frame, 1, 24, 308), edge)
        b2 = vits.burst_phases(capture.lines(frame, 2, 24, 308), edge)
        # Field 2 line k+1 is 312 lines after field 1 line k.
        d312 += list(wrap(b2[:-1] - b1[1:]))
        d2 += list(wrap(b1[2:] - b1[:-2]))
    for name, d, nominal in (("312", d312, 179.71), ("2", d2, 181.15)):
        z = np.exp(1j * np.radians(d)).mean()
        print(f"  d={name}: mean {np.degrees(np.angle(z)) % 360:.2f} deg "
              f"(A3: {nominal}), r.m.s. spread "
              f"{np.degrees(np.sqrt(-2 * np.log(abs(z)))):.2f} deg, "
              f"{len(d)} pairs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="*")
    args = parser.parse_args()
    bench_lattice()
    for path in args.captures:
        print(f"\n{os.path.basename(path)} (bursts):")
        capture_lattice(path)


if __name__ == "__main__":
    sys.exit(main())
