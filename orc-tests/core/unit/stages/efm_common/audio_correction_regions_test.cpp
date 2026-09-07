/*
 * File:        audio_correction_regions_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for concealment loss attribution: structural
 *              de-interleave filler vs genuine loss, and per-disc-region
 *              accounting on the latency-compensated audio timeline
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "audio.h"
#include "dec_audiocorrection.h"
#include "efm_constants.h"
#include "section.h"

namespace {

// Section geometry: 98 F1 frames, each carrying 6 stereo pairs.
constexpr int kFramesPerSection = efm::kFramesPerSection;  // 98
constexpr int kPairsPerFrame = 6;
constexpr int kSamplesPerFrame = 12;  // both channels, interleaved L,R

// The de-interleaver delays its output by this many F1 frames, so the first
// this-many output frames are warm-up filler carrying no disc data.
constexpr int kLatencyFrames = efm::kDeinterleaveLatencyF1Frames;  // 111

// How one F1 frame of the synthetic stream should be built.
struct FrameSpec {
  bool padded = false;     // decoder filler rather than disc data
  bool flagError = false;  // CIRC could not recover this frame
};

// Region a section's Q-channel metadata should describe.
enum class Region { LeadIn, LeadOut, Pause, Programme };

SectionMetadata makeMetadata(Region region, uint8_t trackNumber,
                             int32_t absoluteFrame) {
  SectionMetadata metadata;
  switch (region) {
    case Region::LeadIn:
      metadata.setSectionType(SectionType(SectionType::LeadIn), trackNumber);
      break;
    case Region::LeadOut:
      metadata.setSectionType(SectionType(SectionType::LeadOut), trackNumber);
      break;
    case Region::Pause:
      metadata.setSectionType(SectionType(SectionType::UserData), trackNumber);
      metadata.setIndex(0);  // IEC 60908-1999 §17.5.1: INDEX 00 = pause
      break;
    case Region::Programme:
      metadata.setSectionType(SectionType(SectionType::UserData), trackNumber);
      metadata.setIndex(1);
      break;
  }
  metadata.setTrackNumber(trackNumber);
  metadata.setAbsoluteSectionTime(SectionTime(absoluteFrame));
  // A section whose Q-channel decoded successfully is marked valid; attribution
  // deliberately ignores sections that are not, so the fixture must match.
  metadata.setValid(true);
  return metadata;
}

// Build one section from a per-frame description. |globalFrameBase| is the
// index of this section's first frame within the whole stream, so the caller
// can describe the stream frame-by-frame.
AudioSection makeSection(const std::vector<FrameSpec>& stream,
                         int globalFrameBase, const SectionMetadata& metadata) {
  AudioSection section;
  section.metadata = metadata;
  for (int frame = 0; frame < kFramesPerSection; ++frame) {
    const FrameSpec& spec = stream.at(globalFrameBase + frame);
    std::vector<int16_t> data(kSamplesPerFrame, 0);
    std::vector<uint8_t> errors(kSamplesPerFrame, spec.flagError ? 1 : 0);
    std::vector<uint8_t> padded(kSamplesPerFrame, spec.padded ? 1 : 0);
    std::vector<uint8_t> concealed(kSamplesPerFrame, 0);
    Audio audio;
    audio.setData(data);
    audio.setErrorData(errors);
    audio.setConcealedData(concealed);
    audio.setPaddedData(padded);
    section.pushFrame(audio);
  }
  return section;
}

// Push a whole synthetic stream through the corrector and flush it.
void runStream(AudioCorrection& corrector, const std::vector<FrameSpec>& stream,
               const std::vector<SectionMetadata>& sectionMetadata) {
  for (size_t s = 0; s < sectionMetadata.size(); ++s) {
    corrector.pushSection(makeSection(
        stream, static_cast<int>(s) * kFramesPerSection, sectionMetadata[s]));
    while (corrector.isReady()) corrector.popSection();
  }
  corrector.flush();
  while (corrector.isReady()) corrector.popSection();
}

// A clean six-section stream: three sections of lead-in followed by three of
// programme (track 1), with the leading 111 frames marked as de-interleave
// warm-up filler exactly as the CIRC stage emits them.
struct CleanStream {
  std::vector<FrameSpec> frames;
  std::vector<SectionMetadata> sections;
  static constexpr int kSectionCount = 6;
  static constexpr int kLeadInSections = 3;
};

CleanStream makeCleanStream() {
  CleanStream stream;
  stream.frames.resize(CleanStream::kSectionCount * kFramesPerSection);
  for (int i = 0; i < kLatencyFrames; ++i) stream.frames[i].padded = true;
  for (int s = 0; s < CleanStream::kSectionCount; ++s) {
    const bool leadIn = s < CleanStream::kLeadInSections;
    stream.sections.push_back(makeMetadata(
        leadIn ? Region::LeadIn : Region::Programme, 1, s * kFramesPerSection));
  }
  return stream;
}

// Expected sample counts for the clean stream above, per the audio timeline.
// Output frame k carries the disc data of input frame k - 111, so:
//   frames   0..110 : warm-up filler (no disc data)
//   frames 111..404 : disc frames   0..293 -> lead-in (3 sections x 98)
//   frames 405..587 : disc frames 294..476 -> programme
constexpr uint64_t kExpectedWarmupSamples =
    static_cast<uint64_t>(kLatencyFrames) * kSamplesPerFrame;  // 1,332
constexpr uint64_t kExpectedLeadInSamples =
    static_cast<uint64_t>(CleanStream::kLeadInSections * kFramesPerSection) *
    kSamplesPerFrame;  // 3,528
constexpr uint64_t kExpectedProgrammeSamples =
    static_cast<uint64_t>(CleanStream::kSectionCount * kFramesPerSection -
                          kLatencyFrames -
                          CleanStream::kLeadInSections * kFramesPerSection) *
    kSamplesPerFrame;  // 2,196

TEST(AudioCorrectionRegions, AttributesWarmUpFillerToTheWarmUpCauseNotToADisc) {
  AudioCorrection corrector;
  const CleanStream stream = makeCleanStream();

  runStream(corrector, stream.frames, stream.sections);

  EXPECT_EQ(corrector.warmupSamples(), kExpectedWarmupSamples);
  EXPECT_EQ(corrector.drainSamples(), 0u);
  // Filler must never be attributed to a disc region.
  EXPECT_EQ(corrector.regionLoss(DiscRegion::OutsideProgrammeArea).decoded, 0u);
}

// The de-interleave latency is subtracted before a sample is attributed, so the
// lead-in/programme boundary lands 111 frames later in the output than the
// section metadata alone would suggest.
TEST(AudioCorrectionRegions, CompensatesDeinterleaveLatencyWhenAttributing) {
  AudioCorrection corrector;
  const CleanStream stream = makeCleanStream();

  runStream(corrector, stream.frames, stream.sections);

  EXPECT_EQ(corrector.regionLoss(DiscRegion::LeadIn).decoded,
            kExpectedLeadInSamples);
  EXPECT_EQ(corrector.regionLoss(DiscRegion::Programme).decoded,
            kExpectedProgrammeSamples);
}

// Acceptance criterion: the region breakdown and the cause breakdown must each
// account for every sample in the stream.
TEST(AudioCorrectionRegions, RegionAndCauseBreakdownsBothSumToTheTotal) {
  AudioCorrection corrector;
  const CleanStream stream = makeCleanStream();

  runStream(corrector, stream.frames, stream.sections);

  const uint64_t total =
      static_cast<uint64_t>(stream.frames.size()) * kSamplesPerFrame;

  uint64_t regionTotal = 0;
  for (size_t i = 0; i < static_cast<size_t>(DiscRegion::Count); ++i) {
    regionTotal += corrector.regionLoss(static_cast<DiscRegion>(i)).decoded;
  }
  const uint64_t structural =
      corrector.warmupSamples() + corrector.drainSamples();

  EXPECT_EQ(regionTotal + structural, total);
  EXPECT_EQ(corrector.validSamples() + corrector.concealedSamples() +
                corrector.silencedSamples(),
            total);
}

// Acceptance criterion 1: a stream whose only unrecoverable frames are the
// pipeline's own boundaries reports no genuine loss anywhere.
TEST(AudioCorrectionRegions, CleanStreamReportsNoGenuineLoss) {
  AudioCorrection corrector;
  const CleanStream stream = makeCleanStream();

  runStream(corrector, stream.frames, stream.sections);

  for (size_t i = 0; i < static_cast<size_t>(DiscRegion::Count); ++i) {
    const RegionLoss& loss = corrector.regionLoss(static_cast<DiscRegion>(i));
    EXPECT_EQ(loss.concealed, 0u) << "region index " << i;
    EXPECT_EQ(loss.silenced, 0u) << "region index " << i;
  }
  EXPECT_TRUE(corrector.trackLosses().empty());
}

// Acceptance criterion 4: a burst inside the programme area is attributed to
// the programme area and named against its track.
TEST(AudioCorrectionRegions, BurstInProgrammeAreaIsAttributedToItsTrack) {
  AudioCorrection corrector;
  CleanStream stream = makeCleanStream();
  // Disc frames 400..439 are programme; they emerge at output frames 511..550.
  constexpr int kBurstOutputStart = 511;
  constexpr int kBurstFrames = 40;
  for (int i = 0; i < kBurstFrames; ++i) {
    stream.frames[kBurstOutputStart + i].flagError = true;
  }

  runStream(corrector, stream.frames, stream.sections);

  const RegionLoss& programme = corrector.regionLoss(DiscRegion::Programme);
  EXPECT_GT(programme.silenced, 0u);
  EXPECT_EQ(corrector.regionLoss(DiscRegion::LeadIn).silenced, 0u);

  ASSERT_EQ(corrector.trackLosses().size(), 1u);
  const auto& entry = *corrector.trackLosses().begin();
  EXPECT_EQ(entry.first, 1);
  EXPECT_EQ(entry.second.silenced, programme.silenced);
  EXPECT_TRUE(entry.second.haveSilenced);
}

// Acceptance criterion 5: the same burst placed in the lead-in leaves the
// programme figures untouched and is reported under its own region.
TEST(AudioCorrectionRegions, BurstInLeadInDoesNotAffectProgrammeFigures) {
  AudioCorrection corrector;
  CleanStream stream = makeCleanStream();
  // Disc frames 100..139 are lead-in; they emerge at output frames 211..250.
  constexpr int kBurstOutputStart = 211;
  constexpr int kBurstFrames = 40;
  for (int i = 0; i < kBurstFrames; ++i) {
    stream.frames[kBurstOutputStart + i].flagError = true;
  }

  runStream(corrector, stream.frames, stream.sections);

  EXPECT_GT(corrector.regionLoss(DiscRegion::LeadIn).silenced, 0u);
  EXPECT_EQ(corrector.regionLoss(DiscRegion::Programme).silenced, 0u);
  EXPECT_EQ(corrector.regionLoss(DiscRegion::Programme).concealed, 0u);
  EXPECT_TRUE(corrector.trackLosses().empty());
}

// A drain that lands in the lead-out is harmless; one that lands in the
// programme area means the capture stopped too early and the tail of the audio
// really is gone. The two must be distinguishable.
TEST(AudioCorrectionRegions, DrainIsAttributedToTheRegionItFallsIn) {
  AudioCorrection corrector;
  CleanStream stream = makeCleanStream();
  // Mark the final 111 frames as drain filler, as the CIRC flush emits them.
  const int total = static_cast<int>(stream.frames.size());
  for (int i = total - kLatencyFrames; i < total; ++i) {
    stream.frames[i].padded = true;
  }

  runStream(corrector, stream.frames, stream.sections);

  EXPECT_EQ(corrector.drainSamples(),
            static_cast<uint64_t>(kLatencyFrames) * kSamplesPerFrame);
  // The stream ends inside the programme area, so the lost tail is programme
  // audio rather than lead-out silence.
  EXPECT_EQ(corrector.drainSamplesIn(DiscRegion::Programme),
            corrector.drainSamples());
  EXPECT_EQ(corrector.drainSamplesIn(DiscRegion::LeadOut), 0u);
}

// R-6: filler F2SectionCorrection inserted mid-stream to bridge a gap in the
// EFM is padded exactly like the drain, but it is not the drain: the capture
// did not stop, and the tail of the audio is not lost. Charging it to the
// drain made the report claim a truncated capture, and made a mid-disc gap
// look like end-of-stream damage.
TEST(AudioCorrectionRegions, MidStreamGapFillerIsNotChargedToTheDrain) {
  AudioCorrection corrector;
  CleanStream stream = makeCleanStream();

  // Section 3 (mid-stream, inside the programme area) is a fabricated gap
  // filler. Its padded bytes reach the output kLatencyFrames later, spread by
  // the CIRC delay lines, so that is where the padded output frames go; the
  // marker belongs on the originating section, which is what the latency
  // compensation in the attribution resolves back to.
  constexpr int kGapSection = 3;
  const int paddedStart = kGapSection * kFramesPerSection + kLatencyFrames;
  for (int frame = 0; frame < kFramesPerSection; ++frame) {
    stream.frames.at(paddedStart + frame).padded = true;
  }
  stream.sections[kGapSection].setGapFiller(true);

  runStream(corrector, stream.frames, stream.sections);

  EXPECT_GT(corrector.gapFillerSamples(), 0u);
  EXPECT_EQ(corrector.drainSamples(), 0u);
  // Nothing is charged to the drain, so the report cannot claim the capture
  // stopped inside the programme area.
  EXPECT_EQ(corrector.drainSamplesIn(DiscRegion::Programme), 0u);
  EXPECT_EQ(corrector.gapFillerSamplesIn(DiscRegion::Programme),
            corrector.gapFillerSamples());
}

// The same filler without the marker is still the drain, so the existing
// truncated-capture reporting is unaffected.
TEST(AudioCorrectionRegions, UnmarkedMidStreamFillerStillCountsAsDrain) {
  AudioCorrection corrector;
  CleanStream stream = makeCleanStream();

  constexpr int kGapSection = 3;
  const int paddedStart = kGapSection * kFramesPerSection + kLatencyFrames;
  for (int frame = 0; frame < kFramesPerSection; ++frame) {
    stream.frames.at(paddedStart + frame).padded = true;
  }

  runStream(corrector, stream.frames, stream.sections);

  EXPECT_EQ(corrector.gapFillerSamples(), 0u);
  EXPECT_GT(corrector.drainSamples(), 0u);
}

}  // namespace
