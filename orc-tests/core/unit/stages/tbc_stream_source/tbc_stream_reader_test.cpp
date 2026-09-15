/*
 * File:        tbc_stream_reader_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for TBCStreamReader — the sequential, bounded
 *              ring-buffer field-pair reader behind tbc_stream_source
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../../../../../orc/plugins/stages/tbc_stream_source/ntsc_tbc_converter.h"
#include "../../../../../orc/plugins/stages/tbc_stream_source/pal_tbc_converter.h"
#include "../../../../../orc/plugins/stages/tbc_stream_source/tbc_stream_source_stage.h"

namespace orc_unit_test {
namespace {

// Realistic ld-decode 16-bit domain calibration levels (matching an actual
// capture's .tbc.json black16bIre/white16bIre) — the level-derivation math
// for NTSC/PAL_M classifies black/blanking by comparing against ld-decode's
// own nominal 16-bit constants, so small toy numbers would misclassify as
// NTSC-J. These are the values from a real NTSC capture used throughout this
// session's own manual testing.
constexpr int32_t kBlack16bIre = 18048;
constexpr int32_t kWhite16bIre = 51200;

// Mirrors the non-NTSC-J branch of derive_blanking_16b() in
// tbc_stream_source_stage.cpp (file-local there) so a test can compute the
// exact same tbc_blanking the reader derives internally, to predict its
// mapped output. kBlack16bIre/kWhite16bIre above are confirmed (by this same
// math) not to classify as NTSC-J, so the NTSC-J branch is never exercised
// and is not reproduced here. PAL needs no derivation at all (black IS
// blanking), which is why only the NTSC path is duplicated.
constexpr double kNtscSetupIre = 7.5;

int32_t derive_ntsc_blanking_16b(int32_t black_16b_ire, int32_t white_16b_ire) {
  const double units_per_ire =
      static_cast<double>(white_16b_ire - black_16b_ire) /
      (100.0 - kNtscSetupIre);
  return static_cast<int32_t>(
      std::round(black_16b_ire - kNtscSetupIre * units_per_ire));
}

// NTSC field geometry (must match tbc_field_geometry() for VideoSystem::NTSC
// in tbc_stream_source_stage.cpp — duplicated here only as plain constants
// since that function is file-local).
constexpr size_t kNtscFieldLines = 263;
constexpr size_t kNtscUsedField2Lines = 262;
constexpr size_t kNtscSamplesPerLine = 910;
constexpr size_t kNtscFieldWords = kNtscFieldLines * kNtscSamplesPerLine;
constexpr size_t kNtscFrameSamplesExpected = 477'750;

constexpr size_t kPalFieldLines = 313;
constexpr size_t kPalSamplesPerLine = 1135;
constexpr size_t kPalFieldWords = kPalFieldLines * kPalSamplesPerLine;
constexpr size_t kPalFrameSamplesExpected = 709'379;

// Builds `frame_count` NTSC frames' worth of raw wire bytes. Each frame is
// two field slots of kNtscFieldWords words each (field 2's slot is stored
// at the same size as field 1's, per ld-decode's own padding convention —
// see the class comment in tbc_stream_source_stage.h). field1_value fills
// every word of field 1; field2_value fills every REAL word of field 2
// (the first kNtscUsedField2Lines lines); padding_value fills field 2's
// last, discarded line, so a test can confirm it never reaches the output.
std::string make_ntsc_tbc_stream(size_t frame_count, uint16_t field1_value,
                                 uint16_t field2_value,
                                 uint16_t padding_value) {
  std::string bytes;
  bytes.resize(frame_count * 2 * kNtscFieldWords * 2);
  auto* words = reinterpret_cast<uint16_t*>(bytes.data());
  size_t idx = 0;
  for (size_t f = 0; f < frame_count; ++f) {
    for (size_t i = 0; i < kNtscFieldWords; ++i) words[idx++] = field1_value;
    const size_t used_words = kNtscUsedField2Lines * kNtscSamplesPerLine;
    for (size_t i = 0; i < used_words; ++i) words[idx++] = field2_value;
    for (size_t i = used_words; i < kNtscFieldWords; ++i) {
      words[idx++] = padding_value;
    }
  }
  return bytes;
}

std::string make_pal_tbc_stream(size_t frame_count, uint16_t value) {
  std::string bytes;
  bytes.resize(frame_count * 2 * kPalFieldWords * 2);
  auto* words = reinterpret_cast<uint16_t*>(bytes.data());
  for (size_t i = 0; i < frame_count * 2 * kPalFieldWords; ++i) {
    words[i] = value;
  }
  return bytes;
}

}  // namespace

TEST(TBCStreamReaderTest, ReadsNtscFrame_FieldOrderingAndPaddingStripped) {
  constexpr size_t kFrameCount = 2;
  constexpr uint16_t kField1Value = 20000;
  constexpr uint16_t kField2Value = 25000;
  constexpr uint16_t kPaddingValue = 60000;  // must never reach the output

  std::istringstream input(make_ntsc_tbc_stream(kFrameCount, kField1Value,
                                                kField2Value, kPaddingValue),
                           std::ios::binary);

  orc::TBCStreamReader reader(input, orc::VideoSystem::NTSC, kFrameCount,
                              /*buffer_frames=*/kFrameCount, kBlack16bIre,
                              kWhite16bIre);

  const int32_t tbc_blanking =
      derive_ntsc_blanking_16b(kBlack16bIre, kWhite16bIre);
  const orc::TbcLevelScale scale =
      orc::NtscTBCConverter::level_scale(tbc_blanking, kWhite16bIre);
  const int16_t expected_field1 = scale.map(kField1Value);
  const int16_t expected_field2 = scale.map(kField2Value);
  const int16_t forbidden_padding = scale.map(kPaddingValue);

  for (orc::FrameID id = 0; id < static_cast<orc::FrameID>(kFrameCount); ++id) {
    const int16_t* frame = reader.get_frame(id);
    ASSERT_NE(frame, nullptr) << "frame " << id;

    // VFR field 1 (top, 263 lines) is sourced entirely from TBC field 1.
    const size_t field1_samples = kNtscFieldLines * kNtscSamplesPerLine;
    for (size_t s = 0; s < field1_samples; ++s) {
      ASSERT_EQ(frame[s], expected_field1)
          << "frame " << id << " field1 sample " << s;
    }
    // VFR field 2 (bottom, 262 lines) is sourced from TBC field 2's real
    // lines only — the padding value must never appear anywhere.
    const size_t field2_samples = kNtscUsedField2Lines * kNtscSamplesPerLine;
    for (size_t s = 0; s < field2_samples; ++s) {
      const int16_t sample = frame[field1_samples + s];
      ASSERT_EQ(sample, expected_field2)
          << "frame " << id << " field2 sample " << s;
      ASSERT_NE(sample, forbidden_padding)
          << "padding leaked into frame " << id << " sample " << s;
    }
    EXPECT_EQ(field1_samples + field2_samples, kNtscFrameSamplesExpected);
  }
  EXPECT_FALSE(reader.failed());
}

