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

// R-7: the exact failure from the Domesday DD86-DS6 decode.
//
// F2SectionCorrection re-baselines the timeline across a 2290-section gap, but
// the CIRC de-interleave puts a section's bytes about 111 F1 frames behind its
// metadata, so the first sectors cut after the join still carry Q times from
// before it. Their addresses are EDC-verified and correct; only the Q reference
// is stale. Believing the reference instead of the address vetoed the fill, and
// the whole image after that point ended up 2286 sectors out of place.
TEST(SectorAddressIntegrity, StaleQReferenceDoesNotVetoTheFill) {
  SectorCorrection correction;

  constexpr int32_t kSkip = 2290;

  std::vector<Sector> input;
  appendClean(input, kQBase, kSettleSectors);

  // Verified address 2290 ahead, but the Q time only advanced by one.
  input.push_back(
      makeSector(kSettleSectors + kSkip, kQBase + kSettleSectors, true));
  // The timeline catches up and agrees with the established offset again.
  for (int32_t i = 1; i <= 10; ++i) {
    input.push_back(makeSector(kSettleSectors + kSkip + i,
                               kQBase + kSettleSectors + kSkip + i, true));
  }

  const std::vector<Sector> emitted = run(correction, input);

  // The address was believed and the gap it opened was filled, so the image
  // offset still equals the sector address.
  EXPECT_EQ(correction.addressDiscontinuities(), 0u);
  EXPECT_EQ(correction.missingSectors(), static_cast<uint32_t>(kSkip));
  EXPECT_EQ(correction.qReferenceLapses(), 1u);

  ASSERT_EQ(emitted.size(), input.size() + static_cast<size_t>(kSkip));
  for (size_t i = 0; i < emitted.size(); ++i) {
    EXPECT_EQ(emitted[i].address().address(), static_cast<int32_t>(i))
        << "at emitted index " << i;
  }
}

// R-7: while the reference is disowned it must not be used to rewrite an
// unverified header either. On DS6 the stale reference re-anchored the offset
// and five sectors cut from filler - headers reading 0 - were given
// manufactured addresses that displaced the real data behind them.
TEST(SectorAddressIntegrity, StaleQReferenceDoesNotRepairUnverifiedSectors) {
  SectorCorrection correction;

  constexpr int32_t kSkip = 2290;

  std::vector<Sector> input;
  appendClean(input, kQBase, kSettleSectors);
  input.push_back(
      makeSector(kSettleSectors + kSkip, kQBase + kSettleSectors, true));
  // Sectors cut from the filler: EDC failed, header reads 0, and the only
  // thing that could place them is the reference that is currently in doubt.
  for (int32_t i = 1; i <= 5; ++i) {
    input.push_back(untrustedSector(0, kQBase + kSettleSectors + i));
  }
  for (int32_t i = 1; i <= 10; ++i) {
    input.push_back(makeSector(kSettleSectors + kSkip + i,
                               kQBase + kSettleSectors + kSkip + i, true));
  }

  const std::vector<Sector> emitted = run(correction, input);

  EXPECT_EQ(correction.qReferenceLapses(), 1u);
  EXPECT_EQ(correction.unplaceableSectors(), 5u);
  EXPECT_EQ(correction.repairedAddresses(), 0u);
  EXPECT_EQ(correction.addressDiscontinuities(), 0u);

  // The five were dropped rather than placed, and the gap fill covers their
  // addresses, so the image stays offset == address end to end.
  ASSERT_EQ(emitted.size(), input.size() - 5 + static_cast<size_t>(kSkip));
  for (size_t i = 0; i < emitted.size(); ++i) {
    EXPECT_EQ(emitted[i].address().address(), static_cast<int32_t>(i))
        << "at emitted index " << i;
  }
}

// R-7: a real step in the disc's address space is agreed by every sector after
// it, so it must still be adopted - the doubt only has to outlast the handful
// of sectors a de-interleave slip can affect.
TEST(SectorAddressIntegrity, GenuineAddressStepStillReBaselinesTheOffset) {
  SectorCorrection correction;

  constexpr int32_t kStep = 42825;

  std::vector<Sector> input;
  appendClean(input, kQBase, kSettleSectors);
  // Adjacent sections, address space steps: every one of these agrees on the
  // new offset.
  for (int32_t i = 0; i < 12; ++i) {
    const int32_t q = kQBase + kSettleSectors + i;
    input.push_back(makeSector(q + kAddressOffset + kStep, q, true));
  }
  // An unverified sector after the step must now be repaired to the *new*
  // offset, which only works if the re-baseline was adopted.
  const int32_t q = kQBase + kSettleSectors + 12;
  input.push_back(untrustedSector(12345, q));

  const std::vector<Sector> emitted = run(correction, input);

  EXPECT_EQ(correction.repairedAddresses(), 1u);
  EXPECT_EQ(emitted.back().address().address(), q + kAddressOffset + kStep);
}

// R-7: the doubt exists to absorb a slip a few sectors long. If disagreement
// somehow persists, dropping the rest of the capture is the wrong answer, so
// the lapse is bounded and normal handling resumes.
TEST(SectorAddressIntegrity, QReferenceDoubtIsBounded) {
  SectorCorrection correction;

  constexpr int32_t kSkip = 2290;
  constexpr int32_t kUnverified = 200;

  std::vector<Sector> input;
  appendClean(input, kQBase, kSettleSectors);
  input.push_back(
      makeSector(kSettleSectors + kSkip, kQBase + kSettleSectors, true));
  // A long run of unverified sectors and no verified address to end the lapse.
  // Their addresses follow on correctly, so once the bound expires they can be
  // placed as usual.
  for (int32_t i = 1; i <= kUnverified; ++i) {
    input.push_back(untrustedSector(kSettleSectors + kSkip + i,
                                    kQBase + kSettleSectors + kSkip + i));
  }

  const std::vector<Sector> emitted = run(correction, input);

  EXPECT_EQ(correction.qReferenceLapses(), 1u);
  EXPECT_GT(correction.unplaceableSectors(), 0u);
  EXPECT_LT(correction.unplaceableSectors(), static_cast<uint32_t>(kUnverified))
      << "the lapse must not swallow the rest of the capture";
  EXPECT_GT(emitted.size(), static_cast<size_t>(kSettleSectors + kSkip + 100));
}
