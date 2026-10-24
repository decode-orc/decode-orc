/*
 * File:        data24_to_raw_sector_sync_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for Data24ToRawSector sync acquisition when the sync
 *              pattern lies less than a raw sector from the end of the buffer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "dec_data24torawsector.h"
#include "efm_constants.h"
#include "section.h"

namespace {

constexpr int kBytesPerFrame = 24;

// The 12-byte sync pattern that opens every raw CD-ROM sector.
const std::vector<uint8_t> kSyncPattern{0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

// Far enough from the end of a section that the sync is found while fewer
// than kRawSectorSize bytes follow it.
constexpr int kLateSyncPosition = 2000;

// Builds one section's worth of Data24 frames from a flat byte stream. Every
// byte is flagged as both an error and padding when `damaged` is set.
Data24Section makeSection(const std::vector<uint8_t>& bytes, bool damaged) {
  Data24Section section;
  const std::vector<uint8_t> flags(kBytesPerFrame, damaged ? 1 : 0);
  for (int f = 0; f < efm::kFramesPerSection; ++f) {
    Data24 frame;
    frame.setData(
        std::vector<uint8_t>(bytes.begin() + f * kBytesPerFrame,
                             bytes.begin() + (f + 1) * kBytesPerFrame));
    frame.setErrorData(flags);
    frame.setPaddedData(flags);
    section.pushFrame(frame);
  }
  return section;
}

Data24Section sectionWithLateSync() {
  std::vector<uint8_t> bytes(efm::kRawSectorSize, 0x55);
  std::copy(kSyncPattern.begin(), kSyncPattern.end(),
            bytes.begin() + kLateSyncPosition);
  return makeSection(bytes, false);
}

Data24Section plainSection(bool damaged) {
  return makeSection(std::vector<uint8_t>(efm::kRawSectorSize, 0x55), damaged);
}

}  // namespace

// Issue 342: the false-positive check used to count error and padding flags
// over a full sector even when fewer bytes followed the sync, reading past the
// end of the flag buffers (a trap under hardened libc++). Here the bytes that
// really follow the sync are all damaged, so the sync must be rejected; the
// out-of-bounds read saw the clean stale bytes instead and accepted it.
TEST(Data24ToRawSectorSync, LateSyncIsJudgedOnTheBytesThatFollowIt) {
  Data24ToRawSector decoder;

  decoder.pushSection(sectionWithLateSync());
  decoder.pushSection(plainSection(true));
  decoder.pushSection(plainSection(false));

  EXPECT_FALSE(decoder.isReady());
}

// A clean sector whose sync arrives late in one section is still acquired once
// the next section completes it.
TEST(Data24ToRawSectorSync, LateSyncIsAcquiredWhenTheSectorCompletes) {
  Data24ToRawSector decoder;

  decoder.pushSection(sectionWithLateSync());
  decoder.pushSection(plainSection(false));
  EXPECT_FALSE(decoder.isReady());

  decoder.pushSection(plainSection(false));
  ASSERT_TRUE(decoder.isReady());

  EXPECT_EQ(decoder.popSector().size(),
            static_cast<uint32_t>(efm::kRawSectorSize));
}