TEST(TBCStreamReaderTest, AssemblesPalFrame_WithoutThrowing) {
  constexpr size_t kFrameCount = 1;
  std::istringstream input(make_pal_tbc_stream(kFrameCount, 30000),
                           std::ios::binary);

  orc::TBCStreamReader reader(input, orc::VideoSystem::PAL, kFrameCount,
                              /*buffer_frames=*/kFrameCount, kBlack16bIre,
                              kWhite16bIre);

  const int16_t* frame = reader.get_frame(0);
  ASSERT_NE(frame, nullptr);
  EXPECT_FALSE(reader.failed());
  // PAL needs no setup-pedestal derivation: black IS blanking (0 IRE).
  const orc::TbcLevelScale scale =
      orc::PalTBCConverter::level_scale(kBlack16bIre, kWhite16bIre);
  const int16_t expected = scale.map(30000);
  // Spot-check a handful of non-bridge-sample positions rather than the
  // whole 709,379-sample frame; the bridge samples (2 per field boundary,
  // see PalTBCConverter) are deliberately not asserted here since a uniform
  // input makes them equal the same mapped value anyway by construction.
  EXPECT_EQ(frame[0], expected);
  EXPECT_EQ(frame[1000], expected);
  EXPECT_EQ(frame[kPalFrameSamplesExpected - 1], expected);
}

