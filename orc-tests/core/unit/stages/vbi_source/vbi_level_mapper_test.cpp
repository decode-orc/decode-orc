/*
 * File:        vbi_level_mapper_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for card-capture level estimation and mapping
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * The synthetic lines below are built from the design's timing constants
 * rather than from captured data, so every expectation is traceable to a
 * standard rather than to a sample file.
 */

#include "vbi_level_mapper.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "vbi_output_frame.h"
#include "vbi_source_format.h"
#include "vbi_synthetic_line.h"
#include "vbi_teletext_service.h"

namespace orc {
namespace {

using orc::testing::render_synthetic_vbi_line;
using orc::testing::SyntheticVBILine;

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;
  return format;
}

VBITeletextService wst_service() {
  VBITeletextService service;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                   service, error))
      << error;
  return service;
}

VBIOutputLevels pal_output_levels() {
  VBIOutputFrame frame;
  std::string error;
  EXPECT_TRUE(make_vbi_output_frame(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                    frame, error))
      << error;
  return frame.levels;
}

// A 525-line bt8x8 dump of a NABTS service, with its capture offset at the
// figure the calibration fitted on a real bt8x8 dump: the run-in lands 32,88
// samples into a record of the card's 28,64 MHz samples.
constexpr double kBt8x8CaptureOffset = 262.07;

VBISourceFormat bt8x8_nabts_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(expand_vbi_source_preset(
      "bt8x8 card dump, 8-bit (NABTS, NTSC source)", format, error))
      << error;
  format.capture_offset_samples = kBt8x8CaptureOffset;
  format.capture_offset_is_auto = false;
  return format;
}

VBITeletextService nabts_service() {
  VBITeletextService service;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC,
                                   VBITeletextSystem::kNABTS, service, error))
      << error;
  return service;
}

VBIOutputLevels nabts_output_levels() {
  VBIOutputFrame frame;
  std::string error;
  EXPECT_TRUE(make_vbi_output_frame(VBITVSystem::kNTSC,
                                    VBITeletextSystem::kNABTS, frame, error))
      << error;
  return frame.levels;
}

// A NABTS line as a bt8x8 dump of a tape carries it: logic 0 at 33 counts and
// logic 1 at 123, through a VHS channel (design §5.3.6) that has collapsed the
// run-in to a tenth of the data amplitude, with tape noise.  Unless
// |carries_data| is false, which leaves blanking at 45 counts and the noise.
std::vector<double> thriller_line(const VBISourceFormat& format,
                                  const VBITeletextService& service,
                                  uint32_t seed, bool carries_data,
                                  double noise_counts) {
  SyntheticVBILine line;
  line.sample_rate_hz = format.sample_rate_hz;
  line.valid_samples = format.valid_samples;
  line.bit_rate_hz = service.bit_rate_hz;
  line.pattern = service.cri_frc_pattern;
  line.pattern_bits = service.cri_bits + service.frc_bits;
  line.run_in_bits = service.cri_bits;
  line.payload_bits = service.payload_bytes * 8u;
  line.anchor_position_samples = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);
  line.logic0 = carries_data ? 33.0 : 45.0;
  line.logic1 = 123.0;
  line.blur_bit_periods = 0.8;
  line.run_in_amplitude_fraction = 0.1;
  line.noise_amplitude = noise_counts;
  line.seed = seed;
  line.carries_data = carries_data;
  return render_synthetic_vbi_line(line);
}

// Description of a synthetic WST line in source-domain counts.  The clock
// run-in carries its own pair of levels so that a band-limited source, whose
// run-in has collapsed towards the data midpoint, can be modelled.
struct SyntheticLineSpec {
  double logic0 = 32.0;
  double logic1 = 180.0;
  double cri_low = 32.0;
  double cri_high = 180.0;
  double ripple_counts = 0.0;
  bool carries_teletext = true;

  // A level held from the first sample of the record to the clock run-in, as
  // a card window that opens on its clamp or on a disturbed back porch reads.
  // Unset leaves logic 0 there, as a clean back porch reads.
  std::optional<double> head_level;
};

// Deterministic sub-count ripple, standing in for capture noise without
// making the test depend on a random generator.
double ripple_at(uint32_t sample_index, double amplitude) {
  if (amplitude <= 0.0) {
    return 0.0;
  }
  const int step = static_cast<int>((sample_index * 37u) % 5u) - 2;
  return (static_cast<double>(step) / 2.0) * amplitude;
}

// The transmitted bit sequence of a line of |service|: its sixteen alternating
// run-in bits and framing code, then a payload.
std::vector<bool> service_line_bits(const VBITeletextService& service) {
  std::vector<bool> bits;
  const uint32_t prefix_bits = service.cri_bits + service.frc_bits;
  bits.reserve(prefix_bits + service.payload_bytes * 8u);

  for (uint32_t bit = 0; bit < prefix_bits; ++bit) {
    const uint32_t shift = prefix_bits - 1u - bit;
    bits.push_back(((service.cri_frc_pattern >> shift) & 1u) != 0u);
  }
  for (uint32_t bit = 0; bit < service.payload_bytes * 8u; ++bit) {
    // A run-rich payload, so the data region is not a second run-in: three
    // ones, then three zeros, from the first payload bit.
    bits.push_back(((bit / 3u) % 2u) == 0u);
  }
  return bits;
}

