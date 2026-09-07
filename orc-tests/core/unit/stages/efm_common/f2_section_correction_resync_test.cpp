/*
 * File:        f2_section_correction_resync_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for R-3 timeline re-baselining: a forward jump too
 *              large to reconstruct must be filled where it plausibly can be,
 *              and where it cannot, must be counted and marked rather than
 *              silently swallowed
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "dec_f2sectioncorrection.h"
#include "efm_constants.h"
#include "section.h"

namespace {

constexpr int kFramesPerSection = efm::kFramesPerSection;

// The gap a LaserDisc skip during capture actually cost on the Domesday
// DD86-DS2/DS4 National A transfers: 2290 sections (30.5 s). Well past the old
// 375-section (5 s) cap, comfortably inside the sector layer's 4500-section
// (1 minute) one.
constexpr int32_t kDiscSkipSections = 2290;

// Beyond any fill cap: what a corrupt-but-CRC-valid timestamp looks like.
constexpr int32_t kImplausibleJump = 200000;

F2Section makeSection(const SectionMetadata& metadata) {
  F2Section section;
  section.metadata = metadata;
  for (int i = 0; i < kFramesPerSection; ++i) {
    F2Frame frame;
    frame.setData(std::vector<uint8_t>(32, 0));
    frame.setErrorData(std::vector<uint8_t>(32, 0));
    frame.setPaddedData(std::vector<uint8_t>(32, 0));
    section.pushFrame(frame);
  }
  return section;
}

SectionMetadata validMetadata(int32_t absoluteFrame) {
  SectionMetadata metadata;
  metadata.setSectionType(SectionType(SectionType::UserData), 1);
  metadata.setIndex(1);
  metadata.setAbsoluteSectionTime(SectionTime(absoluteFrame));
  metadata.setSectionTime(SectionTime(absoluteFrame));
  metadata.setValid(true);
  return metadata;
}

std::vector<F2Section> drain(F2SectionCorrection& correction) {
  std::vector<F2Section> out;
  while (correction.isReady()) out.push_back(correction.popSection());
  return out;
}

// Settle the timeline on a contiguous run from absolute 0, jump the absolute
// time forward by `jump` sections, then run on contiguously from there.
std::vector<F2Section> runWithForwardJump(F2SectionCorrection& correction,
                                          int leadingSections, int32_t jump,
                                          int trailingSections) {
  std::vector<F2Section> emitted;
  int32_t absolute = 0;
  for (int i = 0; i < leadingSections; ++i) {
    correction.pushSection(makeSection(validMetadata(absolute++)));
    for (auto& section : drain(correction)) emitted.push_back(section);
  }
  absolute += jump;
  for (int i = 0; i < trailingSections; ++i) {
    correction.pushSection(makeSection(validMetadata(absolute++)));
    for (auto& section : drain(correction)) emitted.push_back(section);
  }
  correction.flush();
  for (auto& section : drain(correction)) emitted.push_back(section);
  return emitted;
}

// A multi-second gap of the size a real disc skip costs is within the fill cap,
// so the timeline stays intact: the gap is reconstructed as padding sections
// and no resync happens.
TEST(F2SectionCorrectionResync, DiscSkipSizedGapIsFilledNotResynced) {
  F2SectionCorrection correction;
  const std::vector<F2Section> emitted =
      runWithForwardJump(correction, 20, kDiscSkipSections, 20);

  EXPECT_EQ(correction.timelineResyncs(), 0u);
  EXPECT_EQ(correction.resyncSkippedSections(), 0u);

  // Everything from absolute 0 to the last pushed section is present, and the
  // emitted absolute times are contiguous.
  ASSERT_EQ(emitted.size(), static_cast<size_t>(20 + kDiscSkipSections + 20));
  for (size_t i = 0; i < emitted.size(); ++i) {
    EXPECT_EQ(emitted[i].metadata.absoluteSectionTime().frames(),
              static_cast<int32_t>(i))
        << "at emitted index " << i;
  }
}

// A jump no fill could justify still re-baselines - but it is now counted, and
// the section it restarts on is marked so downstream continuity checks know the
// step is deliberate.
TEST(F2SectionCorrectionResync, ImplausibleJumpIsCountedAndMarked) {
  F2SectionCorrection correction;
  const std::vector<F2Section> emitted =
      runWithForwardJump(correction, 20, kImplausibleJump, 20);

  EXPECT_EQ(correction.timelineResyncs(), 1u);
  EXPECT_EQ(correction.resyncSkippedSections(),
            static_cast<uint32_t>(kImplausibleJump));

  // Exactly one emitted section carries the resync marker, and it is the one
  // whose absolute time steps.
  int markers = 0;
  for (size_t i = 0; i < emitted.size(); ++i) {
    if (!emitted[i].metadata.isTimelineResync()) continue;
    ++markers;
    ASSERT_GT(i, 0u);
    EXPECT_NE(emitted[i].metadata.absoluteSectionTime().frames(),
              emitted[i - 1].metadata.absoluteSectionTime().frames() + 1);
  }
  EXPECT_EQ(markers, 1);

  // Every other step is contiguous: the resync must not leave the rest of the
  // timeline ragged.
  for (size_t i = 1; i < emitted.size(); ++i) {
    if (emitted[i].metadata.isTimelineResync()) continue;
    EXPECT_EQ(emitted[i].metadata.absoluteSectionTime().frames(),
              emitted[i - 1].metadata.absoluteSectionTime().frames() + 1)
        << "at emitted index " << i;
  }
}

// The section count and the absolute-time span disagree by exactly what the
// resync stepped over (plus anything it had to discard). That identity is what
// makes the decode report's duration line explicable.
TEST(F2SectionCorrectionResync, SkippedSectionsAccountForTheTimelineShortfall) {
  F2SectionCorrection correction;
  runWithForwardJump(correction, 20, kImplausibleJump, 20);

  const int32_t span = correction.absoluteEndTime().frames() -
                       correction.absoluteStartTime().frames() + 1;
  const uint32_t unaccounted =
      static_cast<uint32_t>(span) - correction.totalSections();

  EXPECT_EQ(unaccounted, correction.resyncSkippedSections() +
                             correction.resyncDiscardedSections());
}

}  // namespace
