/*
 * File:        closed_caption_cues.h
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     The CC1 caption cues a run of fields' line 21 observations
 *              decode to, for the subtitles the video sink embeds
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_CORE_CLOSED_CAPTION_CUES_H
#define ORC_CORE_CLOSED_CAPTION_CUES_H

#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context_interface.h>
#include <orc/support/eia608_decoder.h>
#include <orc/support/eia608_line21_pairs.h>
#include <orc/support/eia608_service_demux.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace orc {

// What closed_caption_cues() decoded.
struct ClosedCaptionCues {
  std::vector<CaptionCue> cues;
  // CC1's byte pairs the decoder was fed.
  size_t pairs = 0;
  // The run's first stored field is television field 2.
  bool fields_reversed = false;
};

// The CC1 cues of fields [field_start, field_start + field_count), read from
// the closed caption observer's observations, timed from field_start.
//
// The pairs are taken as the Closed Caption Sink takes them
// (EIA608Line21Run): each byte's parity, the codes EIA-608 defines for the
// field, line 21 data around the pair, and the field order the run's control
// codes show, so that a capture whose first stored field is television field
// 2 still has its CC1 found. Line 21 multiplexes four services onto one
// byte-pair stream, so the pairs are routed before they are decoded: fed the
// lot, the decoder lands a second caption service, or a text service's page of
// listings, in the middle of the subtitles. Only the primary caption service
// is embedded — the container carries one mov_text track, and CC1 is what a
// viewer expects to find on it.
//
// O(field_count); eight bytes are held for each field that carried a pair,
// twelve where both its parities held (EIA608Line21Run).
// Reentrant: it keeps no state between calls.
inline ClosedCaptionCues closed_caption_cues(const IObservationContext& context,
                                             uint64_t field_start,
                                             uint64_t field_count,
                                             double frames_per_second) {
  EIA608Line21Run run;
  const uint64_t first_frame = field_start / 2;
  for (uint64_t field = field_start; field < field_start + field_count;
       ++field) {
    run.add(static_cast<uint32_t>(field / 2 - first_frame),
            static_cast<int>(field % 2),
            eia608_field_reading(context, FieldID(field)),
            eia608_other_line_reading(context, FieldID(field)));
  }

  ClosedCaptionCues result;
  result.fields_reversed = run.fields_reversed();
  EIA608Decoder decoder;
  EIA608ServiceDemux demux(EIA608Service::CC1,
                           /*suppress_repeated_controls=*/true);
  run.for_each([&](const EIA608Line21Run::Pair& pair) {
    if (!pair.taken() ||
        !demux.accept(pair.television_field, pair.data0, pair.data1)) {
      return;  // refused, another service's byte pair, or padding
    }
    // Field index to seconds: two fields per frame.
    const uint64_t field = (first_frame + pair.frame_offset) * 2 +
                           static_cast<uint64_t>(pair.stored_field);
    decoder.process_bytes(
        static_cast<double>(field - field_start) / (2.0 * frames_per_second),
        pair.data0, pair.data1);
    ++result.pairs;
  });
  result.cues = decoder.finalize(static_cast<double>(field_count) /
                                 (2.0 * frames_per_second));
  return result;
}

}  // namespace orc

#endif  // ORC_CORE_CLOSED_CAPTION_CUES_H
