/*
 * File:        naplps_render_grid.h
 * Module:      nabts_sink stage plugin
 * Purpose:     The physical pixel grid a NAPLPS page is rendered against
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_SINK_NAPLPS_RENDER_GRID_H
#define ORC_NABTS_SINK_NAPLPS_RENDER_GRID_H

#include <cmath>
#include <cstdint>
#include <string>

#include "vbi-services/nabts_page.h"

namespace orc {

/**
 * @brief The receiver resolution a page is drawn against
 *
 * X3.110 draws into an abstract unit screen, but its rendering rules are stated
 * against a receiver's physical pixels: §5.3.2.2.6 defines the logical pel by
 * "all of those pixels that lie under any portion of the logical pel as it is
 * mapped to the display screen", and guarantees it "will always map to at least
 * one and possibly many display pixels". Stroke width, line texture dot and
 * dash sizes (§5.3.2.4.2), hatch spacing (§5.3.2.4.4) and the raster step of
 * INCREMENTAL POINT are all multiples of that pel, so none of them can be
 * resolved without knowing how many pixels the receiver had.
 *
 * A decode therefore picks the receiver it is emulating. The three pixel modes
 * differ only in that resolution; the vector mode draws the reference
 * receiver's page as resolution-independent geometry, using that receiver's
 * grid for what §5.3.2.2.6 and Appendix B measure in its pixels — the pel's
 * floor, the pixel a point is plotted in — which is what a renderer needs to
 * put a stroke where and as heavy as the receiver did without inventing a
 * thickness of its own.
 *
 * The finer two grids are whole multiples of the reference one. A page is
 * authored against the reference — that is what the service reference model is
 * — so anything the author placed on it lands on a pixel boundary of a whole
 * multiple and between pixels of anything else, and a character pattern, which
 * is a bitmap of a fixed cell, only scales cleanly by a whole number. A grid at
 * some fraction in between draws the same page with its strokes unevenly
 * thickened and its letterforms broken.
 */
enum class NaplpsRenderMode : uint8_t {
  /// The Table D1 receiver: what a set-top decoder of the period displayed.
  kReference = 0,
  kTwice = 1,
  kThrice = 2,
  /// The reference receiver's geometry, drawn as vectors rather than pixels.
  /// (Named when it was resolved against the twice-reference grid; the stored
  /// name follows the grid, see naplps_render_mode_name().)
  kTwiceVector = 3,
};

/**
 * @brief A physical pixel grid covering the guaranteed-visible display area
 *
 * Width and height count pixels across and down the display area — the lower
 * @ref kNabtsDisplayAreaHeight of the unit screen (Table D1 item 10), not the
 * whole unit screen.
 */
struct NaplpsRenderGrid {
  int width = 0;
  int height = 0;

  /// Width of one pixel in unit-screen x.
  constexpr double pitch_x() const { return 1.0 / width; }
  /// Height of one pixel in unit-screen y.
  constexpr double pitch_y() const { return kNabtsDisplayAreaHeight / height; }
};

// Table D1 item 10: "Resolution shall be on the order of 256 pixels horizontal
// by 200 pixels vertical", with unit-screen x 0 to 1 and y 0 to 0.78125
// visible. 0.78125 * 256 = 200 exactly, so the pixel is square in unit space —
// which is the property the other two grids are chosen to keep.
constexpr NaplpsRenderGrid kNaplpsGridReference{256, 200};

/// The reference receiver's pixels to the unit screen, on both axes.
constexpr double kNaplpsReferencePixelsPerUnit = 256.0;

/// Whole pixels of the reference receiver |extent| of the unit screen covers,
/// a field size arithmetic left a hair under a whole number of pixels counting
/// as that number.
inline int naplps_reference_pixels(double extent) {
  return static_cast<int>(
      std::floor(std::fabs(extent) * kNaplpsReferencePixelsPerUnit + 1e-6));
}

/// The smallest character field Table D1 items 2 and 5(g) have characters
/// presented in, in reference pixels: dx = 6/256, dy = 8/256.
constexpr int kNaplpsSmallestFieldColumns = 6;
constexpr int kNaplpsSmallestFieldRows = 8;