std::vector<double> make_synthetic_line(const VBISourceFormat& format,
                                        const VBITeletextService& service,
                                        const SyntheticLineSpec& spec) {
  const double samples_per_bit = service.samples_per_bit(format.sample_rate_hz);
  const double cri_start = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);
  const std::vector<bool> bits = service_line_bits(service);

  std::vector<double> samples;
  samples.reserve(format.valid_samples);
  for (uint32_t index = 0; index < format.valid_samples; ++index) {
    double value = spec.logic0;
    if (spec.head_level.has_value() && static_cast<double>(index) < cri_start) {
      value = *spec.head_level;
    }

    if (spec.carries_teletext) {
      const double bit_position =
          (static_cast<double>(index) - cri_start) / samples_per_bit;
      if (bit_position >= 0.0) {
        const size_t bit_index = static_cast<size_t>(bit_position);
        if (bit_index < bits.size()) {
          const bool is_run_in = bit_index < service.cri_bits;
          const double high = is_run_in ? spec.cri_high : spec.logic1;
          const double low = is_run_in ? spec.cri_low : spec.logic0;
          value = bits[bit_index] ? high : low;
        }
      }
    }

    samples.push_back(value + ripple_at(index, spec.ripple_counts));
  }
  return samples;
}

// Sample position of the centre of payload bit |bit| on a line of |service|.
double payload_bit_centre(const VBISourceFormat& format,
                          const VBITeletextService& service, uint32_t bit) {
  const double samples_per_bit = service.samples_per_bit(format.sample_rate_hz);
  const double cri_start = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);
  return cri_start +
         samples_per_bit *
             (static_cast<double>(service.cri_bits + service.frc_bits + bit) +
              0.5);
}

// The run-rich payload's first bits are 111000: bit 1 sits in the middle of a
// run of ones and bit 4 in the middle of a run of zeros.
constexpr uint32_t kPayloadOneBit = 1;
constexpr uint32_t kPayloadZeroBit = 4;

// A record's sample at a position after the mapper's levels are applied to
// it, which is what the frame builder does with them.
double mapped_sample_at(const std::vector<double>& samples,
                        const VBILineLevels& levels, double position,
                        const VBIOutputLevels& output_levels) {
  const VBISampleMap map = make_vbi_sample_map(levels, output_levels);
  const size_t index = static_cast<size_t>(std::lround(position));
  return map.apply(samples.at(index));
}

// The mapped logic 1 and logic 0 of a line of |service|, read at the centres
// of a payload run of ones and of zeros.
double mapped_logic1(const VBISourceFormat& format,
                     const VBITeletextService& service,
                     const std::vector<double>& samples,
                     const VBILineLevels& levels,
                     const VBIOutputLevels& output_levels) {
  return mapped_sample_at(samples, levels,
                          payload_bit_centre(format, service, kPayloadOneBit),
                          output_levels);
}

double mapped_logic0(const VBISourceFormat& format,
                     const VBITeletextService& service,
                     const std::vector<double>& samples,
                     const VBILineLevels& levels,
                     const VBIOutputLevels& output_levels) {
  return mapped_sample_at(samples, levels,
                          payload_bit_centre(format, service, kPayloadZeroBit),
                          output_levels);
}

std::vector<VBILineRecord> make_frame_records(
    const std::vector<std::vector<double>>& lines) {
  std::vector<VBILineRecord> records;
  records.reserve(lines.size());
  for (size_t index = 0; index < lines.size(); ++index) {
    VBILineRecord record;
    record.frame_index = 0;
    record.field_index = (index < lines.size() / 2) ? 0u : 1u;
    record.record_index = static_cast<uint32_t>(
        index % (lines.size() / 2 > 0 ? lines.size() / 2 : 1));
    record.samples = lines[index];
    records.push_back(record);
  }
  return records;
}

// ---------------------------------------------------------------------------
// Service timing
// ---------------------------------------------------------------------------

TEST(VBITeletextServiceTable, WSTTimingMatchesTheServiceTable) {
  const VBITeletextService service = wst_service();

  EXPECT_DOUBLE_EQ(service.t_offset_ns, 10300.0);
  EXPECT_DOUBLE_EQ(service.bit_rate_hz, 6937500.0);
  EXPECT_EQ(service.cri_bits, 16u);
  EXPECT_EQ(service.frc_bits, 8u);
  EXPECT_EQ(service.cri_frc_pattern, 0xAAAAE4u);
  EXPECT_EQ(service.payload_bytes, 42u);

  // 444 x fH, the 625-line frequency being 15 625 Hz.
  EXPECT_DOUBLE_EQ(service.bit_rate_hz, 444.0 * 15625.0);
}

// The record's sample 0 sits capture_offset samples after 0H, so the anchor
// moves back by exactly that much.
TEST(VBITeletextServiceTable, WSTAnchorsRelativeToTheCaptureWindow) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  // 10.3 us at 8 x fsc PAL.
  EXPECT_NEAR(service.cri_start_samples(format.sample_rate_hz, 0.0), 365.33,
              0.01);
  EXPECT_NEAR(service.cri_start_samples(format.sample_rate_hz, 244.0), 121.33,
              0.01);
  EXPECT_NEAR(service.samples_per_bit(format.sample_rate_hz), 5.1126, 0.0005);
}

// ITU-R BT.653 Table 3: System B on 525 lines keeps the 625-line run-in and
// framing code but not its bit rate or packet length, which is why a 625-line
// slicer cannot read a 525-line line.
TEST(VBITeletextServiceTable, WSTOn525LinesCarriesItsOwnTimings) {
  VBITeletextService service;
  std::string error;

  ASSERT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                   service, error))
      << error;

  // Measured on the circulating captures, not taken from libzvbi's table,
  // which disagrees with them by seven bit periods.
  EXPECT_DOUBLE_EQ(service.t_offset_ns, 9300.0);
  EXPECT_DOUBLE_EQ(service.bit_rate_hz, 5727272.0);
  EXPECT_EQ(service.cri_bits, 16u);
  EXPECT_EQ(service.frc_bits, 8u);
  EXPECT_EQ(service.cri_frc_pattern, 0xAAAAE4u);
  EXPECT_EQ(service.payload_bytes, 34u);

  // At 4 x fsc NTSC that is exactly two and a half samples per bit.
  EXPECT_NEAR(service.samples_per_bit(kNtscSampleRate), 2.5, 1e-6);
}

