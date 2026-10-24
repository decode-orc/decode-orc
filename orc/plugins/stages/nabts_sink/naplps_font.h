/*
 * File:        naplps_font.h
 * Module:      nabts_sink stage plugin
 * Purpose:     Fixed bitmap faces for depositing NAPLPS characters into a
 *              receiver's pixel grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_SINK_NAPLPS_FONT_H
#define ORC_NABTS_SINK_NAPLPS_FONT_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "naplps_render_grid.h"

namespace orc {

/**
 * @file
 * @brief The character patterns a receiver deposits for the coded sets
 *
 * X3.110 §5.1 leaves the patterns to the receiver — "the particular patterns
 * (font) chosen for the characters are implementation-dependent and are
 * constrained only by the specified character field at each size for a given
 * display resolution" — so a decoder emulating one has to choose a font. The
 * clause makes that choice a property of the receiver rather than of the page:
 * a set with more pixels held a character generator with finer patterns, and
 * magnifying a coarse one is not the same thing. The plugin therefore carries
 * several faces and picks between them by the field a character is drawn into
 * (@ref naplps_font_face_for_field).
 *
 * Every face is a member of the X11 "misc-fixed" family, which is public
 * domain (X.Org font-misc-misc; "Public domain font. Share and enjoy.") and so
 * redistributable under this project's GPL-3.0-or-later:
 *
 * - 6 by 10, from `6x10.bdf`. The cell Appendix B arrives at from first
 *   principles — "psychological studies have shown that the most readable
 *   characters in this size range are 6 pixels by 10 pixels" — and the one a
 *   256 by 200 receiver gives the default character field. It is legible at
 *   the smallest field the standard requires (Table D1 item 5(g): dx = 6/256,
 *   dy = 8/256), which is what the reference receiver needs of it.
 * - 8 by 12, from `6x13.bdf`: the larger of the two generators the receivers
 *   of the day held (see naplps_font_face_for_field()). The 6x13's capitals are
 *   the Norpak TTX 650's own, pattern for pattern; its top row, which only the
 *   accents of five characters reach, is dropped to make the receivers' 12-row
 *   cell, and it is centred in their eight columns.
 * - 9 by 15, from `9x15.bdf`.
 * - 10 by 20, from `10x20.bdf`.
 *
 * The tables are generated from those BDF sources, which are kept unchanged
 * in `tools/misc-fixed/`, by `tools/generate_naplps_font.py`, so what is in
 * the tree can be checked against what it came from rather than trusted.
 * Beyond the 8 by 12's cell, the generator makes one documented departure from
 * upstream: the 6 by 10 and the 6x13 draw a full stop as a five-pixel diamond
 * straddling the baseline, and build the colon and semicolon from the same
 * mark, which on a page of leader dots reads as a row of crosses. Those three
 * glyphs are replaced by the square block the finer faces use, scaled to the
 * cell. The substitutions are stated in the generator, and are the only cells
 * in any face that are not upstream's.
 *
 * The repertoire is the one X3.110 §7.1 defines, as far as every face holds
 * it: the primary set, the spacing characters of the supplementary set (§5.2,
 * Figure 8) apart from Table 25's line and triangle graphics, which the
 * raster draws from the character field itself, Table 18's accented letters,
 * and the spacing and overlay forms of the non-spacing marks of Tables 26
 * and 27. Every face carries exactly the same set, so a character legible on
 * one receiver is legible on all of them.
 *
 * Patterns are keyed by Unicode code point rather than by code position,
 * because the mapping from a NAPLPS code position to a character is already
 * made once for text extraction (nabts_primary_to_unicode() and
 * nabts_supplementary_to_unicode()); keying on the result is what stops the
 * page that is drawn and the text that is searched disagreeing about what a
 * byte meant.
 */

/**
 * @brief One fixed bitmap face
 *
 * @ref rows holds @ref count patterns of @ref height rows each, in the order
 * @ref codes lists them. A pattern's rows run top down, and within a row bit
 * (@ref width - 1) is the leftmost column — so a row reads left to right as it
 * is written.
 */
