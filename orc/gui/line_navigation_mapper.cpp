/*
 * File:        line_navigation_mapper.cpp
 * Module:      orc-gui
 * Purpose:     Centralized line-scope navigation mapping helper
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "line_navigation_mapper.h"

#include <algorithm>

namespace orc::gui {

LineNavigationTarget computeLineNavigationTarget(
    const LineNavigationRequest& request,
    const std::function<orc::FieldToImageMappingResult(
        uint64_t field_index, int field_line, int image_height)>&
        map_field_to_image,
    const std::function<orc::ImageToFieldMappingResult(
        int image_y, int image_height)>& map_image_to_field) {
  LineNavigationTarget target;

  if (request.direction == 0 || request.image_height <= 0) {
    return target;
  }

  const auto current_image_pos = map_field_to_image(
      request.current_field, request.current_line, request.image_height);
  if (!current_image_pos.is_valid) {
    return target;
  }

  // Defensive normalization: some frame mappings can report the asymmetric
  // extra-line position just outside the image range (e.g. NTSC bottom line).
  // Clamp to the nearest valid pixel row so reverse navigation still works.
  const int current_image_y =
      std::clamp(current_image_pos.image_y, 0, request.image_height - 1);

  // Navigation moves by exactly one preview row. Every line the user can
  // select is one row of the preview - clicking the cross-hairs resolves the
  // clicked row through the same mapping - so a single-row step is what makes
  // the whole picture reachable from the buttons. It is also its own inverse:
  // stepping down then up returns to the row that was showing, which skipping
  // rows does not (in a weaved frame the two fields sit on opposite sides of
  // the current row, so a skip is not symmetric).
  //
  // In a weaved frame preview consecutive rows alternate between the two
  // fields, so the field index changes on every press while the field-relative
  // line number only advances on every second press. That is the frame's real
  // line order, and the scope's line label reports the frame-flat position, so
  // each press is a visibly different line.
  const int step = (request.direction > 0) ? 1 : -1;
  const int new_image_y = current_image_y + step;
  if (new_image_y < 0 || new_image_y >= request.image_height) {
    return target;
  }

  const auto next_mapping =
      map_image_to_field(new_image_y, request.image_height);
  if (!next_mapping.is_valid) {
    return target;
  }

  target.is_valid = true;
  target.field_index = next_mapping.field_index;
  target.line_number = next_mapping.field_line;
  return target;
}

}  // namespace orc::gui
