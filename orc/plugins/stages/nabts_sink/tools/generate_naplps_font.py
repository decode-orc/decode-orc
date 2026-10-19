#!/usr/bin/env python3
#
# File:        generate_naplps_font.py
# Module:      nabts_sink stage plugin
# Purpose:     Generate naplps_font.cpp from the upstream X11 misc-fixed BDF
#              sources
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# The faces a NAPLPS receiver draws its built-in characters from are compiled
# into the plugin as tables. Generating them rather than transcribing them is
# what lets a reader check the tables against the face they came from.
#
# The BDF sources are kept beside this script in misc-fixed/, copied unchanged
# from the X.Org font-misc-misc 1.1.3 release (6x10.bdf, 6x13.bdf, 9x15.bdf and
# 10x20.bdf, with upstream's COPYING), so regenerating needs nothing outside
# the repository:
#
#   orc/plugins/stages/nabts_sink/tools/generate_naplps_font.py \
#       > orc/plugins/stages/nabts_sink/naplps_font.cpp
#   clang-format -i orc/plugins/stages/nabts_sink/naplps_font.cpp
#
# A directory holding the same four files may be named as the one argument
# instead. Regenerating from the same upstream release reproduces the committed
# file byte for byte, so a diff is the check.

import os
import sys
import unicodedata

# The faces the plugin carries, ascending: the cell each is emitted in, its
# name, the upstream face its patterns come from, how many of that face's top
# rows it drops and how many blank columns it adds on each side, and whether it
# is the receivers' larger generator (see naplps_font_face_for_field()). Every
# one is a member of the X11 "misc-fixed" family, which is public domain, so
# the whole set shares one provenance and one house style: a receiver drawing a
# page at a finer grid picks a finer member rather than a different typeface.
#
# The 8 by 12 is the larger generator both receivers of the day held beside
# the normal 6 by 10 — the Norpak TTX 650's "medium" and the Panasonic
# TU-2000x's "bold" face, on an 8 by 12 cell with a capital 9 rows tall, 2 rows
# of descent and 1 above — which is a heading size services set their titles
# in. The 6x13 is that face to the row: its capitals are the TTX 650's
# pattern for pattern. Its cell is one row taller, and the row it drops is the
# top one, which only the accents of five characters of the repertoire reach
# (d and l with caron, l with acute, U with ring, and the ring alone); those
# lose that row. Centred in eight columns, the capital stands one column in, as
# both receivers draw it.
FACES = [
    ("6x10", "misc-fixed 6x10", "6x10", 0, 0, False),
    ("8x12", "misc-fixed 6x13 in an 8x12 cell", "6x13", 1, 1, True),
    ("9x15", "misc-fixed 9x15", "9x15", 0, 0, False),
    ("10x20", "misc-fixed 10x20", "10x20", 0, 0, False),
]

# ISO 8859-1's printable positions, which cover X3.110's primary set (§5.1,
# Figure 7) and the Latin-1 half of the characters its supplementary set
# (§5.2, Figure 8) spells or composes.
LATIN_1 = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100))

# The spacing characters of the supplementary set (§5.2, Figure 8) that a face
# draws, as nabts_supplementary_to_unicode() maps them. Left out are column 4,
# the non-spacing marks (MARKS below); 5/6 to 5/11 and 6/5, Table 25's line and
# triangle graphics, which notes 1 to 7 define by the character field itself
# and the raster therefore draws from the field rather than from a face; and
# 7/15, which the set leaves unassigned.
SUPPLEMENTARY_SPACING = [
    # 2/1 - 2/15 (Tables 20, 21 and 25)
    0x00A1, 0x00A2, 0x00A3, 0x0024, 0x00A5, 0x0023, 0x00A7, 0x00A4, 0x2018,
    0x201C, 0x00AB, 0x2190, 0x2191, 0x2192, 0x2193,
    # 3/0 - 3/15 (Tables 21 to 25)
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00D7, 0x00B5, 0x00B6, 0x00B7, 0x00F7,
    0x2019, 0x201D, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    # 5/0 - 5/5 and 5/12 - 5/15 (Tables 23 to 25)
    0x2015, 0x00B9, 0x00AE, 0x00A9, 0x2122, 0x266A, 0x215B, 0x215C, 0x215D,
    0x215E,
    # 6/0 - 6/4 and 6/6 - 6/15 (Tables 18 and 25)
    0x2126, 0x00C6, 0x0110, 0x00AA, 0x0126, 0x0132, 0x013F, 0x0141, 0x00D8,
    0x0152, 0x00BA, 0x00DE, 0x0166, 0x014A, 0x0149,
    # 7/0 - 7/14 (Table 18)
    0x0138, 0x00E6, 0x0111, 0x00F0, 0x0127, 0x0131, 0x0133, 0x0140, 0x0142,
    0x00F8, 0x0153, 0x00DF, 0x00FE, 0x0167, 0x014B,
]

