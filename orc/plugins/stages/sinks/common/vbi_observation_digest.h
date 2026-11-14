/*
 * File:        vbi_observation_digest.h
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     The VBI values the video sink embeds, kept per frame in compact
 *              form so the observation context can release each frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context_interface.h>
#include <orc/support/eia608_line21_pairs.h>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "disc-metadata/disc_metadata_collect.h"
#include "disc-metadata/disc_metadata_document.h"

namespace orc {

// The observers write every field's observations into the run's observation
// context, which keeps them all: on a six-hour capture that is over a million
// fields of maps. The closed-caption track, the chapters and the disc metadata
// each need only a few values per field, so the sink takes those out of the
// context as each frame is observed and clears the frame there.
//
// Frame n of the export is source frame first_frame + n, whose fields the
// observers key as FieldID(2 × source frame) and the next.
//
// Thread-safety: none; the caller serialises take_*() (the export observes
// under one mutex) and reads the digest once the frames are all observed.
class VbiObservationDigest {
 public:
  // A field's line 21 as the closed caption observer read it, and the line
  // next to it where line 21 carried no pair: what closed_caption_cues()
  // decides the field's byte pair from (EIA608Line21Run).
  struct CaptionField {
    EIA608FieldReading line21;
    EIA608FieldReading other_line;
  };

  VbiObservationDigest() = default;

  // Size the digest for |frame_count| export frames starting at source frame
  // |first_frame|, keeping what each enabled output needs.
  void configure(uint64_t first_frame, uint64_t frame_count, bool captions,
                 bool chapters, bool disc_metadata) {
    first_frame_ = first_frame;
    frame_count_ = frame_count;
    captions_.assign(captions ? frame_count * 2 : 0, CaptionField{});
    chapters_.assign(chapters ? frame_count * 2 : 0, std::nullopt);
    disc_.assign(disc_metadata ? frame_count : 0, DiscMetadataFrame{});
  }

  uint64_t first_field_index() const { return first_frame_ * 2; }
  uint64_t frame_count() const { return frame_count_; }

  // Copy source frame |frame|'s closed-caption observations, then clear the
  // frame's two fields from |context|.
  void take_captions(IObservationContext& context, uint64_t frame) {
    if (frame < first_frame_ || frame - first_frame_ >= frame_count_) return;
    const uint64_t n = frame - first_frame_;
    for (uint64_t f = 0; f < 2 && !captions_.empty(); ++f) {
      const FieldID fid(static_cast<uint32_t>(frame * 2 + f));
      captions_[static_cast<size_t>(n * 2 + f)] =
          CaptionField{eia608_field_reading(context, fid),
                       eia608_other_line_reading(context, fid)};
    }
    clear_frame(context, frame);
  }

  // Copy source frame |frame|'s VBI observations (chapter numbers and the
  // disc metadata record), then clear the frame's two fields from |context|.
  void take_vbi(IObservationContext& context, uint64_t frame) {
    if (frame < first_frame_ || frame - first_frame_ >= frame_count_) return;
    const uint64_t n = frame - first_frame_;
    for (uint64_t f = 0; f < 2 && !chapters_.empty(); ++f) {
      const FieldID fid(static_cast<uint32_t>(frame * 2 + f));
      const auto v = context.get(fid, "vbi", "chapter_number");
      if (v && std::holds_alternative<int32_t>(*v)) {
        chapters_[static_cast<size_t>(n * 2 + f)] = std::get<int32_t>(*v);
      }
    }
    if (!disc_.empty()) {
      disc_[static_cast<size_t>(n)] =
          collect_disc_metadata_frame(context, frame * 2);
    }
    clear_frame(context, frame);
  }

  // Export field |offset| (0 = the first field of the first frame).
  const CaptionField* caption(uint64_t offset) const {
    return offset < captions_.size() ? &captions_[static_cast<size_t>(offset)]
                                     : nullptr;
  }
  std::optional<int32_t> chapter(uint64_t offset) const {
    return offset < chapters_.size() ? chapters_[static_cast<size_t>(offset)]
                                     : std::nullopt;
  }
  const std::vector<DiscMetadataFrame>& disc_frames() const { return disc_; }

 private:
  static void clear_frame(IObservationContext& context, uint64_t frame) {
    context.clear_field(FieldID(static_cast<uint32_t>(frame * 2)));
    context.clear_field(FieldID(static_cast<uint32_t>(frame * 2 + 1)));
  }

  uint64_t first_frame_ = 0;
  uint64_t frame_count_ = 0;
  std::vector<CaptionField> captions_;            // per export field
  std::vector<std::optional<int32_t>> chapters_;  // per export field
  std::vector<DiscMetadataFrame> disc_;           // per export frame
};

}  // namespace orc
