/*
 * File:        source_align_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     Frame Map (from the Disc Mapper) and Source Align on captures
 *              of one disc that start late, end early and replay a range:
 *              the alignment found must be the captures' picture-number
 *              offsets, and the disc-anchored dropouts must then coincide
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/dropout/dropout_run.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "alignment_support.h"
#include "source_align_stage.h"
#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

using orc_videosynth::apply_frame_map;
using orc_videosynth::CaseFile;
using orc_videosynth::map_disc;
using orc_videosynth::read_pictures;
using orc_videosynth::readings_of;
using orc_videosynth::VfrPtr;

// The disc: picture numbers 1-16 (projects/alignment/*_disc.yaml).
constexpr int32_t kDiscPictures = 16;

// The captures projects/variants.json (alignment-captures) derives, and the
// picture numbers each holds, in order.
struct Capture {
  const char* suffix;
  std::vector<int32_t> pictures;
};

std::vector<int32_t> range(int32_t first, int32_t last) {
  std::vector<int32_t> pictures;
  for (int32_t p = first; p <= last; ++p) pictures.push_back(p);
  return pictures;
}

std::vector<int32_t> concat(std::vector<int32_t> a,
                            const std::vector<int32_t>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

const std::vector<Capture>& captures() {
  static const std::vector<Capture> list = {
      {"_cap1", range(1, 16)},
      {"_cap2", range(5, 16)},  // starts later
      {"_cap3", concat(concat(range(1, 12), range(9, 12)), range(13, 16))},
      {"_cap4", range(1, 12)},  // ends early
  };
  return list;
}

using RunShape = std::tuple<uint64_t, uint32_t, uint8_t>;

std::vector<RunShape> run_shapes(const orc::VideoFrameRepresentation& vfr,
                                 orc::FrameID f) {
  std::vector<RunShape> shapes;
  for (const auto& run : vfr.get_dropout_hints(f)) {
    shapes.emplace_back(run.sample_start, run.sample_count, run.severity);
  }
  std::sort(shapes.begin(), shapes.end());
  return shapes;
}

void run_alignment_case(const std::string& system_name,
                        orc::VideoSystem system) {
  // Each capture reads as its project states.
  std::vector<orc::ArtifactPtr> artifacts;
  for (const auto& capture : captures()) {
    const std::string name = system_name + "_disc" + capture.suffix;
    auto artifact =
        orc_videosynth::open_case(CaseFile{name, "videosynth_" + name, system});
    ASSERT_NE(artifact, nullptr);
    const auto pictures = read_pictures(*orc_videosynth::as_vfr(artifact));
    std::vector<std::optional<int32_t>> expected(capture.pictures.begin(),
                                                 capture.pictures.end());
    ASSERT_EQ(pictures, expected) << name;
    artifacts.push_back(artifact);
  }

  // The replayed range is removed by Frame Map, mapped by the Disc Mapper:
  // four duplicates removed, nothing padded.
  const auto decision = map_disc(*orc_videosynth::as_vfr(artifacts[2]));
  const size_t pictures_mapped =
      decision.success
          ? decision.stats.final_frames - decision.stats.padding_frames
          : 0;
  orc_videosynth::expect_metrics(
      {{"disc_mapper_pictures_mapped", static_cast<double>(pictures_mapped),
        orc_videosynth::Better::Higher, 0.0}});
  ASSERT_TRUE(decision.success) << decision.rationale;
  EXPECT_EQ(decision.stats.removed_duplicates, 4u) << decision.rationale;
  EXPECT_EQ(decision.stats.padding_frames, 0u) << decision.rationale;
  orc::FrameMapStage frame_map;
  artifacts[2] =
      apply_frame_map(artifacts[2], frame_map, decision.mapping_spec);
  ASSERT_NE(artifacts[2], nullptr);
  {
    const auto pictures = read_pictures(*orc_videosynth::as_vfr(artifacts[2]));
    const auto disc = range(1, kDiscPictures);
    ASSERT_EQ(pictures,
              std::vector<std::optional<int32_t>>(disc.begin(), disc.end()))
        << "frame_map with '" << decision.mapping_spec << "'";
  }

  // The alignment found from the picture numbers is each capture's offset
  // on the disc: the late start is padded by four frames.
  std::vector<std::vector<orc::PictureNumberReading>> readings;
  for (const auto& artifact : artifacts) {
    readings.push_back(
        readings_of(read_pictures(*orc_videosynth::as_vfr(artifact))));
  }
  const auto alignment = orc::compute_pad_alignment(readings);
  ASSERT_EQ(alignment.first_picture, 1);
  const std::vector<std::optional<size_t>> expected_pads = {0, 4, 0, 0};
  ASSERT_EQ(alignment.pads, expected_pads);

  orc::SourceAlignStage align;
  const std::map<std::string, orc::ParameterValue> parameters{
      {"alignmentMode", std::string("pad_for_alignment")},
      {"alignmentMap", orc::format_alignment_map(alignment.pads)}};
  ASSERT_TRUE(align.set_parameters(parameters));
  orc::ObservationContext ctx;
  const auto aligned = align.execute(artifacts, parameters, ctx);
  ASSERT_EQ(aligned.size(), artifacts.size());

  // Every aligned frame holds its disc picture or is padding, and the
  // disc-anchored dropouts of every capture holding a picture coincide.
  std::vector<VfrPtr> outputs;
  for (const auto& a : aligned) {
    outputs.push_back(orc_videosynth::as_vfr(a));
    ASSERT_NE(outputs.back(), nullptr);
    ASSERT_EQ(outputs.back()->frame_count(),
              static_cast<size_t>(kDiscPictures));
  }
  for (size_t i = 0; i < outputs.size(); ++i) {
    const auto pictures = read_pictures(*outputs[i]);
    for (orc::FrameID f = 0; f < pictures.size(); ++f) {
      const auto desc = outputs[i]->get_frame_descriptor(f);
      ASSERT_TRUE(desc.has_value());
      if (desc->is_padding_frame) {
        EXPECT_FALSE(pictures[f].has_value())
            << "capture " << i + 1 << " frame " << f;
        continue;
      }
      EXPECT_EQ(pictures[f], static_cast<int32_t>(f) + 1)
          << "capture " << i + 1 << " frame " << f;
      EXPECT_EQ(run_shapes(*outputs[i], f), run_shapes(*outputs[0], f))
          << "capture " << i + 1 << " frame " << f;
    }
  }
  // The padding is where the captures lack pictures.
  for (const auto& [capture, last_real] :
       {std::pair<size_t, orc::FrameID>{1, 3}, {3, 11}}) {
    for (orc::FrameID f = 0; f < kDiscPictures; ++f) {
      const bool missing = capture == 1 ? f <= last_real : f > last_real;
      EXPECT_EQ(outputs[capture]->get_frame_descriptor(f)->is_padding_frame,
                missing)
          << "capture " << capture + 1 << " frame " << f;
    }
  }
}

}  // namespace

TEST(SourceAlignTruth, PAL) {
  run_alignment_case("pal", orc::VideoSystem::PAL);
}

TEST(SourceAlignTruth, NTSC) {
  run_alignment_case("ntsc", orc::VideoSystem::NTSC);
}

TEST(SourceAlignTruth, PALM) {
  run_alignment_case("pal_m", orc::VideoSystem::PAL_M);
}

}  // namespace orc_videosynth_test