// EIA-516 System C: NABTS shares the 525-line lines and bit rate with System B
// and differs in framing code and packet length — which is why the framing
// code is the only thing that tells the two apart on a capture.
TEST(VBITeletextServiceTable, NABTSCarriesItsOwnFramingCodeAndPacketLength) {
  VBITeletextService wst;
  VBITeletextService nabts;
  std::string error;

  ASSERT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                   wst, error))
      << error;
  ASSERT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC,
                                   VBITeletextSystem::kNABTS, nabts, error))
      << error;

  EXPECT_EQ(nabts.cri_frc_pattern, 0xAAAAE7u);
  EXPECT_EQ(nabts.payload_bytes, 33u);
  // Measured on the ExtraVision captures; libzvbi tabulates 10 480 ns, which
  // leaves the head of the run-in on the edge of the written window.
  EXPECT_DOUBLE_EQ(nabts.t_offset_ns, 10300.0);

  // What the two share.
  EXPECT_DOUBLE_EQ(nabts.bit_rate_hz, wst.bit_rate_hz);
  EXPECT_EQ(nabts.cri_bits, wst.cri_bits);
  EXPECT_EQ(nabts.frc_bits, wst.frc_bits);

  // And what they do not: the framing code is the discriminator.
  EXPECT_NE(nabts.cri_frc_pattern, wst.cri_frc_pattern);
  EXPECT_NE(nabts.payload_bytes, wst.payload_bytes);
}

// CTA-608-E §4 in the terms the correlator and the level reader work in,
// counted in half-bit symbols from the run-in's first rising midpoint: its
// seven peaks and the six troughs between them are thirteen alternating
// symbols, the two 0 start bits and the 1 start bit six more, and the sixteen
// data bits thirty-two, all at twice the 32 x fH bit rate.
TEST(VBITeletextServiceTable, LineTwentyOneCaptionsAreDescribedInHalfBits) {
  VBITeletextService service;
  std::string error;
  ASSERT_TRUE(vbi_caption_service(VBITVSystem::kNTSC, service, error)) << error;

  EXPECT_DOUBLE_EQ(service.t_offset_ns, 10500.0);
  EXPECT_NEAR(service.bit_rate_hz, 2.0 * 32.0 * 4500000.0 / 286.0, 1e-6);
  EXPECT_EQ(service.cri_bits, 13u);
  EXPECT_EQ(service.frc_bits, 6u);
  EXPECT_EQ(service.cri_frc_pattern, 0x55543u);
  EXPECT_EQ(service.payload_bytes, 4u);

  // CTA-608-E Table 2: the 1 start bit begins 8.75 bit periods after the
  // run-in does, which is 8.5 after its first rising midpoint — seventeen
  // symbols, the four of the two 0 start bits after the run-in's thirteen.
  uint32_t start_bit = 0;
  const uint32_t symbols = service.cri_bits + service.frc_bits;
  for (uint32_t index = service.cri_bits; index < symbols; ++index) {
    if (((service.cri_frc_pattern >> (symbols - 1u - index)) & 1u) != 0u) {
      start_bit = index;
      break;
    }
  }
  EXPECT_DOUBLE_EQ(static_cast<double>(start_bit) / 2.0, 8.5);

  EXPECT_FALSE(vbi_caption_service(VBITVSystem::kPAL, service, error));
}

// NABTS is a 525-line service and has no line list on 625, so the pairing is
// refused rather than placed somewhere invented.
TEST(VBITeletextServiceTable, NABTSIsNotDefinedOn625Lines) {
  VBITeletextService service;
  std::string error;

  EXPECT_FALSE(vbi_teletext_service(VBITVSystem::kPAL,
                                    VBITeletextSystem::kNABTS, service, error));
  EXPECT_FALSE(error.empty());
}

// ---------------------------------------------------------------------------
// Level reading windows
// ---------------------------------------------------------------------------

// The levels are read from the payload — from the end of the framing code,
// 24 bits after the run-in opens at 121,33 samples, to the end of the 336-bit
// payload, at 5,11 samples a bit for bt8x8 PAL — and logic 1 from the run-in's
// sixteen bits too.
TEST(VBILevelMapper, Bt8x8PALWindowsCoverThePayloadAndTheRunIn) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  const VBIRecordWindows windows = vbi_record_windows(format, service);

  EXPECT_EQ(windows.data.payload_begin, 245u);
  EXPECT_EQ(windows.data.payload_end, 1961u);
  EXPECT_EQ(windows.data.run_in_begin, 122u);
  EXPECT_EQ(windows.data.run_in_end, 203u);

  // A line carries the service when its run-in is found within the scatter
  // the format allows the fit, either side of where the fit put it.
  EXPECT_NEAR(windows.presence.begin_samples, 121.33 - 8.0, 0.01);
  EXPECT_NEAR(windows.presence.end_samples, 121.33 + 8.0, 0.01);
}

TEST(VBILevelMapper, WindowsStayInsideTheValidSamplesOfARecord) {
  VBISourceFormat format = bt8x8_pal_format();
  format.valid_samples = 150;
  const VBITeletextService service = wst_service();

  const VBIRecordWindows windows = vbi_record_windows(format, service);

  EXPECT_LE(windows.data.payload_begin, 150u);
  EXPECT_LE(windows.data.payload_end, 150u);
  EXPECT_LE(windows.data.run_in_end, 150u);
}

// ---------------------------------------------------------------------------
// Per-line estimation
// ---------------------------------------------------------------------------

TEST(VBILevelMapper, CleanLineLevelsAreReadFromThePayload) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIRecordWindows windows = vbi_record_windows(format, service);

  SyntheticLineSpec spec;
  spec.logic0 = 32.0;
  spec.logic1 = 180.0;

  const VBILineLevels levels =
      estimate_vbi_data_levels(make_synthetic_line(format, service, spec),
                               windows.data, VBILevelMapperConfig{});

  EXPECT_TRUE(levels.usable);
  EXPECT_NEAR(levels.logic0, 32.0, 0.5);
  EXPECT_NEAR(levels.logic1, 180.0, 0.5);
}