# The sixteen non-spacing marks of column 4 (Tables 26 and 27), as the Unicode
# combining marks nabts_supplementary_to_unicode() returns for them, each with
# the Unicode spacing character Table 26's "S 4/x 2/0" — the mark with SPACE —
# stands for. Unicode has no spacing form of Table 27's vector overbar (4/0) or
# slant (4/9), so those are drawn as the overlay alone.
MARKS = [
    (0x40, 0x20D7, None),    # vector overbar (Table 27)
    (0x41, 0x0300, 0x0060),  # grave accent
    (0x42, 0x0301, 0x00B4),  # acute accent
    (0x43, 0x0302, 0x005E),  # circumflex
    (0x44, 0x0303, 0x007E),  # tilde
    (0x45, 0x0304, 0x00AF),  # macron
    (0x46, 0x0306, 0x02D8),  # breve
    (0x47, 0x0307, 0x02D9),  # dot
    (0x48, 0x0308, 0x00A8),  # diaeresis or umlaut mark
    (0x49, 0x0338, None),    # slant (Table 27)
    (0x4A, 0x030A, 0x02DA),  # ring
    (0x4B, 0x0327, 0x00B8),  # cedilla
    (0x4C, 0x0332, 0x005F),  # underline (Table 27)
    (0x4D, 0x030B, 0x02DD),  # double acute accent
    (0x4E, 0x0328, 0x02DB),  # ogonek
    (0x4F, 0x030C, 0x02C7),  # caron
]

# Table 18's accented letters: for each mark of column 4, the primary-set
# letters its coded representation "S 4/x" composes with to make a character
# of the repertoire (§7.1). The upper case E with macron, which the Coded
# Representation column prints as S 4/5 5/5, is S 4/5 4/5 by its name and by
# ISO 6937.
TABLE_18 = {
    0x41: "aAeEiIoOuU",
    0x42: "aAcCeEgiIlLnNoOrRsSuUyYzZ",
    0x43: "aAcCeEgGhHiIjJoOsSuUwWyY",
    0x44: "aAiInNoOuU",
    0x45: "aAeEiIoOuU",
    0x46: "aAgGuU",
    0x47: "cCeEgGIzZ",
    0x48: "aAeEiIoOuUyY",
    0x4A: "aAuU",
    0x4B: "cCGkKlLnNrRsStT",
    0x4D: "oOuU",
    0x4E: "aAeEiIuU",
    0x4F: "cCdDeElLnNrRsStTzZ",
}

ASCII_NAMES = {
    0x22: '"', 0x5C: "\\\\", 0x27: "'",
}

