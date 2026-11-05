/*
 * File:        channel_equalise_truth_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     channel_equalise on a pressing whose master carries the
 *              Domesday CAV channel: the smear Channel Calibration reads, and
 *              the stage's output against the same pressing without the
 *              channel
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <channel_calibration.h>
#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/common_types.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/channel_equalise.h>
#include <truth_comparison.h>

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "channel_equalise_stage.h"
#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

using orc_videosynth::Better;
using orc_videosynth::CaseFile;
using orc_videosynth::Metric;
using orc_videosynth::VfrPtr;

// The one-pole smear that best explains the case's line 19 tails: an
// independent least-squares fit of the same model (bar, 2T pulse and base,
// samples 0.4 us or more from an edge) over all twelve frames reads this.
// It is not the 0.24 / 6.1 the project puts in, because the fit sees the
// smear and the inner-radius replay response together, as it does on a
// capture.
constexpr double kChannelFraction = 0.2011;
constexpr double kChannelTau = 5.405;
// The fraction must land within a tenth of the smear itself.
constexpr double kFractionTolerance = 0.02;

// The burst after the stage: within this share of the nominal.
constexpr double kBurstTolerance = 0.01;

// The stage's output on |input|, which it refers to while it is read.
struct Equalised {
  std::unique_ptr<orc::ChannelEqualiseStage> stage;
  std::unique_ptr<orc::ObservationContext> context;
  VfrPtr output;
};

Equalised equalise(const orc::ArtifactPtr& input, double fraction,
                   double tau_samples) {
  Equalised r;
  r.stage = std::make_unique<orc::ChannelEqualiseStage>();
  r.context = std::make_unique<orc::ObservationContext>();
  const std::map<std::string, orc::ParameterValue> parameters{
      {"smear_fraction", fraction}, {"smear_tau_samples", tau_samples}};
  const auto outputs = r.stage->execute({input}, parameters, *r.context);
  if (outputs.size() == 1) r.output = orc_videosynth::as_vfr(outputs[0]);
  return r;
}

struct Cases {
  orc::ArtifactPtr channel_artifact;
  orc::ArtifactPtr none_artifact;
  VfrPtr channel;  // the pressing with the channel
  VfrPtr none;     // the same pressing without it
};

Cases open_cases() {
  Cases c;
  c.channel_artifact = orc_videosynth::open_case(
      CaseFile{"pal_channel", "videosynth_pal_channel", orc::VideoSystem::PAL});
  c.none_artifact = orc_videosynth::open_case(
      CaseFile{"pal_channel_none", "videosynth_pal_channel_none",
               orc::VideoSystem::PAL});
  c.channel = orc_videosynth::as_vfr(c.channel_artifact);
  c.none = orc_videosynth::as_vfr(c.none_artifact);
  return c;
}

// The sections of projects/channel/pal_channel.yaml, three frames each.
constexpr const char* kSections[] = {"bars", "test_card", "town", "photo"};
constexpr orc::FrameID kSectionFrames = 3;

}  // namespace

// The smear the tool reads with the channel, and that it reads none
// without.
TEST(ChannelEqualiseTruth, PAL_Calibration) {
  const auto cases = open_cases();
  ASSERT_TRUE(cases.channel && cases.none);
  const auto with = orc::calibrate_channel(
      *cases.channel, orc::ChannelCalibrationOptions{}, nullptr);
  const auto without = orc::calibrate_channel(
      *cases.none, orc::ChannelCalibrationOptions{}, nullptr);
  ASSERT_EQ(with.windows.size(), 1u);
  EXPECT_EQ(with.windows[0].frames_used, cases.channel->frame_count());
  EXPECT_EQ(with.verdict, orc::SmearVerdict::Smear);
  EXPECT_NEAR(with.fraction, kChannelFraction, kFractionTolerance);
  EXPECT_EQ(without.verdict, orc::SmearVerdict::NoSmear);

  using orc_videosynth::new_tolerance;
  orc_videosynth::expect_metrics(
      {{"fraction_error", std::abs(with.fraction - kChannelFraction),
        Better::Lower, new_tolerance(0.0, 0.0, 0.002)},
       {"tau_error_samples", std::abs(with.tau_samples - kChannelTau),
        Better::Lower, new_tolerance(0.0, 0.0, 0.05)}});
}

// What the stage at the measured smear leaves of the channel in each
// section, against what the capture carries, and the chroma level it sets.
TEST(ChannelEqualiseTruth, PAL) {
  const auto cases = open_cases();
  ASSERT_TRUE(cases.channel && cases.none);
  ASSERT_EQ(cases.channel->frame_count(),
            std::size(kSections) * kSectionFrames);
  ASSERT_EQ(cases.none->frame_count(), cases.channel->frame_count());
  const auto params = cases.none->get_video_parameters();
  ASSERT_TRUE(params.has_value());
  const double ire = orc_videosynth::ire_per_code(*params);

  const auto calibration = orc::calibrate_channel(
      *cases.channel, orc::ChannelCalibrationOptions{}, nullptr);
  ASSERT_EQ(calibration.verdict, orc::SmearVerdict::Smear);
  const auto equalised = equalise(cases.channel_artifact, calibration.fraction,
                                  calibration.tau_samples);
  ASSERT_TRUE(equalised.output);

  using orc_videosynth::new_tolerance;
  std::vector<Metric> metrics;
  double worst_burst = 0.0;
  for (size_t s = 0; s < std::size(kSections); ++s) {
    const std::string name = kSections[s];
    orc_videosynth::BandErrorMeter before(orc::VideoSystem::PAL, ire);
    orc_videosynth::BandErrorMeter after(orc::VideoSystem::PAL, ire);
    for (orc::FrameID k = 0; k < kSectionFrames; ++k) {
      const orc::FrameID f = (s * kSectionFrames) + k;
      // Owned copies: a representation's frame pointer lasts only until its
      // next call.
      const auto capture = cases.channel->get_frame_copy(f);
      const auto reference = cases.none->get_frame_copy(f);
      const auto out = equalised.output->get_frame_copy(f);
      ASSERT_EQ(capture.size(), reference.size());
      ASSERT_EQ(out.size(), reference.size());
      before.add_frame(capture.data(), reference.data(), *params);
      after.add_frame(out.data(), reference.data(), *params);
      const std::vector<double> samples(out.begin(), out.end());
      const auto burst = orc::channel_equalise::burst_amplitude(
          samples, params->frame_width_nominal, params->blanking_level,
          params->white_level);
      ASSERT_TRUE(burst.has_value());
      worst_burst = std::max(
          worst_burst,
          std::abs((*burst / orc::channel_equalise::kNominalBurstAmplitude) -
                   1.0));
    }
    const auto before_rms = before.rms_ire();
    const auto after_rms = after.rms_ire();
    for (size_t b = 0; b < before.bands().size(); ++b) {
      const std::string band = name + "_" + before.bands()[b].name;
      metrics.push_back({band + "_channel_rms_ire", before_rms[b],
                         Better::Lower,
                         new_tolerance(before_rms[b], 0.02, 0.001)});
      metrics.push_back({band + "_left_rms_ire", after_rms[b], Better::Lower,
                         new_tolerance(after_rms[b], 0.05, 0.002)});
    }
    // The stage brings the picture nearer the pressing without the channel.
    EXPECT_LT(after.total_rms_ire(), before.total_rms_ire()) << name;
  }
  EXPECT_LE(worst_burst, kBurstTolerance);
  metrics.push_back({"burst_error_share", worst_burst, Better::Lower,
                     new_tolerance(worst_burst, 0.1, 0.0005)});
  orc_videosynth::expect_metrics(metrics);
}

// With fraction 0 every frame passes bit-identical, on either case.
TEST(ChannelEqualiseTruth, PAL_PassThrough) {
  const auto cases = open_cases();
  ASSERT_TRUE(cases.channel && cases.none);
  for (const auto& artifact : {cases.channel_artifact, cases.none_artifact}) {
    const auto source = orc_videosynth::as_vfr(artifact);
    const auto passed = equalise(artifact, 0.0, 6.1);
    ASSERT_TRUE(source && passed.output);
    for (orc::FrameID f = 0; f < source->frame_count(); ++f) {
      EXPECT_EQ(passed.output->get_frame_copy(f), source->get_frame_copy(f))
          << "frame " << f;
    }
  }
}

}  // namespace orc_videosynth_test
