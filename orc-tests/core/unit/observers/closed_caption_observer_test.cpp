/*
 * File:        closed_caption_observer_test.cpp
 * Module:      orc-tests/core/unit/observers
 * Purpose:     Unit tests for ClosedCaptionObserver's line 21 reading
 *
 * Tests: a caption line read; parity checked over all eight bits; nothing read
 * from a line with no clock run-in, from noise, or where the start bits are
 * missing; run-ins from 7.2 to 11 us after 0H; a last bit the capture cut
 * short; a weak run-in read only when its bits are clear; both fields of a
 * frame observed. Lines are synthesised in memory; no I/O is performed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include <closed_caption_observer.h>
#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace orc {
namespace tests {

namespace {

// A 4 fsc NTSC line in the CVBS_U10_4FSC domain: 910 samples, blanking at
// 240, the data level 50 IRE above it [CTA-608-E Table 2].
constexpr size_t kLine = 910;
constexpr double kBit = 910.0 / 32.0;
constexpr int16_t kBlanking = 240;
constexpr int16_t kHigh = 520;
// 10.5 us after 0H, where CTA-608-E Table 2 puts the run-in's start.
constexpr double kNominalStart = 150.0;
constexpr double kTwoPi = 6.283185307179586;

/// |value|'s seven bits with the odd parity bit CTA-608-E §4 asks for.
uint8_t with_parity(uint8_t value) {
  int ones = 0;
  for (int bit = 0; bit < 7; ++bit) {
    ones += (value >> bit) & 1;
  }
  return static_cast<uint8_t>((value & 0x7F) | ((ones % 2 == 0) ? 0x80 : 0));
}

/// A line 21 carrying |byte0| and |byte1| as transmitted (parity bits
/// included): seven cycles of run-in from |start| samples, two 0 start bits
/// and a 1, then sixteen bits, least significant first. The run-in's negative
/// midpoints fall on the bit boundaries [CTA-608-E Table 2 note 3].
std::vector<int16_t> caption_line(uint8_t byte0, uint8_t byte1,
                                  double start = kNominalStart,
                                  bool run_in = true) {
  std::vector<int16_t> line(kLine, kBlanking);
  const double mid = (kBlanking + kHigh) / 2.0;
  const double half = (kHigh - kBlanking) / 2.0;
  for (size_t i = 0; i < kLine && run_in; ++i) {
    const double t = static_cast<double>(i) - start;
    if (t >= 0 && t < 6.75 * kBit) {
      line[i] = static_cast<int16_t>(
          std::lround(mid - half * std::cos(kTwoPi * t / kBit)));
    }
  }
  std::vector<int> bits = {0, 0, 1};
  for (const uint8_t byte : {byte0, byte1}) {
    for (int bit = 0; bit < 8; ++bit) {
      bits.push_back((byte >> bit) & 1);
    }
  }
  for (size_t b = 0; b < bits.size(); ++b) {
    const double from = start + (6.75 + static_cast<double>(b)) * kBit;
    for (auto i = static_cast<size_t>(std::ceil(from));
         i < static_cast<size_t>(from + kBit) && i < kLine; ++i) {
      line[i] = bits[b] != 0 ? kHigh : kBlanking;
    }
  }
  return line;
}

/// A deterministic pseudo-random sequence for noise.
class Noise {
 public:
  explicit Noise(uint32_t seed) : state_(seed) {}
  // Uniform in [-amplitude, amplitude].
  double next(double amplitude) {
    state_ = state_ * 1664525U + 1013904223U;
    return amplitude * (static_cast<double>(state_ >> 8U) /
                            static_cast<double>(1U << 24U) * 2.0 -
                        1.0);
  }

 private:
  uint32_t state_;
};

std::optional<ClosedCaptionObserver::Line21Pair> read(
    const std::vector<int16_t>& line) {
  return ClosedCaptionObserver::read_line21(line.data(), line.size());
}

// ---------------------------------------------------------------------------
// One line
// ---------------------------------------------------------------------------

TEST(ClosedCaptionObserverLine21, ReadsTheBytePairOfACaptionLine) {
  const auto pair = read(caption_line(with_parity('H'), with_parity('i')));
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->data0, 'H');
  EXPECT_EQ(pair->data1, 'i');
  EXPECT_TRUE(pair->parity0_valid);
  EXPECT_TRUE(pair->parity1_valid);
}

// Recordings put the run-in anywhere from 9.6 us after 0H to 10.5 us, and
// some a little later; stretches of eight tapes at 7.75 to 8 us.
TEST(ClosedCaptionObserverLine21, ReadsRunInsFromSevenToElevenMicroseconds) {
  for (const double start : {103.0, 111.0, 113.0, 137.0, 143.5, 150.0, 157.5}) {
    const auto pair =
        read(caption_line(with_parity(0x14), with_parity(0x2C), start));
    ASSERT_TRUE(pair.has_value()) << "run-in at " << start;
    EXPECT_EQ(pair->data0, 0x14) << "run-in at " << start;
    EXPECT_EQ(pair->data1, 0x2C) << "run-in at " << start;
    EXPECT_TRUE(pair->parity0_valid && pair->parity1_valid);
  }
}

// The last data bit ends 61.6 us after 0H, where captures stop the line:
// vbi_source writes a crop up to sample 873 and pads the rest with blanking.
// Behind a late run-in the last bit is mostly padding; it is read from the
// early part of its cell.
TEST(ClosedCaptionObserverLine21, ReadsALastBitTheCaptureCutShort) {
  // A run-in 11 us after 0H, and a null second byte (80h): its parity bit,
  // the last bit, is a 1 that the padding covers most of.
  auto line = caption_line(with_parity(0x14), with_parity(0x00), 157.5);
  std::fill(line.begin() + 873, line.end(), kBlanking);
  const auto pair = read(line);
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->data0, 0x14);
  EXPECT_EQ(pair->data1, 0x00);
  EXPECT_TRUE(pair->parity0_valid);
  EXPECT_TRUE(pair->parity1_valid);
}

// Odd parity over all eight bits: a byte whose seven data bits are odd and
// whose parity bit is set holds an even count of ones. Observer 1.x passed
// such a byte, which is half of the noise it read as captions.
TEST(ClosedCaptionObserverLine21, ChecksParityOverAllEightBits) {
  // 'C' (43h) has three ones among its data bits, so its parity bit is clear;
  // set, the byte is even.
  const uint8_t odd_data_bad_parity = static_cast<uint8_t>('C' | 0x80);
  // 'A' (41h) has two: its parity bit must be set; clear, the byte is even.
  const uint8_t even_data_no_parity = 'A';
  const auto pair =
      read(caption_line(odd_data_bad_parity, even_data_no_parity));
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->data0, 'C');
  EXPECT_EQ(pair->data1, 'A');
  EXPECT_FALSE(pair->parity0_valid);
  EXPECT_FALSE(pair->parity1_valid);
}

// The start bits and data with nothing before them: no clock run-in, no line
// 21 data, however cleanly the rest slices.
TEST(ClosedCaptionObserverLine21, ReadsNothingWithoutAClockRunIn) {
  EXPECT_FALSE(read(caption_line(with_parity('H'), with_parity('i'),
                                 kNominalStart, /*run_in=*/false))
                   .has_value());
}

