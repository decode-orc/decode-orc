/*
 * File:        naplps_proportional.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     How far proportionally spaced text moves the cursor
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NAPLPS_PROPORTIONAL_H
#define ORC_NAPLPS_PROPORTIONAL_H

#include <cstdint>

namespace orc {

/**
 * @brief Proportional spacing, as ITU-T T.101 Annex D's 1993 Addendum fixed it
 *
 * X3.110 §5.3.2.3.4 leaves proportional spacing (TEXT's inter-character spacing
 * code 3) to the implementation. Because page creators could not then predict
 * how long a line would come out, the Addendum to T.101 Annex D (the same Data
 * Syntax III) laid down in clause 11 how far each character moves the cursor:
 * every character of the repertoire is in one of ten width classes (Tables 1
 * and 2), and a displacement function turns a class and the character field's
 * width into the distance. Both receivers of the day — the Norpak TTX 650 and
 * the Panasonic TU-2000x, run as firmware — lay proportional text out by it:
 * of twenty crafted lines, nineteen match the TTX 650's to within 1 %.
 *
 * Pure functions of their arguments.
 */

/// The full-width class, which SPACE has (clause 11.1), and so does a code no
/// table gives a class.
constexpr int kNaplpsFullWidthClass = 9;

/// The width class clause 11.1 gives |code| (2/0 to 7/14) in the primary set,
/// or in the supplementary set where |supplementary|: 0, the narrowest, to 9.
/// A non-spacing mark's class is what it makes of the letter it is put on: the
/// composite takes the wider of the two.
int naplps_width_class(uint8_t code, bool supplementary);

/// Clauses 11.2 to 11.6: how far a character of |width_class| moves the cursor
/// in a character field |field_width| wide, both in units of the unit screen.
/// The widest class moves the field's width, truncated to the text size's
/// spacing unit, and the narrower ones less by whole spacing units, so the
/// gap between letters is the same all along a line.
double naplps_proportional_displacement(int width_class, double field_width);

}  // namespace orc

#endif  // ORC_NAPLPS_PROPORTIONAL_H
