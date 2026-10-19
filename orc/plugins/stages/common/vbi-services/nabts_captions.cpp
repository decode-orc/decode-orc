/*
 * File:        nabts_captions.cpp
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     The NABTS caption service as a cue list (CEA-516 §7.3.10)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include "nabts_page.h"
#include "vbi_analysis_results.h"

namespace orc {

namespace {

/// CEA-516 §5.2.2: types 0, 1 and 3 carry presentation data, which §6.1 makes
/// NAPLPS. Only those have anything a caption could be read out of.
bool type_is_presentation(uint8_t type) {
  return type == 0 || type == 1 || type == 3;
}

// SMPTE 170M: 525-line NTSC scans 59,94 fields per second, so a frame is
// 1001/30000 of a second.
constexpr double kNtscFramesPerSecond = 30000.0 / 1001.0;

}  // namespace

std::vector<NabtsCaptionCue> nabts_caption_cues(
    const std::vector<NabtsCataloguedRecord>& records) {
  std::vector<NabtsCaptionCue> cues;
  for (const NabtsCataloguedRecord& record : records) {
    // The Caption Flag of §5.2.7.3, not the channel: §7.3.10 makes A00/000 the
    // entry point a receiver acquires captioning through, while the captions
    // themselves are whatever records carry the flag.
    if (!record.caption || !type_is_presentation(record.record_type)) {
      continue;
    }
    NabtsCaptionCue cue;
    cue.start_frame = record.first_seen_frame;
    cue.end_frame = record.last_seen_frame;
    cue.channel = record.channel;
    cue.address_text = record.address_text;
    cue.version = record.version;
    cue.text = nabts_page_text(record.page);
    cues.push_back(std::move(cue));
  }

  // Stable, so captions the stream delivered in arrival order keep it where
  // the frame and the version do not separate them.
  std::stable_sort(cues.begin(), cues.end(),
                   [](const NabtsCaptionCue& lhs, const NabtsCaptionCue& rhs) {
                     if (lhs.start_frame != rhs.start_frame) {
                       return lhs.start_frame < rhs.start_frame;
                     }
                     // Two captions first seen in the same frame are ordered by
                     // the version §7.3.10.1 has the service increment per
                     // caption.
                     return lhs.version < rhs.version;
                   });

  std::vector<NabtsCaptionCue> out;
  out.reserve(cues.size());
  // Whether the screen was cleared since the last cue kept, which is what
  // separates a caption shown again from one that simply stayed up.
  bool erased = true;
  for (size_t i = 0; i < cues.size(); ++i) {
    const uint64_t next_start =
        (i + 1 < cues.size()) ? cues[i + 1].start_frame : cues[i].end_frame;
    if (cues[i].text.empty()) {
      erased = true;  // an erase, which ends the cue before it
      continue;
    }
    if (!erased && !out.empty() && out.back().text == cues[i].text &&
        out.back().channel == cues[i].channel &&
        out.back().address_text == cues[i].address_text) {
      // The same words at the same address: the caption is still up.
      out.back().end_frame =
          std::max({out.back().end_frame, next_start, cues[i].start_frame + 1});
      continue;
    }
    NabtsCaptionCue cue = cues[i];
    // A cue always covers at least the frame it was first seen at, so the
    // timing stays monotonic even where two captions share a frame.
    cue.end_frame = std::max(next_start, cue.start_frame + 1);
    out.push_back(std::move(cue));
    erased = false;
  }
  return out;
}

std::string nabts_caption_time(uint64_t frame, char separator) {
  const double seconds_total =
      static_cast<double>(frame) / kNtscFramesPerSecond;
  const int64_t millis_total = std::llround(seconds_total * 1000.0);
  return fmt::format("{:02}:{:02}:{:02}{}{:03}", millis_total / 3'600'000,
                     (millis_total / 60'000) % 60, (millis_total / 1'000) % 60,
                     separator, millis_total % 1'000);
}

}  // namespace orc
