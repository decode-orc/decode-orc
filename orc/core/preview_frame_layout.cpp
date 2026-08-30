/*
 * File:        preview_frame_layout.cpp
 * Module:      orc-core
 * Purpose:     Row layout mapping between preview images and field lines
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_frame_layout.h"

namespace orc {

PreviewFrameLayout preview_frame_layout_for_option(
    const std::string& option_id) {
  const bool is_sequential = option_id.rfind("sequential", 0) == 0 ||
                             option_id.find("_sequential") != std::string::npos;
  return is_sequential ? PreviewFrameLayout::FieldSequential
                       : PreviewFrameLayout::Weaved;
}

namespace {

// Frame previews and the split view both show two fields at once; the split
// view and the field-sequential frame layout share the same stacked-block row
// order, so they use one mapping.
bool uses_stacked_blocks(PreviewOutputType output_type,
                         PreviewFrameLayout layout) {
  if (output_type == PreviewOutputType::Split) {
    return true;
  }
  return (output_type == PreviewOutputType::Frame_Field1_First ||
          output_type == PreviewOutputType::Frame_Reversed) &&
         layout == PreviewFrameLayout::FieldSequential;
}

bool uses_weaved_rows(PreviewOutputType output_type,
                      PreviewFrameLayout layout) {
  return (output_type == PreviewOutputType::Frame_Field1_First ||
          output_type == PreviewOutputType::Frame_Reversed) &&
         layout == PreviewFrameLayout::Weaved;
}

}  // namespace

ImageToFieldMappingResult map_preview_row_to_field(
    PreviewOutputType output_type, PreviewFrameLayout layout,
    uint64_t output_index, int image_y, const PreviewFieldGeometry& geometry) {
  ImageToFieldMappingResult result;
  result.is_valid = false;
  result.field_index = 0;
  result.field_line = 0;

  const size_t f1_lines = geometry.field1_lines;
  const size_t f2_lines = geometry.field2_lines;

  if (output_type == PreviewOutputType::Frame_Field1 ||
      output_type == PreviewOutputType::Frame_Field2) {
    // Flat single-field display: image_y maps directly to a line of the one
    // field on screen. These views navigate by sequential field number rather
    // than by frame - the GUI converts a frame position to a field position
    // when switching into them, and the line-sample reader resolves the frame
    // as index / 2 - so output_index already identifies the field, and its
    // parity selects which of the two field heights bounds the view.
    const size_t field_lines = (output_index % 2 == 0) ? f1_lines : f2_lines;
    if (image_y < 0 || static_cast<size_t>(image_y) >= field_lines) {
      return result;
    }
    result.is_valid = true;
    result.field_index = output_index;
    result.field_line = image_y;
    return result;
  }

  if (uses_weaved_rows(output_type, layout)) {
    // Interlaced frame: even lines come from field1 (Frame_Field1_First) or
    // field2 (Frame_Reversed); odd lines from the other field.
    bool is_reversed = (output_type == PreviewOutputType::Frame_Reversed);
    bool field1_on_even = !is_reversed;

    bool is_even_line = (image_y % 2) == 0;
    bool use_field1 = (is_even_line == field1_on_even);

    size_t target_field_height = use_field1 ? f1_lines : f2_lines;
    int tentative_field_line = image_y / 2;

    // Clamp to available field height
    if (tentative_field_line < 0 ||
        static_cast<size_t>(tentative_field_line) >= target_field_height) {
      // Out of bounds for chosen field — try the other
      use_field1 = !use_field1;
      target_field_height = use_field1 ? f1_lines : f2_lines;
      if (static_cast<size_t>(tentative_field_line) >= target_field_height) {
        return result;
      }
    }

    result.field_index =
        use_field1 ? (output_index * 2) : (output_index * 2 + 1);
    result.field_line = tentative_field_line;

    // Validate the final mapping against VFR-derived field heights
    size_t target_height = (result.field_index % 2 == 0) ? f1_lines : f2_lines;
    if (result.field_line < 0 ||
        static_cast<size_t>(result.field_line) >= target_height) {
      return result;  // Line out of bounds for this field
    }

    result.is_valid = true;
    return result;
  }

  if (uses_stacked_blocks(output_type, layout)) {
    // Stacked blocks: the top block is field1 (flat, VFR line 0..f1_lines-1)
    // and the bottom block is field2 (VFR line f1_lines..height-1).
    if (image_y < static_cast<int>(f1_lines)) {
      result.field_index = output_index * 2;
      result.field_line = image_y;
      if (result.field_line < 0) return result;
    } else {
      result.field_index = output_index * 2 + 1;
      result.field_line = image_y - static_cast<int>(f1_lines);
      if (result.field_line < 0 ||
          static_cast<size_t>(result.field_line) >= f2_lines) {
        return result;
      }
    }

    result.is_valid = true;
    return result;
  }

  // Unsupported output type
  return result;
}

FieldToImageMappingResult map_field_to_preview_row(
    PreviewOutputType output_type, PreviewFrameLayout layout,
    uint64_t output_index, uint64_t field_index, int field_line,
    const PreviewFieldGeometry& geometry) {
  FieldToImageMappingResult result;
  result.is_valid = false;
  result.image_y = 0;

  if (output_type == PreviewOutputType::Frame_Field1 ||
      output_type == PreviewOutputType::Frame_Field2) {
    // Flat single-field view: output_index is the sequential field number of
    // the only field on screen, so any other field has no row to map to.
    if (field_index != output_index) {
      return result;
    }
    const size_t field_lines =
        (output_index % 2 == 0) ? geometry.field1_lines : geometry.field2_lines;
    if (field_line < 0 || static_cast<size_t>(field_line) >= field_lines) {
      return result;
    }
    result.is_valid = true;
    result.image_y = field_line;
    return result;
  }

  const uint64_t frame_field1_index = output_index * 2;
  const uint64_t frame_field2_index = output_index * 2 + 1;

  if (uses_weaved_rows(output_type, layout)) {
    // Interlaced frame: field1 on even lines (Frame_Field1_First) or odd
    // lines (Frame_Reversed).
    bool is_reversed = (output_type == PreviewOutputType::Frame_Reversed);
    bool field1_on_even = !is_reversed;

    if (field_index == frame_field1_index) {
      result.image_y = field1_on_even ? (field_line * 2) : (field_line * 2 + 1);
    } else if (field_index == frame_field2_index) {
      result.image_y = field1_on_even ? (field_line * 2 + 1) : (field_line * 2);
    } else {
      return result;
    }
    result.is_valid = true;
    return result;
  }

  if (uses_stacked_blocks(output_type, layout)) {
    // Stacked blocks: the top block is field1 (field1_lines rows), the bottom
    // block is field2.
    if (field_index == frame_field1_index) {
      result.image_y = field_line;
    } else if (field_index == frame_field2_index) {
      result.image_y = field_line + static_cast<int>(geometry.field1_lines);
    } else {
      return result;
    }
    result.is_valid = true;
    return result;
  }

  // Unsupported output type
  return result;
}

}  // namespace orc
