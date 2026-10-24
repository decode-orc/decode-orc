/*
 * File:        naplps_incremental.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     INCREMENTAL LINE and POLYGON move strings (X3.110 §5.3.3.6)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "naplps_incremental.h"

namespace orc {

NaplpsIncrementalPath naplps_incremental_path(NabtsPoint start, NabtsPoint step,
                                              const std::vector<uint8_t>& moves,
                                              bool polygon) {
  NaplpsIncrementalPath path;
  path.points.push_back(start);

  // §5.3.3.6.4: "The draw flag is initially on whenever the INCREMENTAL LINE
  // opcode is encountered", and §5.3.3.6.5 keeps it on for a polygon.
  bool draw = true;
  bool modifier_next = false;
  NabtsPoint current = start;

  for (const uint8_t byte : moves) {
    for (int shift = 4; shift >= 0; shift -= 2) {
      const int nibble = (byte >> shift) & 0x3;
      if (modifier_next) {
        // Table 17.
        switch (nibble) {
          case 0:
            if (polygon) {
              // §5.3.3.6.5(2): a null, and the next nibble is still a modify
              // parameter instruction.
              continue;
            }
            draw = !draw;
            break;
          case 1:
            step.x = -step.x;
            break;
          case 2:
            step.y = -step.y;
            break;
          default:
            step.x = -step.x;
            step.y = -step.y;
            break;
        }
        modifier_next = false;
        continue;
      }
      // Table 16.
      if (nibble == 0) {
        modifier_next = true;
        continue;
      }
      if (nibble & 0x1) {
        current.x += step.x;
      }
      if (nibble & 0x2) {
        current.y += step.y;
      }
      path.points.push_back(current);
      path.drawn.push_back(draw);
    }
  }
  return path;
}

}  // namespace orc