# The one place the embedded tables depart from upstream's patterns (the 8 by
# 12's dropped row and added columns apart, above).
#
# The misc-fixed 6x10 draws a full stop as a five-pixel diamond straddling the
# baseline, and builds the colon and semicolon out of the same mark. That is a
# terminal font's device for keeping a full stop visible in a cell only ten
# rows tall, and it works at the size a terminal is read at; on a NAPLPS page,
# where a line of leader dots is a common device and the page is looked at
# rather than read close up, it reads as a row of crosses. The finer faces have
# no such trouble and use a plain square block, so these three glyphs are
# replaced by the same block scaled to the 6x10 cell: two pixels square, on the
# baseline at rows 6 and 7, centred in the six-pixel advance.
#
# Rows run top down; within a row bit 5 is the leftmost column. The face's
# baseline is row 7 (FONT_ASCENT 8), which the diamond dipped below.
#
# The 6x13 the 8 by 12 is drawn from uses the same diamond, for the same
# reason, and takes the same block: two pixels square on its baseline, row 9
# of the twelve, in columns 3 and 4 of eight (bit 7 is the leftmost there).
SUBSTITUTIONS = {
    ("6x10", 0x2E): [0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                     0x0C, 0x0C, 0x00, 0x00],  # full stop
    ("6x10", 0x3A): [0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00,
                     0x0C, 0x0C, 0x00, 0x00],  # colon
    ("6x10", 0x3B): [0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00,
                     0x0C, 0x08, 0x10, 0x00],  # semicolon, keeping its tail
    ("8x12", 0x2E): [0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x18, 0x18, 0x00, 0x00],  # full stop
    ("8x12", 0x3A): [0x00, 0x00, 0x00, 0x00, 0x18, 0x18,
                     0x00, 0x00, 0x18, 0x18, 0x00, 0x00],  # colon
    ("8x12", 0x3B): [0x00, 0x00, 0x00, 0x00, 0x18, 0x18,
                     0x00, 0x00, 0x18, 0x10, 0x20, 0x00],  # semicolon
}


def read_bdf(path):
    """Return (width, height, descent, {code: [row bits, top first]})."""
    with open(path, "r", errors="replace") as handle:
        lines = handle.read().splitlines()

    bounding = None
    descent = None
    glyphs = {}
    index = 0
    while index < len(lines):
        line = lines[index]
        if line.startswith("FONTBOUNDINGBOX"):
            bounding = [int(value) for value in line.split()[1:]]
        elif line.startswith("FONT_DESCENT"):
            descent = int(line.split()[1])
        elif line.startswith("STARTCHAR"):
            code = None
            box = None
            bitmap = []
            while index < len(lines) and not lines[index].startswith("ENDCHAR"):
                entry = lines[index]
                if entry.startswith("ENCODING"):
                    code = int(entry.split()[1])
                elif entry.startswith("BBX"):
                    box = [int(value) for value in entry.split()[1:]]
                elif entry.startswith("BITMAP"):
                    index += 1
                    while index < len(lines) and not lines[index].startswith(
                        "ENDCHAR"
                    ):
                        bitmap.append(lines[index].strip())
                        index += 1
                    break
                index += 1
            if code is not None and code >= 0:
                glyphs[code] = (box, bitmap)
        index += 1

    if bounding is None:
        raise SystemExit(f"{path}: no FONTBOUNDINGBOX")
    if descent is None:
        raise SystemExit(f"{path}: no FONT_DESCENT")
    width, height = bounding[0], bounding[1]

    patterns = {}
    for code, (box, bitmap) in glyphs.items():
        if box[:2] != [width, height]:
            # Every misc-fixed glyph fills its cell, which is what lets a
            # pattern be stored as rows alone with no per-glyph placement.
            continue
        if len(bitmap) != height:
            raise SystemExit(f"{path}: U+{code:04X} has {len(bitmap)} rows")
        rows = []
        for hex_row in bitmap:
            bits = int(hex_row, 16)
            rows.append(bits >> (4 * len(hex_row) - width))
        patterns[code] = rows
    return width, height, descent, patterns


def label(code):
    """The trailing comment for a code point."""
    if 0x21 <= code <= 0x7E:
        glyph = ASCII_NAMES.get(code, chr(code))
        return f"// U+{code:04X} '{glyph}'"
    return f"// U+{code:04X}"


def held_by_every_face(code, faces):
    return all(code in face[5] for face in faces)


