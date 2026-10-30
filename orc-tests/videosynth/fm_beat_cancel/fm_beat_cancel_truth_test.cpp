/*
 * File:        fm_beat_cancel_truth_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     fm_beat_cancel on a pressing whose master carries the FM-beat
 *              herringbone: the beat it removes against the beat videosynth
 *              recorded, and the carrier FM Beat Calibration measures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <fm_beat_calibration.h>
#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/common_types.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <truth_comparison.h>

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fm_beat_cancel_stage.h"
#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

using orc_videosynth::Better;
using orc_videosynth::CaseFile;
using orc_videosynth::Metric;
using orc_videosynth::VfrPtr;

// The carrier the case's master was cut with (projects/fm_beat/).
constexpr double kCaseCarrierMhz = 7.085;

// The stage's output on |input|, which it refers to while it is read.
struct Cancelled {
  std::unique_ptr<orc::FmBeatCancelStage> stage;
  std::unique_ptr<orc::ObservationContext> context;
  VfrPtr output;
};

Cancelled cancel(const orc::ArtifactPtr& input, double carrier_mhz) {
  Cancelled r;
  r.stage = std::make_unique<orc::FmBeatCancelStage>();
  r.context = std::make_unique<orc::ObservationContext>();
  const std::map<std::string, orc::ParameterValue> parameters{
      {"carrier_black_mhz", carrier_mhz}};
  const auto outputs = r.stage->execute({input}, parameters, *r.context);
  if (outputs.size() == 1) r.output = orc_videosynth::as_vfr(outputs[0]);
  return r;
}

struct Cases {
  orc::ArtifactPtr capture_artifact;
  orc::ArtifactPtr clean_artifact;
  VfrPtr capture;  // the beat, with noise
  VfrPtr clean;    // the beat alone on the picture
  VfrPtr truth;    // the picture without the beat
};

Cases open_cases() {
  Cases c;
  c.capture_artifact = orc_videosynth::open_case(
      CaseFile{"pal_fm_beat", "videosynth_pal_fm_beat", orc::VideoSystem::PAL});
  c.clean_artifact = orc_videosynth::open_case(
      CaseFile{"pal_fm_beat_clean", "videosynth_pal_fm_beat_clean",
               orc::VideoSystem::PAL});
  const auto truth_artifact = orc_videosynth::open_case(
      CaseFile{"pal_fm_beat_truth", "videosynth_pal_fm_beat_truth",
               orc::VideoSystem::PAL});
  c.capture = orc_videosynth::as_vfr(c.capture_artifact);
  c.clean = orc_videosynth::as_vfr(c.clean_artifact);
  c.truth = orc_videosynth::as_vfr(truth_artifact);
  return c;
}

// The sections of projects/fm_beat/pal_fm_beat.yaml, four frames each, and
// whether the beat on each must come down by 10 dB (phase 4's gate: flat
// saturated colour) or be no larger afterwards (pictures the stage mostly
// leaves as they are).
struct Section {
  const char* name;
  bool gated;
};
constexpr Section kSections[] = {{"bars", true},
                                 {"red_bars", true},
                                 {"test_card", false},
                                 {"tartan", false}};
constexpr orc::FrameID kSectionFrames = 4;
constexpr double kGateDb = 10.0;

// The band the beat lies in (2.7-3.2 MHz on flat colour).
constexpr size_t kBeatBand = 1;  // luma_high, 2.0-3.5 MHz

}  // namespace

// The beat each section carries, what is left of it after the stage at the
// master's carrier (on the capture and on the noise-free copy), and at the
// nominal carrier, uncalibrated.
TEST(FmBeatCancelTruth, PAL) {
  const auto cases = open_cases();
  ASSERT_TRUE(cases.capture && cases.clean && cases.truth);
  ASSERT_EQ(cases.capture->frame_count(),
            std::size(kSections) * kSectionFrames);
  ASSERT_EQ(cases.truth->frame_count(), cases.capture->frame_count());
  const auto params = cases.truth->get_video_parameters();
  ASSERT_TRUE(params.has_value());
  const double ire = orc_videosynth::ire_per_code(*params);

  const auto calibrated = cancel(cases.capture_artifact, kCaseCarrierMhz);
  const auto noise_free = cancel(cases.clean_artifact, kCaseCarrierMhz);
  const auto nominal = cancel(cases.capture_artifact, 7.1);
  ASSERT_TRUE(calibrated.output && noise_free.output && nominal.output);

  using orc_videosynth::new_tolerance;
  std::vector<Metric> metrics;
  for (size_t s = 0; s < std::size(kSections); ++s) {
    const std::string name = kSections[s].name;
    orc_videosynth::BandErrorMeter beat(orc::VideoSystem::PAL, ire);
    orc_videosynth::BandErrorMeter left(orc::VideoSystem::PAL, ire);
    orc_videosynth::BandErrorMeter left_noise_free(orc::VideoSystem::PAL, ire);
    orc_videosynth::BandErrorMeter left_nominal(orc::VideoSystem::PAL, ire);
    int unchanged = 0;
    for (orc::FrameID k = 0; k < kSectionFrames; ++k) {
      const orc::FrameID f = (s * kSectionFrames) + k;
      // Owned copies: a representation's frame pointer lasts only until its
      // next call.
      const auto capture = cases.capture->get_frame_copy(f);
      const auto clean = cases.clean->get_frame_copy(f);
      const auto truth = cases.truth->get_frame_copy(f);
      const auto out = calibrated.output->get_frame_copy(f);
      const auto out_clean = noise_free.output->get_frame_copy(f);
      const auto out_nominal = nominal.output->get_frame_copy(f);
      ASSERT_EQ(capture.size(), truth.size());
      ASSERT_EQ(out.size(), truth.size());
      // The capture without its beat: the noise is added after the beat, so
      // the capture less the beat (clean - truth) is the truth plus noise.
      std::vector<int16_t> without(truth.size());
      for (size_t i = 0; i < truth.size(); ++i) {
        without[i] = static_cast<int16_t>(capture[i] - clean[i] + truth[i]);
      }
      beat.add_frame(capture.data(), without.data(), *params);
      left.add_frame(out.data(), without.data(), *params);
      left_noise_free.add_frame(out_clean.data(), truth.data(), *params);
      left_nominal.add_frame(out_nominal.data(), without.data(), *params);
      if (out == capture) ++unchanged;
    }
    const double beat_rms = beat.rms_ire()[kBeatBand];
    const double left_rms = left.rms_ire()[kBeatBand];
    const double noise_free_rms = left_noise_free.rms_ire()[kBeatBand];
    const double nominal_rms = left_nominal.rms_ire()[kBeatBand];
    const double reduction =
        20.0 * std::log10(beat_rms / std::max(left_rms, 1e-9));
    if (kSections[s].gated) {
      EXPECT_GE(reduction, kGateDb) << name;
    } else {
      // A picture the stage changes must be no further from the truth.
      EXPECT_LE(left.total_rms_ire(), beat.total_rms_ire() + 1e-9) << name;
    }
    metrics.push_back({name + "_beat_rms_ire", beat_rms, Better::Lower,
                       new_tolerance(beat_rms, 0.02, 0.001)});
    metrics.push_back({name + "_left_rms_ire", left_rms, Better::Lower,
                       new_tolerance(left_rms, 0.05, 0.002)});
    metrics.push_back({name + "_left_noise_free_rms_ire", noise_free_rms,
                       Better::Lower,
                       new_tolerance(noise_free_rms, 0.05, 0.002)});
    metrics.push_back({name + "_left_nominal_carrier_rms_ire", nominal_rms,
                       Better::Lower, new_tolerance(nominal_rms, 0.05, 0.002)});
    metrics.push_back({name + "_frames_unchanged",
                       static_cast<double>(unchanged), Better::Higher, 0.0});
  }
  orc_videosynth::expect_metrics(metrics);
}

// On the same pictures without a beat, the stage finds little to remove:
// whatever it changes is damage.
TEST(FmBeatCancelTruth, PAL_NoBeat) {
  const auto truth_artifact = orc_videosynth::open_case(
      CaseFile{"pal_fm_beat_truth", "videosynth_pal_fm_beat_truth",
               orc::VideoSystem::PAL});
  const auto truth = orc_videosynth::as_vfr(truth_artifact);
  ASSERT_TRUE(truth);
  const auto params = truth->get_video_parameters();
  ASSERT_TRUE(params.has_value());
  const double ire = orc_videosynth::ire_per_code(*params);
  const auto cancelled = cancel(truth_artifact, kCaseCarrierMhz);
  ASSERT_TRUE(cancelled.output);

  std::vector<Metric> metrics;
  for (size_t s = 0; s < std::size(kSections); ++s) {
    const std::string name = kSections[s].name;
    orc_videosynth::BandErrorMeter changed(orc::VideoSystem::PAL, ire);
    int unchanged = 0;
    for (orc::FrameID k = 0; k < kSectionFrames; ++k) {
      const orc::FrameID f = (s * kSectionFrames) + k;
      const auto t = truth->get_frame_copy(f);
      const auto out = cancelled.output->get_frame_copy(f);
      ASSERT_EQ(out.size(), t.size());
      changed.add_frame(out.data(), t.data(), *params);
      if (out == t) ++unchanged;
    }
    const double change = changed.total_rms_ire();
    metrics.push_back({name + "_change_rms_ire", change, Better::Lower,
                       orc_videosynth::new_tolerance(change, 0.1, 0.005)});
    metrics.push_back({name + "_frames_unchanged",
                       static_cast<double>(unchanged), Better::Higher, 0.0});
  }
  orc_videosynth::expect_metrics(metrics);
}

// The calibration finds the master's carrier on the capture.
TEST(FmBeatCancelTruth, PAL_Calibration) {
  const auto cases = open_cases();
  ASSERT_TRUE(cases.capture);
  const auto calibration = orc::calibrate_fm_beat(
      *cases.capture, orc::FmBeatCalibrationOptions{}, {});
  ASSERT_TRUE(calibration.found);
  const double error_khz =
      (calibration.carrier_black_hz / 1.0e6 - kCaseCarrierMhz) * 1.0e3;
  EXPECT_LT(std::abs(error_khz), 1.0);
  orc_videosynth::expect_metrics(
      {{"carrier_error_khz", std::abs(error_khz), Better::Lower, 0.25},
       {"fields_searched", static_cast<double>(calibration.fields),
        Better::Higher, 0.0},
       {"lines_corrected", static_cast<double>(calibration.lines_corrected),
        Better::Higher,
        orc_videosynth::new_tolerance(
            static_cast<double>(calibration.lines_corrected), 0.05, 1.0)}});
}

}  // namespace orc_videosynth_test