// A line of noise slices into a start bit and sixteen bits all the same, and
// an odd parity holds on half of any byte of it.
TEST(ClosedCaptionObserverLine21, ReadsNothingFromNoise) {
  Noise noise(12345);
  for (int trial = 0; trial < 200; ++trial) {
    std::vector<int16_t> line(kLine, kBlanking);
    for (size_t i = 140; i < 880; ++i) {
      line[i] = static_cast<int16_t>(
          std::lround(kBlanking + 140.0 + noise.next(140.0)));
    }
    EXPECT_FALSE(read(line).has_value()) << "trial " << trial;
  }
  const std::vector<int16_t> flat(kLine, kBlanking);
  EXPECT_FALSE(read(flat).has_value());
}

// A run-in that runs on where the start bits belong is not a caption line.
TEST(ClosedCaptionObserverLine21, ReadsNothingWithoutStartBits) {
  std::vector<int16_t> line(kLine, kBlanking);
  const double mid = (kBlanking + kHigh) / 2.0;
  const double half = (kHigh - kBlanking) / 2.0;
  for (size_t i = 150; i < kLine; ++i) {
    line[i] = static_cast<int16_t>(std::lround(
        mid -
        half * std::cos(kTwoPi * (static_cast<double>(i) - 150.0) / kBit)));
  }
  EXPECT_FALSE(read(line).has_value());
}

