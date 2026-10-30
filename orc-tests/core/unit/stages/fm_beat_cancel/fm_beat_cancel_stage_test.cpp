/*
 * File:        fm_beat_cancel_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for FmBeatCancelStage: the beat removed, frames
 *              without flat colour passed bit-identical, and its parameter
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/fm_beat_cancel/fm_beat_cancel_stage.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../../include/pal_colour_bars_source.h"

namespace orc_unit_test {
namespace {

constexpr double kCarrierMhz = 7.092;

std::shared_ptr<const orc::VideoFrameRepresentation> run(
    orc::FmBeatCancelStage& stage,
    const std::shared_ptr<PalColourBarsSource>& source, double carrier_mhz) {
  orc::ObservationContext ctx;
  const auto outputs =
      stage.execute({source}, {{"carrier_black_mhz", carrier_mhz}}, ctx);
  EXPECT_EQ(outputs.size(), 1u);
  return std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
      outputs.at(0));
}

double rms(const std::vector<int16_t>& a, const std::vector<int16_t>& b) {
  double sum = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    const double d = a[i] - b[i];
    sum += d * d;
  }
  return std::sqrt(sum / static_cast<double>(a.size()));
}

TEST(FmBeatCancelStageTest, RemovesTheBeat) {
  PalColourBarsSource::Options options;
  options.beat_carrier_hz = kCarrierMhz * 1e6;
  auto source = std::make_shared<PalColourBarsSource>(options);
  orc::FmBeatCancelStage stage;
  const auto out = run(stage, source, kCarrierMhz);
  ASSERT_NE(out, nullptr);
  const auto clean = source->clean(0);
  const auto input = source->get_frame_copy(0);
  const auto output = out->get_frame_copy(0);
  ASSERT_EQ(output.size(), input.size());
  EXPECT_LT(rms(output, clean), 0.1 * rms(input, clean));
  // get_line reads the same corrected frame.
  const int16_t* line = out->get_line(0, 100);
  ASSERT_NE(line, nullptr);
  const size_t base =
      orc::frame_line_sample_offset(orc::VideoSystem::PAL, 1135, 100);
  for (size_t n = 0; n < 1135; ++n) EXPECT_EQ(line[n], output[base + n]);
}

// The beat's phase along a line follows the carrier: 10 kHz off, most of it
// stays.
TEST(FmBeatCancelStageTest, NeedsTheMastersCarrier) {
  PalColourBarsSource::Options options;
  options.beat_carrier_hz = kCarrierMhz * 1e6;
  auto source = std::make_shared<PalColourBarsSource>(options);
  orc::FmBeatCancelStage stage;
  const auto out = run(stage, source, kCarrierMhz + 0.010);
  const auto clean = source->clean(0);
  EXPECT_GT(rms(out->get_frame_copy(0), clean),
            0.5 * rms(source->get_frame_copy(0), clean));
}

TEST(FmBeatCancelStageTest, PassesAFrameWithoutFlatColourBitIdentical) {
  PalColourBarsSource::Options options;
  options.colour = false;
  options.noise_codes = 3;
  auto source = std::make_shared<PalColourBarsSource>(options);
  orc::FmBeatCancelStage stage;
  const auto out = run(stage, source, kCarrierMhz);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->get_frame(0), source->get_frame(0));
  EXPECT_EQ(out->get_frame_copy(0), source->get_frame_copy(0));
  EXPECT_EQ(out->get_line(0, 200), source->get_line(0, 200));
}

TEST(FmBeatCancelStageTest, IsPalOnly) {
  orc::FmBeatCancelStage stage;
  EXPECT_EQ(stage.get_node_type_info().compatible_formats,
            orc::VideoFormatCompatibility::PAL_ONLY);
}

TEST(FmBeatCancelStageTest, CarrierParameter) {
  orc::FmBeatCancelStage stage;
  const auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::PAL, orc::SourceType::Composite);
  ASSERT_EQ(descriptors.size(), 1u);
  EXPECT_EQ(descriptors[0].name, "carrier_black_mhz");
  EXPECT_DOUBLE_EQ(std::get<double>(*descriptors[0].constraints.default_value),
                   7.1);
  EXPECT_DOUBLE_EQ(
      std::get<double>(stage.get_parameters().at("carrier_black_mhz")), 7.1);
  EXPECT_TRUE(stage.set_parameters({{"carrier_black_mhz", 7.0885}}));
  EXPECT_DOUBLE_EQ(
      std::get<double>(stage.get_parameters().at("carrier_black_mhz")), 7.0885);
  EXPECT_FALSE(stage.set_parameters({{"carrier_black_mhz", 7.5}}));
  EXPECT_FALSE(stage.set_parameters({{"carrier_black_mhz", 6.5}}));
  EXPECT_FALSE(
      stage.set_parameters({{"carrier_black_mhz", std::string("7.1")}}));
  EXPECT_FALSE(stage.set_parameters({{"carrier", 7.1}}));
}

}  // namespace
}  // namespace orc_unit_test
