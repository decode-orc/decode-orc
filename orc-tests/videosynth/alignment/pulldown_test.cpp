/*
 * File:        pulldown_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     The Disc Mapper on videosynth's 2:3 pulldown film fixtures:
 *              a clean capture of film keeps every frame where it is,
 *              pulldown frames included, on CAV and CLV, NTSC and PAL-M
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/common_types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "alignment_support.h"
#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

using orc_videosynth::CaseFile;

// The upstream fixtures (projects/general/ntsc_film_*.yaml and the film-clv
// and film-pal-m variants): film with 2:3 pulldown, and on ntsc_film_mixed
// film and video sections cut together with the cadence broken, carried on
// mid-letter and ended on an orphan field. Each is a clean capture of its
// disc, so the map must be the identity: every frame kept in place, none
// padded, and every frame the disc gave no number taken as a pulldown frame.
// On CAV those are the frames without a picture number; on CLV the numbers
// count video frames and skip them, one frame in five of the film, and the
// first frame is running time 0:00:00.00, picture 0.
void run_film_case(const std::string& name, orc::VideoSystem system, bool cav) {
  const auto artifact =
      orc_videosynth::open_case(CaseFile{name, "videosynth_" + name, system});
  ASSERT_NE(artifact, nullptr);
  const auto vfr = orc_videosynth::as_vfr(artifact);
  const size_t frames = vfr->frame_count();

  // On CAV the pictures are numbered densely, one per film frame, and the
  // frames between them carry none.
  size_t unnumbered = 0;
  if (cav) {
    const auto pictures = orc_videosynth::read_pictures(*vfr);
    std::optional<int32_t> previous;
    for (size_t f = 0; f < pictures.size(); ++f) {
      if (!pictures[f]) {
        ++unnumbered;
        continue;
      }
      if (previous) {
        ASSERT_EQ(*pictures[f], *previous + 1) << name << " frame " << f;
      }
      previous = pictures[f];
    }
    ASSERT_GT(unnumbered, 0u) << name << ": no pulldown frames to test";
  }

  const auto decision = orc_videosynth::map_disc(*vfr);
  const size_t kept_in_place =
      decision.success && decision.stats.padding_frames == 0
          ? decision.stats.final_frames
          : 0;
  orc_videosynth::expect_metrics(
      {{"frames_kept_in_place", static_cast<double>(kept_in_place),
        orc_videosynth::Better::Higher, 0.0}});
  ASSERT_TRUE(decision.success) << decision.rationale;
  EXPECT_EQ(decision.mapping_spec, "0-" + std::to_string(frames - 1))
      << decision.rationale;
  EXPECT_EQ(decision.stats.removed_unmappable, 0u);
  EXPECT_EQ(decision.stats.padding_frames, 0u);
  if (cav) {
    EXPECT_EQ(decision.stats.pulldown_frames, unnumbered);
  } else {
    EXPECT_GT(decision.stats.pulldown_frames, 0u);
  }
}

}  // namespace

TEST(PulldownTruth, NTSC_Film) {
  run_film_case("ntsc_film_pulldown", orc::VideoSystem::NTSC, true);
}

TEST(PulldownTruth, NTSC_FilmMixed) {
  run_film_case("ntsc_film_mixed", orc::VideoSystem::NTSC, true);
}

TEST(PulldownTruth, NTSC_FilmClv) {
  run_film_case("ntsc_film_pulldown_clv", orc::VideoSystem::NTSC, false);
}

TEST(PulldownTruth, NTSC_FilmMixedClv) {
  run_film_case("ntsc_film_mixed_clv", orc::VideoSystem::NTSC, false);
}

TEST(PulldownTruth, PALM_Film) {
  run_film_case("ntsc_film_pulldown_pal_m", orc::VideoSystem::PAL_M, true);
}

TEST(PulldownTruth, PALM_FilmMixed) {
  run_film_case("ntsc_film_mixed_pal_m", orc::VideoSystem::PAL_M, true);
}

}  // namespace orc_videosynth_test
