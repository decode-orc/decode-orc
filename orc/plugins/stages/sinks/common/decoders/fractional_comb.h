/*
 * File:        fractional_comb.h
 * Module:      orc-core
 * Purpose:     NTSC 2D line comb along the picture's own direction, at any
 *              slope
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef FRACTIONAL_COMB_H
#define FRACTIONAL_COMB_H

#include <cstdint>
#include <utility>
#include <vector>

#include "comb_plane.h"

namespace ntsc_comb {

// A line comb cancels luma by combining lines that carry the same picture. At
// an edge running d samples per field line, the same picture point is d
// samples along on the line above and -d on the line below, d rarely a whole
// number.
//
// For each slope d (from -kMaxSlope to kMaxSlope in kSlopeStep steps) a short
// filter on the current line and the lines above and below is fitted by least
// squares so that, over the chroma band, luma running along the edge comes out
// as zero and chroma running along it comes out unchanged (its carrier stays
// on the sample grid, so the neighbour lines' chroma is the current line's
// inverted, only its baseband moved by d). Each fit spans a small range of
// slopes, so it still cancels luma when the slope estimate is a little off;
// d = 0 is close to the 3-line comb. Near +-2 samples per field line chroma
// and luma line up the same way and no filter separates them, which the fit's
// residual and noise gain show; those slopes are not trusted.
//
// Per sample the luma's structure tensor (on the chroma-free 4-sample mean)
// gives the slope and how one-directional the picture is. The two nearest
// filters are interpolated; where the picture has no clear direction (texture,
// flat) or the slope no trusted filter, the 3-line comb is used. Nearer
// horizontal, where the 3-line comb would average the different chroma of
// the lines either side of the edge, the 2-line comb with the neighbour line
// that matches is used, or where neither does, the 3-line comb plus the
// near-subcarrier part of what it rejected.
//
// Thread safety: the filters are built once, on first use, and only read
// afterwards; separate() may be called from any number of threads.
class FractionalComb {
 public:
  // The chroma of |frame| (the composite, flat rows), over the whole frame.
  // Rows without both same-field neighbours inside |picture| take the
  // bandpass alone. |lineCombed|, if given, receives the same with the
  // 3-line comb on horizontal edges too: its errors there (chroma averaged
  // across the edge) show as chroma that fails to invert from line to line,
  // those of the combs taken there instead (luma detail along the line taken
  // for chroma) need not. |measured|, if
  // given, is the whole signal the picture's direction is measured on, where
  // |frame| carries only the chroma (a Y/C source's Y + C, and its C).
  static Plane separate(const Plane& frame, const FieldLayout& layout,
                        const ActivePicture& picture,
                        Plane* lineCombed = nullptr,
                        const Plane* measured = nullptr);

  // One slope's filter: tap weights at (line offset, sample offset)
  // positions, and how far it can be trusted (0..1).
  struct Design {
    double slope = 0.0;
    std::vector<std::pair<std::pair<int32_t, int32_t>, double>> taps;
    double residual = 0.0;
    double noiseGain = 0.0;
    double quality = 0.0;
  };

  // The filter fitted for slope |d|. Exposed for tests.
  static Design design(double d);

  // Slopes the filters are fitted at.
  static constexpr double kMaxSlope = 4.0;
  static constexpr double kSlopeStep = 0.125;
};

}  // namespace ntsc_comb

#endif  // FRACTIONAL_COMB_H