// A run-in a dropout has half buried in noise is still read when every data
// bit stands clear of the slicing level, and not when one does not: a weak
// run-in with an ambiguous bit is as likely noise that slices into a pair.
TEST(ClosedCaptionObserverLine21, ReadsAWeakRunInOnlyWhenItsBitsAreClear) {
  auto line = caption_line(with_parity('O'), with_parity('K'));
  Noise noise(777);
  // Noise over the run-in only, enough to bring its purity under a half.
  for (size_t i = 160; i < 330; ++i) {
    line[i] = static_cast<int16_t>(std::lround(line[i] + noise.next(250.0)));
  }
  const auto weak = read(line);
  ASSERT_TRUE(weak.has_value());
  EXPECT_EQ(weak->data0, 'O');
  EXPECT_EQ(weak->data1, 'K');

  // The first data bit pulled to the slicing level.
  const double cell = kNominalStart + 9.75 * kBit;
  for (auto i = static_cast<size_t>(cell); i < static_cast<size_t>(cell + kBit);
       ++i) {
    line[i] = (kBlanking + kHigh) / 2;
  }
  EXPECT_FALSE(read(line).has_value());
}

TEST(ClosedCaptionObserverLine21, ReadsNothingFromAShortLine) {
  const std::vector<int16_t> line(32, kHigh);
  EXPECT_FALSE(read(line).has_value());
  EXPECT_FALSE(ClosedCaptionObserver::read_line21(nullptr, kLine).has_value());
}

// ---------------------------------------------------------------------------
// Whole frames
// ---------------------------------------------------------------------------

SourceParameters make_ntsc_params() {
  SourceParameters p{};
  p.system = VideoSystem::NTSC;
  p.frame_width_nominal = static_cast<int32_t>(kLine);
  p.frame_height = kNtscFrameLines;
  p.blanking_level = kNtscBlanking;
  p.white_level = kNtscWhite;
  return p;
}

// One NTSC frame over a flat buffer: line n of the frame at n * 910.
class FrameVFR : public VideoFrameRepresentation {
 public:
  FrameVFR(std::vector<int16_t> samples, SourceParameters params)
      : samples_(std::move(samples)), params_(std::move(params)) {}

  FrameIDRange frame_range() const override { return {0, 0}; }
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
    d.samples_per_line_nominal =
        static_cast<size_t>(params_.frame_width_nominal);
    return d;
  }
  const sample_type* get_frame(FrameID id) const override {
    return id == FrameID{0} ? samples_.data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    return id == FrameID{0} ? samples_ : std::vector<sample_type>{};
  }
  std::optional<SourceParameters> get_video_parameters() const override {
    return params_;
  }

 private:
  std::vector<int16_t> samples_;
  SourceParameters params_;
};

// A frame whose line 21 carries |first| on the first field and |second| on
// the second (empty: blanking).
FrameVFR make_frame(const std::vector<int16_t>& first,
                    const std::vector<int16_t>& second) {
  const auto params = make_ntsc_params();
  std::vector<int16_t> samples(kLine * static_cast<size_t>(params.frame_height),
                               kBlanking);
  const size_t f1 = field1_lines(VideoSystem::NTSC);
  if (!first.empty()) {
    std::copy(first.begin(), first.end(), samples.begin() + 20 * kLine);
  }
  if (!second.empty()) {
    std::copy(second.begin(), second.end(),
              samples.begin() + (f1 + 20) * kLine);
  }
  return FrameVFR(std::move(samples), params);
}

bool read_bool(const ObservationContext& context, FieldID field,
               const std::string& key) {
  const auto value = context.get(field, "closed_caption", key);
  return value.has_value() && std::holds_alternative<bool>(*value) &&
         std::get<bool>(*value);
}

int32_t read_int(const ObservationContext& context, FieldID field,
                 const std::string& key) {
  const auto value = context.get(field, "closed_caption", key);
  if (!value || !std::holds_alternative<int32_t>(*value)) return -1;
  return std::get<int32_t>(*value);
}

// The first field's line 21 carries CC1, the second's the Extended Data
// Service: both are observed.
TEST(ClosedCaptionObserverFrames, ObservesLine21OfBothFields) {
  const FrameVFR frame =
      make_frame(caption_line(with_parity(0x14), with_parity(0x2C)),
                 caption_line(with_parity(0x05), with_parity(0x01)));
  ClosedCaptionObserver observer;
  ObservationContext context;
  observer.process_frame(frame, FrameID{0}, context);

  EXPECT_TRUE(read_bool(context, FieldID(0), "present"));
  EXPECT_EQ(read_int(context, FieldID(0), "data0"), 0x14);
  EXPECT_EQ(read_int(context, FieldID(0), "data1"), 0x2C);
  EXPECT_TRUE(read_bool(context, FieldID(0), "parity0_valid"));
  EXPECT_TRUE(read_bool(context, FieldID(0), "parity1_valid"));

  EXPECT_TRUE(read_bool(context, FieldID(1), "present"));
  EXPECT_EQ(read_int(context, FieldID(1), "data0"), 0x05);
  EXPECT_EQ(read_int(context, FieldID(1), "data1"), 0x01);
}

