/*
 * File:        picture_band_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     The capture comparison's picture band on captures whose luma
 *              ends at a known frequency: two captures of each of two
 *              pressings of the Domesday model, luma band-limited
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <capture_comparison.h>
#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/video_frame_representation.h>

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

using orc_videosynth::CaseFile;

// The cutoff each system's projects/picture_band/*_band_?.yaml sets, MHz.
double set_cutoff(orc::VideoSystem system) {
  return system == orc::VideoSystem::PAL ? 4.5 : 3.8;
}

double subcarrier_mhz(orc::VideoSystem system) {
  switch (system) {
    case orc::VideoSystem::PAL:
      return orc::kPalFsc / 1.0e6;
    case orc::VideoSystem::PAL_M:
      return orc::kPalMFsc / 1.0e6;
    default:
      return orc::kNtscFsc / 1.0e6;
  }
}

void run_picture_band_case(const std::string& system_name,
                           orc::VideoSystem system) {
  std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>> captures;
  for (const char* capture : {"A_cap1", "A_cap2", "B_cap1", "B_cap2"}) {
    const std::string name = system_name + "_band_" + capture;
    captures.push_back(orc_videosynth::as_vfr(orc_videosynth::open_case(
        CaseFile{name, "videosynth_" + name, system})));
    ASSERT_NE(captures.back(), nullptr) << name;
  }
  std::vector<const orc::VideoFrameRepresentation*> sources;
  std::vector<std::optional<int64_t>> starts;
  for (const auto& capture : captures) {
    sources.push_back(capture.get());
    starts.emplace_back(0);
  }

  const orc::CaptureComparison comparison =
      orc::compare_captures(sources, starts);
  ASSERT_TRUE(comparison.fit.trusted) << comparison.fit.worst_misfit;
  ASSERT_EQ(comparison.families, 2u);
  ASSERT_TRUE(comparison.picture_band.has_value());
  const orc::PictureBand& band = *comparison.picture_band;
  EXPECT_EQ(band.pairs, 4u);

  // The luma ends where the projects end it: the coherence reaches its floor
  // within the filter's 0.4 MHz transition, or just past it where the
  // stopband still leaves picture above the noise.
  ASSERT_TRUE(band.luma_cutoff_mhz.has_value());
  const double cutoff = set_cutoff(system);
  EXPECT_GT(*band.luma_cutoff_mhz, cutoff - 0.2);
  EXPECT_LT(*band.luma_cutoff_mhz, cutoff + 0.5);
  // The chroma, left whole, runs past the subcarrier.
  ASSERT_TRUE(band.chroma_cutoff_mhz.has_value());
  EXPECT_GT(*band.chroma_cutoff_mhz, subcarrier_mhz(system) + 0.3);

  // The curves, per 0.25 MHz, for the run report.
  for (const auto& [name, curve] :
       {std::pair{"luma", &band.luma_coherence},
        std::pair{"chroma", &band.chroma_coherence}}) {
    std::string text = std::string(name) + " coherence:";
    for (double from = 0.0; from < band.frequency_mhz.back(); from += 0.25) {
      double sum = 0.0;
      size_t count = 0;
      for (size_t k = 0; k < curve->size(); ++k) {
        if (band.frequency_mhz[k] >= from &&
            band.frequency_mhz[k] < from + 0.25) {
          sum += (*curve)[k];
          ++count;
        }
      }
      if (count == 0) continue;
      char entry[32];
      std::snprintf(entry, sizeof(entry), " %.2f:%.3f", from,
                    sum / static_cast<double>(count));
      text += entry;
    }
    orc_videosynth::note(text);
  }

  orc_videosynth::expect_metrics(
      {{"luma_cutoff_error_mhz", std::abs(*band.luma_cutoff_mhz - cutoff),
        orc_videosynth::Better::Lower, 0.1},
       {"chroma_cutoff_mhz", *band.chroma_cutoff_mhz,
        orc_videosynth::Better::Higher, 0.1},
       {"luma_coherence_floor", band.luma_floor, orc_videosynth::Better::Lower,
        0.01}});
}

}  // namespace

TEST(PictureBand, PAL) { run_picture_band_case("pal", orc::VideoSystem::PAL); }

TEST(PictureBand, NTSC) {
  run_picture_band_case("ntsc", orc::VideoSystem::NTSC);
}

TEST(PictureBand, PALM) {
  run_picture_band_case("pal_m", orc::VideoSystem::PAL_M);
}

}  // namespace orc_videosynth_test