TEST(VBILevelMapper, ALineWithNoDataServiceIsNotUsable) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIRecordWindows windows = vbi_record_windows(format, service);

  SyntheticLineSpec spec;
  spec.carries_teletext = false;
  spec.ripple_counts = 1.0;

  const VBILineLevels levels =
      estimate_vbi_data_levels(make_synthetic_line(format, service, spec),
                               windows.data, VBILevelMapperConfig{});

  EXPECT_FALSE(levels.usable);
  EXPECT_LT(levels.amplitude(), 8.0);
}

// A payload that is hardly ever 1 — line-21 caption padding is an eighth ones,
// each a lone bit a band-limited channel never lets settle — does not reach
// logic 1 at its upper tenth.  The run-in, half ones whatever the payload
// carries, does, and logic 1 is read from it.
TEST(VBILevelMapper, APayloadShortOfOnesTakesLogicOneFromTheRunIn) {
  const VBISourceFormat format = bt8x8_nabts_format();
  const VBITeletextService service = nabts_service();

  std::vector<bool> payload;
  for (uint32_t bit = 0; bit < service.payload_bytes * 8u; ++bit) {
    payload.push_back((bit % 8u) == 7u);
  }
  SyntheticVBILine line;
  line.sample_rate_hz = format.sample_rate_hz;
  line.valid_samples = format.valid_samples;
  line.bit_rate_hz = service.bit_rate_hz;
  line.pattern = service.cri_frc_pattern;
  line.payload = payload;
  line.anchor_position_samples = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);
  line.logic0 = 40.0;
  line.logic1 = 160.0;
  const std::vector<double> samples = render_synthetic_vbi_line(line);

  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});
  const VBILineLevels levels = estimate_vbi_data_levels(
      samples, mapper.windows().data, VBILevelMapperConfig{});

  VBIDataWindows payload_only = mapper.windows().data;
  payload_only.run_in_end = payload_only.run_in_begin;
  const VBILineLevels payload_levels =
      estimate_vbi_data_levels(samples, payload_only, VBILevelMapperConfig{});

  ASSERT_TRUE(levels.usable);
  EXPECT_NEAR(levels.logic0, 40.0, 2.0);
  EXPECT_NEAR(levels.logic1, 160.0, 12.0);
  EXPECT_GT(levels.logic1, payload_levels.logic1 + 20.0);
}

// The regression test for a bt8x8 dump of a tape.  The card's window
// opens a microsecond ahead of the run-in on whatever its clamp left there —
// 57-119 counts on that capture, against a data logic 0 of 33 — and through a
// VHS channel the run-in itself has collapsed to the data midpoint.  Read from
// the record's head, logic 0 landed near the midpoint and the gain came out
// nearly twice too large, which put the data's lower third below the output's
// floor.  Read from the payload, the line maps onto the standard's levels.
TEST(VBILevelMapper, AHighRecordHeadDoesNotMoveTheLevels) {
  const VBISourceFormat format = bt8x8_nabts_format();
  const VBITeletextService service = nabts_service();
  const VBIOutputLevels output = nabts_output_levels();

  std::vector<double> samples =
      thriller_line(format, service, 1, true, /*noise_counts=*/3.0);
  const double cri_start = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);
  for (size_t index = 0; static_cast<double>(index) < cri_start - 3.0;
       ++index) {
    samples[index] = 115.0;
  }

  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});
  EXPECT_TRUE(mapper.carries_service(samples));

  std::vector<VBILineRecord> records(2);
  for (VBILineRecord& record : records) {
    record.samples = samples;
  }
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(records, mapped);

  ASSERT_EQ(mapped.size(), 2u);
  ASSERT_TRUE(mapped[0].usable);

  // Through a VHS channel the payload's tenths sit a little inside the levels
  // its long runs settle at.
  EXPECT_NEAR(mapped[0].logic0, 33.0, 8.0);
  EXPECT_NEAR(mapped[0].logic1, 123.0, 12.0);

  // No payload sample is pushed onto the output's floor or ceiling.
  const VBISampleMap map = make_vbi_sample_map(mapped[0], output);
  const VBIRecordWindows& windows = mapper.windows();
  for (uint32_t index = windows.data.payload_begin;
       index < windows.data.payload_end; ++index) {
    const double value = map.apply(samples[index]);
    EXPECT_GT(value, static_cast<double>(kVBIOutputSampleMin)) << index;
    EXPECT_LT(value, static_cast<double>(kVBIOutputSampleMax)) << index;
  }

  // The head keeps its own place, far above logic 0: it is not read as one.
  EXPECT_GT(map.apply(samples[0]),
            output.logic0 + 0.5 * output.data_amplitude());
}

// A quantile by sorting, interpolated between the two order statistics either
// side of it: the value the level estimate has to give, whether it selects or
// counts.
double sorted_quantile_of(std::vector<double> values, double quantile) {
  std::sort(values.begin(), values.end());
  const double position =
      std::clamp(quantile, 0.0, 1.0) * static_cast<double>(values.size() - 1);
  const double floor_position = std::floor(position);
  const auto lower = static_cast<size_t>(floor_position);
  if (lower + 1 >= values.size()) {
    return values[lower];
  }
  return values[lower] +
         (values[lower + 1] - values[lower]) * (position - floor_position);
}