// The same grid at 2x. Appendix D Scope note (2) names 512 horizontal pixels
// as its example of a receiver that "may exceed the requirements of the
// respective SRM" and so "may produce more pleasing images".
constexpr NaplpsRenderGrid kNaplpsGridTwice{512, 400};

// The same grid at 3x, for reading a page more finely still. The standard
// names no such receiver, and neither did anyone build one; it is offered
// because a page drawn for the reference model is a drawing rather than a
// photograph, and the finest whole multiple shows most of what the drawing
// says.
constexpr NaplpsRenderGrid kNaplpsGridThrice{768, 600};

/// The grid @p mode renders against.
constexpr NaplpsRenderGrid naplps_render_grid(NaplpsRenderMode mode) {
  switch (mode) {
    case NaplpsRenderMode::kReference:
    // The vector mode is the reference receiver's page as shapes: its strokes
    // are as heavy as that receiver drew them and lie on the pixels it lit,
    // which is how both receivers of the day were measured to draw them.
    case NaplpsRenderMode::kTwiceVector:
      return kNaplpsGridReference;
    case NaplpsRenderMode::kTwice:
      return kNaplpsGridTwice;
    case NaplpsRenderMode::kThrice:
      return kNaplpsGridThrice;
  }
  return kNaplpsGridReference;
}

/// Whether @p mode deposits pixels rather than emitting geometry.
constexpr bool naplps_mode_emits_pixels(NaplpsRenderMode mode) {
  return mode != NaplpsRenderMode::kTwiceVector;
}

/**
 * @brief The shape the display area is drawn at, as height over width
 *
 * §4.2.2 puts the unit screen's visible portion in "the display area, which is
 * a rectangular area of the device's physical display screen", and its worked
 * example is a television set whose display area "has the same 4:3 aspect
 * ratio". Table D1 item 10 requires exactly that portion — x 0 to 1, y 0 to
 * 0.78125 — to be visible in it. The pixels of every grid above are therefore
 * not square on screen: 256 across and 200 down displayed at 4:3 gives each
 * one an aspect of about 1.042, which is the rectangular pixel a 525-line
 * receiver drew.
 */
constexpr double kNaplpsDisplayAspectHeight = 3.0 / 4.0;

/**
 * @brief The stable name @p mode is configured and stored under
 *
 * The grid itself, because the grid is what the setting chooses. The line-count
 * shorthand a television is described by — 240p, 480p — names the raster a
 * receiver scanned rather than the buffer it drew into, and the two are not the
 * same number: the reference model's buffer is 200 rows whatever the set
 * displaying it scanned. Naming these after the sets they suit invites the
 * reading that 240p means 240 rows, which none of them has.
 */
inline std::string naplps_render_mode_name(NaplpsRenderMode mode) {
  switch (mode) {
    case NaplpsRenderMode::kReference:
      return "256 x 200";
    case NaplpsRenderMode::kTwice:
      return "512 x 400";
    case NaplpsRenderMode::kThrice:
      return "768 x 600";
    case NaplpsRenderMode::kTwiceVector:
      return "256 x 200 (vector)";
  }
  return "256 x 200";
}

/**
 * @brief The mode @p name selects, or @p fallback if it names none
 *
 * A name that is not one of the four is not an error worth refusing a decode
 * over: the page still renders, at the resolution the caller asked to fall back
 * to.
 */
inline NaplpsRenderMode naplps_render_mode_from_name(
    const std::string& name,
    NaplpsRenderMode fallback = NaplpsRenderMode::kReference) {
  for (const NaplpsRenderMode mode :
       {NaplpsRenderMode::kReference, NaplpsRenderMode::kTwice,
        NaplpsRenderMode::kThrice, NaplpsRenderMode::kTwiceVector}) {
    if (naplps_render_mode_name(mode) == name) {
      return mode;
    }
  }
  // The name the vector mode was stored under while it was resolved against
  // the twice-reference grid, so whatever kept it still selects the mode.
  if (name == "512 x 400 (vector)") {
    return NaplpsRenderMode::kTwiceVector;
  }
  return fallback;
}

}  // namespace orc

#endif  // ORC_NABTS_SINK_NAPLPS_RENDER_GRID_H
