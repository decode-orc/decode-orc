/*
 * File:        channel_equalise_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for ChannelEqualiseStage and its support code: the
 *              smear undone, the burst brought to the nominal level, frames
 *              passed bit-identical at fraction 0, and its parameters
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/channel_equalise/channel_equalise_stage.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/support/channel_equalise.h>

#include <cmath>
#include <memory>
#include <vector>

#include "../../include/pal_colour_bars_source.h"

namespace orc_unit_test {
namespace {

namespace ce = orc::channel_equalise;

std::shared_ptr<const orc::VideoFrameRepresentation> run(
    orc::ChannelEqualiseStage& stage,
    const std::shared_ptr<PalColourBarsSource>& source, double fraction) {
  orc::ObservationContext ctx;
  const auto outputs = stage.execute(
      {source}, {{"smear_fraction", fraction}, {"smear_tau_samples", 6.1}},
      ctx);
  EXPECT_EQ(outputs.size(), 1u);
  return std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
      outputs.at(0));
}

// videosynth's edge_smear: (1 - c) x + c onepole(x), primed to x[0].
std::vector<double> smear(const std::vector<double>& x, double fraction,
                          double tau) {
  const double pole = std::exp(-1.0 / tau);
  std::vector<double> y(x.size());
  double acc = x.front();
  for (size_t i = 0; i < x.size(); ++i) {
    acc = (pole * acc) + ((1.0 - pole) * x[i]);
    y[i] = ((1.0 - fraction) * x[i]) + (fraction * acc);
  }
  return y;
}

// The bars source's burst fills the measured window exactly and starts and
// stops abruptly, so the shelf and the inverse spread its edges into the
// window: the burst reads a little low afterwards. videosynth's shaped burst
// reads within 0.1 % (ChannelEqualiseTruth).
constexpr double kBurstTolerance = 0.005;

double frame_burst(const std::vector<int16_t>& frame) {
  const std::vector<double> samples(frame.begin(), frame.end());
  return ce::burst_amplitude(samples, PalColourBarsSource::kSpl,
                             PalColourBarsSource::kBlack,
                             PalColourBarsSource::kWhite)
      .value_or(0.0);
}

TEST(ChannelEqualiseTest, InverseUndoesTheSmear) {
  std::vector<double> x(4000);
  for (size_t i = 0; i < x.size(); ++i) {
    x[i] = 256.0 + (((i / 97) % 2) * 500.0) +
           (40.0 * std::sin(static_cast<double>(i) * M_PI / 2.0));
  }
  auto y = smear(x, 0.24, 6.1);
  ce::unsmear(y, ce::EdgeSmear{0.24, 6.1});
  for (size_t i = 0; i < x.size(); ++i) ASSERT_NEAR(y[i], x[i], 1e-9) << i;
}

TEST(ChannelEqualiseTest, InactiveSmearLeavesTheStream) {
  std::vector<double> x = {1.0, 5.0, -3.0, 2.0};
  const auto copy = x;
  ce::unsmear(x, ce::EdgeSmear{0.0, 6.1});
  EXPECT_EQ(x, copy);
}

// The shelf is 0 at DC and 1 at the subcarrier: flat lines keep their level
// and the burst comes to the nominal amplitude.
TEST(ChannelEqualiseTest, ChromaGainScalesTheSubcarrierOnly) {
  const PalColourBarsSource source(PalColourBarsSource::Options{});
  const auto frame = source.get_frame_copy(0);
  const auto result =
      ce::equalise_frame(frame.data(), frame.size(), PalColourBarsSource::kSpl,
                         PalColourBarsSource::kBlack,
                         PalColourBarsSource::kWhite, ce::EdgeSmear{1e-9, 6.1});
  // The source's burst is 21 IRE in U and V: 0.297 of white.
  EXPECT_NEAR(result.gain, ce::kNominalBurstAmplitude / 0.297, 0.01);
  EXPECT_NEAR(frame_burst(result.frame), ce::kNominalBurstAmplitude,
              kBurstTolerance);
  // Line 5 is blanking after its burst: unchanged.
  const size_t base = orc::frame_line_sample_offset(
      orc::VideoSystem::PAL, PalColourBarsSource::kSpl, 4);
  for (size_t n = 150; n < PalColourBarsSource::kSpl; ++n) {
    EXPECT_EQ(result.frame[base + n], frame[base + n]);
  }
}

TEST(ChannelEqualiseStageTest, PassesThroughAtZero) {
  auto source = std::make_shared<PalColourBarsSource>(
      PalColourBarsSource::Options{2, true, std::nullopt, {}, 3, 1});
  orc::ChannelEqualiseStage stage;
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Yellow);
  const auto out = run(stage, source, 0.0);
  ASSERT_NE(out, nullptr);
  for (orc::FrameID f = 0; f < 2; ++f) {
    EXPECT_EQ(out->get_frame_copy(f), source->get_frame_copy(f));
  }
}

TEST(ChannelEqualiseStageTest, EqualisesAndSetsTheBurst) {
  auto source =
      std::make_shared<PalColourBarsSource>(PalColourBarsSource::Options{});
  orc::ChannelEqualiseStage stage;
  const auto out = run(stage, source, 0.24);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Green);
  const auto input = source->get_frame_copy(0);
  const auto output = out->get_frame_copy(0);
  ASSERT_EQ(output.size(), input.size());
  EXPECT_NE(output, input);
  EXPECT_NEAR(frame_burst(output), ce::kNominalBurstAmplitude,
              2.0 * kBurstTolerance);
  // get_line reads the same equalised frame.
  const int16_t* line = out->get_line(0, 100);
  ASSERT_NE(line, nullptr);
  const size_t base =
      orc::frame_line_sample_offset(orc::VideoSystem::PAL, 1135, 100);
  for (size_t n = 0; n < 1135; ++n) EXPECT_EQ(line[n], output[base + n]);
}

TEST(ChannelEqualiseStageTest, RejectsParametersOutOfRange) {
  orc::ChannelEqualiseStage stage;
  EXPECT_FALSE(stage.set_parameters({{"smear_fraction", 0.5}}));
  EXPECT_FALSE(stage.set_parameters({{"smear_fraction", -0.1}}));
  EXPECT_FALSE(stage.set_parameters({{"smear_tau_samples", 1.0}}));
  EXPECT_FALSE(stage.set_parameters({{"unknown", 1.0}}));
  EXPECT_TRUE(stage.set_parameters(
      {{"smear_fraction", 0.243}, {"smear_tau_samples", 6.13}}));
  const auto params = stage.get_parameters();
  EXPECT_DOUBLE_EQ(std::get<double>(params.at("smear_fraction")), 0.243);
  EXPECT_DOUBLE_EQ(std::get<double>(params.at("smear_tau_samples")), 6.13);
}

}  // namespace
}  // namespace orc_unit_test
