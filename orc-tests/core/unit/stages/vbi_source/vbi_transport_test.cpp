/*
 * File:        vbi_transport_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for raw VBI capture transport detection and the
 *              video transport's luma conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_transport.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace orc {
namespace {

TEST(VBITransport, FlacStreamMarkerIsDetectedAtOffsetZero) {
  const std::vector<uint8_t> flac_header = {'f',  'L',  'a',  'C',
                                            0x00, 0x00, 0x00, 0x22};

  EXPECT_TRUE(has_flac_stream_marker(flac_header.data(), flac_header.size()));
}

TEST(VBITransport, RawCaptureBytesAreNotMistakenForFlac) {
  // A bt8x8 record opens with back porch samples, not a magic number.
  const std::vector<uint8_t> raw_header = {0x80, 0x7F, 0x81, 0x80,
                                           0x7E, 0x82, 0x80, 0x80};

  EXPECT_FALSE(has_flac_stream_marker(raw_header.data(), raw_header.size()));
}

TEST(VBITransport, TruncatedOrAbsentHeaderIsNotFlac) {
  const std::vector<uint8_t> truncated = {'f', 'L', 'a'};

  EXPECT_FALSE(has_flac_stream_marker(truncated.data(), truncated.size()));
  EXPECT_FALSE(has_flac_stream_marker(nullptr, 0));
}

// The marker only counts at offset 0; the same bytes occurring inside sample
// data must not trigger unwrapping.
TEST(VBITransport, MarkerMustBeTheFirstBytesOfTheStream) {
  const std::vector<uint8_t> shifted = {0x80, 'f', 'L', 'a', 'C', 0x00};

  EXPECT_FALSE(has_flac_stream_marker(shifted.data(), shifted.size()));
}

TEST(VBITransport, MatroskaMarkerIsDetectedAtOffsetZero) {
  // The EBML header element of a Matroska file, then its size.
  const std::vector<uint8_t> matroska_header = {0x1A, 0x45, 0xDF, 0xA3,
                                                0x01, 0x00, 0x00, 0x00};

  EXPECT_TRUE(has_matroska_stream_marker(matroska_header.data(),
                                         matroska_header.size()));
  EXPECT_FALSE(
      has_flac_stream_marker(matroska_header.data(), matroska_header.size()));
}

// A .tbc crop opens on the sync tip, 0x0400 little-endian, and a card dump on
// blanking; neither may be taken for a video file.
TEST(VBITransport, RawCapturesAreNotMistakenForMatroska) {
  const std::vector<uint8_t> tbc_crop_head = {0x00, 0x04, 0x02, 0x04,
                                              0xFE, 0x03, 0x00, 0x04};
  const std::vector<uint8_t> card_dump_head = {0x80, 0x7F, 0x81, 0x80};
  const std::vector<uint8_t> flac_head = {'f', 'L', 'a', 'C'};

  EXPECT_FALSE(
      has_matroska_stream_marker(tbc_crop_head.data(), tbc_crop_head.size()));
  EXPECT_FALSE(
      has_matroska_stream_marker(card_dump_head.data(), card_dump_head.size()));
  EXPECT_FALSE(has_matroska_stream_marker(flac_head.data(), flac_head.size()));
}

TEST(VBITransport, TruncatedOrShiftedMatroskaMarkerIsNotDetected) {
  const std::vector<uint8_t> truncated = {0x1A, 0x45, 0xDF};
  const std::vector<uint8_t> shifted = {0x00, 0x1A, 0x45, 0xDF, 0xA3};

  EXPECT_FALSE(has_matroska_stream_marker(truncated.data(), truncated.size()));
  EXPECT_FALSE(has_matroska_stream_marker(shifted.data(), shifted.size()));
  EXPECT_FALSE(has_matroska_stream_marker(nullptr, 0));
}

// FFmpeg makes limited-range 8-bit luma of a 16-bit grey word w as
// 16 + 219 w / 65 280, so 16 and 235 are the words 0 and 65 280, and each step
// between them is 65 280 / 219 words.
TEST(VBITransport, LimitedRangeEightBitLumaComesBackOnTheCaptureScale) {
  const VBIVideoLumaCoding coding{8, false};

  EXPECT_EQ(vbi_sample_word_from_luma(16, coding), 0);
  EXPECT_EQ(vbi_sample_word_from_luma(235, coding), 65280);
  EXPECT_EQ(vbi_sample_word_from_luma(17, coding), 298);  // 298,08
  EXPECT_EQ(vbi_sample_word_from_luma(126, coding), 32789);
}

// The levels a .tbc crop writes, at the luma a vhs-decode vbi.mkv holds
// them at: blanking (word 15 360) at 67,5 and 50 IRE (33 280) at 127,7, so the
// nearest luma comes back within half a step, 149 words, of each.
TEST(VBITransport, TheCropsBlankingAndCaptionLevelsSurviveTheRoundTrip) {
  const VBIVideoLumaCoding coding{8, false};
  constexpr int kHalfStep = 149;

  EXPECT_NEAR(vbi_sample_word_from_luma(68, coding), 15360, kHalfStep);
  EXPECT_NEAR(vbi_sample_word_from_luma(128, coding), 33280, kHalfStep);
}

// Below black and above white the luma has no word to stand for, and the
// conversion holds the word's range rather than wrapping.
TEST(VBITransport, LumaOutsideTheLimitedRangeIsClampedToTheWord) {
  const VBIVideoLumaCoding coding{8, false};

  EXPECT_EQ(vbi_sample_word_from_luma(0, coding), 0);
  EXPECT_EQ(vbi_sample_word_from_luma(15, coding), 0);
  EXPECT_EQ(vbi_sample_word_from_luma(255, coding), 65535);
}

TEST(VBITransport, FullRangeLumaSpansTheWholeWord) {
  const VBIVideoLumaCoding eight_bit{8, true};
  const VBIVideoLumaCoding sixteen_bit{16, true};

  EXPECT_EQ(vbi_sample_word_from_luma(0, eight_bit), 0);
  EXPECT_EQ(vbi_sample_word_from_luma(255, eight_bit), 65535);
  EXPECT_EQ(vbi_sample_word_from_luma(60, eight_bit), 15420);  // 60 x 257

  // Sixteen-bit grey is the crop's own word.
  EXPECT_EQ(vbi_sample_word_from_luma(15360, sixteen_bit), 15360);
  EXPECT_EQ(vbi_sample_word_from_luma(65535, sixteen_bit), 65535);
}

// Each bit of depth above eight doubles black and the span alike, so a 10-bit
// limited-range picture puts black at 64 and white at 940.
TEST(VBITransport, LimitedRangeScalesWithTheBitDepth) {
  const VBIVideoLumaCoding ten_bit{10, false};

  EXPECT_EQ(vbi_sample_word_from_luma(64, ten_bit), 0);
  EXPECT_EQ(vbi_sample_word_from_luma(940, ten_bit), 65280);
}

}  // namespace
}  // namespace orc
