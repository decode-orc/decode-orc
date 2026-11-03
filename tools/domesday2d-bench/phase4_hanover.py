#!/usr/bin/env python3
#
# File:        phase4_hanover.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 4 step 2: the anti-Hanover measures on the
#              bench with the Phase 2 per-line chroma errors
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage: phase4_hanover.py [--json out.json]
#
# Every still of the clean and the noisy bench is impaired with the Phase 2
# figures (hanover.PHASE2, and hanover.ALL_SHARED with all the line jitter
# shared by burst and picture), and decoded by pal2d (the prototype, which
# matches decode-orc's), the Phase 3 candidate and the Phase 4 candidate
# with each anti-Hanover measure. The line-alternating matrix is fitted
# once per bench case and impairment, pooled over all twelve stills, as a
# decoder would pool it over a capture's frames ('matrix'); 'matrix_frame'
# fits it from each still alone. Reported per still: the
# metrics of bench.measure(); on flat colour, the Hanover residue (the
# field-line alternation of the decoded U/V, amplitude in percent of
# white); and on the edge card's two single green/magenta edges, the
# 10-90 % rise of the decoded chroma in rows.

import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adaptive  # noqa: E402
import bench  # noqa: E402
import hanover  # noqa: E402
import proto  # noqa: E402
import transform  # noqa: E402

FRAME_UV = proto.radial_kernel("frame")
FIELD_UV = proto.radial_kernel("field")
FIELD_LUMA = proto.luma_kernels("field")


def pal2d(raster, c, phi, s, m):
    u0, v0 = proto.demodulate(raster, phi, s)
    u, v = proto.filter_planes(u0, v0, FIELD_UV)
    yu, yv = proto.filter_by_parity(u0, v0, FIELD_LUMA)
    return proto.Decoded(raster - proto.remodulate(yu, yv, phi, s), u, v)


def phase3(raster, c, phi, s, m):
    u, v = proto.demodulate(c, phi, s)
    u, v = proto.filter_planes(u, v, FRAME_UV)
    return proto.Decoded(raster - c, u, v)


def phase3_matrix(raster, c, phi, s, m):
    """The Phase 3 candidate with the correction on each row before its
    vertical kernel mixes the two switch states."""
    u, v = hanover.corrector(m)(*proto.demodulate(c, phi, s), s)
    u, v = proto.filter_planes(u, v, FRAME_UV)
    return proto.Decoded(raster - c, u, v)


def phase4(matrix=None, line_burst=False, masked=False):
    def run(raster, c, phi, s, m):
        if line_burst:
            phi = hanover.line_lattice(raster, phi, s)
            m = run.line_matrix
        if matrix == "frame":
            m, _ = hanover.fit_matrix(
                [hanover.flat_pairs(*adaptive.demodulate_rows(c, phi, s),
                                    s)])
        kw = {}
        if matrix is not None:
            kw["correction"] = hanover.corrector(m)
        if masked:
            kw["prefilter"] = hanover.masked_average()
        return adaptive.decode_separated(raster, c, phi, s, **kw)
    run.line_burst = line_burst
    return run


DECODERS = {
    "pal2d": pal2d,
    "phase3": phase3,
    "phase3_matrix": phase3_matrix,
    "phase4": phase4(),
    "phase4_matrix_frame": phase4("frame"),
    "phase4_matrix": phase4("pooled"),
    "phase4_masked": phase4(masked=True),
    "phase4_matrix_masked": phase4("pooled", masked=True),
    "phase4_line_burst_matrix": phase4("pooled", line_burst=True),
}


def flat_mask(truth, mask, min_chroma=0.05, flat=0.002, span=8):
    """Truth-grid samples of flat colour: the field lines either side and
    |span| samples either side carry the same chroma."""
    w = truth[1] + 1j * truth[2]
    ok = mask & (np.abs(w) > min_chroma)
    for d, axis in ((2, 0), (-2, 0), (span, 1), (-span, 1)):
        ok &= np.abs(w - np.roll(w, d, axis=axis)) < flat
    ok[:4] = False
    ok[-4:] = False
    return ok