def derive(width, height, descent, patterns, drop_top, pad):
    """|patterns| less their first |drop_top| rows, with |pad| blank columns
    added on each side."""
    derived = {code: [row << pad for row in rows[drop_top:]]
               for code, rows in patterns.items()}
    return width + 2 * pad, height - drop_top, descent, derived


def repertoire_and_tables(faces):
    """The code points every face must carry, the compositions and the forms
    each mark is drawn in.

    A composite of Table 18 is carried only where every face holds its
    precomposed letter; one that any face lacks is left out, and a receiver
    draws it as the letter with the mark laid over it, the same way at every
    resolution. A mark is laid over in its combining form where every face
    holds that, and in its spacing form otherwise.
    """
    required = set(LATIN_1) | set(SUPPLEMENTARY_SPACING)

    marks = []
    for position, combining, spacing in MARKS:
        if spacing is not None:
            required.add(spacing)
        overlay = combining if held_by_every_face(combining, faces) else spacing
        if overlay is None:
            raise SystemExit(
                f"S {position >> 4}/{position & 15}: no face-wide overlay for "
                f"U+{combining:04X} and no spacing form to fall back on")
        required.add(overlay)
        marks.append((combining, overlay, spacing or 0))

    compositions = []
    combining_of = {position: combining for position, combining, _ in MARKS}
    for position, letters in TABLE_18.items():
        mark = combining_of[position]
        for letter in letters:
            composed = unicodedata.normalize("NFC", letter + chr(mark))
            if len(composed) != 1:
                raise SystemExit(f"{letter} + U+{mark:04X} does not compose")
            if not held_by_every_face(ord(composed), faces):
                continue
            required.add(ord(composed))
            compositions.append((ord(letter), mark, ord(composed)))

    missing = [
        code for code in sorted(required) if not held_by_every_face(code, faces)
    ]
    if missing:
        raise SystemExit(
            "no pattern in every face for "
            + ", ".join(f"U+{code:04X}" for code in missing))
    return sorted(required), sorted(compositions), sorted(marks)


