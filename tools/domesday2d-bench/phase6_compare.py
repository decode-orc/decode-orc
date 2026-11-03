#!/usr/bin/env python3
#
# File:        phase6_compare.py
# Module:      tools/domesday2d-bench
# Purpose:     Domesday2D Phase 6: decode-orc's domesday2d decoder and
#              channel_equalise stage against their prototypes
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Usage:
#   phase6_compare.py decoder YUV_DIR
#       YUV_DIR/pal_stills_clean_domesday2d.yuv and pal_stills_domesday2d.yuv,
#       the video sink's decodes of the still bench (raw yuv,
#       output_padding 1), against adaptive.decode_separated() with the
#       masked averaging and the per-frame Hanover rotation
#       (hanover.frame_matrix): the largest difference per plane in 16-bit output
#       codes, and each still's CIEDE2000 from both.
#   DOMESDAY2D_P5_DIR=P5 phase6_compare.py equalise CASE SMEAR TAU OUT.cvbs
#       channel_equalise's output OUT.cvbs on Phase 5's render CASE, at the
#       smear SMEAR / TAU, against phase5_decoders.stage(): the largest
#       difference per frame in 10-bit codes.
#
# The decoder's output clips Y at codes 256 and 65216, where the prototype
# does not, so Y is compared inside that range only; and on the
# noise-free bench, U and V tie exactly in many transform bins, which the
# prototype breaks by its FFT's rounding and the decoder breaks towards U.

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adaptive  # noqa: E402
import bench  # noqa: E402
import hanover  # noqa: E402
import phase5_decoders  # noqa: E402
import transform  # noqa: E402


def compare_decoder(yuv_dir):
    b = bench.Bench()
    u_scale = (1 - 0.114) * bench.KB / (112 * 256)
    v_scale = (1 - 0.299) * bench.KR / (112 * 256)
    for case, stem in (("clean", "pal_stills_clean"),
                       ("noisy", "pal_stills")):
        path = os.path.join(yuv_dir, stem + "_domesday2d.yuv")
        for f, still in b.stills:
            raster, c, phi, s = transform.separate(
                bench.composite(case, f), "frame", ytile=32)
            # Phase 7: the decoder's default per-frame rotation, with no
            # hanover_* matrix set.
            m = hanover.frame_matrix(*adaptive.demodulate_rows(c, phi, s), s,
                                     np.zeros((2, 2)))
            kw = {"correction": hanover.corrector(m)} if m.any() else {}
            d = adaptive.decode_separated(raster, c, phi, s,
                                          prefilter=hanover.masked_average(),
                                          **kw)
            proto = d.truth_grid()
            got = bench.decoded(path, f)
            # Y where the output can hold it: the sink clips to codes
            # 256-65216 (Y -15/219 to 238.75/219).
            inside = (proto[0] > -15.0 / 219) & (proto[0] < 238.75 / 219)
            dy = np.where(inside, (got[0] - proto[0]) * 219 * 256, 0.0)
            du = (got[1] - proto[1]) / u_scale
            dv = (got[2] - proto[2]) / v_scale
            e_got = bench.measure(got, b.features[f], b.masks[f])
            e_proto = bench.measure(proto, b.features[f], b.masks[f])
            print(f"{case:5s} {still:20s} max |dY| {np.abs(dy).max():8.1f} "
                  f"|dU| {np.abs(du).max():7.1f} |dV| {np.abs(dv).max():7.1f}"
                  f"  dE {e_got['delta_e_mean']:.3f} / "
                  f"{e_proto['delta_e_mean']:.3f}", flush=True)


def compare_equalise(case, fraction, tau, path):
    out = np.memmap(path, dtype="<i2", mode="r")
    for f in range(bench.FRAMES):
        ref = phase5_decoders.stage(bench.composite(case, f),
                                    (fraction, tau))
        got = np.asarray(out[f * bench.CVBS_FRAME:(f + 1) * bench.CVBS_FRAME],
                         dtype=np.float64)
        worst = 0.0
        at = 0
        for line in range(625):
            n = 1137 if line in (312, 624) else bench.LINE
            worst = max(worst, float(np.abs(got[at:at + n] -
                                            ref[line, :n]).max()))
            at += n
        print(f"frame {f:2d} max difference {worst:.3f} codes", flush=True)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "decoder":
        compare_decoder(sys.argv[2])
    elif len(sys.argv) == 6 and sys.argv[1] == "equalise":
        compare_equalise(sys.argv[2], float(sys.argv[3]), float(sys.argv[4]),
                         sys.argv[5])
    else:
        print(__doc__ or "see the header comment", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