def hanover_residue(u, v, mask):
    """Half the r.m.s. of W(r) less the mean of the field lines either
    side, in percent of white."""
    w = u + 1j * v
    alt = w - 0.5 * (np.roll(w, 2, axis=0) + np.roll(w, -2, axis=0))
    return float(np.sqrt((np.abs(alt[mask]) ** 2).mean()) * 50.0)


def edge_rise(u, v, truth, reach=6):
    """Median over columns of the 10-90 % rise (rows) of the decoded
    chroma across each single green/magenta edge, projected on the truth
    step."""
    w = u + 1j * v
    tw = truth[1] + 1j * truth[2]
    rises = []
    for edge in bench.GREEN_EDGE_ROWS:
        top, bottom = tw[edge - reach], tw[edge + reach]
        step = bottom - top
        cols = np.abs(step) > 0.1
        cols[:bench.EDGE_EXCLUSION] = False
        cols[-bench.EDGE_EXCLUSION:] = False
        seg = w[edge - reach:edge + reach + 1][:, cols]
        p = ((seg - top[cols]) * np.conj(step[cols])).real / \
            np.abs(step[cols]) ** 2
        rows = np.arange(seg.shape[0], dtype=float)
        for col in p.T:
            col = np.maximum.accumulate(np.clip(col, 0.0, 1.0))
            a = np.interp(0.1, col, rows)
            b = np.interp(0.9, col, rows)
            rises.append(b - a)
    return float(np.median(rises))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json")
    parser.add_argument("--only", nargs="*")
    args = parser.parse_args()
    b = bench.Bench()
    results = {"matrices": {}}
    impairments = {"none": None, "phase2": hanover.PHASE2,
                   "all_shared": hanover.ALL_SHARED}
    for case in ("clean", "noisy"):
        for imp_name, imp in impairments.items():
            key = f"{case}/{imp_name}"
            frames = {}
            pooled, pooled_line = [], []
            for f, _ in b.stills:
                raster = bench.frame_raster(bench.composite(case, f))
                phi, s = proto.lattice(raster)
                if imp is not None:
                    raster = hanover.impair(raster, b.truth[f], phi, s,
                                            seed=f, **imp)
                    phi, s = proto.lattice(raster)
                c = transform.chroma_frame(raster, ytile=32)
                frames[f] = (raster, c, phi, s)
                pooled.append(hanover.flat_pairs(
                    *adaptive.demodulate_rows(c, phi, s), s))
                line_phi = hanover.line_lattice(raster, phi, s)
                pooled_line.append(hanover.flat_pairs(
                    *adaptive.demodulate_rows(c, line_phi, s), s))
            m, n = hanover.fit_matrix(pooled)
            m_line, _ = hanover.fit_matrix(pooled_line)
            results["matrices"][key] = {"matrix": m.tolist(), "samples": n}
            print(key, "M", np.round(m, 4).tolist(), n, flush=True)
            results[key] = {}
            for f, still in b.stills:
                raster, c, phi, s = frames[f]
                flat = flat_mask(b.truth[f], b.masks[f])
                for name, dec in DECODERS.items():
                    if args.only and name not in args.only:
                        continue
                    dec.line_matrix = m_line
                    grid = dec(raster, c, phi, s, m).truth_grid()
                    r = bench.measure(grid, b.features[f], b.masks[f])
                    if flat.sum() > 1000:
                        r["hanover_residue_pct"] = hanover_residue(
                            grid[1], grid[2], flat)
                    if f == bench.GREEN_EDGES:
                        r["edge_rise_rows"] = edge_rise(grid[1], grid[2],
                                                        b.truth[f])
                        r["edge_dots_codes"] = bench.edge_dots(
                            grid[0], b.truth[f][0])
                    results[key].setdefault(name, {})[still] = r
            print(key, "done", flush=True)
            if args.json:
                with open(args.json, "w") as out:
                    json.dump(results, out, indent=1)


if __name__ == "__main__":
    sys.exit(main())
