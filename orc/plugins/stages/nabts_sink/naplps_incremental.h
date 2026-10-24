/*
 * File:        naplps_incremental.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     INCREMENTAL LINE and POLYGON move strings (X3.110 §5.3.3.6)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NAPLPS_INCREMENTAL_H
#define ORC_NAPLPS_INCREMENTAL_H

#include <cstdint>
#include <vector>

#include "vbi-services/nabts_page.h"

namespace orc {

/**
 * @brief The path an INCREMENTAL LINE or INCREMENTAL POLYGON string steps out
 *
 * X3.110 §5.3.3.6.4: after the step size, each numeric byte carries three
 * two-bit nibbles, "interpreted b6 to b1". A nibble of 01 steps the drawing
 * point by dx, 10 by dy and 11 by both (Table 16); 00 makes the next nibble a
 * modify parameter instruction — toggle the draw flag, or change the sign of
 * dx, dy or both (Table 17) — and the nibble after that is a step again. Every
 * step with the draw flag on draws a line from the point before the step to
 * the point after it.
 */
struct NaplpsIncrementalPath {
  /// Every point the drawing point stepped to, starting with where it began.
  std::vector<NabtsPoint> points;
  /// Whether each step (points[i] to points[i + 1]) was drawn.
  std::vector<bool> drawn;
};

/**
 * @brief Steps out an INCREMENTAL LINE or INCREMENTAL POLYGON move string
 *
 * @param start   The drawing point when the opcode arrived.
 * @param step    The step size (dx, dy), the command's first multi-value
 *                operand, signed.
 * @param moves   The move string's numeric bytes; only b6 to b1 are read.
 * @param polygon INCREMENTAL POLYGON (Filled): §5.3.3.6.5 keeps the draw flag
 *                always on, and a toggle instruction "is treated as a null,
 *                and the following nibble is interpreted as the 'modify
 *                parameter' instruction".
 */
NaplpsIncrementalPath naplps_incremental_path(NabtsPoint start, NabtsPoint step,
                                              const std::vector<uint8_t>& moves,
                                              bool polygon);

}  // namespace orc

#endif  // ORC_NAPLPS_INCREMENTAL_H
