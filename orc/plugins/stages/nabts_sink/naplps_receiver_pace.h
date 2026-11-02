/*
 * File:        naplps_receiver_pace.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     How long a receiver of the day took over each part of a NAPLPS
 *              page
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NAPLPS_RECEIVER_PACE_H
#define ORC_NAPLPS_RECEIVER_PACE_H

#include <cstdint>

#include "vbi-services/nabts_page.h"

namespace orc {

/**
 * @brief Whose drawing times a page is played at
 *
 * X3.110 times nothing but WAIT and the blink intervals: a receiver draws as
 * fast as it can, and a set of the day was slow enough for a page to be seen
 * being built (NaplpsInterpreter::set_receiver_pace()). Two receivers' times
 * are tabled here, each measured by running its own firmware over blocks of
 * identical commands at two lengths, so that the difference is the commands'
 * own cost and nothing at the block's edges, and each checked against that
 * firmware's drawing of the recorded pages. The receivers differ by up to five
 * times on one kind of figure and by little over a whole page, so each table
 * is right for its own receiver only.
 */
enum class NaplpsReceiverPace : uint8_t {
  /// None: drawing between two WAITs is instant, and the clock moves only for
  /// a WAIT (§5.3.2.8).
  kOff,
  /// The Norpak TTX 650. Its table predicts a median 0.95 of each stage's
  /// drawing time on the recorded pages, three stages in four within a
  /// quarter or 0.1 s; it fills and clears the screen from the bottom up.
  kNorpakTtx650,
  /// The Panasonic TU-2000x, a home decoder of the period. Its table predicts
  /// a median 0.99 of its stages, 85 % of them within a quarter or 0.1 s; it
  /// fills and clears the screen from the top down, as a recording of a
  /// headend decoder shows too.
  kPanasonicTu2000x,
};

/*
 * The receiver did more than draw: a command that drew nothing still cost a
 * few milliseconds, and a clear cost the same in every colour mode. A figure is
 * charged by what its drawing loops did, in the receiver's own pixels (256 to
 * the unit screen on both axes): a fill by its rows and its area, a line by how
 * much of it was horizontal, vertical or sloping (the TTX 650 runs a
 * horizontal line as a fill, nearly free, and plots every other pixel; the
 * TU-2000x plots every pixel, a sloping one dearer), an arc by its length, a
 * character by the face it is drawn in and how far that face is scaled.
 *
 * Pure functions of their arguments; times are in microseconds, and every one
 * is 0 with the pace off.
 */

/// A clear of the display (CS, or RESET's screen clear), in any colour mode.
int64_t naplps_pace_clear_us(NaplpsReceiverPace pace);

/// Calling a macro (§6.2.2), before what the macro does.
int64_t naplps_pace_macro_us(NaplpsReceiverPace pace);

/// Each byte collected into a macro, DRCS or texture definition (§6.2.2 to
/// §6.2.4).
int64_t naplps_pace_definition_byte_us(NaplpsReceiverPace pace);

/// The time a PDI takes for itself once its operands are in, before any
/// figure it draws (X3.110 §5.3.2 and §5.3.3, by opcode 2/0 to 3/15).
int64_t naplps_pace_pdi_us(NaplpsReceiverPace pace, uint8_t opcode);

/// The time a C0 control takes (§6.1). ESC is charged as the C1 control it
/// introduces, and CS as the clear it makes (naplps_pace_clear_us()), so both
/// are free here.
int64_t naplps_pace_c0_us(NaplpsReceiverPace pace, uint8_t code);

/// The time a C1 control takes (§6.2), from 4/0 to 5/15.
int64_t naplps_pace_c1_us(NaplpsReceiverPace pace, uint8_t control);

/// The time drawing |figure| takes: a point, line, arc, rectangle or polygon,
/// a character, a run of incremental points or a scroll, as the interpreter
/// emits it.
int64_t naplps_pace_figure_us(NaplpsReceiverPace pace,
                              const NabtsPrimitive& figure);

/// Which way the receiver swept a filled figure or a clear across its screen,
/// a row at a time: the TTX 650 from the bottom up, the TU-2000x from the top
/// down (NabtsPresentationTimeline::fill_sweep).
NabtsFillSweep naplps_pace_fill_sweep(NaplpsReceiverPace pace);

}  // namespace orc

#endif  // ORC_NAPLPS_RECEIVER_PACE_H
