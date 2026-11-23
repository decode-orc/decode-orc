/*
 * File:        ntsc3d_comb.h
 * Module:      orc-core
 * Purpose:     NTSC motion-adaptive 3D comb: frame comb where the picture is
 *              still and the 2D comb would get it wrong, 2D comb elsewhere
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef NTSC3D_COMB_H
#define NTSC3D_COMB_H

#include "comb_plane.h"

namespace ntsc_comb {

// The chroma of one frame from it and up to two frames either side.
//
// A frame comb takes a still picture's chroma exactly (NTSC's subcarrier
// inverts from one frame to the next), where a 2D comb mistakes fine luma
// for chroma; on moving picture it leaves dots, and the 2D comb is right. So
// for each sample this decides how still the picture is, against which
// neighbours, and whether a frame comb is needed there at all:
//
// - Still: the low-frequency luma change from each neighbour against a noise
//   floor. The floor is the picture's own (the quietest frame pair's 25th
//   percentile) held to 0.5..2 times the disc's, measured on the sync tip,
//   which motion cannot touch; it rises with the local chroma-band
//   amplitude, by a slope measured between frames of the same chroma phase
//   and held to the colour burst's.
// - Averaged against both neighbours where both are still, or the picture
//   moves steadily (the frame is the mean of its neighbours, and of the
//   frames two away), as long as the two frame combs agree; against one
//   still neighbour otherwise, if the frame beyond it (the current frame's
//   chroma phase) matches in the chroma band.
// - No frame comb where the current frame is a flash: the frames either side
//   match each other and the current frame matches neither frame two away,
//   or the frames two away match each other far better than the current
//   frame matches either.
// - Only where needed: where the 2D comb's chroma inverts cleanly from line
//   to line, as chroma does, the 2D comb is as right and smoother (a frame
//   comb keeps the luma's noise near the subcarrier as a fine dot pattern);
//   and only where the frames two away confirm the detail stays.
// - The weights are smoothed along the line, so the output never switches
//   between combs from one sample to the next.
//
// Elsewhere the 2D comb is FractionalComb, a line comb along each edge's own
// slope.
//
// Thread safety: stateless; separate() may run on several threads at once.
class Ntsc3dComb {
 public:
  struct Settings {
    // Low-frequency luma change, in multiples of the measured noise floor,
    // still counted as still. Higher frame-combs slower motion too.
    double motionThreshold = 4.0;
    // How far the 2D comb's chroma must fail to invert from line to line, in
    // multiples of its noise, before the frame comb is used. Lower frame-
    // combs more widely (fewer 2D errors, more fine luma noise); 0 frame-combs
    // wherever the picture is still.
    double detailThreshold = 2.0;
  };

  // The frames, as composite in flat rows; nullptr where a frame does not
  // exist (the start or end of a sequence, a missing or out-of-phase frame).
  // The current frame is required.
  struct Frames {
    const Plane* before2 = nullptr;
    const Plane* before = nullptr;
    const Plane* current = nullptr;
    const Plane* after = nullptr;
    const Plane* after2 = nullptr;
  };

  struct Result {
    // The current frame's chroma, whole frame: the 2D comb's beyond a few
    // lines and samples of the picture.
    Plane chroma;
    Plane frameCombWeight;  // 0 (2D comb) .. 1 (frame comb), per sample
  };

  static Result separate(const Frames& frames, const FieldLayout& layout,
                         const ActivePicture& picture,
                         const Settings& settings);

  // As above, for a channel that carries only the chroma (a Y/C source's C):
  // |frames| are separated, while the motion, and the picture's direction
  // for the 2D comb, are measured on |measured|, the whole signal (Y + C) of
  // the same frames, present where |frames| are. On the chroma alone a scene
  // cut or moving luma would read as still, and every edge as flat.
  static Result separate(const Frames& frames, const Frames& measured,
                         const FieldLayout& layout,
                         const ActivePicture& picture,
                         const Settings& settings);
};

}  // namespace ntsc_comb

#endif  // NTSC3D_COMB_H