// An 8-bit card's windows are counted rather than selected from, and give the
// levels to the bit: two-level data with its edges, whole-range noise, a flat
// line, a lone sample, windows running past the record, and quantiles at
// either end.  A window with a fraction in it is selected from, as before.
TEST(VBILevelMapper, ByteWindowsGiveTheLevelsSelectionGives) {
  std::mt19937 random(20261012);
  const auto below = [&random](size_t bound) {
    return static_cast<size_t>(random() % bound);
  };
  const double quantiles[][2] = {
      {0.10, 0.90}, {0.0, 1.0}, {0.5, 0.5}, {0.03, 0.97}, {0.25, 0.75}};

  for (int trial = 0; trial < 600; ++trial) {
    const size_t length = 1 + below(1600);
    std::vector<double> samples(length);
    const int shape = trial % 4;
    for (size_t index = 0; index < length; ++index) {
      int value = 0;
      if (shape == 0) {
        value =
            ((index / 5) % 3 == 0 ? 160 : 40) + static_cast<int>(below(9)) - 4;
      } else if (shape == 1) {
        value = static_cast<int>(below(256));
      } else if (shape == 2) {
        value = 117;
      } else {
        value = static_cast<int>(below(4)) * 60;
      }
      samples[index] = static_cast<double>(std::clamp(value, 0, 255));
    }
    if (trial % 7 == 6) {
      samples[below(length)] += 0.25;
    }

    VBIDataWindows windows;
    windows.payload_begin = static_cast<uint32_t>(below(length + 20));
    windows.payload_end =
        windows.payload_begin + static_cast<uint32_t>(below(length + 20));
    windows.run_in_begin = static_cast<uint32_t>(below(length + 20));
    windows.run_in_end =
        windows.run_in_begin + static_cast<uint32_t>(below(120));

    VBILevelMapperConfig config;
    config.data_low_quantile = quantiles[trial % 5][0];
    config.data_high_quantile = quantiles[trial % 5][1];
    const VBILineLevels levels =
        estimate_vbi_data_levels(samples, windows, config);

    const auto window = [&samples](uint32_t begin, uint32_t end) {
      const size_t first = std::min<size_t>(begin, samples.size());
      const size_t last = std::min<size_t>(end, samples.size());
      return last > first ? std::vector<double>(samples.begin() + first,
                                                samples.begin() + last)
                          : std::vector<double>{};
    };
    const std::vector<double> payload =
        window(windows.payload_begin, windows.payload_end);
    if (payload.empty()) {
      EXPECT_FALSE(levels.usable) << "trial " << trial;
      continue;
    }
    const double logic0 = sorted_quantile_of(payload, config.data_low_quantile);
    double logic1 = sorted_quantile_of(payload, config.data_high_quantile);
    const std::vector<double> run_in =
        window(windows.run_in_begin, windows.run_in_end);
    if (!run_in.empty()) {
      logic1 = std::max(logic1,
                        sorted_quantile_of(run_in, config.data_high_quantile));
    }
    EXPECT_EQ(levels.logic0, logic0) << "trial " << trial;
    EXPECT_EQ(levels.logic1, logic1) << "trial " << trial;

    // Bytes vouched for are counted unchecked, to the same levels.
    if (trial % 7 != 6) {
      const VBILineLevels vouched =
          estimate_vbi_data_levels(samples, windows, config, true);
      EXPECT_EQ(vouched.logic0, logic0) << "trial " << trial;
      EXPECT_EQ(vouched.logic1, logic1) << "trial " << trial;
    }
  }
}

// ---------------------------------------------------------------------------
// Mapping into the output domain
// ---------------------------------------------------------------------------

TEST(VBILevelMapper, CleanLineMapsOntoTheDerivedPALLogicLevels) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIOutputLevels output = pal_output_levels();

  SyntheticLineSpec spec;
  const std::vector<std::vector<double>> lines(
      2, make_synthetic_line(format, service, spec));

  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 2u);
  ASSERT_TRUE(mapped[0].usable);

  EXPECT_NEAR(mapped_logic0(format, service, lines[0], mapped[0], output),
              256.0, 2.0);
  EXPECT_NEAR(mapped_logic1(format, service, lines[0], mapped[0], output),
              644.0, 2.0);
}

TEST(VBILevelMapper, MildCaptureNoiseDoesNotShiftTheMappedLogicLevels) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIOutputLevels output = pal_output_levels();

  SyntheticLineSpec spec;
  spec.ripple_counts = 1.0;
  const std::vector<std::vector<double>> lines(
      2, make_synthetic_line(format, service, spec));

  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 2u);
  EXPECT_NEAR(mapped_logic0(format, service, lines[0], mapped[0], output),
              256.0, 6.0);
  EXPECT_NEAR(mapped_logic1(format, service, lines[0], mapped[0], output),
              644.0, 6.0);
}

// On the VHS waveform measured in design §5.3.6 the run-in has collapsed to 7
// counts peak to peak about the data midpoint, while the data still swings the
// full 127.  Nothing about the run-in enters the levels, so the line keeps its
// full scale.
TEST(VBILevelMapper, ACollapsedRunInDoesNotScaleTheLine) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIOutputLevels output = pal_output_levels();
  const VBIRecordWindows windows = vbi_record_windows(format, service);

  SyntheticLineSpec spec;
  spec.logic0 = 57.0;
  spec.logic1 = 184.0;   // 127 counts peak to peak, midpoint 120.5
  spec.cri_low = 115.6;  // 7 counts peak to peak, mean 119.1
  spec.cri_high = 122.6;

  const std::vector<double> samples =
      make_synthetic_line(format, service, spec);
  const VBILineLevels levels =
      estimate_vbi_data_levels(samples, windows.data, VBILevelMapperConfig{});

  ASSERT_TRUE(levels.usable);
  EXPECT_NEAR(levels.logic0, 57.0, 0.5);
  EXPECT_NEAR(levels.logic1, 184.0, 0.5);

  const std::vector<std::vector<double>> lines(2, samples);
  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 2u);
  const double mapped_zero =
      mapped_logic0(format, service, samples, mapped[0], output);
  const double mapped_one =
      mapped_logic1(format, service, samples, mapped[0], output);

  EXPECT_NEAR(mapped_zero, 256.0, 2.0);
  EXPECT_NEAR(mapped_one, 644.0, 2.0);
  EXPECT_NEAR(mapped_one - mapped_zero, 388.0, 4.0);
}

