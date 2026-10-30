/*
 * File:        picture_band_limit_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for PictureBandLimitStage: the low-pass's response,
 *              the luma-only and whole-composite paths, and its parameters
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/picture_band_limit/picture_band_limit_stage.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/support/frame_line_util.h>

#include <cmath>
#include <complex>
#include <memory>
#include <vector>

namespace orc_unit_test {
namespace {

// The low-pass's gain at |frequency_hz|.
double gain_at(const std::vector<double>& taps, double frequency_hz,
               double sample_rate_hz) {
  double gain = taps[0];
  for (size_t k = 1; k < taps.size(); ++k) {
    gain += 2.0 * taps[k] *
            std::cos(2.0 * M_PI * frequency_hz * static_cast<double>(k) /
                     sample_rate_hz);
  }
  return gain;
}

TEST(PictureBandLimitTest, LowPassIsFlatBelowAndStopsAboveTheRolloff) {
  constexpr double kCutoff = 5.4e6;
  constexpr double kRolloff = 0.4e6;
  const auto taps =
      orc::picture_band_taps(kCutoff, kRolloff, orc::kPalSampleRate);
  for (double f = 0.0; f <= kCutoff - (kRolloff / 2.0); f += 50e3) {
    EXPECT_NEAR(gain_at(taps, f, orc::kPalSampleRate), 1.0, 0.005)
        << f / 1e6 << " MHz";
  }
  EXPECT_NEAR(gain_at(taps, kCutoff, orc::kPalSampleRate), 0.5, 0.01);
  for (double f = kCutoff + (kRolloff / 2.0); f < orc::kPalSampleRate / 2.0;
       f += 50e3) {
    EXPECT_LT(std::abs(gain_at(taps, f, orc::kPalSampleRate)), 0.005)
        << f / 1e6 << " MHz";
  }
}

// PAL's chroma reaches past any cutoff the picture needs, so only its luma
// is cut; NTSC's ends at 4.88 MHz, below a roll-off from 5.2 MHz up.
TEST(PictureBandLimitTest, CutsTheWholeCompositeOnlyAboveTheChromaBand) {
  EXPECT_FALSE(
      orc::picture_band_limits_composite(orc::VideoSystem::PAL, 5.4e6, 0.4e6));
  EXPECT_TRUE(
      orc::picture_band_limits_composite(orc::VideoSystem::NTSC, 5.4e6, 0.4e6));
  EXPECT_FALSE(
      orc::picture_band_limits_composite(orc::VideoSystem::NTSC, 5.0e6, 0.4e6));
}

TEST(PictureBandLimitTest, DefaultsFollowTheProjectFormat) {
  orc::PictureBandLimitStage stage;
  auto cutoff_default = [&](orc::VideoSystem system) {
    for (const auto& d :
         stage.get_parameter_descriptors(system, orc::SourceType::Composite)) {
      if (d.name == "cutoff_mhz") {
        return std::get<double>(*d.constraints.default_value);
      }
    }
    return 0.0;
  };
  EXPECT_DOUBLE_EQ(cutoff_default(orc::VideoSystem::PAL), 5.4);
  EXPECT_DOUBLE_EQ(cutoff_default(orc::VideoSystem::NTSC), 5.0);
  EXPECT_DOUBLE_EQ(cutoff_default(orc::VideoSystem::PAL_M), 5.0);
}

TEST(PictureBandLimitTest, SetParametersChecksRanges) {
  orc::PictureBandLimitStage stage;
  EXPECT_TRUE(
      stage.set_parameters({{"cutoff_mhz", 4.5}, {"rolloff_mhz", 0.6}}));
  const auto params = stage.get_parameters();
  EXPECT_DOUBLE_EQ(std::get<double>(params.at("cutoff_mhz")), 4.5);
  EXPECT_DOUBLE_EQ(std::get<double>(params.at("rolloff_mhz")), 0.6);
  EXPECT_FALSE(stage.set_parameters({{"cutoff_mhz", 9.0}}));
  EXPECT_FALSE(stage.set_parameters({{"rolloff_mhz", 0.0}}));
  EXPECT_FALSE(stage.set_parameters({{"cutoff_mhz", std::string("5")}}));
  EXPECT_FALSE(stage.set_parameters({{"bandwidth", 5.0}}));
}

// A PAL frame: on every active line, a luma step and colour, plus a luma tone
// above the picture; a VBI line carrying the same tone. Lines advance the
// subcarrier as PAL does, with V-switch.
class FakeToneSource : public orc::VideoFrameRepresentation,
                       public orc::Artifact {
 public:
  static constexpr double kToneHz = 6.5e6;
  static constexpr double kToneAmplitude = 40.0;
  static constexpr size_t kVbiLine = 10;

  FakeToneSource() : orc::Artifact(orc::ArtifactID("fake"), orc::Provenance{}) {
    buffer_.resize(orc::frame_line_sample_offset(system_, kSpl, kHeight));
    with_tone_ = buffer_;
    for (size_t line = 0; line < kHeight; ++line) {
      const size_t base = orc::frame_line_sample_offset(system_, kSpl, line);
      const size_t length = orc::frame_line_sample_count(system_, kSpl, line);
      const double phase = 2.0 * M_PI * 0.7516 * static_cast<double>(line);
      const double v_switch = (line % 2) != 0 ? -1.0 : 1.0;
      for (size_t n = 0; n < length; ++n) {
        const double theta = phase + (static_cast<double>(n) * M_PI / 2.0);
        double sample = 256.0;
        if (n >= 98 && n < 138) {
          sample +=
              (-60.0 * std::sin(theta)) + (v_switch * 60.0 * std::cos(theta));
        } else if (n >= 140 && n < 1120) {
          sample = (n < 600 ? 350.0 : 600.0) + (50.0 * std::sin(theta)) +
                   (v_switch * -40.0 * std::cos(theta));
        }
        const double tone = kToneAmplitude * std::sin(2.0 * M_PI * kToneHz *
                                                      static_cast<double>(n) /
                                                      orc::kPalSampleRate);
        buffer_[base + n] = static_cast<int16_t>(std::lround(sample));
        with_tone_[base + n] = static_cast<int16_t>(std::lround(sample + tone));
      }
    }
  }
  std::string type_name() const override { return "fake_tone"; }
  orc::FrameIDRange frame_range() const override {
    return orc::FrameIDRange{0u, 1u};
  }
  size_t frame_count() const override { return 1; }
  bool has_frame(orc::FrameID id) const override { return id == 0; }
  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (id != 0) return std::nullopt;
    orc::FrameDescriptor d;
    d.frame_id = 0;
    d.system = system_;
    d.height = kHeight;
    d.samples_total = with_tone_.size();
    d.samples_per_line_nominal = kSpl;
    return d;
  }
  const sample_type* get_frame(orc::FrameID id) const override {
    return id == 0 ? with_tone_.data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return id == 0 ? with_tone_ : std::vector<sample_type>{};
  }
  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters p;
    p.system = system_;
    p.frame_width_nominal = static_cast<int32_t>(kSpl);
    p.frame_height = static_cast<int32_t>(kHeight);
    p.blanking_level = 256;
    p.black_level = 256;
    p.white_level = 844;
    p.active_video_start = orc::kPalActiveVideoStart;
    p.active_video_end = orc::kPalActiveVideoEnd;
    p.first_active_frame_line = orc::kPalFirstActiveFrameLine;
    p.last_active_frame_line = orc::kPalLastActiveFrameLine;
    return p;
  }
  // The frame without the tone.
  int16_t picture(size_t line, size_t n) const {
    return buffer_[orc::frame_line_sample_offset(system_, kSpl, line) + n];
  }
  int16_t input(size_t line, size_t n) const {
    return with_tone_[orc::frame_line_sample_offset(system_, kSpl, line) + n];
  }

 private:
  static constexpr size_t kSpl = 1135;
  static constexpr size_t kHeight = 625;
  orc::VideoSystem system_ = orc::VideoSystem::PAL;
  std::vector<int16_t> buffer_;
  std::vector<int16_t> with_tone_;
};

// The tone above the picture goes; the luma and the colour stay, away from
// the luma step, where the low-pass rings as any cut of a sharp edge does.
TEST(PictureBandLimitTest, RemovesTheToneAndKeepsThePicture) {
  orc::PictureBandLimitStage stage;
  orc::ObservationContext ctx;
  auto source = std::make_shared<FakeToneSource>();
  const auto outputs = stage.execute({source}, {}, ctx);
  ASSERT_EQ(outputs.size(), 1u);
  auto out =
      std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(outputs[0]);
  ASSERT_NE(out, nullptr);
  for (size_t line : {100u, 101u, 400u}) {
    const int16_t* row = out->get_line(0, line);
    ASSERT_NE(row, nullptr);
    for (size_t n = 220; n < 1040; ++n) {
      if (n > 540 && n < 660) continue;  // the luma step
      EXPECT_NEAR(row[n], source->picture(line, n), 3)
          << "line " << line << " sample " << n;
    }
  }
  // Outside the active picture the input passes as it is.
  const int16_t* vbi = out->get_line(0, FakeToneSource::kVbiLine);
  for (size_t n = 0; n < 1135; ++n) {
    EXPECT_EQ(vbi[n], source->input(FakeToneSource::kVbiLine, n));
  }
  const auto frame = out->get_frame_copy(0);
  EXPECT_EQ(frame.size(), static_cast<size_t>(orc::kPalFrameSamples));
}

}  // namespace
}  // namespace orc_unit_test
