/*
 * File:        decoder_schedule.h
 * Module:      orc-core
 * Purpose:     Per-frame-range chroma decoder selection for the video sink
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/frame_id.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace orc::chroma_sink {

// The decoder_type value that hands decoder selection to decoder_schedule.
inline constexpr const char kMultipleDecoders[] = "multiple";

// Every decoder name a schedule may use. Whether a decoder suits the source
// (a PAL decoder on an NTSC source, say) is decided when it is built, as it is
// for a single decoder_type.
inline const std::vector<std::string>& schedulable_decoder_types() {
  static const std::vector<std::string> kTypes = {
      "pal2d",  "transform2d", "transform3d",   "secam", "ntsc1d",
      "ntsc2d", "ntsc3d",      "ntsc3dnoadapt", "mono"};
  return kTypes;
}

// One "first-last:decoder" entry; both ends inclusive, 0-based frame ids of
// the sink's input.
struct DecoderScheduleRange {
  FrameID first = 0;
  FrameID last = 0;
  std::string decoder;
};

// The schedule entry prefix for the decoder of held pictures.
inline constexpr const char kHeldPictureEntry[] = "held";

// Which decoder decodes each frame. Ranges are sorted by first frame and never
// overlap; a frame outside every range takes held_decoder when its picture is
// held (as the caller decides it, from the frame descriptor), otherwise
// default_decoder. Either is empty when the schedule gives none.
//
// Thread safety: immutable once built; concurrent reads are safe.
struct DecoderSchedule {
  std::vector<DecoderScheduleRange> ranges;
  std::string default_decoder;
  std::string held_decoder;

  // A schedule that decodes every frame with one decoder.
  static DecoderSchedule single(const std::string& decoder) {
    DecoderSchedule schedule;
    schedule.default_decoder = decoder;
    return schedule;
  }

  // The decoder for frame_id, whose picture is held or not, or nullptr when
  // the schedule covers no such frame. A range wins over held_decoder, so a
  // hand-written range can always overrule it. O(log ranges).
  const std::string* decoder_for(FrameID frame_id, bool held = false) const {
    auto it = std::upper_bound(
        ranges.begin(), ranges.end(), frame_id,
        [](FrameID id, const DecoderScheduleRange& r) { return id < r.first; });
    if (it != ranges.begin() && frame_id <= std::prev(it)->last) {
      return &std::prev(it)->decoder;
    }
    if (held && !held_decoder.empty()) return &held_decoder;
    return default_decoder.empty() ? nullptr : &default_decoder;
  }

  // Each decoder the schedule names, once, in the order first named.
  std::vector<std::string> decoder_types() const {
    std::vector<std::string> types;
    auto add = [&types](const std::string& type) {
      if (!type.empty() &&
          std::find(types.begin(), types.end(), type) == types.end()) {
        types.push_back(type);
      }
    };
    add(default_decoder);
    add(held_decoder);
    for (const auto& range : ranges) add(range.decoder);
    return types;
  }

  // The first frame of `frames` the schedule gives no decoder, if any.
  // held_decoder covers nothing here: whether a frame is held is not known
  // until it is decoded.
  std::optional<FrameID> first_uncovered(FrameIDRange frames) const {
    if (!default_decoder.empty() || frames.last < frames.first) {
      return std::nullopt;
    }
    FrameID next = frames.first;
    for (const auto& range : ranges) {
      if (range.last < next) continue;
      if (range.first > next) return next;
      if (range.last >= frames.last) return std::nullopt;
      next = range.last + 1;
    }
    return next;
  }
};

namespace decoder_schedule_detail {

inline std::string trim(const std::string& text) {
  const size_t start = text.find_first_not_of(" \t");
  if (start == std::string::npos) return {};
  const size_t end = text.find_last_not_of(" \t");
  return text.substr(start, end - start + 1);
}

inline std::optional<FrameID> parse_frame(const std::string& text) {
  if (text.empty()) return std::nullopt;
  FrameID value = 0;
  for (char c : text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    const FrameID digit = static_cast<FrameID>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
    value = value * 10 + digit;
  }
  return value;
}

}  // namespace decoder_schedule_detail

struct DecoderScheduleParseResult {
  std::optional<DecoderSchedule> schedule;
  std::string error;  // Set when schedule is empty
};

// Parse a stored (0-based) decoder schedule:
//   spec  := entry ("," entry)*
//   entry := FRAME ":" DECODER | FRAME "-" FRAME ":" DECODER | DECODER
//          | "held" ":" DECODER
// A bare DECODER is the default for frames no range lists; at most one is
// allowed. A "held" entry decodes the frames no range lists whose picture is
// held (FrameDescriptor::held_frames_before/after); at most one is allowed,
// and only beside a default for the frames that are not held. Ranges may come
// in any order but must not overlap. O(n log n) in the number of entries. Error
// messages name entries by position rather than quoting frame numbers, which
// are 0-based here but 1-based wherever a user reads them.
inline DecoderScheduleParseResult parse_decoder_schedule(
    const std::string& spec) {
  using decoder_schedule_detail::parse_frame;
  using decoder_schedule_detail::trim;

  DecoderScheduleParseResult result;
  DecoderSchedule schedule;
  std::vector<size_t> range_entries;  // Entry number of each range, for errors
  const auto& known = schedulable_decoder_types();
  auto fail = [&result](std::string message) {
    result.error = std::move(message);
    return result;
  };

  size_t pos = 0;
  size_t entry_number = 0;
  while (pos <= spec.size()) {
    const size_t comma = spec.find(',', pos);
    const std::string entry = trim(spec.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos));
    pos = (comma == std::string::npos) ? spec.size() + 1 : comma + 1;
    if (entry.empty()) continue;
    ++entry_number;
    const std::string where = "entry " + std::to_string(entry_number);

    const size_t colon = entry.find(':');
    const std::string decoder =
        trim(colon == std::string::npos ? entry : entry.substr(colon + 1));
    if (std::find(known.begin(), known.end(), decoder) == known.end()) {
      return fail(where + ": unknown decoder '" + decoder + "'");
    }

    if (colon == std::string::npos) {
      if (!schedule.default_decoder.empty()) {
        return fail("more than one default decoder ('" +
                    schedule.default_decoder + "' and '" + decoder + "')");
      }
      schedule.default_decoder = decoder;
      continue;
    }

    const std::string frames = trim(entry.substr(0, colon));
    if (frames == kHeldPictureEntry) {
      if (!schedule.held_decoder.empty()) {
        return fail("more than one held decoder ('" + schedule.held_decoder +
                    "' and '" + decoder + "')");
      }
      schedule.held_decoder = decoder;
      continue;
    }
    const size_t dash = frames.find('-');
    const auto first = parse_frame(
        trim(dash == std::string::npos ? frames : frames.substr(0, dash)));
    const auto last = dash == std::string::npos
                          ? first
                          : parse_frame(trim(frames.substr(dash + 1)));
    if (!first || !last) {
      return fail(where + ": invalid frame range");
    }
    if (*last < *first) {
      return fail(where + ": frame range ends before it starts");
    }
    schedule.ranges.push_back({*first, *last, decoder});
    range_entries.push_back(entry_number);
  }

  if (schedule.ranges.empty() && schedule.default_decoder.empty() &&
      schedule.held_decoder.empty()) {
    return fail("no decoders given");
  }
  if (!schedule.held_decoder.empty() && schedule.default_decoder.empty()) {
    return fail(
        "a held decoder needs a default decoder for the frames that are not "
        "held");
  }

  std::vector<size_t> order(schedule.ranges.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&schedule](size_t a, size_t b) {
    return schedule.ranges[a].first < schedule.ranges[b].first;
  });
  for (size_t i = 1; i < order.size(); ++i) {
    const auto& previous = schedule.ranges[order[i - 1]];
    if (schedule.ranges[order[i]].first <= previous.last) {
      size_t a = range_entries[order[i - 1]];
      size_t b = range_entries[order[i]];
      if (b < a) std::swap(a, b);
      return fail("entries " + std::to_string(a) + " and " + std::to_string(b) +
                  ": frame ranges overlap");
    }
  }
  std::vector<DecoderScheduleRange> sorted;
  sorted.reserve(order.size());
  for (size_t index : order)
    sorted.push_back(std::move(schedule.ranges[index]));
  schedule.ranges = std::move(sorted);

  result.schedule = std::move(schedule);
  return result;
}

}  // namespace orc::chroma_sink