// Nothing is done to the samples but a linear map: a blurred waveform must
// arrive downstream still blurred, because the deconvolving slicer recovers
// data by matching it (design §5.3.6).
TEST(VBILevelMapper, MappingIsLinearAndPreservesTheWaveformShape) {
  VBILineLevels levels;
  levels.logic0 = 40.0;
  levels.logic1 = 160.0;
  levels.usable = true;

  const VBISampleMap map = make_vbi_sample_map(levels, pal_output_levels());

  EXPECT_NEAR(map.apply(40.0), 256.0, 1e-9);
  EXPECT_NEAR(map.apply(160.0), 644.0, 1e-9);

  // Midpoints stay midpoints and the map extends outside the logic levels
  // without any knee: overshoot is dealt with by the output clamp, not here.
  EXPECT_NEAR(map.apply(100.0), 450.0, 1e-9);
  EXPECT_NEAR(map.apply(220.0), 838.0, 1e-9);
  EXPECT_NEAR(map.apply(-20.0), 62.0, 1e-9);
}

// A line with no level reference has no honest scale, so its map produces
// blanking whatever it is given (design §5.3.4).
TEST(VBILevelMapper, AnUnusableMeasurementMapsEverythingToBlanking) {
  const VBIOutputLevels output = pal_output_levels();
  const VBISampleMap map = make_vbi_sample_map(VBILineLevels{}, output);

  EXPECT_DOUBLE_EQ(map.apply(0.0), output.blanking);
  EXPECT_DOUBLE_EQ(map.apply(255.0), output.blanking);
}

// ---------------------------------------------------------------------------
// Which lines carry the service
// ---------------------------------------------------------------------------

// A line carries the service when its run-in and framing code are where the
// capture-offset fit put them.  A blank line does not, and nor does a burst a
// few bits away from that place, which is no part of the service the fit
// found.
TEST(VBILevelMapper, ALineCarriesTheServiceOnlyWhereTheFitPutItsRunIn) {
  const VBISourceFormat format = bt8x8_nabts_format();
  const VBITeletextService service = nabts_service();
  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});

  EXPECT_TRUE(mapper.carries_service(
      thriller_line(format, service, 3, true, /*noise_counts=*/3.0)));
  EXPECT_FALSE(mapper.carries_service(
      thriller_line(format, service, 3, false, /*noise_counts=*/15.0)));

  VBISourceFormat displaced = format;
  displaced.capture_offset_samples -=
      4.0 * service.samples_per_bit(format.sample_rate_hz);
  EXPECT_FALSE(mapper.carries_service(
      thriller_line(displaced, service, 3, true, /*noise_counts=*/3.0)));
}

// A noisy tape's blank line reads a third of the data amplitude between its
// payload quantiles.  Its own levels would stretch that noise into a
// full-scale burst for the slicer to chase; it is mapped through the levels of
// the frame's data lines instead, which are the card's gain at that moment.
TEST(VBILevelMapper, NoisyBlankLinesAreMappedThroughTheDataLinesLevels) {
  const VBISourceFormat format = bt8x8_nabts_format();
  const VBITeletextService service = nabts_service();
  const VBIOutputLevels output = nabts_output_levels();

  // Twelve records a field; field lines 15 and 16 carry the data, as on such
  // a dump.
  std::vector<VBILineRecord> records;
  for (uint32_t field = 0; field < 2u; ++field) {
    for (uint32_t index = 0; index < 12u; ++index) {
      const bool data = index == 5u || index == 6u;
      VBILineRecord record;
      record.field_index = field;
      record.record_index = index;
      record.samples =
          thriller_line(format, service, field * 12u + index + 1u, data,
                        /*noise_counts=*/data ? 3.0 : 15.0);
      records.push_back(std::move(record));
    }
  }

  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});
  const VBIRecordWindows& windows = mapper.windows();

  // A blank line's own payload quantiles are a usable measurement: what keeps
  // them out is the line's amplitude against the data lines'.
  const VBILineLevels blank_own = estimate_vbi_data_levels(
      records[0].samples, windows.data, VBILevelMapperConfig{});
  ASSERT_TRUE(blank_own.usable);
  EXPECT_GT(blank_own.amplitude(), 20.0);

  std::vector<VBILineLevels> mapped;
  mapper.map_frame(records, mapped);
  ASSERT_EQ(mapped.size(), records.size());

  const VBILineLevels& data_levels = mapped[5];
  ASSERT_TRUE(data_levels.usable);
  EXPECT_NEAR(data_levels.logic0, 33.0, 8.0);
  EXPECT_NEAR(data_levels.amplitude(), 90.0, 15.0);

  for (size_t index = 0; index < records.size(); ++index) {
    if (records[index].record_index == 5u ||
        records[index].record_index == 6u) {
      continue;
    }
    ASSERT_TRUE(mapped[index].usable) << index;
    EXPECT_NEAR(mapped[index].amplitude(), data_levels.amplitude(), 12.0)
        << index;

    // The noise stays the third of the data amplitude it is, about blanking.
    const VBISampleMap map = make_vbi_sample_map(mapped[index], output);
    for (uint32_t sample = windows.data.payload_begin;
         sample < windows.data.payload_end; ++sample) {
      EXPECT_LT(map.apply(records[index].samples[sample]),
                output.logic0 + 0.5 * output.data_amplitude())
          << index << " " << sample;
    }
  }
}

// ---------------------------------------------------------------------------
// Mode policy
// ---------------------------------------------------------------------------

// A frame of nominal lines with one line whose gain is slightly off.
std::vector<std::vector<double>> frame_with_gain_outlier(
    const VBISourceFormat& format, const VBITeletextService& service,
    size_t outlier_index, double outlier_logic1) {
  std::vector<std::vector<double>> lines;
  for (size_t index = 0; index < 32; ++index) {
    SyntheticLineSpec spec;
    if (index == outlier_index) {
      spec.logic1 = outlier_logic1;
      spec.cri_high = outlier_logic1;
    }
    lines.push_back(make_synthetic_line(format, service, spec));
  }
  return lines;
}

