/*
 * File:        content_offset_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     The content check on captures aligned by picture number, one
 *              of them numbered one high from its eleventh frame: Disc
 *              Mapper, Frame Map and the pad alignment place it, and the
 *              check must find where its content sits off its numbers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <content_offsets.h>
#include <gtest/gtest.h>
#include <orc/stage/video_frame_representation.h>
#include <source_alignment_offsets.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "alignment_support.h"
#include "frame_map_stage.h"
#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

using orc_videosynth::CaseFile;

// projects/relabel/*_relabel_disc.yaml: 20 frames, numbered 1-20.
constexpr int64_t kDiscPictures = 20;

void run_relabel_case(const std::string& system_name, orc::VideoSystem system) {
  // Two captures of the disc and one of the late-numbered pressing, each
  // mapped by the Disc Mapper and Frame Map as the GUI's flow maps them.
  std::vector<std::unique_ptr<orc::FrameMapStage>> frame_maps;
  std::vector<orc::ArtifactPtr> mapped;
  for (const char* suffix :
       {"_relabel_disc", "_relabel_disc", "_relabel_late"}) {
    const std::string name = system_name + suffix;
    const auto artifact =
        orc_videosynth::open_case(CaseFile{name, "videosynth_" + name, system});
    ASSERT_NE(artifact, nullptr) << name;
    const auto decision =
        orc_videosynth::map_disc(*orc_videosynth::as_vfr(artifact));
    ASSERT_TRUE(decision.success) << name << ": " << decision.rationale;
    frame_maps.push_back(std::make_unique<orc::FrameMapStage>());
    mapped.push_back(orc_videosynth::apply_frame_map(
        artifact, *frame_maps.back(), decision.mapping_spec));
    ASSERT_NE(mapped.back(), nullptr) << name;
  }

  // Placed by picture number, as Source Alignment Analysis places them.
  std::vector<std::vector<orc::PictureNumberReading>> readings;
  std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>> vfrs;
  for (const auto& artifact : mapped) {
    vfrs.push_back(orc_videosynth::as_vfr(artifact));
    readings.push_back(orc_videosynth::readings_of(
        orc_videosynth::read_pictures(*vfrs.back())));
  }
  const orc::PadAlignment alignment = orc::compute_pad_alignment(readings);
  ASSERT_TRUE(alignment.first_picture.has_value());
  std::vector<const orc::VideoFrameRepresentation*> sources;
  std::vector<std::optional<int64_t>> starts;
  for (size_t i = 0; i < vfrs.size(); ++i) {
    const std::optional<size_t> pad = alignment.pads[i];
    ASSERT_TRUE(pad.has_value()) << i;
    sources.push_back(vfrs[i].get());
    starts.emplace_back(static_cast<int64_t>(vfrs[i]->frame_range().first) -
                        static_cast<int64_t>(*pad));
  }

  const orc::ContentOffsets offsets =
      orc::check_content_offsets(sources, starts);
  EXPECT_TRUE(offsets.spans[0].empty());
  EXPECT_TRUE(offsets.spans[1].empty());
  ASSERT_EQ(offsets.spans[2].size(), 1u);
  const orc::ContentOffsetSpan& span = offsets.spans[2][0];
  ASSERT_TRUE(span.offset.has_value());
  EXPECT_EQ(*span.offset, 1);
  // From output frame 12 (index 11) on, it holds each picture one frame
  // later; at index 10 it holds the padding Frame Map put where picture 11
  // would be.  The reference is the source covering the most frames, this
  // one, so no frame within one of its padding can be read: the change is
  // bracketed, not found to the frame.
  EXPECT_LE(span.earliest_first, 11);
  EXPECT_GE(span.first, 11);
  EXPECT_LE(span.first - span.earliest_first, 3);
  EXPECT_EQ(span.last, kDiscPictures - 1);
  EXPECT_FALSE(orc::whole_content_offset(offsets, 2).has_value());

  orc_videosynth::expect_metrics(
      {{"change_bracket_frames",
        static_cast<double>(span.first - span.earliest_first),
        orc_videosynth::Better::Lower, 0.0},
       {"windows_with_motion", static_cast<double>(offsets.windows_with_motion),
        orc_videosynth::Better::Higher, 0.0}});
}

}  // namespace

TEST(ContentOffsetTruth, PAL) {
  run_relabel_case("pal", orc::VideoSystem::PAL);
}

TEST(ContentOffsetTruth, NTSC) {
  run_relabel_case("ntsc", orc::VideoSystem::NTSC);
}

}  // namespace orc_videosynth_test
