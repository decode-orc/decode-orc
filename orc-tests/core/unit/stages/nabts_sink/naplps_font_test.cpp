/*
 * File:        naplps_font_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NAPLPS character faces and the choice
 *              between them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_font.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "vbi-services/nabts_page.h"

namespace orc {
namespace {

/// The face a receiver at |grid| draws a |columns| by |rows| field from, named.
std::string face_for(const NaplpsRenderGrid& grid, int columns, int rows) {
  return naplps_font_face_for_field(grid, columns, rows).name;
}

// ---------------------------------------------------------------------------
// The faces themselves
// ---------------------------------------------------------------------------

// A page is drawn once and read at whatever resolution the reader picked, so a
// character legible on one receiver must not vanish on another: every face has
// to carry what the first one does.
TEST(NaplpsFontTest, EveryFaceCarriesTheSameRepertoireAsTheFirst) {
  ASSERT_GT(naplps_font_face_count(), 1u);

  const NaplpsFontFace& reference = naplps_font_face(0);
  ASSERT_GT(reference.count, 0u);

  for (size_t index = 1; index < naplps_font_face_count(); ++index) {
    const NaplpsFontFace& face = naplps_font_face(index);
    EXPECT_EQ(face.count, reference.count) << face.name;
    for (size_t entry = 0; entry < reference.count; ++entry) {
      const char32_t code = reference.codes[entry];
      EXPECT_NE(naplps_face_pattern(face, code), nullptr)
          << face.name << " has no pattern for code point " << code;
    }
  }
}

// ISO 8859-1's printable positions hold X3.110's primary set (§5.1) and the
// Latin-1 half of what its supplementary set (§5.2) spells and composes.
TEST(NaplpsFontTest, TheRepertoireCoversThePrintableLatinPositions) {
  for (size_t index = 0; index < naplps_font_face_count(); ++index) {
    const NaplpsFontFace& face = naplps_font_face(index);
    for (char32_t code = 0x20; code <= 0x7E; ++code) {
      EXPECT_NE(naplps_face_pattern(face, code), nullptr)
          << face.name << " at " << code;
    }
    for (char32_t code = 0xA0; code <= 0xFF; ++code) {
      EXPECT_NE(naplps_face_pattern(face, code), nullptr)
          << face.name << " at " << code;
    }
    // A code point outside it draws nothing rather than drawing rubbish.
    EXPECT_EQ(naplps_face_pattern(face, 0x0080), nullptr) << face.name;
    EXPECT_EQ(naplps_face_pattern(face, 0x20AC), nullptr) << face.name;
  }
}

// A pattern row is read by testing bits below the face's width, so a bit set
// above it would be ink the deposit could never place — and a generator that
// mis-shifted a row would leave exactly that.
TEST(NaplpsFontTest, NoPatternHasInkOutsideItsOwnCell) {
  for (size_t index = 0; index < naplps_font_face_count(); ++index) {
    const NaplpsFontFace& face = naplps_font_face(index);
    const uint16_t beyond =
        static_cast<uint16_t>(~((1u << face.width) - 1u) & 0xFFFFu);
    for (size_t entry = 0; entry < face.count; ++entry) {
      const uint16_t* pattern = naplps_face_pattern(face, face.codes[entry]);
      ASSERT_NE(pattern, nullptr) << face.name;
      for (int row = 0; row < face.height; ++row) {
        EXPECT_EQ(pattern[row] & beyond, 0u)
            << face.name << " row " << row << " of code point "
            << face.codes[entry];
      }
    }
  }
}

// §5.2: every spacing character of the supplementary set is drawn from the
// faces — all but Table 25's line and triangle graphics at 5/6 to 5/11 and
// 6/5, which notes 1 to 7 define by the character field and the raster draws
// from it, and 7/15, which the set leaves unassigned. A character with no
// pattern would vanish from the page (§8.3.3(3) wants it presented).
TEST(NaplpsFontTest, EverySpacingSupplementaryCharacterHasAPattern) {
  for (int code = 0x21; code <= 0x7E; ++code) {
    const bool mark = code >= 0x40 && code <= 0x4F;
    const bool field_graphic = (code >= 0x56 && code <= 0x5B) || code == 0x65;
    if (mark || field_graphic) {
      continue;
    }
    const char32_t character =
        nabts_supplementary_to_unicode(static_cast<uint8_t>(code));
    for (size_t index = 0; index < naplps_font_face_count(); ++index) {
      const NaplpsFontFace& face = naplps_font_face(index);
      EXPECT_NE(naplps_face_pattern(face, character), nullptr)
          << face.name << " has no pattern for S " << (code >> 4) << "/"
          << (code & 15);
    }
  }
}

// Table 18's accented letters are held precomposed where every face holds
// them, so a receiver draws the face's own letterform; a pairing Table 18 does
// not list, or one a face lacks, is left to be laid over (§7.1).
TEST(NaplpsFontTest, Table18sAccentedLettersArePrecomposed) {
  EXPECT_EQ(naplps_font_composed(U'e', U'\u0301'), U'\u00E9');  // S 4/2 6/5
  EXPECT_EQ(naplps_font_composed(U'E', U'\u0301'), U'\u00C9');  // S 4/2 4/5
  EXPECT_EQ(naplps_font_composed(U'S', U'\u030C'), U'\u0160');  // S 4/15 5/3
  EXPECT_EQ(naplps_font_composed(U'u', U'\u030B'), U'\u0171');  // S 4/13 7/5
  EXPECT_EQ(naplps_font_composed(U'I', U'\u0307'), U'\u0130');  // S 4/7 4/9
  // Not in Table 18.
  EXPECT_EQ(naplps_font_composed(U'x', U'\u0301'), U'\0');
  EXPECT_EQ(naplps_font_composed(U'i', U'\u0307'), U'\0');
  // In Table 18 (S 4/2 6/7), but the 6 by 10 face has no g with acute.
  EXPECT_EQ(naplps_font_composed(U'g', U'\u0301'), U'\0');
  for (size_t index = 0; index < naplps_font_face_count(); ++index) {
    EXPECT_NE(naplps_face_pattern(naplps_font_face(index), U'\u0160'), nullptr);
  }
}

// Every one of the sixteen marks of Tables 26 and 27 can be drawn both ways:
// over a letter, and on its own as Table 26's mark with SPACE. Unicode has a
// spacing form of every mark but the vector overbar (4/0) and the slant
// (4/9), which are drawn as the overlay alone.
TEST(NaplpsFontTest, EveryMarkHasAnOverlayAndASpacingForm) {
  for (uint8_t code = 0x40; code <= 0x4F; ++code) {
    const char32_t mark = nabts_supplementary_to_unicode(code);
    const NaplpsMarkForms* forms = naplps_font_mark_forms(mark);
    ASSERT_NE(forms, nullptr) << "S 4/" << (code & 15);
    EXPECT_EQ(forms->mark, mark);
    const bool unicode_has_spacing = code != 0x40 && code != 0x49;
    EXPECT_EQ(forms->spacing != 0, unicode_has_spacing)
        << "S 4/" << (code & 15);
    for (size_t index = 0; index < naplps_font_face_count(); ++index) {
      const NaplpsFontFace& face = naplps_font_face(index);
      EXPECT_NE(naplps_face_pattern(face, forms->overlay), nullptr)
          << face.name << " S 4/" << (code & 15);
      if (forms->spacing != 0) {
        EXPECT_NE(naplps_face_pattern(face, forms->spacing), nullptr)
            << face.name << " S 4/" << (code & 15);
      }
    }
  }
  EXPECT_EQ(naplps_font_mark_forms(U'e'), nullptr);
  // The acute's spacing form is Table 26's "S 4/2 2/0".
  EXPECT_EQ(naplps_font_mark_forms(U'\u0301')->spacing, U'\u00B4');
}

// Table D1 item 2 puts the visual baseline at 20% of the character field's
// height, and every face of the normal generator puts its own there, so a
// pattern that fills a field lands on the field's baseline. The larger
// generator's cell is the receivers' 8 by 12, with its capitals on the third
// row from the foot, as both firmwares draw it.
TEST(NaplpsFontTest, EveryFaceHasItsBaselineWhereTheReceiversPutIt) {
  size_t larger = 0;
  for (size_t index = 0; index < naplps_font_face_count(); ++index) {
    const NaplpsFontFace& face = naplps_font_face(index);
    if (face.larger) {
      ++larger;
      EXPECT_EQ(face.width, 8) << face.name;
      EXPECT_EQ(face.height, 12) << face.name;
      EXPECT_EQ(face.descent, 2) << face.name;
    } else {
      EXPECT_EQ(face.descent * 5, face.height) << face.name;
    }
  }
  EXPECT_EQ(larger, 1u);
  EXPECT_FALSE(naplps_font_face(0).larger);
}

/// The pattern for |code| in face |index|, drawn as one string per row.
std::vector<std::string> pattern_rows(size_t index, char32_t code) {
  const NaplpsFontFace& face = naplps_font_face(index);
  const uint16_t* pattern = naplps_face_pattern(face, code);
  std::vector<std::string> out;
  for (int row = 0; row < face.height; ++row) {
    std::string line;
    for (int column = 0; column < face.width; ++column) {
      line.push_back(
          (pattern[row] & (1u << (face.width - 1 - column))) != 0u ? '#' : '.');
    }
    out.push_back(line);
  }
  return out;
}

// The 6 by 10 face upstream draws a full stop as a five-pixel diamond
// straddling the baseline, and its colon and semicolon from the same mark,
// which is a terminal font's device for a cell only ten rows tall. On a page —
// where a line of leader dots is a common device — it reads as a row of
// crosses, so the generator substitutes the square block the finer faces use.
// A regeneration that dropped the substitution would put the crosses back.
TEST(NaplpsFontTest, TheSmallestFaceStopsWithADotRatherThanACross) {
  EXPECT_EQ(pattern_rows(0, U'.'),
            (std::vector<std::string>{"......", "......", "......", "......",
                                      "......", "......", "..##..", "..##..",
                                      "......", "......"}));
  EXPECT_EQ(pattern_rows(0, U':'),
            (std::vector<std::string>{"......", "......", "......", "..##..",
                                      "..##..", "......", "..##..", "..##..",
                                      "......", "......"}));
  // The semicolon keeps the tail that tells it apart from a colon.
  EXPECT_EQ(pattern_rows(0, U';'),
            (std::vector<std::string>{"......", "......", "......", "..##..",
                                      "..##..", "......", "..##..", "..#...",
                                      ".#....", "......"}));
}

// The larger face is the TTX 650's medium face to the pixel: its capitals are
// 5 by 9, one column in and on the third row from the foot of the 8 by 12
// cell, and it stops with the same square block as the 6 by 10.
TEST(NaplpsFontTest, TheLargerFaceDrawsTheReceiversCapitals) {
  size_t index = 0;
  while (!naplps_font_face(index).larger) {
    ++index;
  }
  EXPECT_EQ(pattern_rows(index, U'H'),
            (std::vector<std::string>{"........", ".#...#..", ".#...#..",
                                      ".#...#..", ".#...#..", ".#####..",
                                      ".#...#..", ".#...#..", ".#...#..",
                                      ".#...#..", "........", "........"}));
  EXPECT_EQ(pattern_rows(index, U'E'),
            (std::vector<std::string>{"........", ".#####..", ".#......",
                                      ".#......", ".#......", ".####...",
                                      ".#......", ".#......", ".#......",
                                      ".#####..", "........", "........"}));
  EXPECT_EQ(pattern_rows(index, U'.'),
            (std::vector<std::string>{"........", "........", "........",
                                      "........", "........", "........",
                                      "........", "........", "...##...",
                                      "...##...", "........", "........"}));
}

// ---------------------------------------------------------------------------
// Choosing a face for a character field
// ---------------------------------------------------------------------------

constexpr const char* kLarger = "misc-fixed 6x13 in an 8x12 cell";

// §5.1 leaves the patterns to the receiver "for a given display resolution",
// and the receivers of the day held two: a normal face on a 6 by 10 cell and a
// larger one on an 8 by 12. The Norpak TTX 650 draws a field in whichever
// leaves fewer of its columns and rows over at whole multiples, and the larger
// where the two leave the same — measured on its firmware, field by field.
TEST(NaplpsFontTest, TheReferenceReceiverChoosesBetweenItsTwoGenerators) {
  // The default field, and the character field sizes §6.2.7.6-10 select.
  EXPECT_EQ(face_for(kNaplpsGridReference, 6, 10), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridReference, 3, 10), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridReference, 6, 20), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridReference, 13, 20), "misc-fixed 6x10");
  // Heading sizes services use: the larger face.
  EXPECT_EQ(face_for(kNaplpsGridReference, 8, 12), kLarger);
  EXPECT_EQ(face_for(kNaplpsGridReference, 8, 13), kLarger);
  // Doubled where it divides the field better than the normal face does.
  EXPECT_EQ(face_for(kNaplpsGridReference, 16, 24), kLarger);
  EXPECT_EQ(face_for(kNaplpsGridReference, 40, 60), kLarger);
  // The normal face where that leaves less over: 12 by 12 is two normal cells
  // across with two rows spare, against four columns spare in the larger.
  EXPECT_EQ(face_for(kNaplpsGridReference, 12, 12), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridReference, 12, 18), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridReference, 12, 20), "misc-fixed 6x10");
  // A tie goes to the larger: 16 by 40 leaves four either way.
  EXPECT_EQ(face_for(kNaplpsGridReference, 16, 40), kLarger);
  EXPECT_EQ(face_for(kNaplpsGridReference, 12, 24), kLarger);
  // A field narrower than the larger cell keeps the normal face however tall.
  EXPECT_EQ(face_for(kNaplpsGridReference, 6, 12), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridReference, 7, 24), "misc-fixed 6x10");
}

// The choice is the reference receiver's at every grid, so a heading stays
// larger than the text around it; the larger generator has no finer face, so a
// finer receiver draws it at its multiples.
TEST(NaplpsFontTest, AFinerReceiverKeepsTheReferenceReceiversChoice) {
  EXPECT_EQ(face_for(kNaplpsGridTwice, 16, 24), kLarger);
  EXPECT_EQ(face_for(kNaplpsGridThrice, 24, 36), kLarger);
  EXPECT_EQ(face_for(kNaplpsGridThrice, 24, 39), kLarger);
  EXPECT_EQ(face_for(kNaplpsGridTwice, 12, 20), "misc-fixed 10x20");
}

// A receiver above the reference grid held a character generator with finer
// patterns, so the default field — 12.8 by 20 cells at twice the reference
// grid, 19.2 by 30 at three times — comes from a finer face rather than from a
// magnified 6 by 10.
TEST(NaplpsFontTest, AFinerReceiverDrawsTheDefaultFieldFromAFinerFace) {
  EXPECT_EQ(face_for(kNaplpsGridTwice, 13, 20), "misc-fixed 10x20");
  EXPECT_EQ(face_for(kNaplpsGridThrice, 19, 30), "misc-fixed 9x15");
}

// The face has to divide the field a whole number of times: the pattern is
// deposited by sampling the field back into it, so a field that is not a whole
// multiple duplicates some of the face's rows and not others, which is what
// breaks a letterform. At 19 by 30 the 10 by 20 fits and has the most elements
// of any face that does, and is still not the one chosen — 30 is not a
// multiple of 20.
TEST(NaplpsFontTest, AFaceMustDivideTheFieldRatherThanMerelyFitIt) {
  EXPECT_EQ(face_for(kNaplpsGridThrice, 19, 30), "misc-fixed 9x15");
  EXPECT_EQ(face_for(kNaplpsGridThrice, 19, 60), "misc-fixed 10x20");
  EXPECT_EQ(face_for(kNaplpsGridThrice, 19, 45), "misc-fixed 9x15");
}

// Where no face divides the field, the finest one that fits is drawn instead:
// some resampling is unavoidable, and a coarser face would not avoid it.
TEST(NaplpsFontTest, AFieldNoFaceDividesTakesTheFinestThatFits) {
  EXPECT_EQ(face_for(kNaplpsGridTwice, 13, 22), "misc-fixed 10x20");
  EXPECT_EQ(face_for(kNaplpsGridTwice, 9, 17), "misc-fixed 9x15");
}

// A field below the smallest cell the plugin carries still draws: the 6 by 10
// is squeezed into it, which is what a receiver with one generator and a field
// under it does. Table D1 item 5(g) puts the standard's own floor at
// dx = 6/256 by dy = 8/256, which is under the face's ten rows.
TEST(NaplpsFontTest, AFieldBelowTheSmallestCellFallsBackToTheSixByTen) {
  EXPECT_EQ(face_for(kNaplpsGridTwice, 6, 8), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridTwice, 4, 6), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridThrice, 1, 1), "misc-fixed 6x10");
}

// A field narrow for its height takes a face narrow enough to sit in it, so a
// condensed field does not pick a face it would have to lose columns from.
TEST(NaplpsFontTest, ANarrowFieldTakesAFaceThatFitsAcross) {
  EXPECT_EQ(face_for(kNaplpsGridTwice, 6, 20), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridThrice, 9, 30), "misc-fixed 9x15");
}

}  // namespace
}  // namespace orc