TEST(TBCStreamReaderTest, OutOfDeclaredRange_ReturnsNullptr) {
  constexpr size_t kFrameCount = 1;
  std::istringstream input(
      make_ntsc_tbc_stream(kFrameCount, 20000, 25000, 60000), std::ios::binary);

  orc::TBCStreamReader reader(input, orc::VideoSystem::NTSC, kFrameCount,
                              /*buffer_frames=*/kFrameCount, kBlack16bIre,
                              kWhite16bIre);

  EXPECT_EQ(reader.get_frame(static_cast<orc::FrameID>(kFrameCount)), nullptr);
}

TEST(TBCStreamReaderTest, ShortInput_FailsWithClearError) {
  // Only 1 frame's worth of data for a stream declaring 2.
  std::istringstream input(make_ntsc_tbc_stream(1, 20000, 25000, 60000),
                           std::ios::binary);

  orc::TBCStreamReader reader(input, orc::VideoSystem::NTSC,
                              /*frame_count=*/2, /*buffer_frames=*/2,
                              kBlack16bIre, kWhite16bIre);

  EXPECT_EQ(reader.get_frame(1), nullptr);
  EXPECT_TRUE(reader.failed());
  EXPECT_NE(reader.last_error().find("unexpected end of input"),
            std::string::npos);
}

TEST(TBCStreamReaderTest, RequestingAnEvictedFrame_FailsLoudly) {
  constexpr size_t kFrameCount = 5;
  constexpr size_t kBufferFrames = 1;
  std::istringstream input(
      make_ntsc_tbc_stream(kFrameCount, 20000, 25000, 60000), std::ios::binary);

  orc::TBCStreamReader reader(input, orc::VideoSystem::NTSC, kFrameCount,
                              kBufferFrames, kBlack16bIre, kWhite16bIre);

  // Advance the reader well past the buffer window by requesting a late
  // frame first, then go back for an early one that must have scrolled out.
  ASSERT_NE(reader.get_frame(4), nullptr);
  EXPECT_EQ(reader.get_frame(0), nullptr);
  EXPECT_TRUE(reader.failed());
  EXPECT_NE(reader.last_error().find("scrolled out"), std::string::npos);
}

// Regression coverage for the same read-ahead-throttle bug fixed in
// CVBSStreamReader (see cvbs_stream_reader_test.cpp's own version of this
// test) — TBCStreamReader shares the fix via orc::pipe_io::ThrottledRingReader,
// but the wiring here (reading two field slots per produce() call) is new
// and worth confirming directly.
TEST(TBCStreamReaderTest, IdleStartupDelay_DoesNotEvictEarlyFrames) {
  constexpr size_t kFrameCount = 8;
  constexpr size_t kBufferFrames = 2;
  std::istringstream input(
      make_ntsc_tbc_stream(kFrameCount, 20000, 25000, 60000), std::ios::binary);

  orc::TBCStreamReader reader(input, orc::VideoSystem::NTSC, kFrameCount,
                              kBufferFrames, kBlack16bIre, kWhite16bIre);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const int16_t* frame = reader.get_frame(0);
  ASSERT_NE(frame, nullptr);
  EXPECT_FALSE(reader.failed());
}

}  // namespace orc_unit_test
