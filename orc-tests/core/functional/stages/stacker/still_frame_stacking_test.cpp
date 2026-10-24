/*
 * File:        still_frame_stacking_test.cpp
 * Module:      orc-core-functional-tests
 * Purpose:     Still-frame repeat stacking on a real capture: copies are used
 *              on held stills and not across a crossfade, frames without
 *              copies pass through unchanged, and VBI lines are the frame's
 *              own
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cvbs_source_stage.h"
#include "stacker_stage.h"

namespace orc_functional_test {

namespace {

using sample_type = orc::VideoFrameRepresentation::sample_type;

// Volcanoes side 1, capture DS1, file frames 9440-9487
// (docs-tech/still-frame-stacking-plan.md, Test material): the last frames
// of a held world map (0-18 here), an 18-frame crossfade to a map of the
// plate boundaries (19-33), and that map held (34-47).  It is not checked
// into the repository (test-data/ is ignored), so every test here skips when
// it is absent.  Cut it from the capture with
//   dd if=<DS1>.cvbs of=volcanoes-ds1-9440.cvbs bs=1418758 skip=9440 count=48
// and the .meta and .dropouts.meta sidecars cut to match (set
// cvbs_file.number_of_sequential_frames to 48, keep the dropout_run rows of
// frames 9440-9487 and renumber them from 0).
const std::string kSampleBase =
    std::string(ORC_STILL_TEST_DATA_DIR) + "/still-stacking/volcanoes-ds1-9440";

constexpr size_t kFrames = 48;
constexpr size_t kWidth = 1135;
constexpr size_t kHeight = 625;

bool sample_available() {
  return std::filesystem::exists(kSampleBase + ".cvbs") &&
         std::filesystem::exists(kSampleBase + ".meta");
}

std::shared_ptr<const orc::VideoFrameRepresentation> read_sample(
    orc::PALCVBSSourceStage& source) {
  const std::map<std::string, orc::ParameterValue> parameters{
      {"input_path", kSampleBase + ".cvbs"}};
  EXPECT_TRUE(source.set_parameters(parameters));
  orc::ObservationContext observation_context;
  const auto outputs = source.execute({}, parameters, observation_context);
  EXPECT_EQ(outputs.size(), 1u);
  return outputs.empty()
             ? nullptr
             : std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
                   outputs[0]);
}

std::shared_ptr<const orc::StackedVideoFrameRepresentation> stack_stills(
    orc::StackerStage& stacker,
    const std::shared_ptr<const orc::VideoFrameRepresentation>& input) {
  EXPECT_TRUE(stacker.set_parameters(
      {{"still_frame_stacking", true}, {"still_frame_search", int32_t{8}}}));
  return std::dynamic_pointer_cast<const orc::StackedVideoFrameRepresentation>(
      stacker.process({input}));
}

// True when line |line| of the frame is not active picture: the lines copies
// never touch.
bool is_vbi_line(size_t line) {
  const auto field1 = static_cast<size_t>(orc::kPalField1Lines);
  const size_t field_line = line < field1 ? line : line - field1;
  return field_line < static_cast<size_t>(orc::kPalFirstActiveFrameLine) / 2 ||
         field_line >= static_cast<size_t>(orc::kPalLastActiveFrameLine) / 2;
}

}  // namespace

// Copies are used on both held maps and on no frame of the crossfade.
TEST(StillFrameStacking, VolcanoesCut_CopiesOnHeldFramesOnly) {
  if (!sample_available()) {
    GTEST_SKIP() << "Sample not present: " << kSampleBase << ".cvbs";
  }
  orc::PALCVBSSourceStage source;
  const auto input = read_sample(source);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(input->frame_count(), kFrames);

  orc::StackerStage stacker;
  const auto stacked = stack_stills(stacker, input);
  ASSERT_NE(stacked, nullptr);

  for (orc::FrameID f = 0; f < kFrames; ++f) {
    const auto copies = stacked->still_copies(f);
    if (f <= 14 || (f >= 36 && f <= 44)) {
      EXPECT_GE(copies.size(),
                orc::StackedVideoFrameRepresentation::kMinStillCopies)
          << "frame " << f << " is a held still";
    } else if (f >= 20 && f <= 32) {
      EXPECT_TRUE(copies.empty()) << "frame " << f << " is in the crossfade";
    }
    for (const auto& copy : copies) {
      EXPECT_EQ(copy.source, 0u);
      // Only frames at the PAL colour cadence carry the same phase.
      EXPECT_EQ((copy.id > f ? copy.id - f : f - copy.id) % 4, 0u)
          << "frame " << f << " copy " << copy.id;
    }
  }
}

// A frame without copies is the input frame, samples and dropouts.  A field
// with copies changes in active picture and keeps its level, a field without
// them is unchanged, and the VBI lines are the frame's own.
TEST(StillFrameStacking, VolcanoesCut_OutputKeepsUncopiedFramesAndVbi) {
  if (!sample_available()) {
    GTEST_SKIP() << "Sample not present: " << kSampleBase << ".cvbs";
  }
  orc::PALCVBSSourceStage source;
  const auto input = read_sample(source);
  ASSERT_NE(input, nullptr);

  orc::StackerStage stacker;
  const auto stacked = stack_stills(stacker, input);
  ASSERT_NE(stacked, nullptr);
  ASSERT_EQ(stacked->frame_count(), kFrames);

  size_t frames_with_copies = 0;
  for (orc::FrameID f = 0; f < kFrames; ++f) {
    const std::vector<sample_type> in = input->get_frame_copy(f);
    const std::vector<sample_type> out = stacked->get_frame_copy(f);
    ASSERT_EQ(in.size(), out.size()) << "frame " << f;

    const auto copies = stacked->still_copies(f);
    if (copies.empty()) {
      EXPECT_EQ(in, out) << "frame " << f << " has no copies";
      EXPECT_EQ(input->get_dropout_hints(f).size(),
                stacked->get_dropout_hints(f).size())
          << "frame " << f;
      continue;
    }
    ++frames_with_copies;

    // A copy can be used for one field only.
    std::array<bool, 2> copied{false, false};
    for (const auto& copy : copies) {
      copied[0] = copied[0] || copy.field[0];
      copied[1] = copied[1] || copy.field[1];
    }

    size_t vbi_changed = 0;
    std::array<double, 2> active_sum{0.0, 0.0};
    std::array<size_t, 2> active_count{0, 0};
    std::array<size_t, 2> active_changed{0, 0};
    for (size_t line = 0; line < kHeight; ++line) {
      const size_t field =
          line < static_cast<size_t>(orc::kPalField1Lines) ? 0 : 1;
      const size_t start =
          orc::frame_line_sample_offset(orc::VideoSystem::PAL, kWidth, line);
      const size_t count =
          orc::frame_line_sample_count(orc::VideoSystem::PAL, kWidth, line);
      for (size_t n = start; n < start + count; ++n) {
        const int difference = out[n] - in[n];
        if (is_vbi_line(line)) {
          if (difference != 0) ++vbi_changed;
          continue;
        }
        active_sum[field] += difference;
        ++active_count[field];
        if (difference != 0) ++active_changed[field];
      }
    }
    EXPECT_EQ(vbi_changed, 0u) << "frame " << f;
    for (size_t field = 0; field < 2; ++field) {
      if (!copied[field]) {
        EXPECT_EQ(active_changed[field], 0u)
            << "frame " << f << " field " << field + 1 << " has no copies";
        continue;
      }
      EXPECT_GT(active_changed[field], active_count[field] / 2)
          << "frame " << f << " field " << field + 1;
      // Noise is taken out, not level: the mean change stays well inside a
      // code.
      EXPECT_LT(std::abs(active_sum[field] /
                         static_cast<double>(active_count[field])),
                1.0)
          << "frame " << f << " field " << field + 1;
    }
  }
  EXPECT_GE(frames_with_copies, 24u);
}

// With the option off, one input passes through as it always has.
TEST(StillFrameStacking, VolcanoesCut_OffPassesOneInputThrough) {
  if (!sample_available()) {
    GTEST_SKIP() << "Sample not present: " << kSampleBase << ".cvbs";
  }
  orc::PALCVBSSourceStage source;
  const auto input = read_sample(source);
  ASSERT_NE(input, nullptr);

  orc::StackerStage stacker;
  EXPECT_EQ(stacker.process({input}), input);
}

}  // namespace orc_functional_test
