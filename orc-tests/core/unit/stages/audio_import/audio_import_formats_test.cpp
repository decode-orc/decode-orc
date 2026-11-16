/*
 * File:        audio_import_formats_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the Audio Import stage's container detection,
 *              FLAC stream rules and decoded-sample conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/audio_import/audio_import_formats.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::AudioImportContainer;
using orc::DecodedSampleLayout;

AudioImportContainer detect(const std::string& head) {
  return orc::detect_audio_import_container(
      reinterpret_cast<const uint8_t*>(head.data()), head.size());
}

template <typename T>
std::vector<uint8_t> as_bytes(const std::vector<T>& values) {
  std::vector<uint8_t> bytes(values.size() * sizeof(T));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

// --- Container detection ---------------------------------------------------

TEST(AudioImportFormats, RiffRf64AndBw64HeadsAreWav) {
  EXPECT_EQ(detect("RIFF"), AudioImportContainer::kWav);
  EXPECT_EQ(detect("RF64"), AudioImportContainer::kWav);
  EXPECT_EQ(detect("BW64"), AudioImportContainer::kWav);
}

TEST(AudioImportFormats, FlacMarkerHeadIsFlac) {
  EXPECT_EQ(detect("fLaC"), AudioImportContainer::kFlac);
}

TEST(AudioImportFormats, Id3TaggedHeadIsTakenForFlac) {
  EXPECT_EQ(detect(std::string("ID3\x04", 4)), AudioImportContainer::kFlac);
}

TEST(AudioImportFormats, OtherHeadsAreUnknown) {
  EXPECT_EQ(detect("OggS"), AudioImportContainer::kUnknown);
  EXPECT_EQ(detect("riff"), AudioImportContainer::kUnknown);
  EXPECT_EQ(detect("fLa"), AudioImportContainer::kUnknown);  // too short
  EXPECT_EQ(detect(""), AudioImportContainer::kUnknown);
  EXPECT_EQ(orc::detect_audio_import_container(nullptr, 4),
            AudioImportContainer::kUnknown);
}

// --- FLAC stream rules -----------------------------------------------------

TEST(AudioImportFormats, StereoFlacUpTo24BitsIsImportable) {
  EXPECT_EQ(orc::flac_stream_problem(2, 16, 48000), "");
  EXPECT_EQ(orc::flac_stream_problem(2, 24, 44100), "");
  EXPECT_EQ(orc::flac_stream_problem(2, 8, 32000), "");
  EXPECT_EQ(orc::flac_stream_problem(2, 20, 96000), "");
}

TEST(AudioImportFormats, NonStereoFlacIsRefused) {
  EXPECT_EQ(orc::flac_stream_problem(1, 16, 48000),
            "not stereo (1 channel(s))");
  EXPECT_EQ(orc::flac_stream_problem(6, 24, 48000),
            "not stereo (6 channel(s))");
}

TEST(AudioImportFormats, FlacWiderThan24BitsIsRefused) {
  EXPECT_EQ(orc::flac_stream_problem(2, 32, 48000),
            "not 24-bit or narrower (32 bits/sample)");
  EXPECT_EQ(orc::flac_stream_problem(2, 0, 48000),
            "not 24-bit or narrower (0 bits/sample)");
}

TEST(AudioImportFormats, FlacWithoutASampleRateIsRefused) {
  EXPECT_EQ(orc::flac_stream_problem(2, 16, 0), "invalid sample rate (0 Hz)");
}

// --- Decoded-sample conversion ---------------------------------------------

TEST(AudioImportFormats, InterleavedS16IsWidenedToTheCarrier) {
  const auto bytes =
      as_bytes(std::vector<int16_t>{0, 1, -1, 32767, -32768, 1234});
  const uint8_t* planes[] = {bytes.data()};
  std::vector<int32_t> out;
  orc::append_decoded_pairs(DecodedSampleLayout::kS16Interleaved, planes, 3,
                            out);
  EXPECT_EQ(out, (std::vector<int32_t>{0, 256, -256, 32767 * 256, -32768 * 256,
                                       1234 * 256}));
}

TEST(AudioImportFormats, PlanarS16IsInterleaved) {
  const auto left = as_bytes(std::vector<int16_t>{1, 2});
  const auto right = as_bytes(std::vector<int16_t>{-1, -2});
  const uint8_t* planes[] = {left.data(), right.data()};
  std::vector<int32_t> out;
  orc::append_decoded_pairs(DecodedSampleLayout::kS16Planar, planes, 2, out);
  EXPECT_EQ(out, (std::vector<int32_t>{256, -256, 512, -512}));
}

TEST(AudioImportFormats, LeftJustified24BitS32IsExact) {
  // The decoder hands a 24-bit sample over in the top 24 bits of a word.
  const std::vector<int32_t> samples24 = {0, 1, -1, 8388607, -8388608, -4242};
  std::vector<int32_t> words;
  for (const int32_t v : samples24) {
    words.push_back(static_cast<int32_t>(static_cast<uint32_t>(v) << 8));
  }
  const auto bytes = as_bytes(words);
  const uint8_t* planes[] = {bytes.data()};
  std::vector<int32_t> out;
  orc::append_decoded_pairs(DecodedSampleLayout::kS32Interleaved, planes, 3,
                            out);
  EXPECT_EQ(out, samples24);
}

TEST(AudioImportFormats, LeftJustified20BitS32LandsOnThe24BitScale) {
  // A 20-bit sample sits in the top 20 bits: it reaches the carrier as the
  // same level on the 24-bit scale, exactly (value << 4).
  const std::vector<int32_t> samples20 = {524287, -524288};
  std::vector<int32_t> left;
  std::vector<int32_t> right;
  left.push_back(
      static_cast<int32_t>(static_cast<uint32_t>(samples20[0]) << 12));
  right.push_back(
      static_cast<int32_t>(static_cast<uint32_t>(samples20[1]) << 12));
  const auto left_bytes = as_bytes(left);
  const auto right_bytes = as_bytes(right);
  const uint8_t* planes[] = {left_bytes.data(), right_bytes.data()};
  std::vector<int32_t> out;
  orc::append_decoded_pairs(DecodedSampleLayout::kS32Planar, planes, 1, out);
  EXPECT_EQ(out, (std::vector<int32_t>{524287 * 16, -524288 * 16}));
}

TEST(AudioImportFormats, ConversionAppendsToExistingSamples) {
  const auto bytes = as_bytes(std::vector<int16_t>{1, 2});
  const uint8_t* planes[] = {bytes.data()};
  std::vector<int32_t> out = {7, 8};
  orc::append_decoded_pairs(DecodedSampleLayout::kS16Interleaved, planes, 1,
                            out);
  EXPECT_EQ(out, (std::vector<int32_t>{7, 8, 256, 512}));
}

TEST(AudioImportFormats, NoPlanesOrNoPairsAppendNothing) {
  std::vector<int32_t> out;
  orc::append_decoded_pairs(DecodedSampleLayout::kS16Interleaved, nullptr, 4,
                            out);
  const int16_t sample = 1;
  const uint8_t* planes[] = {reinterpret_cast<const uint8_t*>(&sample)};
  orc::append_decoded_pairs(DecodedSampleLayout::kS16Interleaved, planes, 0,
                            out);
  EXPECT_TRUE(out.empty());
}

}  // namespace
}  // namespace orc_unit_test
