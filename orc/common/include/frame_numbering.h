/*
 * File:        frame_numbering.h
 * Module:      orc-common
 * Purpose:     Presentation conversion for 0-indexed frame/line range specs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>

namespace orc {

// ============================================================================
// Frame/line numbering presentation conversion
// ============================================================================
// All frame and line indices are stored 0-based internally and in project
// YAML.  All user-visible presentation is 1-based, matching the preview
// dialog (see orc/gui/field_frame_presentation.h for the presentation
// convention).  These helpers convert range-specification parameter strings
// between the two conventions at the presentation boundary.
//
// Range spec grammar (frame_map "ranges", mask_line "lineSpec"):
//   spec  := token ("," token)*
//   token := INDEX | INDEX "-" INDEX | "PAD_" COUNT
// PAD_<count> tokens are counts, not indices, and are never shifted.
//
// Decoder schedule grammar (video_sink "decoder_schedule"):
//   spec  := entry ("," entry)*
//   entry := INDEX ":" NAME | INDEX "-" INDEX ":" NAME | NAME
// Only the indices before ':' are shifted; bare NAME entries are copied.
//
// Dropout map grammar (dropout_map "map"):
//   [{frame:N,add:[{line:L,start:S,end:E},...],remove:[...]},...]
// Only the values of "frame" keys are frame indices; line/start/end are
// frame-flat 0-based coordinates and are not shifted.
//
// Thread safety: all functions are pure and thread-safe.

namespace frame_numbering_detail {

// Parse a run of decimal digits as uint64_t; returns nullopt when the token
// is empty, contains non-digits, or overflows.
inline std::optional<uint64_t> parse_index(const std::string& token) {
  if (token.empty()) return std::nullopt;
  uint64_t value = 0;
  for (char c : token) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
    value = value * 10 + digit;
  }
  return value;
}

// Shift a single index by delta (+1 to presentation, -1 to internal).
// Rejects a shift below zero (presentation indices start at 1).
inline std::optional<uint64_t> shift_index(uint64_t value, int delta) {
  if (delta < 0 && value == 0) return std::nullopt;
  return delta < 0 ? value - 1 : value + 1;
}

// Shift every index in a range spec by delta.  Output is normalised
// (whitespace trimmed, comma-joined).  Returns nullopt when any token is
// malformed or a shift would take an index below zero.
inline std::optional<std::string> shift_range_spec(const std::string& spec,
                                                   int delta) {
  std::string result;
  size_t pos = 0;
  bool first = true;

  while (pos <= spec.size()) {
    size_t comma = spec.find(',', pos);
    std::string token = spec.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);

    // Trim whitespace
    const size_t start = token.find_first_not_of(" \t");
    if (start != std::string::npos) {
      const size_t end = token.find_last_not_of(" \t");
      token = token.substr(start, end - start + 1);
    } else {
      token.clear();
    }

    if (!token.empty()) {
      std::string converted;
      if (token.rfind("PAD_", 0) == 0) {
        // PAD_<count> holds a count, not an index; validate but do not shift.
        if (!parse_index(token.substr(4))) return std::nullopt;
        converted = token;
      } else {
        const size_t dash = token.find('-');
        if (dash == std::string::npos) {
          auto value = parse_index(token);
          if (!value) return std::nullopt;
          auto shifted = shift_index(*value, delta);
          if (!shifted) return std::nullopt;
          converted = std::to_string(*shifted);
        } else {
          auto lhs = parse_index(token.substr(0, dash));
          auto rhs = parse_index(token.substr(dash + 1));
          if (!lhs || !rhs) return std::nullopt;
          auto lhs_shifted = shift_index(*lhs, delta);
          auto rhs_shifted = shift_index(*rhs, delta);
          if (!lhs_shifted || !rhs_shifted) return std::nullopt;
          converted =
              std::to_string(*lhs_shifted) + "-" + std::to_string(*rhs_shifted);
        }
      }

      if (!first) result += ",";
      result += converted;
      first = false;
    }

    if (comma == std::string::npos) break;
    pos = comma + 1;
  }

  return result;
}

// Shift the frame range of every "range:name" entry in a decoder schedule by
// delta.  Bare names are copied.  Output is normalised (whitespace trimmed,
// comma-joined).  Returns nullopt when a range is malformed, is a PAD_ token,
// or a shift would take an index below zero.  Names are not checked here; the
// stage validates them.
inline std::optional<std::string> shift_decoder_schedule(
    const std::string& spec, int delta) {
  auto trim = [](const std::string& text) {
    const size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos) return std::string();
    const size_t end = text.find_last_not_of(" \t");
    return text.substr(start, end - start + 1);
  };

  std::string result;
  size_t pos = 0;
  while (pos <= spec.size()) {
    const size_t comma = spec.find(',', pos);
    const std::string entry = trim(spec.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos));
    pos = (comma == std::string::npos) ? spec.size() + 1 : comma + 1;
    if (entry.empty()) continue;

    std::string converted;
    const size_t colon = entry.find(':');
    if (colon == std::string::npos) {
      converted = entry;
    } else {
      // Spaces inside a range ("1001 - 1500") are allowed and dropped.
      std::string range = entry.substr(0, colon);
      range.erase(
          std::remove_if(range.begin(), range.end(),
                         [](unsigned char c) { return std::isspace(c); }),
          range.end());
      if (range.empty() || range.rfind("PAD_", 0) == 0) return std::nullopt;
      // "held:decoder" names no frames (the video sink's decoder for held
      // pictures), so it passes through unchanged.
      if (range == "held") {
        converted = range + ":" + trim(entry.substr(colon + 1));
      } else {
        auto shifted = shift_range_spec(range, delta);
        if (!shifted) return std::nullopt;
        converted = *shifted + ":" + trim(entry.substr(colon + 1));
      }
    }

    if (!result.empty()) result += ",";
    result += converted;
  }

  return result;
}

// Shift the value of every "frame" key in a dropout map spec by delta.  All
// other content (line/start/end coordinates, structure) is copied verbatim.
// Returns nullopt when a frame value is malformed or a shift would take an
// index below zero.
inline std::optional<std::string> shift_dropout_map_frames(
    const std::string& map_spec, int delta) {
  std::string result;
  result.reserve(map_spec.size());
  size_t pos = 0;

  while (pos < map_spec.size()) {
    const char c = map_spec[pos];

    if (std::isalpha(static_cast<unsigned char>(c))) {
      // Capture a key token
      std::string key;
      while (pos < map_spec.size() &&
             std::isalpha(static_cast<unsigned char>(map_spec[pos]))) {
        key += map_spec[pos++];
      }
      result += key;

      // Copy whitespace and the ':' separator (if present)
      while (pos < map_spec.size() &&
             std::isspace(static_cast<unsigned char>(map_spec[pos]))) {
        result += map_spec[pos++];
      }
      if (pos < map_spec.size() && map_spec[pos] == ':') {
        result += map_spec[pos++];
        while (pos < map_spec.size() &&
               std::isspace(static_cast<unsigned char>(map_spec[pos]))) {
          result += map_spec[pos++];
        }
        if (key == "frame") {
          std::string digits;
          while (pos < map_spec.size() &&
                 std::isdigit(static_cast<unsigned char>(map_spec[pos]))) {
            digits += map_spec[pos++];
          }
          auto value = parse_index(digits);
          if (!value) return std::nullopt;
          auto shifted = shift_index(*value, delta);
          if (!shifted) return std::nullopt;
          result += std::to_string(*shifted);
        }
      }
      continue;
    }

    result += c;
    ++pos;
  }

  return result;
}

}  // namespace frame_numbering_detail

// ============================================================================
// Range spec conversion (frame_map "ranges", mask_line "lineSpec")
// ============================================================================

// Convert a stored 0-based range spec to its 1-based presentation form.
// Malformed specs are returned unmodified so the user can see and fix the
// raw value.
inline std::string range_spec_to_presentation(const std::string& spec) {
  auto shifted = frame_numbering_detail::shift_range_spec(spec, +1);
  return shifted ? *shifted : spec;
}

// Convert a 1-based presentation range spec to the stored 0-based form.
// Returns nullopt when the spec is malformed or contains index 0
// (presentation indices start at 1).
inline std::optional<std::string> range_spec_from_presentation(
    const std::string& spec) {
  return frame_numbering_detail::shift_range_spec(spec, -1);
}

// ============================================================================
// Dropout map conversion (dropout_map "map")
// ============================================================================

// Convert a stored dropout map spec to presentation form (1-based frame
// numbers; line/sample coordinates unchanged).  Malformed specs are returned
// unmodified.
inline std::string dropout_map_spec_to_presentation(const std::string& spec) {
  auto shifted = frame_numbering_detail::shift_dropout_map_frames(spec, +1);
  return shifted ? *shifted : spec;
}

// Convert a presentation dropout map spec to the stored 0-based form.
// Returns nullopt when a frame value is malformed or 0.
inline std::optional<std::string> dropout_map_spec_from_presentation(
    const std::string& spec) {
  return frame_numbering_detail::shift_dropout_map_frames(spec, -1);
}

// ============================================================================
// Decoder schedule conversion (video_sink "decoder_schedule")
// ============================================================================

// Convert a stored 0-based decoder schedule to its 1-based presentation form.
// Malformed schedules are returned unmodified.
inline std::string decoder_schedule_to_presentation(const std::string& spec) {
  auto shifted = frame_numbering_detail::shift_decoder_schedule(spec, +1);
  return shifted ? *shifted : spec;
}

// Convert a 1-based presentation decoder schedule to the stored 0-based form.
// Returns nullopt when a range is malformed or contains frame 0.
inline std::optional<std::string> decoder_schedule_from_presentation(
    const std::string& spec) {
  return frame_numbering_detail::shift_decoder_schedule(spec, -1);
}

// ============================================================================
// Parameter classification
// ============================================================================

// Kinds of stage parameters that hold 0-based index specifications and
// therefore require presentation conversion at the UI boundary.
enum class IndexedSpecKind {
  kNone,                 // Not an indexed spec; no conversion
  kRangeSpec,            // frame/line range spec ("0-10,20-30,PAD_5")
  kDropoutMapSpec,       // dropout map spec ("[{frame:N,add:[...]}]")
  kDecoderScheduleSpec,  // decoder schedule ("pal2d,0-99:transform3d")
};

// Identify whether a stage parameter is a 0-based index specification.
// stage_name is the internal stage identifier (e.g. "frame_map"), not the
// display name.
inline IndexedSpecKind indexed_spec_kind(const std::string& stage_name,
                                         const std::string& parameter_name) {
  if (stage_name == "frame_map" && parameter_name == "ranges") {
    return IndexedSpecKind::kRangeSpec;
  }
  if (stage_name == "mask_line" && parameter_name == "lineSpec") {
    return IndexedSpecKind::kRangeSpec;
  }
  if (stage_name == "dropout_map" && parameter_name == "dropout_map") {
    return IndexedSpecKind::kDropoutMapSpec;
  }
  if (stage_name == "video_sink" && parameter_name == "decoder_schedule") {
    return IndexedSpecKind::kDecoderScheduleSpec;
  }
  return IndexedSpecKind::kNone;
}

// Convert a stored parameter value to presentation form for the given kind.
inline std::string indexed_spec_to_presentation(IndexedSpecKind kind,
                                                const std::string& value) {
  switch (kind) {
    case IndexedSpecKind::kRangeSpec:
      return range_spec_to_presentation(value);
    case IndexedSpecKind::kDropoutMapSpec:
      return dropout_map_spec_to_presentation(value);
    case IndexedSpecKind::kDecoderScheduleSpec:
      return decoder_schedule_to_presentation(value);
    case IndexedSpecKind::kNone:
      break;
  }
  return value;
}

// Convert a presentation parameter value back to stored 0-based form for the
// given kind.  Returns nullopt when the presentation value is invalid.
inline std::optional<std::string> indexed_spec_from_presentation(
    IndexedSpecKind kind, const std::string& value) {
  switch (kind) {
    case IndexedSpecKind::kRangeSpec:
      return range_spec_from_presentation(value);
    case IndexedSpecKind::kDropoutMapSpec:
      return dropout_map_spec_from_presentation(value);
    case IndexedSpecKind::kDecoderScheduleSpec:
      return decoder_schedule_from_presentation(value);
    case IndexedSpecKind::kNone:
      break;
  }
  return value;
}

}  // namespace orc
