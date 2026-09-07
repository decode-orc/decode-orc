/*
 * File:        sector_address_integrity_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for R-5 sector address integrity: an unverifiable
 *              header address must be reconstructed from the Q-channel
 *              timeline rather than believed, and a gap must be filled only
 *              when the Q timeline corroborates that data is actually missing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "dec_sectorcorrection.h"
#include "sector.h"

namespace {

// The header-to-Q offset these fixtures use. Non-zero on purpose: on a real
// LV-ROM the sector address and the Q-channel absolute time do not coincide
// (Domesday DD86-DS4 starts its Q timeline at 00:02:00 and its first sector at
// 00:41:73), so the correction stage must learn the offset rather than assume
// the two are equal. It is negative so the fixtures start at sector address 0
// and no leading fill is generated to shift the emitted indices.
constexpr int32_t kQBase = 3000;
constexpr int32_t kAddressOffset = -kQBase;

Sector makeSector(int32_t address, int32_t qFrames, bool addressTrusted) {
  Sector sector;
  sector.setAddress(SectorAddress(address));
  sector.setMode(1);
  sector.dataValid(addressTrusted);
  sector.setQSectionFrames(qFrames);
  sector.addressTrusted(addressTrusted);
  sector.pushData(std::vector<uint8_t>(2048, 0));
  sector.pushErrorData(std::vector<uint8_t>(2048, 0));
  return sector;
}

// A sector whose header EDC passed: its address is verified.
Sector trustedSector(int32_t qFrames) {
  return makeSector(qFrames + kAddressOffset, qFrames, true);
}

// A sector whose EDC failed: the address is best-effort and must not be
// believed on its own.
Sector untrustedSector(int32_t headerAddress, int32_t qFrames) {
  return makeSector(headerAddress, qFrames, false);
}

std::vector<Sector> drain(SectorCorrection& correction) {
  std::vector<Sector> out;
  while (correction.isReady()) out.push_back(correction.popSector());
  return out;
}

std::vector<Sector> run(SectorCorrection& correction,
                        const std::vector<Sector>& sectors) {
  std::vector<Sector> emitted;
  for (const Sector& sector : sectors) {
    correction.pushSector(sector);
    for (const Sector& out : drain(correction)) emitted.push_back(out);
  }
  // The real pipeline flushes at end of stream to release the opening anchor
  // window; the fixtures must too or a short capture never emits.
  correction.flush();
  for (const Sector& out : drain(correction)) emitted.push_back(out);
  return emitted;
}

// The opening window deliberately votes on the header-to-Q offset before
// placing anything, so a test of the steady-state behaviour must get past it
// first. Everything these fixtures are actually about happens after this run
// of clean, verified sectors.
constexpr int32_t kSettleSectors = 20;

void appendClean(std::vector<Sector>& input, int32_t firstQ, int32_t count) {
  for (int32_t q = firstQ; q < firstQ + count; ++q) {
    input.push_back(trustedSector(q));
  }
}

// The exact failure from the Domesday DD86-DS4 decode: one EDC-failed sector
// whose header claimed an address 145051 sectors ahead. Believing it shifted
// every subsequent sector in the image.
TEST(SectorAddressIntegrity, CorruptHeaderOnUntrustedSectorIsRepaired) {
  SectorCorrection correction;

  std::vector<Sector> input;
  appendClean(input, kQBase, kSettleSectors);
  input.push_back(untrustedSector(145051, kQBase + kSettleSectors));
  appendClean(input, kQBase + kSettleSectors + 1, 10);

  const std::vector<Sector> emitted = run(correction, input);

  EXPECT_EQ(correction.repairedAddresses(), 1u);
  EXPECT_EQ(correction.addressDiscontinuities(), 0u);
  EXPECT_EQ(correction.missingSectors(), 0u);

  // Nothing was fabricated and the addresses run contiguously: the image
  // offset still equals the sector address.
  ASSERT_EQ(emitted.size(), input.size());
  for (size_t i = 1; i < emitted.size(); ++i) {
    EXPECT_EQ(emitted[i].address().address(),
              emitted[i - 1].address().address() + 1)
        << "at emitted index " << i;
  }
}

// A backward header address is just as corrupt as a forward one, and used to
// walk the baseline backwards in silence.
TEST(SectorAddressIntegrity, BackwardHeaderOnUntrustedSectorIsRepaired) {
  SectorCorrection correction;

  std::vector<Sector> input;
  appendClean(input, kQBase, kSettleSectors);
  // A header claiming an address far behind the timeline.
  input.push_back(untrustedSector(2, kQBase + kSettleSectors));
  appendClean(input, kQBase + kSettleSectors + 1, 10);

  const std::vector<Sector> emitted = run(correction, input);

  EXPECT_EQ(correction.repairedAddresses(), 1u);
  EXPECT_EQ(correction.backwardAddresses(), 0u);
  EXPECT_EQ(correction.missingSectors(), 0u);
  ASSERT_EQ(emitted.size(), input.size());
  for (size_t i = 1; i < emitted.size(); ++i) {
    EXPECT_EQ(emitted[i].address().address(),
              emitted[i - 1].address().address() + 1);
  }
}

// A run of sectors genuinely dropped upstream leaves the Q timeline showing
// the same gap as the addresses. That is real missing data and must be filled,
// so the image stays aligned.
TEST(SectorAddressIntegrity, CorroboratedGapIsFilled) {
  SectorCorrection correction;

  std::vector<Sector> input;
  appendClean(input, kQBase, kSettleSectors);
  // The next ten sections produced no sector: Q time and address both skip.
  appendClean(input, kQBase + kSettleSectors + 10, 10);

  const std::vector<Sector> emitted = run(correction, input);

  EXPECT_EQ(correction.missingSectors(), 10u);
  EXPECT_EQ(correction.addressDiscontinuities(), 0u);

  ASSERT_EQ(emitted.size(), static_cast<size_t>(kSettleSectors + 20));
  for (size_t i = 1; i < emitted.size(); ++i) {
    EXPECT_EQ(emitted[i].address().address(),
              emitted[i - 1].address().address() + 1);
  }
}

// The DS4 42825-sector jump: verified addresses on both sides, but the Q
// timeline shows the two sections were adjacent. No data is missing - the
// disc's address space steps - so filling it would fabricate 42825 phantom
// sectors. Report it instead.
TEST(SectorAddressIntegrity, UncorroboratedJumpIsNotFilled) {
  SectorCorrection correction;

  std::vector<Sector> input;
  appendClean(input, kQBase, kSettleSectors);
  // Adjacent sections, but the address space jumps by 42825.
  for (int32_t q = kQBase + kSettleSectors; q < kQBase + kSettleSectors + 10;
       ++q) {
    input.push_back(makeSector(q + kAddressOffset + 42825, q, true));
  }

  const std::vector<Sector> emitted = run(correction, input);

  EXPECT_EQ(correction.addressDiscontinuities(), 1u);
  EXPECT_EQ(correction.missingSectors(), 0u);
  EXPECT_EQ(emitted.size(), input.size());

  // The verified address is kept - it is the disc's truth, not the decoder's
  // preference - and the offset re-learns so later sectors stay contiguous.
  const size_t jumpIndex = static_cast<size_t>(kSettleSectors);
  EXPECT_EQ(emitted[jumpIndex].address().address(), kSettleSectors + 42825);
  for (size_t i = jumpIndex + 1; i < emitted.size(); ++i) {
    EXPECT_EQ(emitted[i].address().address(),
              emitted[i - 1].address().address() + 1);
  }
}

// With no Q reference at all the stage must still behave as before: fill what
// it plausibly can, refuse an implausible jump, and say the fill was taken on
// header evidence alone.
TEST(SectorAddressIntegrity, WithoutQReferenceFallsBackToTheCap) {
  SectorCorrection correction;

  std::vector<Sector> input;
  for (int32_t a = 0; a < kSettleSectors; ++a) {
    input.push_back(makeSector(a, -1, true));
  }
  input.push_back(makeSector(200000, -1, true));

  run(correction, input);

  EXPECT_EQ(correction.missingSectors(), 0u);
  EXPECT_EQ(correction.addressDiscontinuities(), 1u);
}

TEST(SectorAddressIntegrity, WithoutQReferenceASmallGapIsStillFilled) {
  SectorCorrection correction;

  std::vector<Sector> input;
  for (int32_t a = 0; a < kSettleSectors; ++a) {
    input.push_back(makeSector(a, -1, true));
  }
  input.push_back(makeSector(kSettleSectors + 5, -1, true));

  run(correction, input);

  EXPECT_EQ(correction.missingSectors(), 5u);
  EXPECT_EQ(correction.uncorroboratedFills(), 1u);
  EXPECT_EQ(correction.addressDiscontinuities(), 0u);
}

// The leading fill used to be gated on the first address not landing on a
// frame boundary, which skipped it entirely for addresses that are a multiple
// of 75.
TEST(SectorAddressIntegrity, LeadingFillIsNotGatedOnFrameNumber) {
  SectorCorrection correction;

  std::vector<Sector> input;
  for (int32_t q = 0; q < 5; ++q) {
    input.push_back(makeSector(150 + q, kQBase + q, true));
  }

  const std::vector<Sector> emitted = run(correction, input);

  EXPECT_EQ(correction.missingLeadingSectors(), 150u);
  ASSERT_EQ(emitted.size(), 155u);
  EXPECT_EQ(emitted.front().address().address(), 0);
  EXPECT_EQ(correction.anchorOutliers(), 0u);
}

// The remaining hole the Domesday DD86-DS4 decode exposed: the very first
// sector anchors the image - it decides where the leading fill stops - but it
// is the one address with nothing to check it against, because the
// header-to-Q offset is learned from the sectors themselves. Its header
// claimed 00:41:73 when the disc really starts at 00:01:74, which fabricated
// 3148 phantom leading sectors and left the whole image offset.
TEST(SectorAddressIntegrity, CorruptFirstSectorDoesNotAnchorTheImage) {
  SectorCorrection correction;

  std::vector<Sector> input;
  // A first sector whose header is 2999 sectors ahead of the truth. It is
  // marked verified, as it was on the disc - a 32-bit EDC admits a rare
  // false-valid, and the opening window is the one place that must be assumed.
  input.push_back(makeSector(kQBase + kAddressOffset + 2999, kQBase, true));
  appendClean(input, kQBase + 1, kSettleSectors);

  const std::vector<Sector> emitted = run(correction, input);

  EXPECT_EQ(correction.anchorOutliers(), 1u);
  // The outlier is overruled, so no phantom leading fill and no spurious
  // discontinuity.
  EXPECT_EQ(correction.missingLeadingSectors(), 0u);
  EXPECT_EQ(correction.addressDiscontinuities(), 0u);
  EXPECT_EQ(correction.backwardAddresses(), 0u);

  ASSERT_EQ(emitted.size(), input.size());
  EXPECT_EQ(emitted.front().address().address(), 0);
  for (size_t i = 1; i < emitted.size(); ++i) {
    EXPECT_EQ(emitted[i].address().address(),
              emitted[i - 1].address().address() + 1)
        << "at emitted index " << i;
  }
}

// A capture shorter than the anchor window must still be emitted, not stranded
// in the buffer.
TEST(SectorAddressIntegrity, CaptureShorterThanTheAnchorWindowStillEmits) {
  SectorCorrection correction;

  std::vector<Sector> input;
  appendClean(input, kQBase, 3);

  const std::vector<Sector> emitted = run(correction, input);

  EXPECT_EQ(emitted.size(), 3u);
  EXPECT_EQ(emitted.front().address().address(), 0);
}

}  // namespace
