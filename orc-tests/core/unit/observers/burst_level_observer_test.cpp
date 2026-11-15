/*
 * File:        burst_level_observer_test.cpp
 * Module:      orc-tests/core/unit/observers
 * Purpose:     Unit tests for BurstLevelObserver
 *
 * Tests: on the PAL 4fsc lattice the burst sits at a fixed time after 0H,
 * which is up to 2 samples later on the last rows of a field than on the
 * first; the observer reads the same level from the first and last rows it
 * samples.  Frame data is synthesised in memory; no I/O is performed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/observers/burst_level_observer.h>
#include <orc/support/pal_line_locked_resample.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace orc {
namespace tests {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kSpl = static_cast<size_t>(kPalSamplesPerLineNominal);
constexpr size_t kBurstSamples = 40;
constexpr double kBurstCodes = 60.0;

class FlatBufferVFR : public VideoFrameRepresentation {
 public:
  FlatBufferVFR(std::vector<int16_t> samples, SourceParameters params)
      : samples_(std::move(samples)), params_(std::move(params)) {}

  FrameIDRange frame_range() const override { return {0, 1}; }
  size_t frame_count() const override { return 1; }
  bool has_frame(FrameID id) const override { return id == FrameID{0}; }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (id != FrameID{0}) return std::nullopt;
    FrameDescriptor d;
    d.frame_id = id;
    d.system = params_.system;
    d.height = static_cast<size_t>(params_.frame_height);
    d.samples_total = samples_.size();
    d.samples_per_line_nominal = kSpl;
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
  std::vector<int16_t> samples_;
  SourceParameters params_;
};

SourceParameters pal_params() {
  SourceParameters p{};
  p.system = VideoSystem::PAL;
  p.frame_width_nominal = kPalSamplesPerLineNominal;
  p.frame_height = kPalFrameLines;
  p.blanking_level = kPalBlanking;
  p.white_level = kPalWhite;
  p.first_active_frame_line = kPalFirstActiveFrameLine;
  p.last_active_frame_line = kPalLastActiveFrameLine;
  return p;
}

// Burst sample k (0-based) of a burst; the same on every row it is placed on.
int16_t burst_sample(size_t k) {
  return static_cast<int16_t>(std::lround(
      kPalBlanking +
      kBurstCodes * std::sin((static_cast<double>(k) * kPi / 2.0) + 0.3)));
}

// A frame of blanking with a 40-sample burst starting at |first(field, row)|
// on field rows |rows| of both fields.
template <typename First>
std::vector<int16_t> frame_with_bursts(const std::vector<int32_t>& rows,
                                       First first) {
  const size_t f1 = field1_lines(VideoSystem::PAL);
  std::vector<int16_t> frame(
      frame_line_sample_offset(VideoSystem::PAL, kSpl, kPalFrameLines),
      static_cast<int16_t>(kPalBlanking));
  for (const int field : {1, 2}) {
    for (const int32_t row : rows) {
      const size_t flat = (field == 1 ? 0 : f1) + static_cast<size_t>(row);
      int16_t* line =
          frame.data() + frame_line_sample_offset(VideoSystem::PAL, kSpl, flat);
      const size_t start = first(field, row);
      for (size_t k = 0; k < kBurstSamples; ++k) {
        line[start + k] = burst_sample(k);
      }
    }
  }
  return frame;
}

std::optional<double> measure(std::vector<int16_t> frame) {
  const SourceParameters p = pal_params();
  FlatBufferVFR vfr(std::move(frame), p);
  ObservationContext context;
  BurstLevelObserver observer;
  observer.process_frame(vfr, FrameID{0}, context);
  const auto value =
      context.get(FieldID(0), "burst_level", "median_burst_10bit");
  if (!value || !std::holds_alternative<double>(*value)) return std::nullopt;
  return std::get<double>(*value);
}

// The level the observer reads from a row whose burst starts at the window's
// first sample: the window spans 41 samples, the burst's 40 and one of
// blanking.
double row_zero_level() {
  std::vector<double> window;
  for (size_t k = 0; k < kBurstSamples; ++k) window.push_back(burst_sample(k));
  window.push_back(kPalBlanking);
  double sum = 0.0;
  for (const double s : window) sum += s;
  const double mean = sum / static_cast<double>(window.size());
  double sum_sq = 0.0;
  for (const double s : window) sum_sq += (s - mean) * (s - mean);
  return std::sqrt(sum_sq / static_cast<double>(window.size())) *
         std::sqrt(2.0);
}

// The rows the observer samples are field rows 11, the middle and the last
// before the bottom ten; on a 625-line frame those are 11, 157, 302 (field 1)
// and 11, 156, 301 (field 2), whose 0H shifts are 0, 1 and 2 samples.
const std::vector<int32_t> kFirstAndLastRows = {11, 301, 302};

}  // namespace

// A burst at 0H + 5.6 µs on every row, placed as the lattice places it, reads
// the same level on the last rows of each field as on the first.  The middle
// rows carry no burst and read 0, which the median of six passes over.
TEST(BurstLevelObserverTest, Pal_ReadsTheLatticeBurstAlikeOnFirstAndLastRows) {
  const auto level =
      measure(frame_with_bursts(kFirstAndLastRows, [](int field, int32_t row) {
        return static_cast<size_t>(kPalColourBurstStart) +
               static_cast<size_t>(
                   orc::pal_line_locked::row_0h_shift(field, row));
      }));
  ASSERT_TRUE(level.has_value());
  EXPECT_NEAR(*level, row_zero_level(), 1e-9);
}

}  // namespace tests
}  // namespace orc
