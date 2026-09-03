/*
 * File:        efm_tvalue_packing_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the packed EFM t-value byte helpers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/video_frame_representation.h>

#include <cstdint>

using namespace orc;

// CVBS EFM extension format: the t-value occupies bits 3-0 and the producer's
// doubt bits 7-4.

TEST(EFMTValuePacking, ExtractsTValueFromLowNibble) {
  EXPECT_EQ(efm_tvalue(0x03), 3);
  EXPECT_EQ(efm_tvalue(0xF3), 3);
  EXPECT_EQ(efm_tvalue(0x8B), 11);
}

TEST(EFMTValuePacking, ExtractsDoubtFromHighNibble) {
  EXPECT_EQ(efm_doubt(0x03), 0);
  EXPECT_EQ(efm_doubt(0xF3), kEfmDoubtMax);
  EXPECT_EQ(efm_doubt(0x8B), 8);
}

// The doubt (rather than confidence) sense is what makes the format backward
// compatible: a fully trusted t-value packs to its plain value, so a producer
// with nothing to doubt emits the raw t-value byte stream.
TEST(EFMTValuePacking, ZeroDoubtPacksToThePlainTValue) {
  for (uint8_t t = 3; t <= 11; ++t) {
    EXPECT_EQ(efm_pack(t, 0), t);
  }
}

TEST(EFMTValuePacking, PackRoundTripsBothFields) {
  for (uint8_t t = 0; t <= 15; ++t) {
    for (uint8_t doubt = 0; doubt <= 15; ++doubt) {
      const uint8_t packed = efm_pack(t, doubt);
      EXPECT_EQ(efm_tvalue(packed), t);
      EXPECT_EQ(efm_doubt(packed), doubt);
    }
  }
}

// Every byte value is a legal packed t-value, so a consumer must never reject a
// stream on the raw byte.
TEST(EFMTValuePacking, EveryByteValueUnpacksToFourBitFields) {
  for (int value = 0; value <= 255; ++value) {
    const auto packed = static_cast<uint8_t>(value);
    EXPECT_LE(efm_tvalue(packed), 15);
    EXPECT_LE(efm_doubt(packed), 15);
    EXPECT_EQ(efm_pack(efm_tvalue(packed), efm_doubt(packed)), packed);
  }
}