struct NaplpsFontFace {
  /// The upstream face, for diagnostics and for naming it in a test failure.
  const char* name;
  int width;
  int height;
  /// Rows of the cell below the baseline (the face's FONT_DESCENT). Every face
  /// but the 8 by 12 puts its baseline at a fifth of its height, which is
  /// where Table D1 item 2 puts it in the character field; the 8 by 12 has the
  /// receivers' two rows of twelve. A pattern placed in a field taller than it
  /// is raised by this to keep its baseline on the field's.
  int descent;
  const char32_t* codes;
  size_t count;
  const uint16_t* rows;
  /// The receivers' larger generator, drawn in the fields it fills better
  /// than the normal one (naplps_font_face_for_field()).
  bool larger;
};

/**
 * @brief How a receiver draws one non-spacing mark of Tables 26 and 27
 *
 * §5.2 makes column 4 of the supplementary set non-spacing, and §7.2 has the
 * mark's code precede the letter it modifies. Where every face holds the
 * accented letter precomposed (naplps_font_composed()), that pattern is drawn;
 * otherwise the letter is drawn with @ref overlay laid over it. A mark
 * followed by no letter — Table 26's "S 4/x 2/0", the mark with SPACE — is
 * drawn in @ref spacing on its own.
 */
struct NaplpsMarkForms {
  /// The mark, as the combining character nabts_supplementary_to_unicode()
  /// gives for it.
  char32_t mark;
  /// The pattern laid over a letter: the combining form where every face
  /// holds one, and the spacing form otherwise.
  char32_t overlay;
  /// The mark with SPACE, as the Unicode spacing character it is, or 0 for
  /// the two marks of Table 27 Unicode has no spacing form of (the vector
  /// overbar and the slant), which are drawn as @ref overlay alone.
  char32_t spacing;
};

/// The precomposed letter every face holds for @p base with the non-spacing
/// @p mark over it (X3.110 Table 18), or 0 where they hold none.
char32_t naplps_font_composed(char32_t base, char32_t mark);

/// How @p mark is drawn, or nullptr where it is not one of the sixteen
/// non-spacing marks.
const NaplpsMarkForms* naplps_font_mark_forms(char32_t mark);

/// How many faces the plugin carries.
size_t naplps_font_face_count();

/// Face @p index, ordered by ascending cell size.
const NaplpsFontFace& naplps_font_face(size_t index);

/**
 * @brief The pattern for @p code in @p face, or nullptr where it has none
 *
 * A code point no face carries is not an error: §5.1 does not guarantee
 * "character legibility ... at all sizes, in all colors, and at all display
 * resolutions", let alone that a receiver holds every character. §8.3.3(3)
 * still has every character of the repertoire presented, so the raster draws a
 * replacement box for it rather than nothing (naplps_replacement_pattern()).
 */
const uint16_t* naplps_face_pattern(const NaplpsFontFace& face, char32_t code);

/**
 * @brief Columns and rows a @p columns by @p rows field leaves over when
 *        @p face is drawn in it at whole multiples of its cell, or -1 where
 *        the field cannot hold the cell once
 */
inline int naplps_font_leftover(const NaplpsFontFace& face, int columns,
                                int rows) {
  if (face.width > columns || face.height > rows) {
    return -1;
  }
  return columns % face.width + rows % face.height;
}

/// The larger generator's face: the first the table marks |larger|, or
/// nullptr where it has none.
inline const NaplpsFontFace* naplps_font_larger_face() {
  for (size_t index = 0; index < naplps_font_face_count(); ++index) {
    if (naplps_font_face(index).larger) {
      return &naplps_font_face(index);
    }
  }
  return nullptr;
}