// Per-line normalisation follows every line's own gain, which is what carries
// per-line estimation noise into the output.
TEST(VBILevelMapper, PerLineModeNormalisesEachLineByItsOwnEstimate) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIOutputLevels output = pal_output_levels();

  VBILevelMapperConfig config;
  config.mode = VBILevelMode::kPerLine;

  const std::vector<std::vector<double>> lines =
      frame_with_gain_outlier(format, service, 7, 188.0);

  VBILevelMapper mapper(format, service, config);
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 32u);
  EXPECT_NEAR(mapped[7].logic1, 188.0, 0.5);

  // The line's own gain takes it back to the nominal logic 1.
  EXPECT_NEAR(mapped_logic1(format, service, lines[7], mapped[7], output),
              644.0, 2.0);
}

// Rolling mode holds a line at the frame's median levels, so a single line's
// gain noise does not reach the output as a gain change.
TEST(VBILevelMapper, RollingModeDoesNotPropagateSingleLineGainNoise) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIOutputLevels output = pal_output_levels();
  const VBIRecordWindows windows = vbi_record_windows(format, service);

  VBILevelMapperConfig config;
  config.mode = VBILevelMode::kRolling;

  const std::vector<std::vector<double>> lines =
      frame_with_gain_outlier(format, service, 7, 188.0);

  // The outlier really is at 188 when it is measured on its own.
  EXPECT_NEAR(estimate_vbi_data_levels(lines[7], windows.data, config).logic1,
              188.0, 0.5);

  VBILevelMapper mapper(format, service, config);
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 32u);

  // Every line in the frame is mapped through the same gain, so the outlier
  // remains visible as signal rather than being normalised away.
  EXPECT_NEAR(mapped[7].logic1, 180.0, 0.5);
  for (const VBILineLevels& line : mapped) {
    EXPECT_NEAR(line.logic1, mapped[0].logic1, 1e-9);
    EXPECT_NEAR(line.logic0, mapped[0].logic0, 1e-9);
  }
  EXPECT_GT(mapped_logic1(format, service, lines[7], mapped[7], output), 644.0);
}

// A real gain change is larger than estimation noise and is still tracked.
TEST(VBILevelMapper, RollingModeCorrectsALineThatDeviatesSignificantly) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIOutputLevels output = pal_output_levels();

  VBILevelMapperConfig config;
  config.mode = VBILevelMode::kRolling;

  const std::vector<std::vector<double>> lines =
      frame_with_gain_outlier(format, service, 7, 260.0);

  VBILevelMapper mapper(format, service, config);
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 32u);
  EXPECT_NEAR(mapped[7].logic1, 260.0, 0.5);
  EXPECT_NEAR(mapped_logic1(format, service, lines[7], mapped[7], output),
              644.0, 2.0);
}

// A weak line inside a good frame carries no data service; its own estimate
// is noise, so the frame's levels are applied instead of a fabricated gain.
TEST(VBILevelMapper, AWeakLineIsMappedThroughTheFrameLevels) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  std::vector<std::vector<double>> lines;
  for (size_t index = 0; index < 32; ++index) {
    SyntheticLineSpec spec;
    if (index == 3) {
      spec.logic1 = 52.0;  // 20 counts of amplitude against a median of 148
      spec.cri_high = 52.0;
    }
    lines.push_back(make_synthetic_line(format, service, spec));
  }

  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 32u);
  EXPECT_TRUE(mapped[3].usable);
  EXPECT_NEAR(mapped[3].logic1, 180.0, 0.5);
}

// With no line of the frame carrying the service and nothing measured when the
// capture was opened, there is no honest scale to apply, so the records become
// ordinary blanking rather than amplified noise.
TEST(VBILevelMapper, AFrameWithNoDataServiceIsEmittedAsBlanking) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  SyntheticLineSpec spec;
  spec.carries_teletext = false;
  spec.ripple_counts = 1.0;
  const std::vector<std::vector<double>> lines(
      4, make_synthetic_line(format, service, spec));

  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 4u);
  for (const VBILineLevels& line : mapped) {
    EXPECT_FALSE(line.usable);
    const VBISampleMap map = make_vbi_sample_map(line, pal_output_levels());
    for (const double sample : lines[0]) {
      EXPECT_DOUBLE_EQ(map.apply(sample), 256.0);
    }
  }
}

// A frame in which no line carries the service says nothing about the card's
// gain, so it is mapped at the levels the capture's data lines were measured
// at when it was opened: its noise keeps the size it has rather than being
// stretched to the data amplitude.
TEST(VBILevelMapper, AFrameWithNoDataLineIsMappedAtTheCaptureLevels) {
  const VBISourceFormat format = bt8x8_nabts_format();
  const VBITeletextService service = nabts_service();

  VBILevelMapperConfig config;
  VBILineLevels capture;
  capture.logic0 = 33.0;
  capture.logic1 = 123.0;
  capture.usable = true;
  config.capture_levels = capture;

  std::vector<VBILineRecord> records(4);
  for (size_t index = 0; index < records.size(); ++index) {
    records[index].record_index = static_cast<uint32_t>(index);
    records[index].samples =
        thriller_line(format, service, static_cast<uint32_t>(index) + 1u, false,
                      /*noise_counts=*/15.0);
  }

  VBILevelMapper mapper(format, service, config);
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(records, mapped);

  ASSERT_EQ(mapped.size(), 4u);
  for (const VBILineLevels& line : mapped) {
    EXPECT_TRUE(line.usable);
    EXPECT_DOUBLE_EQ(line.logic0, 33.0);
    EXPECT_DOUBLE_EQ(line.logic1, 123.0);
  }
}

