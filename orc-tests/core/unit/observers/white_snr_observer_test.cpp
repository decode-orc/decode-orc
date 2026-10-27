/*
 * File:        white_snr_observer_test.cpp
 * Module:      orc-tests/core/unit/observers
 * Purpose:     Unit tests for WhiteSNRObserver
 *
 * Tests: the white flag window sits clear of the bar's rising edge, so the
 * measurement is the flag's noise and does not move with the capture's
 * sample grid (PAL line 19, NTSC line 13); a flag with no white is not
 * measured.  Frame data is synthesised in memory; no I/O is performed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>
#include <white_snr_observer.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <variant>
#include <vector>

namespace orc {
namespace tests {

namespace {

// Minimal in-memory VFR backed by a flat sample buffer.
class FlatBufferVFR : public VideoFrameRepresentation {
 public:
  FlatBufferVFR(VideoSystem system, std::vector<int16_t> samples,
                SourceParameters params)
      : system_(system),
        samples_(std::move(samples)),
        params_(std::move(params)) {}

  FrameIDRange frame_range() const override { return {0, 1}; }
  size_t frame_count() const override { return 1; }
  bool has_frame(FrameID id) const override { return id == FrameID{0}; }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (id != FrameID{0}) return std::nullopt;
    FrameDescriptor d;
    d.frame_id = id;
    d.system = system_;
    d.height = static_cast<size_t>(params_.frame_height);
    d.samples_total = samples_.size();
    d.samples_per_line_nominal =
        static_cast<size_t>(params_.frame_width_nominal);
    return d;
  }

  const sample_type* get_frame(FrameID id) const override {
    return (id == FrameID{0}) ? samples_.data() : nullptr;
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    return (id == FrameID{0}) ? samples_ : std::vector<sample_type>{};
  }

  std::optional<SourceParameters> get_video_parameters() const override {
    return params_;
  }

 private:
  VideoSystem system_;
  std::vector<int16_t> samples_;
  SourceParameters params_;
};

SourceParameters make_params(VideoSystem system) {
  SourceParameters p{};
  p.system = system;
  if (system == VideoSystem::PAL) {
    p.frame_width_nominal = kPalSamplesPerLineNominal;
    p.frame_height = kPalFrameLines;
    p.blanking_level = kPalBlanking;
    p.white_level = kPalWhite;
  } else {
    p.frame_width_nominal = kNtscSamplesPerLine;
    p.frame_height = kNtscFrameLines;
    p.blanking_level = kNtscBlanking;
    p.white_level = kNtscWhite;
  }
  return p;
}

// A white bar as a LaserDisc decode delivers it: blanking, then a step to
// 86 % of white at |edge_us| (µs from 0H) that settles linearly to white
// over |settle_us|, white until |end_us|, blanking after.  Measured on
// Community South line 19: 50 % at 11.9 µs, 86 % at 12.0 µs, settled at
// 13.3 µs.
struct Bar {
  double edge_us;
  double settle_us;
  double end_us;
};

// A frame of blanking with |bar| on field 1 line |field_line|, every sample
// carrying Gaussian noise of |sigma| codes drawn from a fixed seed, so two
// frames differing only in |bar| carry the same noise sample for sample.
std::vector<int16_t> frame_with_bar(const SourceParameters& p,
                                    size_t field_line, const Bar& bar,
                                    double sigma) {
  const auto spl = static_cast<size_t>(p.frame_width_nominal);
  const size_t total = frame_line_sample_offset(
      p.system, spl, static_cast<size_t>(p.frame_height));
  std::vector<int16_t> frame(total, static_cast<int16_t>(p.blanking_level));
  const double us_per_line = p.system == VideoSystem::PAL ? 64.0 : 63.5;
  const double samples_per_us = static_cast<double>(spl) / us_per_line;
  const double swing = p.white_level - p.blanking_level;

  std::mt19937 rng(1234);
  std::normal_distribution<double> noise(0.0, sigma);
  const size_t base = frame_line_sample_offset(p.system, spl, field_line - 1);
  for (size_t n = 0; n < spl; ++n) {
    const double t = static_cast<double>(n) / samples_per_us;
    double level = 0.0;
    if (t >= bar.edge_us && t < bar.end_us) {
      const double settled = std::min((t - bar.edge_us) / bar.settle_us, 1.0);
      level = 0.86 + 0.14 * settled;
    }
    frame[base + n] = static_cast<int16_t>(
        std::lround(p.blanking_level + level * swing + noise(rng)));
  }
  return frame;
}

std::optional<double> measure(const SourceParameters& p,
                              std::vector<int16_t> frame) {
  FlatBufferVFR vfr(p.system, std::move(frame), p);
  ObservationContext context;
  WhiteSNRObserver observer;
  observer.process_frame(vfr, FrameID{0}, context);
  const auto value = context.get(FieldID(0), "white_snr", "snr_db");
  if (!value || !std::holds_alternative<double>(*value)) return std::nullopt;
  return std::get<double>(*value);
}

// The SNR of white carrying |sigma| codes of noise, as the observer
// computes it: mean over standard deviation, in IRE.
double noise_only_snr(const SourceParameters& p, double sigma) {
  return 20.0 * std::log10((p.white_level - p.blanking_level) / sigma);
}

}  // namespace

// The PAL window sits on the flag's flat top: what it measures is the noise
// there, not the settling after the edge.
TEST(WhiteSNRObserverTest, PalLine19_MeasuresTheFlagNotItsEdge) {
  const SourceParameters p = make_params(VideoSystem::PAL);
  const double sigma = 10.0;
  const auto snr = measure(p, frame_with_bar(p, 19, {11.95, 1.3, 21.9}, sigma));
  ASSERT_TRUE(snr.has_value());
  EXPECT_NEAR(*snr, noise_only_snr(p, sigma), 0.5);
}

// The same flag a sample later, as one capture's grid lies against
// another's, measures the same: the edge never reaches the window.
TEST(WhiteSNRObserverTest, PalLine19_DoesNotMoveWithTheSampleGrid) {
  const SourceParameters p = make_params(VideoSystem::PAL);
  const double sample_us = 64.0 / kPalSamplesPerLineNominal;
  // Up to 0.6 µs either side of the Community South timing, as decodes put it.
  for (double shift : {-0.6, 0.0, 0.6}) {
    const double edge = 11.95 + shift;
    const auto here = measure(p, frame_with_bar(p, 19, {edge, 1.3, 21.9}, 4.0));
    const auto later = measure(
        p,
        frame_with_bar(p, 19, {edge + sample_us, 1.3, 21.9 + sample_us}, 4.0));
    ASSERT_TRUE(here.has_value() && later.has_value()) << shift;
    EXPECT_DOUBLE_EQ(*here, *later) << shift;
  }
}

// NTSC discs that carry the NTC-7 composite bar on line 13 rather than
// line 20 are measured there, clear of the edge as on PAL.  The bar here
// rises 0.35 µs later than on the NTSC capture it was measured on (50 % at
// 12.15 µs) and settles as slowly as Community South's.
TEST(WhiteSNRObserverTest, NtscLine13_DoesNotMoveWithTheSampleGrid) {
  const SourceParameters p = make_params(VideoSystem::NTSC);
  const double sample_us = 63.5 / kNtscSamplesPerLine;
  const double sigma = 4.0;
  const auto here =
      measure(p, frame_with_bar(p, 13, {12.5, 1.3, 29.96}, sigma));
  const auto later = measure(
      p,
      frame_with_bar(p, 13, {12.5 + sample_us, 1.3, 29.96 + sample_us}, sigma));
  ASSERT_TRUE(here.has_value() && later.has_value());
  EXPECT_DOUBLE_EQ(*here, *later);
  EXPECT_NEAR(*here, noise_only_snr(p, sigma), 0.5);
}

// A line with no white flag gives no measurement rather than a figure for
// whatever is there.
TEST(WhiteSNRObserverTest, NoFlag_IsNotMeasured) {
  const SourceParameters p = make_params(VideoSystem::PAL);
  EXPECT_FALSE(
      measure(p, frame_with_bar(p, 19, {0.0, 1.0, 0.0}, 4.0)).has_value());
}

}  // namespace tests
}  // namespace orc