/**
 * @brief The face a receiver at @p grid draws a @p columns by @p rows field
 * from
 *
 * The generator first, then the face.
 *
 * The receivers of the day held two generators, and chose between them by the
 * field. The Norpak TTX 650 and the Panasonic TU-2000x each carry a normal face
 * on a 6 by 10 cell and a larger one on an 8 by 12, and draw either at whole
 * multiples of its cell — the multiples on the two axes taken apart, the most
 * each axis holds. Driven through both firmwares with fields from 4 by 6 to
 * 32 by 40, the TTX 650 draws each field in whichever face leaves the fewest
 * columns and rows of it over, and the larger where they leave the same; the
 * TU-2000x weighs the rows alone, and the two agree on every field the
 * recordings use but a handful of pages. So an 8 by 12 or an 8 by 13 — a
 * common heading size — comes out in the larger face, a 12 by 20 in
 * the normal face doubled, and a 16 by 24 in the larger face doubled. §5.1
 * leaves the patterns to the receiver, and these are what a viewer of the day
 * saw. The choice is made at the reference grid, where the receivers drew, so
 * a page's headings are larger than its text at every resolution.
 *
 * Then the face. The larger generator is the 8 by 12 at every grid: the family
 * holds no finer face on its proportions, so a finer receiver draws it at its
 * multiples, as the receivers did. The normal generator at the reference grid
 * is the 6 by 10 and nothing else: its default character field is exactly that
 * cell, so a finer pattern is one it could never show. A receiver above the
 * reference grid holds the finer normal faces as well, and draws the finest
 * one the field can hold whose cell height divides the field's a whole number
 * of times. A pattern is laid at a whole multiple of its size, so a field that
 * is not a whole multiple of the face leaves rows to spare, and a finer face
 * that fills it is the better reading. The rule is stated on rows rather than
 * on both axes because only rows can satisfy it: every character field height
 * the standard names is a whole number of pixels on all three grids (the
 * default dy = 5/128 is 10 rows, 20 and 30), while the widths are not
 * (dx = 1/40 is 6.4 pixels, 12.8 and 19.2), so the columns a field holds —
 * @p columns is the whole number of them, the rest being space — are not a
 * multiple of any face. Where no face divides the field, the finest one that
 * fits it is drawn instead, and where none fits — a field narrower or shorter
 * than the smallest cell — the 6 by 10 is squeezed into it, down to the 6 by 8
 * Table D1 item 2 presents characters in; below that the raster presents no
 * pattern (naplps_character_too_small()).
 */
inline const NaplpsFontFace& naplps_font_face_for_field(
    const NaplpsRenderGrid& grid, int columns, int rows) {
  const NaplpsFontFace& normal = naplps_font_face(0);
  const NaplpsFontFace* larger = naplps_font_larger_face();

  // The generator, as the receivers chose it: on the field at the reference
  // grid, which every grid is a whole multiple of.
  const int factor =
      std::max(1, grid.width / std::max(1, kNaplpsGridReference.width));
  if (larger != nullptr) {
    const int as_normal =
        naplps_font_leftover(normal, columns / factor, rows / factor);
    const int as_larger =
        naplps_font_leftover(*larger, columns / factor, rows / factor);
    if (as_larger >= 0 && (as_normal < 0 || as_larger <= as_normal)) {
      return *larger;
    }
  }
  if (factor == 1) {
    return normal;
  }

  // The normal generator's face at a finer receiver.
  const NaplpsFontFace* chosen = nullptr;
  bool chosen_divides = false;
  int chosen_elements = 0;
  for (size_t index = 0; index < naplps_font_face_count(); ++index) {
    const NaplpsFontFace& face = naplps_font_face(index);
    if (face.larger || face.width > columns || face.height > rows) {
      continue;  // the other generator's, or the field cannot hold the cell
    }
    const bool divides = (rows % face.height) == 0;
    const int elements = face.width * face.height;
    if (chosen != nullptr) {
      if (chosen_divides && !divides) {
        continue;
      }
      if (divides == chosen_divides && elements <= chosen_elements) {
        continue;
      }
    }
    chosen = &face;
    chosen_divides = divides;
    chosen_elements = elements;
  }
  return chosen != nullptr ? *chosen : normal;
}

}  // namespace orc

#endif  // ORC_NABTS_SINK_NAPLPS_FONT_H