def emit(directory, out):
    faces = []
    for stem, name, source, drop_top, pad, larger in FACES:
        path = os.path.join(directory, f"{source}.bdf")
        width, height, descent, patterns = read_bdf(path)
        if f"{width}x{height}" != source:
            raise SystemExit(f"{path}: cell {width}x{height} is not {source}")
        width, height, descent, patterns = derive(
            width, height, descent, patterns, drop_top, pad)
        if f"{width}x{height}" != stem:
            raise SystemExit(f"{path}: derived cell {width}x{height} is not {stem}")
        if width > 16:
            raise SystemExit(f"{path}: cell wider than a pattern row holds")
        for (face_stem, code), rows in SUBSTITUTIONS.items():
            if face_stem != stem:
                continue
            if len(rows) != height:
                raise SystemExit(f"{path}: substitute for U+{code:04X} is "
                                 f"{len(rows)} rows, not {height}")
            patterns[code] = rows
        faces.append((stem, name, width, height, descent, patterns, larger))

    repertoire, compositions, marks = repertoire_and_tables(faces)

    digits = max(len(f"{(1 << face[2]) - 1:X}") for face in faces)

    out.write(
        """/*
 * File:        naplps_font.cpp
 * Module:      nabts_sink stage plugin
 * Purpose:     Fixed bitmap faces for depositing NAPLPS characters into a
 *              receiver's pixel grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

// Generated by tools/generate_naplps_font.py from the X11 misc-fixed BDF
// sources; see naplps_font.h for the provenance of each face. Edit the
// generator, not this file.

#include "naplps_font.h"

#include <algorithm>
#include <iterator>

namespace orc {

namespace {

// The repertoire every face carries, ascending so a lookup can bisect.
constexpr char32_t kCodes[] = {
"""
    )
    for code in repertoire:
        out.write(f"    0x{code:04X},\n")
    out.write("};\n\nconstexpr size_t kCodeCount = std::size(kCodes);\n")

    # The tables are laid out here rather than by clang-format, which puts one
    # value per line once a comment appears among the elements — a pattern is
    # only readable as a block of rows.
    per_line = 10
    for stem, name, width, height, _, patterns, _ in faces:
        out.write(
            f"\n// The {name} face, one pattern per entry of kCodes and "
            f"{height} rows\n// per pattern.\n// clang-format off\n"
            f"constexpr uint16_t kRows{stem}[] = {{\n"
        )
        for code in repertoire:
            out.write(f"    {label(code)}\n")
            values = [f"0x{value:0{digits}X}" for value in patterns[code]]
            for start in range(0, len(values), per_line):
                out.write("    " + ", ".join(values[start:start + per_line]) + ",\n")
        out.write("};\n// clang-format on\n")

    out.write("\nconstexpr NaplpsFontFace kFaces[] = {\n")
    for stem, name, width, height, descent, _, larger in faces:
        out.write(
            f'    {{"{name}", {width}, {height}, {descent}, kCodes, kCodeCount, '
            f"kRows{stem}, {'true' if larger else 'false'}}},\n"
        )
    out.write("};\n")

    out.write(
        """
// X3.110 Table 18's accented letters every face holds precomposed: the letter,
// the combining mark over it, and the character the two make, ascending by
// letter and then mark so a lookup can bisect.
struct Composition {
  char32_t base;
  char32_t mark;
  char32_t composed;
};

// clang-format off
constexpr Composition kCompositions[] = {
"""
    )
    for base, mark, composed in compositions:
        out.write(f"    {{0x{base:04X}, 0x{mark:04X}, 0x{composed:04X}}},\n")
    out.write(
        """};
// clang-format on

// How each non-spacing mark of Tables 26 and 27 is drawn: the pattern laid
// over a letter no face holds precomposed with it, and the spacing form, or
// zero where Unicode has none. Ascending by mark.
// clang-format off
constexpr NaplpsMarkForms kMarkForms[] = {
"""
    )
    for combining, overlay, spacing in marks:
        out.write(
            f"    {{0x{combining:04X}, 0x{overlay:04X}, 0x{spacing:04X}}},\n")
    out.write("};\n// clang-format on\n\n}  // namespace\n")

    out.write(
        """
size_t naplps_font_face_count() { return std::size(kFaces); }

const NaplpsFontFace& naplps_font_face(size_t index) {
  return kFaces[std::min(index, std::size(kFaces) - 1)];
}

const uint16_t* naplps_face_pattern(const NaplpsFontFace& face,
                                    char32_t code) {
  const char32_t* const end = face.codes + face.count;
  const char32_t* const found = std::lower_bound(face.codes, end, code);
  if (found == end || *found != code) {
    return nullptr;
  }
  return face.rows + static_cast<size_t>(found - face.codes) *
                         static_cast<size_t>(face.height);
}

char32_t naplps_font_composed(char32_t base, char32_t mark) {
  const auto* const end = std::end(kCompositions);
  const auto* const found = std::lower_bound(
      std::begin(kCompositions), end, Composition{base, mark, 0},
      [](const Composition& a, const Composition& b) {
        return a.base != b.base ? a.base < b.base : a.mark < b.mark;
      });
  if (found == end || found->base != base || found->mark != mark) {
    return 0;
  }
  return found->composed;
}

const NaplpsMarkForms* naplps_font_mark_forms(char32_t mark) {
  const auto* const end = std::end(kMarkForms);
  const auto* const found = std::lower_bound(
      std::begin(kMarkForms), end, mark,
      [](const NaplpsMarkForms& forms, char32_t key) {
        return forms.mark < key;
      });
  if (found == end || found->mark != mark) {
    return nullptr;
  }
  return found;
}

}  // namespace orc
"""
    )


def main():
    if len(sys.argv) > 2:
        raise SystemExit("usage: generate_naplps_font.py [font-directory]")
    directory = (
        sys.argv[1]
        if len(sys.argv) == 2
        else os.path.join(os.path.dirname(os.path.abspath(__file__)), "misc-fixed")
    )
    emit(directory, sys.stdout)


if __name__ == "__main__":
    main()
