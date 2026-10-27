/*
 * File:        parameter_value_rules.cpp
 * Module:      orc-gui
 * Purpose:     Pure helper that reports parameter values whose relation to
 *              each other is wrong
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "parameter_value_rules.h"

#include <frame_numbering.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace orc::gui {

namespace {

// One parameter that must stay below another, and what to say when it does
// not. The names are the stage's, not the labels': see the test that holds
// them to the descriptors the stages publish.
struct OrderedPair {
  const char* lower;
  const char* upper;
  const char* message;
};

// video_params: the active window's ends bound a picture, and the signal
// levels bound a contrast range. Both pairs come from the same stage, whose
// set_parameters() refuses the same combinations.
constexpr OrderedPair kOrderedPairs[] = {
    {"activeVideoStart", "activeVideoEnd",
     "Active Video Start must be less than Active Video End."},
    {"firstActiveFrameLine", "lastActiveFrameLine",
     "First Active Frame Line must be less than Last Active Frame Line."},
    {"blackLevel", "whiteLevel", "Black Level must be less than White Level."},
};

// The value that leaves a video parameter alone, taking the source's instead.
constexpr int32_t kInheritFromSource = -1;

// The whole number a parameter carries, whichever integer type holds it;
// nothing when the parameter is absent or is not a whole number at all.
std::optional<int32_t> whole_number(
    const std::map<std::string, orc::ParameterValue>& values,
    const std::string& name) {
  const auto it = values.find(name);
  if (it == values.end()) {
    return std::nullopt;
  }
  if (const auto* signed_value = std::get_if<int32_t>(&it->second)) {
    return *signed_value;
  }
  if (const auto* unsigned_value = std::get_if<uint32_t>(&it->second)) {
    return static_cast<int32_t>(*unsigned_value);
  }
  return std::nullopt;
}

}  // namespace

std::vector<std::string> crossParameterErrors(
    const std::map<std::string, orc::ParameterValue>& values) {
  std::vector<std::string> errors;

  for (const auto& pair : kOrderedPairs) {
    const auto lower = whole_number(values, pair.lower);
    const auto upper = whole_number(values, pair.upper);
    if (!lower.has_value() || !upper.has_value()) {
      continue;  // This stage does not have the pair.
    }
    if (*lower == kInheritFromSource || *upper == kInheritFromSource) {
      continue;  // One end is the source's; the relation is its business.
    }
    if (*lower >= *upper) {
      errors.emplace_back(pair.message);
    }
  }

  return errors;
}

std::string decoderScheduleError(const std::string& display_schedule,
                                 const std::vector<std::string>& decoders) {
  constexpr const char* kExample =
      " Use 1-based frame numbers, for example "
      "'transform3d, 1001-1500:transform2d'.";

  const auto stored = orc::decoder_schedule_from_presentation(display_schedule);
  if (!stored.has_value()) {
    return std::string("A frame range is not valid.") + kExample;
  }
  if (stored->empty()) {
    return std::string("Enter at least one decoder.") + kExample;
  }

  // The stored form is normalised: comma-joined, no surrounding whitespace,
  // and every range already parsed as N or N-M.
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  bool has_default = false;
  bool has_held = false;
  size_t pos = 0;
  while (pos <= stored->size()) {
    const size_t comma = stored->find(',', pos);
    const std::string entry = stored->substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);
    pos = (comma == std::string::npos) ? stored->size() + 1 : comma + 1;

    const size_t colon = entry.find(':');
    const std::string decoder =
        colon == std::string::npos ? entry : entry.substr(colon + 1);
    if (std::find(decoders.begin(), decoders.end(), decoder) ==
        decoders.end()) {
      std::string choices;
      for (const auto& name : decoders) {
        choices += (choices.empty() ? "" : ", ") + name;
      }
      return "'" + decoder + "' is not a decoder this project can use (" +
             choices + ").";
    }

    if (colon == std::string::npos) {
      if (has_default) {
        return "Only one decoder may be given without a frame range.";
      }
      has_default = true;
      continue;
    }

    const std::string range = entry.substr(0, colon);
    // "held:decoder" names no frames: it decodes the frames an upstream
    // Stacker reports as held pictures.
    if (range == "held") {
      if (has_held) {
        return "Only one held: decoder may be given.";
      }
      has_held = true;
      continue;
    }
    const size_t dash = range.find('-');
    const uint64_t first = std::stoull(range.substr(0, dash));
    const uint64_t last =
        dash == std::string::npos ? first : std::stoull(range.substr(dash + 1));
    if (last < first) {
      return "Frame range " + std::to_string(first + 1) + "-" +
             std::to_string(last + 1) + " ends before it starts.";
    }
    ranges.emplace_back(first, last);
  }

  if (has_held && !has_default) {
    return "A held: decoder needs a decoder without a frame range for the "
           "frames that are not held.";
  }

  std::sort(ranges.begin(), ranges.end());
  for (size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i].first <= ranges[i - 1].second) {
      return "Frame ranges overlap at frame " +
             std::to_string(ranges[i].first + 1) + ".";
    }
  }
  return {};
}

}  // namespace orc::gui
