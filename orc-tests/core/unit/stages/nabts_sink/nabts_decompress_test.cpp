/*
 * File:        nabts_decompress_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the decompressors a framed datacast service's
 *              containers need: zlib's inflate and PKWARE's explode
 *
 * Covers: zlib streams of a stored block, of the fixed codes and of codes of
 * their own; an Adler-32 that fails; a stream broken off; a header that is
 * no zlib stream's; the limit on what is given; the PKWARE documentation's
 * imploded example, whole and broken off; and the CRC-32 check value. The
 * zlib streams are those zlib's own compress gives. Deterministic, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_decompress.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

std::string text_of(const std::vector<uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// "stored bytes", stored.
const std::vector<uint8_t> kStored = {
    0x78, 0x01, 0x01, 0x0C, 0x00, 0xF3, 0xFF, 0x73, 0x74, 0x6F, 0x72, 0x65,
    0x64, 0x20, 0x62, 0x79, 0x74, 0x65, 0x73, 0x1F, 0xCF, 0x04, 0xD9};
// "hello hello hello hello, framed world" by the fixed codes.
const std::vector<uint8_t> kFixed = {
    0x78, 0xDA, 0xCB, 0x48, 0xCD, 0xC9, 0xC9, 0x57, 0xC8, 0x40,
    0x27, 0x75, 0x14, 0xD2, 0x8A, 0x12, 0x73, 0x53, 0x53, 0x14,
    0xCA, 0xF3, 0x8B, 0x72, 0x52, 0x00, 0x04, 0x5A, 0x0D, 0xB4};
// kDynamicText by codes of the block's own.
const std::vector<uint8_t> kDynamic = {
    0x78, 0xDA, 0x0D, 0xCB, 0xB1, 0x11, 0x80, 0x30, 0x0C, 0x03, 0xC0, 0x55,
    0xB4, 0x9A, 0x08, 0x22, 0x71, 0x81, 0x0F, 0x8C, 0x49, 0x91, 0xE9, 0xE3,
    0xFA, 0xEF, 0x73, 0x08, 0x4B, 0x47, 0x10, 0xEC, 0x34, 0xC7, 0xA7, 0x98,
    0xD6, 0x84, 0x2B, 0x78, 0x0B, 0xEF, 0xCF, 0xC8, 0x05, 0xFA, 0x59, 0xE0,
    0x89, 0x87, 0x5D, 0xC8, 0x2A, 0x1B, 0x39, 0x90, 0x14, 0x04};
const char* const kDynamicText =
    "the zebra again service frame quartz and sent page the ";
// The example of imploded data PKWARE's documentation gives.
const std::vector<uint8_t> kImploded = {0x00, 0x04, 0x82, 0x24,
                                        0x25, 0x8F, 0x80, 0x7F};

TEST(NabtsDecompress, InflatesAStoredBlock) {
  const auto out = orc::nabts_inflate(kStored.data(), kStored.size(), 1 << 20);
  EXPECT_TRUE(out.complete);
  EXPECT_EQ(text_of(out.bytes), "stored bytes");
  EXPECT_EQ(out.used, kStored.size());
}

TEST(NabtsDecompress, InflatesTheFixedCodes) {
  const auto out = orc::nabts_inflate(kFixed.data(), kFixed.size(), 1 << 20);
  EXPECT_TRUE(out.complete);
  EXPECT_EQ(text_of(out.bytes), "hello hello hello hello, framed world");
}

TEST(NabtsDecompress, InflatesCodesOfTheBlocksOwn) {
  const auto out =
      orc::nabts_inflate(kDynamic.data(), kDynamic.size(), 1 << 20);
  EXPECT_TRUE(out.complete);
  EXPECT_EQ(text_of(out.bytes), kDynamicText);
}

TEST(NabtsDecompress, SaysWhenTheAdlerFails) {
  std::vector<uint8_t> wrong = kDynamic;
  wrong.back() ^= 0x01;
  const auto out = orc::nabts_inflate(wrong.data(), wrong.size(), 1 << 20);
  EXPECT_TRUE(out.ended);
  EXPECT_FALSE(out.complete);
  EXPECT_EQ(text_of(out.bytes), kDynamicText);
}

TEST(NabtsDecompress, GivesWhatABrokenStreamGaveBeforeItBroke) {
  const auto out =
      orc::nabts_inflate(kDynamic.data(), kDynamic.size() - 20, 1 << 20);
  EXPECT_FALSE(out.ended);
  EXPECT_FALSE(out.complete);
  EXPECT_EQ(std::string(kDynamicText).rfind(text_of(out.bytes), 0), 0u);
}

TEST(NabtsDecompress, ReadsNoStreamWithoutZlibsHeader) {
  std::vector<uint8_t> other = kFixed;
  other[0] = 0x79;
  const auto out = orc::nabts_inflate(other.data(), other.size(), 1 << 20);
  EXPECT_TRUE(out.bytes.empty());
  EXPECT_FALSE(out.ended);
}

TEST(NabtsDecompress, GivesNoMoreThanItsLimit) {
  const auto out = orc::nabts_inflate(kFixed.data(), kFixed.size(), 10);
  EXPECT_LE(out.bytes.size(), 10u);
  EXPECT_FALSE(out.ended);
}

TEST(NabtsDecompress, ExplodesTheDocumentationsExample) {
  const auto out =
      orc::nabts_explode(kImploded.data(), kImploded.size(), 1 << 20);
  EXPECT_TRUE(out.ended);
  EXPECT_TRUE(out.complete);
  EXPECT_EQ(text_of(out.bytes), "AIAIAIAIAIAIA");
}

TEST(NabtsDecompress, ExplodesAsFarAsABrokenStreamGoes) {
  const auto out = orc::nabts_explode(kImploded.data(), 5, 1 << 20);
  EXPECT_FALSE(out.ended);
  EXPECT_EQ(std::string("AIAIAIAIAIAIA").rfind(text_of(out.bytes), 0), 0u);
}

TEST(NabtsDecompress, GivesTheCrc32CheckValue) {
  const std::string check = "123456789";
  EXPECT_EQ(orc::nabts_crc32(reinterpret_cast<const uint8_t*>(check.data()),
                             check.size()),
            0xCBF43926U);
}

}  // namespace
}  // namespace orc_unit_test