// A field whose line 21 carries no waveform records present=false and no
// bytes, as a caption-less recording's every field does.
TEST(ClosedCaptionObserverFrames, RecordsNothingPresentOnAnEmptyLine) {
  const FrameVFR frame =
      make_frame(caption_line(with_parity('H'), with_parity('i')), {});
  ClosedCaptionObserver observer;
  ObservationContext context;
  observer.process_frame(frame, FrameID{0}, context);

  EXPECT_TRUE(read_bool(context, FieldID(0), "present"));
  const auto present = context.get(FieldID(1), "closed_caption", "present");
  ASSERT_TRUE(present.has_value());
  EXPECT_FALSE(std::get<bool>(*present));
  EXPECT_FALSE(context.get(FieldID(1), "closed_caption", "data0").has_value());
}

// A frame whose first field carries |line_samples| on its line |index|
// (0-based), every other line blanking.
FrameVFR make_frame_on_line(size_t index,
                            const std::vector<int16_t>& line_samples) {
  const auto params = make_ntsc_params();
  std::vector<int16_t> samples(kLine * static_cast<size_t>(params.frame_height),
                               kBlanking);
  std::copy(line_samples.begin(), line_samples.end(),
            samples.begin() + static_cast<std::ptrdiff_t>(index * kLine));
  return FrameVFR(std::move(samples), params);
}

// A recording whose time base slipped a line carries its captions on line 22:
// where line 21 carries nothing, the observer reads the line next to it and
// says which line it read.
TEST(ClosedCaptionObserverFrames, ReadsTheLineNextToAnEmptyLine21) {
  const FrameVFR frame = make_frame_on_line(
      21, caption_line(with_parity(0x14), with_parity(0x2C)));
  ClosedCaptionObserver observer;
  ObservationContext context;
  observer.process_frame(frame, FrameID{0}, context);

  EXPECT_FALSE(read_bool(context, FieldID(0), "present"));
  EXPECT_EQ(read_int(context, FieldID(0), "other_line"), 22);
  EXPECT_EQ(read_int(context, FieldID(0), "other_data0"), 0x14);
  EXPECT_EQ(read_int(context, FieldID(0), "other_data1"), 0x2C);
  EXPECT_TRUE(read_bool(context, FieldID(0), "other_parity0_valid"));
  EXPECT_TRUE(read_bool(context, FieldID(0), "other_parity1_valid"));

  const FrameVFR line20 =
      make_frame_on_line(19, caption_line(with_parity('H'), with_parity('i')));
  ObservationContext context20;
  observer.process_frame(line20, FrameID{0}, context20);
  EXPECT_EQ(read_int(context20, FieldID(0), "other_line"), 20);
  EXPECT_EQ(read_int(context20, FieldID(0), "other_data0"), 'H');
}

// Where line 21 carried its pair the lines next to it are not read.
TEST(ClosedCaptionObserverFrames, ReadsNoNeighbourWhereLine21CarriedAPair) {
  const FrameVFR frame =
      make_frame(caption_line(with_parity('O'), with_parity('K')), {});
  ClosedCaptionObserver observer;
  ObservationContext context;
  observer.process_frame(frame, FrameID{0}, context);
  EXPECT_TRUE(read_bool(context, FieldID(0), "present"));
  EXPECT_FALSE(
      context.get(FieldID(0), "closed_caption", "other_line").has_value());
}

TEST(ClosedCaptionObserverFrames, AppliesTo525LineSystemsOnly) {
  ClosedCaptionObserver observer;
  SourceParameters params = make_ntsc_params();
  EXPECT_TRUE(observer.applies_to(params));
  params.system = VideoSystem::PAL_M;
  EXPECT_TRUE(observer.applies_to(params));
  params.system = VideoSystem::PAL;
  EXPECT_FALSE(observer.applies_to(params));
}

}  // namespace

}  // namespace tests
}  // namespace orc
