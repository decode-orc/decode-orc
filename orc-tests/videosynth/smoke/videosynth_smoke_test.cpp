/*
 * File:        videosynth_smoke_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     The smoke cases of the videosynth suite: cvbs_source reads what
 *              the pinned videosynth writes, in PAL, NTSC, PAL-M and SECAM,
 *              composite and Y/C, and the colour-frame observer reads
 *              SECAM's 2-frame sequence
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/dropout/dropout_run.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/observers/biphase_observer.h>
#include <orc/support/observers/colour_frame_phase_observer.h>

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

// SECAM has no LaserDisc, so its smoke case (projects/smoke/secam_smoke.yaml)
// has no picture numbers. In their place: the levels and the unlocked
// declaration SECAM must carry, and the 2-frame sequence, which videosynth
// starts at frame 1 (its truth's secam_frame gives dr_line_parity 1 on frame
// 0; its OSD {phase_id} reads 0, 1, 0, 1).
void expect_secam_sequence(const orc::VideoFrameRepresentation& vfr,
                           size_t frames) {
  orc::ColourFramePhaseObserver observer;
  orc::ObservationContext ctx;
  for (orc::FrameID f = 0; f < frames; ++f) {
    observer.process_frame(vfr, f, ctx);
    const int32_t sequence_frame = f % 2 == 0 ? 1 : 2;
    for (const orc::FrameID field : {f * 2, (f * 2) + 1}) {
      const int32_t field_index = static_cast<int32_t>(field % 2);
      EXPECT_EQ(read_int(ctx, orc::FieldID(field), "colour_frame_phase",
                         "colour_frame_index"),
                sequence_frame)
          << "frame " << f << " field " << field_index + 1;
      EXPECT_EQ(read_int(ctx, orc::FieldID(field), "colour_frame_phase",
                         "field_phase_id"),
                ((sequence_frame - 1) * 2) + field_index + 1)
          << "frame " << f << " field " << field_index + 1;
    }
  }
}

void run_secam_smoke_case(const SmokeCase& c) {
  const auto dir = orc_videosynth::case_dir(c.case_name);
  ASSERT_TRUE(dir.has_value())
      << "ORC_VIDEOSYNTH_OUTPUT_DIR is not set; run through ctest";
  const fs::path base = *dir / c.stem;

  const auto vfr = orc_videosynth::as_vfr(
      orc_videosynth::open_case({c.case_name, c.stem, c.system, c.yc}));
  ASSERT_NE(vfr, nullptr);
  EXPECT_EQ(vfr->has_separate_channels(), c.yc);
  ASSERT_EQ(vfr->frame_count(), kFrameCount);
  for (orc::FrameID f = 0; f < kFrameCount; ++f) {
    const auto desc = vfr->get_frame_descriptor(f);
    ASSERT_TRUE(desc.has_value()) << "frame " << f;
    EXPECT_EQ(desc->system, c.system) << "frame " << f;
    EXPECT_EQ(desc->height, c.height) << "frame " << f;
    EXPECT_EQ(desc->samples_per_line_nominal, c.samples_per_line)
        << "frame " << f;
  }

  // The levels of the 10-bit domain (CVBS spec v1.8.1, Preset SECAM), and
  // STANDARD_STABLE_UNLOCKED: SECAM has no Sc/H convention.
  const auto params = vfr->get_video_parameters();
  ASSERT_TRUE(params.has_value());
  EXPECT_EQ(params->system, orc::VideoSystem::SECAM);
  EXPECT_EQ(params->sync_tip_level, orc::kSecamSyncTip);
  EXPECT_EQ(params->blanking_level, orc::kSecamBlanking);
  EXPECT_EQ(params->black_level, orc::kSecamBlack);
  EXPECT_EQ(params->white_level, orc::kSecamWhite);
  EXPECT_FALSE(params->is_subcarrier_locked);

  expect_secam_sequence(*vfr, kFrameCount);

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

TEST(VideosynthSmoke, SECAM_Composite) {
  run_secam_smoke_case({"secam_smoke", "videosynth_secam_smoke",
                        orc::VideoSystem::SECAM, false, 625, 1135});
}

TEST(VideosynthSmoke, SECAM_YC) {
  run_secam_smoke_case({"secam_smoke_yc", "videosynth_secam_smoke",
                        orc::VideoSystem::SECAM, true, 625, 1135});
}

// Without the field identification signals (secam_field_identification:
// false, projects/identification/), the back-porch reference alone gives the
// same sequence.
TEST(VideosynthSmoke, SECAM_LineIdentificationOnly) {
  const auto vfr = orc_videosynth::as_vfr(orc_videosynth::open_case(
      {"secam_line_identification", "videosynth_secam_line_identification",
       orc::VideoSystem::SECAM}));
  ASSERT_NE(vfr, nullptr);
  ASSERT_EQ(vfr->frame_count(), kFrameCount);
  expect_secam_sequence(*vfr, kFrameCount);
}

}  // namespace orc_videosynth_test
