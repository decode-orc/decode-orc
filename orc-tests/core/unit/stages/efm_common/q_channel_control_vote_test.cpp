/*
 * File:        q_channel_control_vote_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for Q-8: a track's Q-channel control flags must be
 *              the majority verdict of its sections, not the reading of
 *              whichever section arrived first, and a section whose control
 *              nybble the standard does not assign must not vote at all
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

// The correction stage holds a lookahead window before emitting, and the track
// tally is only updated as sections are emitted, so a fixture has to supply
// enough sections to get past it.
constexpr int32_t kSections = 400;

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

// A data-track section: control nybble 0x4 (digital, copy prohibited).
SectionMetadata dataSection(int32_t absoluteFrame) {
  SectionMetadata metadata;
  metadata.setSectionType(SectionType(SectionType::UserData), 1);
  metadata.setIndex(1);
  metadata.setAbsoluteSectionTime(SectionTime(absoluteFrame));
  metadata.setSectionTime(SectionTime(absoluteFrame));
  metadata.setValid(true);
  metadata.setControlValid(true);
  metadata.setAudio(false);
  metadata.setCopyProhibited(true);
  metadata.set2Channel(true);
  metadata.setPreemphasis(false);
  return metadata;
}

// The same section as an audio reading: control nybble 0x0.
SectionMetadata audioSection(int32_t absoluteFrame) {
  SectionMetadata metadata = dataSection(absoluteFrame);
  metadata.setAudio(true);
  return metadata;
}

void run(F2SectionCorrection& correction,
         const std::vector<SectionMetadata>& sections) {
  for (const SectionMetadata& metadata : sections) {
    correction.pushSection(makeSection(metadata));
    while (correction.isReady()) correction.popSection();
  }
  correction.flush();
  while (correction.isReady()) correction.popSection();
}

// The failure this exists to prevent: the flags used to be sampled from the
// first section of the track and never revisited, so one mis-decoded control
// nybble at the head of a 27-minute data track reported the whole disc as
// audio.
TEST(QChannelControlVote, OneCorruptFirstSectionDoesNotDecideTheTrack) {
  F2SectionCorrection correction;

  std::vector<SectionMetadata> sections;
  sections.push_back(audioSection(0));
  for (int32_t i = 1; i < kSections; ++i) sections.push_back(dataSection(i));

  run(correction, sections);

  ASSERT_EQ(correction.trackIsAudio().size(), 1u);
  EXPECT_FALSE(correction.trackIsAudio()[0]);
}

// The vote must be a vote, not a last-seen: damage at the end of a track is no
// more authoritative than damage at the start.
TEST(QChannelControlVote, OneCorruptLastSectionDoesNotDecideTheTrack) {
  F2SectionCorrection correction;

  std::vector<SectionMetadata> sections;
  for (int32_t i = 0; i < kSections - 1; ++i)
    sections.push_back(dataSection(i));
  sections.push_back(audioSection(kSections - 1));

  run(correction, sections);

  ASSERT_EQ(correction.trackIsAudio().size(), 1u);
  EXPECT_FALSE(correction.trackIsAudio()[0]);
}

// A genuinely audio-flagged track must still report as audio - the vote must
// not have a thumb on the scale. The Domesday DD86 National A sides really are
// mastered this way over mode-1 sectors, so this is not a hypothetical.
TEST(QChannelControlVote, AUnanimousAudioTrackIsReportedAsAudio) {
  F2SectionCorrection correction;

  std::vector<SectionMetadata> sections;
  for (int32_t i = 0; i < kSections; ++i) sections.push_back(audioSection(i));

  run(correction, sections);

  ASSERT_EQ(correction.trackIsAudio().size(), 1u);
  EXPECT_TRUE(correction.trackIsAudio()[0]);
}

// A control nybble the standard leaves unassigned (0x5, 0x7, 0xC-0xF) leaves
// the metadata at its constructor values - 2-channel audio, copy prohibited -
// which is indistinguishable from a real reading. Such a section must abstain,
// or corruption votes with full confidence.
TEST(QChannelControlVote, UnassignedControlNybbleDoesNotVote) {
  F2SectionCorrection correction;

  std::vector<SectionMetadata> sections;
  // A majority of sections carry an unassigned nybble, so the defaults would
  // win outright if they were allowed to vote.
  for (int32_t i = 0; i < kSections; ++i) {
    if (i % 3 == 0) {
      sections.push_back(dataSection(i));
    } else {
      SectionMetadata metadata = dataSection(i);
      metadata.setControlValid(false);
      metadata.setAudio(true);  // the constructor default, not a reading
      sections.push_back(metadata);
    }
  }

  run(correction, sections);

  ASSERT_EQ(correction.trackIsAudio().size(), 1u);
  EXPECT_FALSE(correction.trackIsAudio()[0]);
}

// Pre-emphasis may legitimately change within a track, so it cannot simply be
// voted away - but an isolated flip is damage, not a change of emphasis, and
// must not raise the "varied" marker.
TEST(QChannelControlVote, IsolatedPreemphasisFlipsAreNotReportedAsVaried) {
  F2SectionCorrection correction;

  std::vector<SectionMetadata> sections;
  for (int32_t i = 0; i < kSections; ++i) {
    SectionMetadata metadata = dataSection(i);
    if (i == 5 || i == 100 || i == 250) metadata.setPreemphasis(true);
    sections.push_back(metadata);
  }

  run(correction, sections);

  ASSERT_EQ(correction.trackPreemphasis().size(), 1u);
  EXPECT_FALSE(correction.trackPreemphasis()[0]);
  EXPECT_FALSE(correction.trackPreemphasisVaried()[0]);
}

// A change that lasts long enough to be audible is a real change and must be
// reported as one.
TEST(QChannelControlVote, ASustainedPreemphasisChangeIsReportedAsVaried) {
  F2SectionCorrection correction;

  std::vector<SectionMetadata> sections;
  for (int32_t i = 0; i < kSections; ++i) {
    SectionMetadata metadata = dataSection(i);
    // Well over a second of sections carrying the other reading.
    if (i >= 100 && i < 250) metadata.setPreemphasis(true);
    sections.push_back(metadata);
  }

  run(correction, sections);

  ASSERT_EQ(correction.trackPreemphasisVaried().size(), 1u);
  EXPECT_TRUE(correction.trackPreemphasisVaried()[0]);
}

}  // namespace
