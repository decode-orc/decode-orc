/*
 * File:        dropout_util.cpp
 * Module:      orc-sdk-support
 * Purpose:     Frame-flat ↔ field/line/sample coordinate conversion utilities
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/dropout_util.h>
#include <orc/support/frame_line_util.h>

#include <algorithm>
#include <stdexcept>

namespace orc {
namespace dropout_util {

// ---------------------------------------------------------------------------
// frame_sample_to_field_line
// ---------------------------------------------------------------------------

FieldLineSample frame_sample_to_field_line(VideoSystem sys,
                                           uint64_t frame_sample_offset) {
  if (is_625_line(sys)) {
    // Walk lines using cumulative offsets.  We search for the last line whose
    // cumulative start offset is <= frame_sample_offset.  Binary search would
    // be O(log 625); since this path is only called for dropout metadata
    // loading (not per-sample), a linear scan is acceptable.
    int32_t flat_line = 0;
    uint64_t cumulative = 0;
    for (int32_t l = 0; l < kPalFrameLines; ++l) {
      int32_t line_len = static_cast<int32_t>(frame_line_sample_count(
          VideoSystem::PAL, static_cast<size_t>(kPalSamplesPerLineNominal),
          static_cast<size_t>(l)));
      if (cumulative + static_cast<uint64_t>(line_len) > frame_sample_offset) {
        flat_line = l;
        break;
      }
      cumulative += static_cast<uint64_t>(line_len);
      flat_line = l + 1;
    }
    int32_t sample_in_line =
        static_cast<int32_t>(frame_sample_offset - cumulative);

    // Map flat line to (field, line-within-field).
    int32_t field, line_in_field;
    if (flat_line < kPalField1Lines) {
      field = 1;
      line_in_field = flat_line;
    } else {
      field = 2;
      line_in_field = flat_line - kPalField1Lines;
    }
    return {field, line_in_field, sample_in_line};

  } else if (sys == VideoSystem::NTSC) {
    // Orthogonal: all lines have kNtscSamplesPerLine samples.
    int32_t flat_line =
        static_cast<int32_t>(frame_sample_offset / kNtscSamplesPerLine);
    int32_t sample_in_line =
        static_cast<int32_t>(frame_sample_offset % kNtscSamplesPerLine);
    int32_t field, line_in_field;
    if (flat_line < kNtscField1Lines) {
      field = 1;
      line_in_field = flat_line;
    } else {
      field = 2;
      line_in_field = flat_line - kNtscField1Lines;
    }
    return {field, line_in_field, sample_in_line};

  } else if (sys == VideoSystem::PAL_M) {
    // Orthogonal: all lines have kPalMSamplesPerLine samples.
    int32_t flat_line =
        static_cast<int32_t>(frame_sample_offset / kPalMSamplesPerLine);
    int32_t sample_in_line =
        static_cast<int32_t>(frame_sample_offset % kPalMSamplesPerLine);
    int32_t field, line_in_field;
    if (flat_line < kPalMField1Lines) {
      field = 1;
      line_in_field = flat_line;
    } else {
      field = 2;
      line_in_field = flat_line - kPalMField1Lines;
    }
    return {field, line_in_field, sample_in_line};

  } else {
    return {0, 0, 0};
  }
}

// ---------------------------------------------------------------------------
// field_line_to_frame_sample
// ---------------------------------------------------------------------------

uint64_t field_line_to_frame_sample(VideoSystem sys, int32_t field,
                                    int32_t line, int32_t sample_within_line) {
  if (is_625_line(sys)) {
    // Convert field-relative line to frame-flat line.
    int32_t flat_line = (field == 1) ? line : kPalField1Lines + line;
    return static_cast<uint64_t>(frame_line_sample_offset(
               VideoSystem::PAL, static_cast<size_t>(kPalSamplesPerLineNominal),
               static_cast<size_t>(flat_line))) +
           static_cast<uint64_t>(sample_within_line);

  } else if (sys == VideoSystem::NTSC) {
    int32_t flat_line = (field == 1) ? line : kNtscField1Lines + line;
    return static_cast<uint64_t>(flat_line) * kNtscSamplesPerLine +
           static_cast<uint64_t>(sample_within_line);

  } else if (sys == VideoSystem::PAL_M) {
    int32_t flat_line = (field == 1) ? line : kPalMField1Lines + line;
    return static_cast<uint64_t>(flat_line) * kPalMSamplesPerLine +
           static_cast<uint64_t>(sample_within_line);

  } else {
    return 0;
  }
}

}  // namespace dropout_util
}  // namespace orc
