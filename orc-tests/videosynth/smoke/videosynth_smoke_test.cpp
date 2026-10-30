/*
 * File:        videosynth_smoke_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     The smoke cases of the videosynth suite: cvbs_source reads what
 *              the pinned videosynth writes, in PAL, NTSC and PAL-M, composite
 *              and Y/C
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <biphase_observer.h>
#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/dropout/dropout_run.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

namespace fs = std::filesystem;

// What every smoke project (orc-tests/videosynth/projects/smoke/*.yaml)
// states: four frames of a CAV disc, picture numbers 1-4.
constexpr size_t kFrameCount = 4;
constexpr int32_t kFirstPictureNumber = 1;

struct SmokeCase {
  const char* case_name;  // orc_videosynth_case() name in CMakeLists.txt
  const char* stem;       // output.video_path without its suffix
  orc::VideoSystem system;
  bool yc;
  size_t height;
  size_t samples_per_line;
};

using orc_videosynth::RunKey;

RunKey key_of(const orc::DropoutRun& run) {
  return {run.frame_id, run.sample_start, run.sample_count, run.severity};
}

std::optional<int32_t> read_int(const orc::ObservationContext& ctx,
                                orc::FieldID field_id, const std::string& ns,
                                const std::string& key) {
  const auto value = ctx.get(field_id, ns, key);
  if (!value || !std::holds_alternative<int32_t>(*value)) return std::nullopt;
  return std::get<int32_t>(*value);
}

void run_smoke_case(const SmokeCase& c) {
  // The suite exists only in a build configured for it, so a missing
  // variable is a failure, not a skip.
  const auto dir = orc_videosynth::case_dir(c.case_name);
  ASSERT_TRUE(dir.has_value())
      << "ORC_VIDEOSYNTH_OUTPUT_DIR is not set; run through ctest";
  const fs::path base = *dir / c.stem;

  // cvbs_source opens the file.
  const auto vfr = orc_videosynth::as_vfr(
      orc_videosynth::open_case({c.case_name, c.stem, c.system, c.yc}));
  ASSERT_NE(vfr, nullptr);
  EXPECT_EQ(vfr->has_separate_channels(), c.yc);

  // The frame count and geometry match the project.
  ASSERT_EQ(vfr->frame_count(), kFrameCount);
  for (orc::FrameID f = 0; f < kFrameCount; ++f) {
    const auto desc = vfr->get_frame_descriptor(f);
    ASSERT_TRUE(desc.has_value()) << "frame " << f;
    EXPECT_EQ(desc->system, c.system) << "frame " << f;
    EXPECT_EQ(desc->height, c.height) << "frame " << f;
    EXPECT_EQ(desc->samples_per_line_nominal, c.samples_per_line)
        << "frame " << f;
  }

  // The picture numbers decode in order, confirmed by both VBI lines.
  // videosynth writes them on field 1 only, so finding them on the first field
  // of each frame, and not on the second, confirms the field order.
  orc::BiphaseObserver observer;
  orc::ObservationContext vbi_context;
  for (orc::FrameID f = 0; f < kFrameCount; ++f) {
    observer.process_frame(*vfr, f, vbi_context);
    const orc::FieldID first_field(f * 2);
    EXPECT_EQ(read_int(vbi_context, first_field, "vbi", "picture_number"),
              kFirstPictureNumber + static_cast<int32_t>(f))
        << "frame " << f;
    EXPECT_EQ(read_int(vbi_context, first_field, "vbi",
                       "picture_number_cross_validated"),
              1)
        << "frame " << f;
    EXPECT_FALSE(
        read_int(vbi_context, orc::FieldID(f * 2 + 1), "vbi", "picture_number")
            .has_value())
        << "frame " << f << ": picture number on the second field";
  }

  // The dropouts cvbs_source reports are the runs videosynth wrote.
  const auto expected =
      orc_videosynth::read_sidecar_runs(base.string() + ".dropouts.meta");
  EXPECT_FALSE(expected.empty()) << "the smoke projects carry dropouts";
  std::vector<RunKey> actual;
  for (orc::FrameID f = 0; f < kFrameCount; ++f) {
    for (const auto& run : vfr->get_dropout_hints(f)) {
      EXPECT_EQ(run.frame_id, f);
      actual.push_back(key_of(run));
    }
  }
  EXPECT_EQ(actual, expected);
}

}  // namespace

TEST(VideosynthSmoke, PAL_Composite) {
  run_smoke_case({"pal_smoke", "videosynth_pal_smoke", orc::VideoSystem::PAL,
                  false, 625, 1135});
}

TEST(VideosynthSmoke, PAL_YC) {
  run_smoke_case({"pal_smoke_yc", "videosynth_pal_smoke", orc::VideoSystem::PAL,
                  true, 625, 1135});
}

TEST(VideosynthSmoke, NTSC_Composite) {
  run_smoke_case({"ntsc_smoke", "videosynth_ntsc_smoke", orc::VideoSystem::NTSC,
                  false, 525, 910});
}

TEST(VideosynthSmoke, NTSC_YC) {
  run_smoke_case({"ntsc_smoke_yc", "videosynth_ntsc_smoke",
                  orc::VideoSystem::NTSC, true, 525, 910});
}

TEST(VideosynthSmoke, PALM_Composite) {
  run_smoke_case({"pal_m_smoke", "videosynth_pal_m_smoke",
                  orc::VideoSystem::PAL_M, false, 525, 909});
}

TEST(VideosynthSmoke, PALM_YC) {
  run_smoke_case({"pal_m_smoke_yc", "videosynth_pal_m_smoke",
                  orc::VideoSystem::PAL_M, true, 525, 909});
}

}  // namespace orc_videosynth_test