// A caption line's data swings from blanking to 50 IRE (CTA-608-E §4), which
// fixes a capture's gain in IRE; the service's logic levels are read off that
// scale where the output puts them — NABTS logic 1 at 70 IRE, WST logic 0 at
// the 7,5 IRE setup.
TEST(VBILevelMapper, CaptionLevelsGiveTheServiceLevelsOnTheIREScale) {
  VBILineLevels caption;
  caption.logic0 = 60.0;
  caption.logic1 = 140.0;  // 1,6 counts per IRE
  caption.usable = true;

  const VBILineLevels nabts =
      vbi_levels_from_caption_levels(caption, nabts_output_levels());
  ASSERT_TRUE(nabts.usable);
  EXPECT_NEAR(nabts.logic0, 60.0, 1e-9);
  EXPECT_NEAR(nabts.logic1, 60.0 + 70.0 * 1.6, 1e-9);

  VBIOutputFrame wst;
  std::string error;
  ASSERT_TRUE(make_vbi_output_frame(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                    wst, error))
      << error;
  const VBILineLevels wst_levels =
      vbi_levels_from_caption_levels(caption, wst.levels);
  EXPECT_NEAR(wst_levels.logic0, 60.0 + 7.5 * 1.6, 1e-9);

  // Mapped through them, the caption's 50 IRE lands at 50 IRE.
  const VBISampleMap map = make_vbi_sample_map(nabts, nabts_output_levels());
  EXPECT_NEAR(map.apply(140.0), 240.0 + 50.0 * 5.6, 1e-9);

  EXPECT_FALSE(
      vbi_levels_from_caption_levels(VBILineLevels{}, nabts_output_levels())
          .usable);
}

// Fixed mode measures nothing and applies the configured levels to every
// line, whatever the lines themselves contain.
TEST(VBILevelMapper, FixedModeAppliesTheConfiguredLevelsToEveryLine) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIOutputLevels output = pal_output_levels();

  VBILevelMapperConfig config;
  config.mode = VBILevelMode::kFixed;
  config.fixed_logic0 = 32.0;
  config.fixed_logic1 = 180.0;

  std::vector<std::vector<double>> lines;
  for (size_t index = 0; index < 4; ++index) {
    SyntheticLineSpec spec;
    spec.carries_teletext = (index % 2u) == 0u;
    lines.push_back(make_synthetic_line(format, service, spec));
  }

  VBILevelMapper mapper(format, service, config);
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 4u);
  for (const VBILineLevels& line : mapped) {
    EXPECT_TRUE(line.usable);
    EXPECT_DOUBLE_EQ(line.logic0, 32.0);
    EXPECT_DOUBLE_EQ(line.logic1, 180.0);
  }

  EXPECT_NEAR(mapped_logic1(format, service, lines[0], mapped[0], output),
              644.0, 2.0);
  // A blank line stays at blanking rather than being stretched to fill the
  // data amplitude.
  EXPECT_NEAR(mapped_logic1(format, service, lines[1], mapped[1], output),
              256.0, 2.0);
}

// The mapper holds configuration only, so the same frame maps identically
// whatever order frames are asked for — the property that lets frames be built
// lazily.
TEST(VBILevelMapper, MappingAFrameIsIndependentOfWhatWasMappedBefore) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  VBILevelMapperConfig config;
  config.mode = VBILevelMode::kRolling;
  VBILevelMapper mapper(format, service, config);

  SyntheticLineSpec quiet_spec;
  quiet_spec.carries_teletext = false;
  const std::vector<VBILineRecord> quiet_frame =
      make_frame_records(std::vector<std::vector<double>>(
          4, make_synthetic_line(format, service, quiet_spec)));
  const std::vector<VBILineRecord> data_frame =
      make_frame_records(frame_with_gain_outlier(format, service, 7, 188.0));

  std::vector<VBILineLevels> first;
  mapper.map_frame(data_frame, first);

  std::vector<VBILineLevels> discarded;
  mapper.map_frame(quiet_frame, discarded);

  std::vector<VBILineLevels> second;
  mapper.map_frame(data_frame, second);

  ASSERT_EQ(first.size(), second.size());
  for (size_t index = 0; index < first.size(); ++index) {
    EXPECT_DOUBLE_EQ(first[index].logic0, second[index].logic0)
        << "line " << index;
    EXPECT_DOUBLE_EQ(first[index].logic1, second[index].logic1)
        << "line " << index;
    EXPECT_EQ(first[index].usable, second[index].usable) << "line " << index;
  }
}

// A time-base corrected capture's samples are already in the output's
// amplitude domain, scaled by the ld-decode factor, so the whole mapping is a
// division by that factor and nothing is estimated.
TEST(VBILevelMapper, AbsoluteLevelsMapTheDecoderDomainStraightOntoTheOutput) {
  VBIOutputFrame output;
  std::string error;
  ASSERT_TRUE(make_vbi_output_frame(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                    output, error))
      << error;

  const VBILevelMapperConfig config = vbi_absolute_level_config(output.levels);
  EXPECT_EQ(config.mode, VBILevelMode::kFixed);
  EXPECT_DOUBLE_EQ(config.fixed_logic0, output.levels.logic0 * 64.0);
  EXPECT_DOUBLE_EQ(config.fixed_logic1, output.levels.logic1 * 64.0);

  VBILineLevels levels;
  levels.logic0 = config.fixed_logic0;
  levels.logic1 = config.fixed_logic1;
  levels.usable = true;
  const VBISampleMap map = make_vbi_sample_map(levels, output.levels);

  // Every level of the domain, not just the two the map was built from.
  for (const double counts :
       {0.0, 15360.0, 18048.0, 39936.0, 51200.0, 65535.0}) {
    EXPECT_NEAR(map.apply(counts), counts / kTBCSampleScale, 1e-9)
        << "source counts " << counts;
  }
}

}  // namespace
}  // namespace orc
