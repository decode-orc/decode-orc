/*
 * File:        picture_band_limit_truth_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     picture_band_limit on the picture-band cases, measured
 *              against their truth: the picture it keeps below the cutoff
 *              and the noise it removes above it, against baselines.json
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/common_types.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <truth_comparison.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "picture_band_limit_stage.h"
#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

using orc_videosynth::Better;
using orc_videosynth::CaseFile;
using orc_videosynth::Metric;
using orc_videosynth::VfrPtr;

// The stage's output on |input|, which it refers to while it is read.
struct Limited {
  std::unique_ptr<orc::PictureBandLimitStage> stage;
  std::unique_ptr<orc::ObservationContext> context;
  VfrPtr output;
};

Limited band_limit(const orc::ArtifactPtr& input, double cutoff_mhz) {
  Limited r;
  r.stage = std::make_unique<orc::PictureBandLimitStage>();
  r.context = std::make_unique<orc::ObservationContext>();
  const std::map<std::string, orc::ParameterValue> parameters{
      {"cutoff_mhz", cutoff_mhz}};
  const auto outputs = r.stage->execute({input}, parameters, *r.context);
  if (outputs.size() == 1) r.output = orc_videosynth::as_vfr(outputs[0]);
  return r;
}

// Each case's picture is luma band-limited at |picture_mhz|
// (projects/picture_band/*_band_A.yaml), with a 0.4 MHz transition. The
// stage cuts 0.4 MHz above it, as the capture comparison's measured cutoff
// sits 0.2-0.3 MHz above the picture's half-amplitude point, so the stage's
// pass band reaches the picture's stop band.
void run_band_limit_case(const std::string& system_name,
                         orc::VideoSystem system, double picture_mhz) {
  const std::string truth_name = system_name + "_band_A_clean";
  const std::string capture_name = system_name + "_band_A_cap1";
  const auto truth_artifact = orc_videosynth::open_case(CaseFile{
      truth_name, "videosynth_" + system_name + "_band_A_clean", system});
  const auto capture_artifact = orc_videosynth::open_case(CaseFile{
      capture_name, "videosynth_" + system_name + "_band_A_cap1", system});
  const auto truth = orc_videosynth::as_vfr(truth_artifact);
  const auto capture = orc_videosynth::as_vfr(capture_artifact);
  ASSERT_TRUE(truth && capture);
  ASSERT_EQ(truth->frame_count(), capture->frame_count());
  const auto params = truth->get_video_parameters();
  ASSERT_TRUE(params.has_value());
  const double ire = orc_videosynth::ire_per_code(*params);

  const double cutoff = picture_mhz + 0.4;
  const auto limited_truth = band_limit(truth_artifact, cutoff);
  const auto limited_capture = band_limit(capture_artifact, cutoff);
  ASSERT_TRUE(limited_truth.output && limited_capture.output);

  // The truth's own content per band; what the stage changes of it; the
  // capture's error against it, before and after.
  orc_videosynth::BandErrorMeter signal(system, ire);
  orc_videosynth::BandErrorMeter kept(system, ire);
  orc_videosynth::BandErrorMeter noise_before(system, ire);
  orc_videosynth::BandErrorMeter noise_after(system, ire);
  for (orc::FrameID f = 0; f < truth->frame_count(); ++f) {
    // Owned copies: a representation's frame pointer lasts only until its
    // next call.
    const auto t = truth->get_frame_copy(f);
    const auto c = capture->get_frame_copy(f);
    const auto lt = limited_truth.output->get_frame_copy(f);
    const auto lc = limited_capture.output->get_frame_copy(f);
    ASSERT_EQ(lt.size(), t.size());
    ASSERT_EQ(lc.size(), t.size());
    const std::vector<int16_t> zero(t.size(), 0);
    signal.add_frame(zero.data(), t.data(), *params);
    kept.add_frame(lt.data(), t.data(), *params);
    noise_before.add_frame(c.data(), t.data(), *params);
    noise_after.add_frame(lc.data(), t.data(), *params);
  }

  using orc_videosynth::new_tolerance;
  std::vector<Metric> metrics;
  const auto& bands = signal.bands();
  const auto signal_rms = signal.rms_ire();
  const auto kept_rms = kept.rms_ire();
  const auto before_rms = noise_before.rms_ire();
  const auto after_rms = noise_after.rms_ire();
  for (size_t b = 0; b < bands.size(); ++b) {
    // The picture the stage changes, relative to the picture in the band:
    // within 1 % below the cutoff.
    const double change = kept_rms[b] / signal_rms[b];
    const bool below_cutoff = bands[b].high_hz <= (cutoff - 0.2) * 1e6;
    if (below_cutoff) {
      EXPECT_LT(change, 0.01) << bands[b].name;
      // Below the cutoff the noise stays: the stage removes only what lies
      // above it.
      EXPECT_NEAR(after_rms[b], before_rms[b], 0.02 * before_rms[b])
          << bands[b].name;
    }
    metrics.push_back({bands[b].name + "_picture_change", change, Better::Lower,
                       new_tolerance(change, 0.05, 0.001)});
    metrics.push_back({bands[b].name + "_noise_rms_ire", after_rms[b],
                       Better::Lower, new_tolerance(after_rms[b], 0.02, 0.01)});
  }
  const double total_before = noise_before.total_rms_ire();
  const double total_after = noise_after.total_rms_ire();
  EXPECT_LT(total_after, total_before);
  metrics.push_back({"total_noise_rms_ire", total_after, Better::Lower,
                     new_tolerance(total_after, 0.02, 0.01)});
  if (const auto gain = orc_videosynth::gain_db(total_before, total_after)) {
    metrics.push_back({"total_noise_gain_db", *gain, Better::Higher, 0.1});
  }
  orc_videosynth::expect_metrics(metrics);
}

}  // namespace

TEST(PictureBandLimitTruth, PAL) {
  run_band_limit_case("pal", orc::VideoSystem::PAL, 4.5);
}

TEST(PictureBandLimitTruth, NTSC) {
  run_band_limit_case("ntsc", orc::VideoSystem::NTSC, 3.8);
}

TEST(PictureBandLimitTruth, PALM) {
  run_band_limit_case("pal_m", orc::VideoSystem::PAL_M, 3.8);
}

}  // namespace orc_videosynth_test
