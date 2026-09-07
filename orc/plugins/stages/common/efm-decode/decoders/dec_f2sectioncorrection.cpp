/*
 * File:        dec_f2sectioncorrection.cpp
 * Purpose:     efm-decoder-f2 - EFM T-values to F2 Section decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dec_f2sectioncorrection.h"

#include <fmt/format.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <queue>
#include <utility>

#include "efm_exception.h"

namespace {
// R-3: a corrupt-but-CRC-valid absolute timestamp (a 16-bit CRC passes ~1/65536
// of corrupt Q blocks) or a genuine multi-second dropout can claim an enormous
// forward jump. Fabricating one fully-populated 98-frame dummy section per
// frame of the jump would allocate unbounded memory (a jump to 99:59:74 is
// ~450000 sections). Any gap larger than this many sections is treated as a
// hard resync point rather than being filled.
//
// 4500 sections = 1 minute, matching kMaxSectorGapFill in dec_sectorcorrection
// so the two layers agree on what counts as fillable: a gap the sector layer
// would happily re-address must not have been thrown away up here first. The
// earlier 5-second cap was set on the assumption that no real medium has a
// longer gap, which LaserDisc captures disprove - a disc skip during capture
// routinely costs tens of seconds of EFM, and dropping it silently loses
// timeline alignment for the rest of the disc. Filling the cap costs
// 4500 * 98 * 32 bytes (~14 MB) worst case, which is bounded and affordable.
constexpr int32_t kMaxMissingSectionFill = 4500;
}  // namespace

F2SectionCorrection::F2SectionCorrection()
    : m_leadinComplete(false),
      m_maximumGapSize(10),
      m_paddingWatermark(5),
      m_totalSections(0),
      m_correctedSections(0),
      m_uncorrectableSections(0),
      m_preLeadinSections(0),
      m_missingSections(0),
      m_paddingSections(0),
      m_outOfOrderSections(0),
      m_tailFilledSections(0),
      m_timelineResyncs(0),
      m_resyncSkippedSections(0),
      m_resyncDiscardedSections(0),
      m_qmode1Sections(0),
      m_qmode2Sections(0),
      m_qmode3Sections(0),
      m_qmode4Sections(0),
      m_absoluteStartTime(99, 59, 74),  // Max-time sentinel (IEC 60908 §17.5.1)
      m_absoluteEndTime(0, 0, 0),
      m_hasToc(false),
      m_tocFirstTrack(0),
      m_tocLastTrack(0),
      m_tocDiscType(0),
      m_leadinSections(0),
      m_currentTrack(0),
      m_noTimecodes(false),
      m_receivedSections(0),
      m_validMetadataSections(0) {}

// Return the index of trackNumber in m_trackNumbers, or -1 if not present.
int F2SectionCorrection::trackIndex(uint8_t trackNumber) const {
  auto it =
      std::find(m_trackNumbers.begin(), m_trackNumbers.end(), trackNumber);
  if (it == m_trackNumbers.end()) return -1;
  return static_cast<int>(std::distance(m_trackNumbers.begin(), it));
}

void F2SectionCorrection::pushSection(const F2Section& data) {
  // Track how much genuinely reached this stage so a failed lead-in can be
  // explained: no sections at all means frame sync never happened.
  ++m_receivedSections;
  if (data.metadata.isValid()) ++m_validMetadataSections;

  // Q-6: harvest the lead-in TOC before the timeline logic gets a chance to
  // discard the section. Lead-in sections carry POINT/PMIN/PSEC/PFRAME rather
  // than a program-area timeline, so they are collected here and then skipped
  // by the settle/correction stages (see recordTocEntry /
  // waitForInputToSettle).
  //
  // No-Timecodes: on discs pre-dating the EFM timecode spec the Q-channel is
  // absent or untrustworthy. A mode-1 block with TNO=0 is classified as
  // lead-in (subcode.cpp), but here it is really a mid-programme section, so
  // its "TOC" is meaningless. Skip the harvest so those sections are neither
  // counted as lead-in nor dropped downstream.
  if (!m_noTimecodes && data.metadata.isValid() &&
      data.metadata.sectionType().type() == SectionType::LeadIn) {
    recordTocEntry(data.metadata);
  }

  // Add the data to the input buffer
  m_inputBuffer.push(data);

  // Process the queue
  processQueue();
}

void F2SectionCorrection::pushSection(F2Section&& data) {
  ++m_receivedSections;
  if (data.metadata.isValid()) ++m_validMetadataSections;

  if (!m_noTimecodes && data.metadata.isValid() &&
      data.metadata.sectionType().type() == SectionType::LeadIn) {
    recordTocEntry(data.metadata);
  }

  // Move the data into the input buffer to avoid a whole-section deep copy.
  m_inputBuffer.push(std::move(data));

  processQueue();
}

// Q-6: assemble the lead-in TOC (IEC 60908 §17.5.1) from one lead-in section's
// POINT/PMIN/PSEC/PFRAME. POINT selects what PMIN/PSEC/PFRAME mean:
//   A0 -> first user track number (PMIN) and disc type (PSEC)
//   A1 -> last user track number (PMIN)
//   A2 -> lead-out start time (PMIN:PSEC:PFRAME)
//   01-99 (BCD) -> that track's start time (PMIN:PSEC:PFRAME)
// Each POINT is broadcast repeatedly through the lead-in, so entries are
// de-duplicated (first value seen wins).
void F2SectionCorrection::recordTocEntry(const SectionMetadata& metadata) {
  ++m_leadinSections;

  const uint8_t point = metadata.point();
  const uint8_t pMin = metadata.pMin();
  const uint8_t pSec = metadata.pSec();
  const uint8_t pFrame = metadata.pFrame();

  if (point == 0xA0) {
    if (!m_hasToc || m_tocFirstTrack == 0) {
      m_tocFirstTrack = pMin;
      m_tocDiscType = pSec;
    }
    m_hasToc = true;
  } else if (point == 0xA1) {
    if (m_tocLastTrack == 0) m_tocLastTrack = pMin;
    m_hasToc = true;
  } else if (point == 0xA2) {
    m_tocLeadOutStart = SectionTime(pMin, pSec, pFrame);
    m_hasToc = true;
  } else {
    // POINT is a BCD track number (01-99).
    const uint8_t track =
        static_cast<uint8_t>((point >> 4) * 10 + (point & 0x0F));
    if (track >= 1 && track <= 99) {
      if (std::find(m_tocTrackNumbers.begin(), m_tocTrackNumbers.end(),
                    track) == m_tocTrackNumbers.end()) {
        m_tocTrackNumbers.push_back(track);
        m_tocTrackStartTimes.push_back(SectionTime(pMin, pSec, pFrame));
      }
      m_hasToc = true;
    }
  }
}

F2Section F2SectionCorrection::popSection() {
  // Move the first item out of the output buffer to avoid a deep copy.
  F2Section section = std::move(m_outputBuffer.front());
  m_outputBuffer.pop();
  return section;
}

bool F2SectionCorrection::isReady() const {
  // Return true if the output buffer is not empty
  return !m_outputBuffer.empty();
}

bool F2SectionCorrection::isValid() const {
  // Return true if the leadin process was complete and we
  // therefore have valid data
  return m_leadinComplete;
}

void F2SectionCorrection::processQueue() {
  // If no timecodes flag is set, skip leadin checks and process all sections
  // directly
  if (m_noTimecodes && !m_leadinComplete) {
    ORC_LOG_DEBUG(
        "F2SectionCorrection::processQueue(): No timecodes flag set, skipping "
        "leadin checks.");
    m_leadinComplete = true;
  }

  // Process the input buffer
  while (!m_inputBuffer.empty()) {
    // Dequeue the next section (move to avoid a whole-section deep copy)
    F2Section f2Section = std::move(m_inputBuffer.front());
    m_inputBuffer.pop();

    // Do we have a last known good section?
    if (!m_leadinComplete) {
      waitForInputToSettle(f2Section);
    } else {
      waitingForSection(f2Section);
    }
  }
}

// This function waits for the input to settle before processing the sections.
// Especially if the input EFM is from a whole disc capture, there will be
// frames at the start in a random order (from the disc spinning up) and we need
// to wait until we receive a few valid sections in chronological order before
// we can start processing them.
//
// This function collects sections until there are 5 valid, chronological
// sections in a row.  Once we have these, we can start processing the sections.
void F2SectionCorrection::waitForInputToSettle(F2Section& f2Section) {
  // Q-6: lead-in sections do not belong to the program-area timeline - their
  // Q-channel carries the TOC (POINT/PMIN/PSEC/PFRAME), already harvested in
  // pushSection(). Skip them here so their (running lead-in) time never seeds
  // the settle buffer; the timeline settles on the first contiguous run of
  // program-area / lead-out sections, exactly as for a program-area capture.
  //
  // No-Timecodes: the lead-in classification is unreliable (a TNO=0 mode-1
  // block is really a mid-programme section here), so the skip must not fire.
  // This path is already short-circuited in that mode - processQueue() forces
  // m_leadinComplete before any section reaches here - but the guard keeps the
  // "never drop a section in No-Timecodes mode" invariant explicit.
  if (!m_noTimecodes && f2Section.metadata.isValid() &&
      f2Section.metadata.sectionType().type() == SectionType::LeadIn) {
    ORC_LOG_DEBUG(
        "F2SectionCorrection::waitForInputToSettle(): Skipping lead-in section "
        "(TOC harvested) with running time {}",
        f2Section.metadata.absoluteSectionTime().toString());
    return;
  }

  // Does the current section have valid metadata?
  if (f2Section.metadata.isValid()) {
    // Do we have any sections in the leadin buffer?
    if (!m_leadinBuffer.empty()) {
      // Ensure that the current section's time-stamp is one greater than the
      // last section in the leadin buffer
      SectionTime expectedAbsoluteTime =
          m_leadinBuffer.back().metadata.absoluteSectionTime() + 1;
      if (f2Section.metadata.absoluteSectionTime() == expectedAbsoluteTime) {
        // Add the new section to the leadin buffer
        m_leadinBuffer.push_back(f2Section);
        ORC_LOG_DEBUG(
            "F2SectionCorrection::waitForInputToSettle(): Added valid section "
            "to leadin buffer with absolute time {}",
            f2Section.metadata.absoluteSectionTime().toString());

        // Do we have 5 valid, contiguous sections in the leadin buffer?
        if (m_leadinBuffer.size() >= 5) {
          m_leadinComplete = true;

          // Feed the leadin buffer into the section correction process
          ORC_LOG_DEBUG(
              "F2SectionCorrection::waitForInputToSettle(): Leadin buffer "
              "complete, pushing collected sections for processing.");
          while (!m_leadinBuffer.empty()) {
            F2Section leadinSection = m_leadinBuffer.front();
            m_leadinBuffer.erase(m_leadinBuffer.begin());
            waitingForSection(leadinSection);
          }
        }
      } else {
        // The current section's time-stamp is not one greater than the last
        // section in the leadin buffer This invalidates the whole leadin buffer
        m_preLeadinSections += static_cast<int32_t>(m_leadinBuffer.size()) + 1;
        m_leadinBuffer.clear();
        ORC_LOG_DEBUG(
            "F2SectionCorrection::waitForInputToSettle(): Got section with "
            "invalid absolute time whilst waiting for input to settle (lead in "
            "buffer discarded).");
      }
    } else {
      // The leadin buffer is empty, so we can now add the collected section to
      // the leadin buffer
      m_leadinBuffer.push_back(f2Section);
      ORC_LOG_DEBUG(
          "F2SectionCorrection::waitForInputToSettle(): Added section to "
          "leadin buffer with valid metadata:");
      ORC_LOG_DEBUG(
          "F2SectionCorrection::waitForInputToSettle():   Absolute time: {}",
          f2Section.metadata.absoluteSectionTime().toString());
      ORC_LOG_DEBUG(
          "F2SectionCorrection::waitForInputToSettle():   Section time: {}",
          f2Section.metadata.sectionTime().toString());
      {
        SectionType st = f2Section.metadata.sectionType();
        std::string sectionTypeStr;
        switch (st.type()) {
          case SectionType::LeadIn:
            sectionTypeStr = "LeadIn";
            break;
          case SectionType::LeadOut:
            sectionTypeStr = "LeadOut";
            break;
          case SectionType::UserData:
            sectionTypeStr = "UserData";
            break;
          default:
            sectionTypeStr =
                fmt::format("Unknown({})", static_cast<int>(st.type()));
            break;
        }
        ORC_LOG_DEBUG(
            "F2SectionCorrection::waitForInputToSettle():   Section type: {}",
            sectionTypeStr);
      }
      ORC_LOG_DEBUG(
          "F2SectionCorrection::waitForInputToSettle():   Track number: {}",
          f2Section.metadata.trackNumber());

      // At this point, we have no idea if the section has a valid absolute time
      // or not a value of 00:00:00 will happen in either case.
    }
  } else {
    // The current section doesn't have valid metadata
    // This invalidates the whole buffer
    m_preLeadinSections += static_cast<int32_t>(m_leadinBuffer.size()) + 1;
    m_leadinBuffer.clear();
    ORC_LOG_DEBUG(
        "F2SectionCorrection::waitForInputToSettle(): Got invalid metadata "
        "section whilst waiting for input to settle (lead in buffer "
        "discarded).");
  }
}

void F2SectionCorrection::waitingForSection(F2Section& f2Section) {
  bool outputSection = true;

  // Q-6: a lead-in section reaching the post-settle path (e.g. a stray lead-in
  // block appearing mid-stream) carries TOC data, not a program-area timeline
  // entry; its TOC was harvested in pushSection(). Drop it so it does not
  // corrupt the internal buffer's monotonic absolute time.
  //
  // No-Timecodes: this guard MUST be gated on !m_noTimecodes. On discs
  // pre-dating the EFM timecode spec, every mode-1 block carries TNO=0 and so
  // is classified as lead-in (subcode.cpp), yet it is genuine programme audio.
  // Dropping it here - before the no-timecodes re-synthesis below re-stamps it
  // as UserData with a monotonic time - discarded the entire disc and left
  // only the handful of Q-CRC-failed sections to be decoded (see issue: whole
  // disc collapsing to a ~63-frame track).
  if (!m_noTimecodes && f2Section.metadata.isValid() &&
      f2Section.metadata.sectionType().type() == SectionType::LeadIn) {
    ORC_LOG_DEBUG(
        "F2SectionCorrection::waitingForSection(): Skipping lead-in section "
        "(TOC harvested).");
    return;
  }

  // If the no timecodes flag is set, the Q-channel timeline is absent or
  // untrustworthy (early CAV discs pre-dating the EFM timecode specification),
  // so no timecode-based checks are possible. Synthesise a monotonic timeline
  // instead: every section receives the expected absolute time and is marked
  // valid. This must happen before the empty-buffer seed check below - on such
  // discs the Q-channel CRC never passes (or DATA-Q is all-zero mode 0), so
  // waiting for a genuinely valid section to seed the timeline would silently
  // discard the entire stream and decode no audio at all.
  if (m_noTimecodes) {
    // For an empty internal buffer this is 00:00:00, seeding the timeline at
    // the start of the stream.
    const SectionTime synthesisedTime = getExpectedAbsoluteTime();
    f2Section.metadata.setAbsoluteSectionTime(synthesisedTime);
    f2Section.metadata.setSectionTime(synthesisedTime);

    // Create a SectionType object and set it to UserData, then pass that to
    // setSectionType
    SectionType st;
    st.setType(SectionType::UserData);
    f2Section.metadata.setSectionType(st,
                                      1);  // Track number 1 for no timecodes

    // The synthesised timeline is authoritative in this mode, so the section
    // is valid regardless of whether its Q-channel decoded.
    f2Section.metadata.setValid(true);

    ORC_LOG_DEBUG(
        "F2SectionCorrection::waitingForSection(): No timecodes flag set, "
        "setting section absolute time to expected time {}",
        synthesisedTime.toString());
  }

  // Check that this isn't the first section in the internal buffer (as we can't
  // calculate the expected time for the first section)
  if (m_internalBuffer.empty()) {
    // Q-7: never seed an empty buffer from a Q-mode 2/3 section. Those sections
    // carry only a frame number (MM:SS are zero) and a fabricated track/time,
    // so using one as the timeline baseline poisons getExpectedAbsoluteTime().
    // Wait for a mode-1 (or mode-4) section to establish the baseline instead.
    const bool isModeWithoutTimeline =
        f2Section.metadata.qMode() == SectionMetadata::QMode2 ||
        f2Section.metadata.qMode() == SectionMetadata::QMode3;
    if (f2Section.metadata.isValid() && !isModeWithoutTimeline) {
      // The internal buffer is empty, so we can just add the section
      m_internalBuffer.push_back(std::move(f2Section));
      ORC_LOG_DEBUG(
          "F2SectionCorrection::waitingForSection(): Added section to internal "
          "buffer with absolute time {}",
          m_internalBuffer.back().metadata.absoluteSectionTime().toString());
      return;
    } else {
      ORC_LOG_DEBUG(
          "F2SectionCorrection::waitingForSection(): Got invalid metadata "
          "section whilst waiting for first section.");
      return;
    }
  }

  // What is the next expected section time?
  SectionTime expectedAbsoluteTime = getExpectedAbsoluteTime();

  // Check for Q-mode 2 and 3 sections - these will only have valid frame
  // numbers in the absolute time (i.e. minutes and seconds will be zero).
  //
  // If found, update the absolute time to the mm:ss expected time (leaving the
  // frame number as-is)
  if (f2Section.metadata.isValid() &&
      (f2Section.metadata.qMode() == SectionMetadata::QMode2 ||
       f2Section.metadata.qMode() == SectionMetadata::QMode3)) {
    // Use the expected time to correct the MM:SS of absolute time (leave the
    // frames as-is, taken from the block's own AFRAME).
    //
    // Q-7: MM:SS comes from the surrounding mode-1 timeline (the expected time)
    // while the frame comes from AFRAME. If a section was lost just before a
    // mode-2/3 block that crosses a second boundary, naively pairing expected
    // MM:SS with AFRAME lands a whole second (75 frames) away from the truth,
    // so the section is dropped as out-of-order or fabricates up to 74 phantom
    // missing sections. Evaluate the reconstruction at expected MM:SS - 1 s,
    // MM:SS and MM:SS + 1 s and keep whichever minimises the frame distance to
    // the expected absolute time.
    const int32_t aFrameNumber =
        f2Section.metadata.absoluteSectionTime().frameNumber();
    constexpr int32_t kFramesPerSecond = 75;  // ECMA-130: 75 sections / second

    SectionTime correctedAbsoluteTime = expectedAbsoluteTime;
    int32_t bestDelta = -1;
    for (int32_t secondOffset = -1; secondOffset <= 1; ++secondOffset) {
      const int32_t baseFrames =
          expectedAbsoluteTime.frames() + secondOffset * kFramesPerSecond;
      if (baseFrames < 0) continue;  // no valid time before 00:00:00
      const SectionTime base(baseFrames);
      const SectionTime candidate(static_cast<uint8_t>(base.minutes()),
                                  static_cast<uint8_t>(base.seconds()),
                                  static_cast<uint8_t>(aFrameNumber));
      const int32_t rawDelta =
          candidate.frames() - expectedAbsoluteTime.frames();
      const int32_t delta = rawDelta < 0 ? -rawDelta : rawDelta;
      if (bestDelta < 0 || delta < bestDelta) {
        bestDelta = delta;
        correctedAbsoluteTime = candidate;
      }
    }

    f2Section.metadata.setAbsoluteSectionTime(correctedAbsoluteTime);

    // Q-2: mode 2/3 blocks encode only the catalogue number / ISRC + AFRAME;
    // they carry no TNO, INDEX or track-relative time (subcode.cpp fabricates
    // track 1 / time 0 at parse time). Inherit the track number, section type
    // and track-relative time from the surrounding mode-1 timeline so the
    // section is attributed to the correct track instead of dragging track 1's
    // statistics to 00:00:00.
    for (int i = static_cast<int>(m_internalBuffer.size()) - 1; i >= 0; --i) {
      if (!m_internalBuffer[i].metadata.isValid()) continue;
      const SectionMetadata& prev = m_internalBuffer[i].metadata;
      const int sectionsSincePrev =
          static_cast<int>(m_internalBuffer.size()) - i;
      f2Section.metadata.setSectionType(prev.sectionType(), prev.trackNumber());
      f2Section.metadata.setSectionTime(prev.sectionTime() + sectionsSincePrev);
      break;
    }

    if (f2Section.metadata.qMode() == SectionMetadata::QMode2) {
      ORC_LOG_DEBUG(
          "F2SectionCorrection::waitingForSection(): Q Mode 2 section "
          "detected, correcting absolute time to {} track {} track-time {}",
          correctedAbsoluteTime.toString(), f2Section.metadata.trackNumber(),
          f2Section.metadata.sectionTime().toString());
    }

    if (f2Section.metadata.qMode() == SectionMetadata::QMode3) {
      ORC_LOG_DEBUG(
          "F2SectionCorrection::waitingForSection(): Q Mode 3 section "
          "detected, correcting absolute time to {} track {} track-time {}",
          correctedAbsoluteTime.toString(), f2Section.metadata.trackNumber(),
          f2Section.metadata.sectionTime().toString());
    }
  }

  // Does the current section have the expected absolute time?
  if (f2Section.metadata.isValid() &&
      f2Section.metadata.absoluteSectionTime() != expectedAbsoluteTime) {
    // The current section is not the expected section
    if (f2Section.metadata.absoluteSectionTime() > expectedAbsoluteTime) {
      // The current section is ahead of the expected section in time, so we
      // have one or more missing sections

      // Note: This will kick up the number of C1/C2 errors in the output.
      // However, some LaserDiscs (like Domesday AIV) have gaps in the EFM data,
      // so there is no actual data loss.  TODO: This could be improved by
      // handling it better in the statistics output.

      // How many sections are missing?
      int32_t missingSections =
          f2Section.metadata.absoluteSectionTime().frames() -
          expectedAbsoluteTime.frames();

      // R-3: guard against fabricating an unbounded number of dummy sections
      // for an implausibly large forward jump (corrupt-but-CRC-valid timestamp
      // or a genuine multi-second dropout). Emit whatever is already provable,
      // drop any unresolved trailing invalid sections, and re-baseline the
      // timeline from this section instead of filling the gap.
      if (missingSections > kMaxMissingSectionFill) {
        ORC_LOG_WARN(
            "F2SectionCorrection::waitingForSection(): Implausible forward "
            "jump "
            "of {} sections (expected {}, got {}); resyncing timeline instead "
            "of filling the gap.",
            missingSections, expectedAbsoluteTime.toString(),
            f2Section.metadata.absoluteSectionTime().toString());
        while (!m_internalBuffer.empty() &&
               m_internalBuffer.front().metadata.isValid()) {
          emitSection();
        }
        // Whatever is left is a short tail of sections whose Q-channel never
        // decoded and which can no longer be bracketed for correction (the
        // anchor on the far side of them has just been declared unrelated).
        // They are discarded, so account for them rather than letting the
        // section count quietly disagree with the timeline.
        m_resyncDiscardedSections +=
            static_cast<uint32_t>(m_internalBuffer.size());
        m_internalBuffer.clear();

        // R-3: the emitted timeline now steps by missingSections at this
        // point. Record it, and mark the new baseline so F2SectionToF1Section
        // recognises the break as deliberate.
        m_timelineResyncs++;
        m_resyncSkippedSections += static_cast<uint32_t>(missingSections);
        f2Section.metadata.setTimelineResync(true);

        m_internalBuffer.push_back(std::move(f2Section));
        return;
      }

      if (missingSections > m_paddingWatermark) {
        // The gap is large - warn the user
        ORC_LOG_WARN(
            "F2SectionCorrection::waitingForSection(): Missing section gap of "
            "{} is larger than 5, expected absolute time is {} actual absolute "
            "time is {}",
            missingSections, expectedAbsoluteTime.toString(),
            f2Section.metadata.absoluteSectionTime().toString());
        ORC_LOG_WARN(
            "F2SectionCorrection::waitingForSection(): Gaps greater than {} "
            "frames will be treated as padding sections (i.e. the decoder "
            "thinks there is a gap in the EFM data rather than actual data "
            "loss).",
            m_paddingWatermark);
      }

      if (missingSections == 1) {
        ORC_LOG_WARN(
            "F2SectionCorrection::waitingForSection(): Missing section "
            "detected, expected absolute time is {} actual absolute time is {}",
            expectedAbsoluteTime.toString(),
            f2Section.metadata.absoluteSectionTime().toString());
      }

      if (missingSections > 1) {
        ORC_LOG_WARN(
            "F2SectionCorrection::waitingForSection(): {} missing sections "
            "detected, expected absolute time is {} actual absolute time is {}",
            missingSections, expectedAbsoluteTime.toString(),
            f2Section.metadata.absoluteSectionTime().toString());
      }

      for (int i = 0; i < missingSections; ++i) {
        // We have to insert a dummy section into the internal buffer or this
        // will throw off the correction process due to the delay lines

        // It's important that all the metadata is correct otherwise track
        // numbers and so on will be incorrect
        F2Section missingSection;

        // Copy the metadata from the next section as a good default
        missingSection.metadata = f2Section.metadata;
        // The resync marker belongs to exactly one real section (R-3); never
        // let a copy carry it.
        missingSection.metadata.setTimelineResync(false);

        missingSection.metadata.setAbsoluteSectionTime(expectedAbsoluteTime +
                                                       i);
        missingSection.metadata.setValid(true);

        // To-do: Perhaps this could be improved if spanning a track boundary?
        // might not be required though...
        missingSection.metadata.setSectionType(
            f2Section.metadata.sectionType(), f2Section.metadata.trackNumber());

        // Q-5: the real section that follows the gap has track time
        // f2Section.sectionTime(). The dummy inserted at gap index i (0-based,
        // i in [0, missingSections)) precedes it by (missingSections - i)
        // sections, so its track time must count *up* to the real section, not
        // down from it. The previous formula (- (i + 1)) made track times run
        // backwards across the gap.
        // Calculate the new frame value before constructing SectionTime to
        // avoid constructing a negative (throwing) SectionTime.
        int32_t newFrames =
            f2Section.metadata.sectionTime().frames() - (missingSections - i);
        if (newFrames >= 0) {
          missingSection.metadata.setSectionTime(SectionTime(newFrames));
        } else {
          missingSection.metadata.setSectionTime(SectionTime(0, 0, 0));
          ORC_LOG_DEBUG(
              "F2SectionCorrection::waitingForSection(): Negative section time "
              "detected, setting section time to 00:00:00");
        }

        // If there are more than m_paddingWatermark missing sections, it's
        // likely that there is a gap in the EFM data so we should flag this as
        // a padding section (this is used downstream to give a better
        // indication of what is really in error).

        // Push 98 error frames in to the missing section
        if (missingSections <= m_paddingWatermark) {
          // Section is considered as missing, so mark it as error
          m_missingSections++;
          ORC_LOG_DEBUG(
              "F2SectionCorrection::waitingForSection(): Inserting missing "
              "section into internal buffer with absolute time: {} - marking "
              "all data as errors",
              missingSection.metadata.absoluteSectionTime().toString());
          for (int i = 0; i < 98; ++i) {
            F2Frame errorFrame;
            errorFrame.setData(std::vector<uint8_t>(32, 0x00));
            errorFrame.setErrorData(
                std::vector<uint8_t>(32, 1));  // Flag as error
            errorFrame.setPaddedData(
                std::vector<uint8_t>(32, 0));  // Not padded
            missingSection.pushFrame(errorFrame);
          }
        } else {
          // Section is considered as padding, so fill it with valid data
          m_paddingSections++;
          ORC_LOG_DEBUG(
              "F2SectionCorrection::waitingForSection(): Inserting missing "
              "section into internal buffer with absolute time: {} - marking "
              "all data as padding",
              missingSection.metadata.absoluteSectionTime().toString());
          for (int i = 0; i < 98; ++i) {
            F2Frame errorFrame;
            // Note: This data pattern will pass C1/C2 error correction
            // resulting in a frame of zeros
            std::vector<uint8_t> data = {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
            errorFrame.setData(data);
            errorFrame.setErrorData(std::vector<uint8_t>(32, 0));
            errorFrame.setPaddedData(std::vector<uint8_t>(32, 1));  // Padded
            missingSection.pushFrame(errorFrame);
          }
        }

        // Push it into the internal buffer
        m_internalBuffer.push_back(missingSection);

        ORC_LOG_DEBUG(
            "F2SectionCorrection::waitingForSection(): Inserted missing "
            "section into internal buffer with absolute time {}",
            missingSection.metadata.absoluteSectionTime().toString());
      }
    } else {
      // The current section is behind the expected section in time, so we have
      // a section that is out of order
      ORC_LOG_WARN(
          "F2SectionCorrection::waitingForSection(): Section out of order "
          "detected, expected absolute time is {} actual absolute time is {}",
          expectedAbsoluteTime.toString(),
          f2Section.metadata.absoluteSectionTime().toString());

      // TODO(sdi): This is not a good way to deal with this...
      outputSection = false;
      m_outOfOrderSections++;
    }
  }

  if (outputSection) m_internalBuffer.push_back(std::move(f2Section));
  processInternalBuffer();
}

// Figure out what absolute time is expected for the next section by looking at
// the internal buffer
SectionTime F2SectionCorrection::getExpectedAbsoluteTime() const {
  SectionTime expectedAbsoluteTime(0, 0, 0);

  // Find the last valid section in the internal buffer
  for (int i = static_cast<int>(m_internalBuffer.size()) - 1; i >= 0; --i) {
    if (m_internalBuffer[i].metadata.isValid()) {
      // The expected time is the time of the last valid section in the internal
      // buffer plus any following sections until the end of the buffer
      expectedAbsoluteTime =
          m_internalBuffer[i].metadata.absoluteSectionTime() +
          (static_cast<int>(m_internalBuffer.size()) - i);
      break;
    }
  }

  return expectedAbsoluteTime;
}

// Process the internal buffer
void F2SectionCorrection::processInternalBuffer() {
  // R-3: bound memory against an extended run of sections whose metadata never
  // becomes valid (a long damaged region where the Q-subcode CRC keeps
  // failing). Precise correction waits for a following valid section to bracket
  // the gap, so such a run would otherwise accumulate in the internal buffer
  // without limit (~10 KB per section -> memory exhaustion on hostile/badly
  // damaged input). Once the trailing invalid run exceeds the correctable gap
  // size it can never be precisely corrected anyway, so forward-fill it with
  // best-effort monotonic metadata from the last known-good section (the same
  // degradation used for an uncorrectable bracketed gap) and let it drain.
  if (!m_internalBuffer.empty() &&
      !m_internalBuffer.back().metadata.isValid()) {
    int lastValid = -1;
    for (int i = static_cast<int>(m_internalBuffer.size()) - 1; i >= 0; --i) {
      if (m_internalBuffer[i].metadata.isValid()) {
        lastValid = i;
        break;
      }
    }
    const int trailingInvalid =
        static_cast<int>(m_internalBuffer.size()) - 1 - lastValid;
    if (lastValid >= 0 && trailingInvalid > m_maximumGapSize) {
      ORC_LOG_WARN(
          "F2SectionCorrection::processInternalBuffer(): {} consecutive "
          "invalid "
          "sections exceed the correctable gap ({}); forward-filling with "
          "best-effort metadata to bound memory.",
          trailingInvalid, m_maximumGapSize);
      const SectionMetadata anchor = m_internalBuffer[lastValid].metadata;
      for (int i = lastValid + 1; i < static_cast<int>(m_internalBuffer.size());
           ++i) {
        const int offset = i - lastValid;
        m_internalBuffer[i].metadata = anchor;
        m_internalBuffer[i].metadata.setAbsoluteSectionTime(
            anchor.absoluteSectionTime() + offset);
        m_internalBuffer[i].metadata.setTrackNumber(anchor.trackNumber());
        m_internalBuffer[i].metadata.setSectionTime(anchor.sectionTime() +
                                                    offset);
        m_internalBuffer[i].metadata.setValid(true);
        m_uncorrectableSections++;
      }
    }
  }

  // Sanity check - there cannot be an invalid section at the start of the
  // buffer
  if (!m_internalBuffer.empty() &&
      !m_internalBuffer.front().metadata.isValid()) {
    ORC_LOG_ERROR(
        "F2SectionCorrection::processInternalBuffer(): Invalid section at "
        "start of internal buffer!");
    throw efm::EfmDecodeError(__func__);
  }

  // Sanity check - there cannot be an invalid section at the end of the buffer
  // so, if there is, exit and wait for more sections
  if (!m_internalBuffer.empty() &&
      !m_internalBuffer.back().metadata.isValid()) {
    return;
  }

  // Sanity check - there must be at least 3 sections in the buffer
  if (m_internalBuffer.size() < 3) {
    ORC_LOG_DEBUG(
        "F2SectionCorrection::correctInternalBuffer(): Not enough sections in "
        "internal buffer to correct.");
    return;
  }

  // Starting from the second section in the buffer, look for an invalid section
  for (int index = 1; index < static_cast<int>(m_internalBuffer.size());
       ++index) {
    // Is the current section invalid?
    int errorStart = -1;
    int errorEnd = -1;

    if (!m_internalBuffer[index].metadata.isValid()) {
      errorStart = index - 1;  // This is the "last known good" section

      // Count how many invalid sections there are before the next valid section
      for (int i = index + 1; i < m_internalBuffer.size(); ++i) {
        if (m_internalBuffer[i].metadata.isValid()) {
          errorEnd = i;
          break;
        }
      }

      int gapLength = errorEnd - errorStart - 1;
      int timeDifference =
          m_internalBuffer[errorEnd].metadata.absoluteSectionTime().frames() -
          m_internalBuffer[errorStart].metadata.absoluteSectionTime().frames() -
          1;

      ORC_LOG_DEBUG(
          "F2SectionCorrection::correctInternalBuffer(): Section metadata "
          "invalid - Error between {} - {} gap length is {} time difference is "
          "{}",
          m_internalBuffer[errorStart]
              .metadata.absoluteSectionTime()
              .toString(),
          m_internalBuffer[errorEnd].metadata.absoluteSectionTime().toString(),
          gapLength, timeDifference);

      // Can we correct the error precisely? We require the number of missing
      // sections to match the timestamp difference across the gap and the gap
      // to be within the conservative interpolation limit. Both a gap larger
      // than the limit and a gap whose length disagrees with the bracketing
      // timestamps are disc conditions (damage, a mastering discontinuity, or a
      // 1/65536 CRC-valid-but-wrong timestamp), not programming errors, so they
      // must degrade gracefully rather than abort the whole decode (R-2).
      if (gapLength == timeDifference && gapLength <= m_maximumGapSize) {
        // We can correct the error
        for (int i = errorStart + 1; i < errorEnd; ++i) {
          SectionMetadata originalMetadata = m_internalBuffer[i].metadata;

          // Firstly copy the metadata from the last known good section to
          // ensure good defaults
          m_internalBuffer[i].metadata = m_internalBuffer[errorStart].metadata;
          m_internalBuffer[i].metadata.setTimelineResync(false);

          // Now set the absolute time for the section
          SectionTime expectedTime =
              m_internalBuffer[errorStart].metadata.absoluteSectionTime() +
              (i - errorStart);
          m_internalBuffer[i].metadata.setAbsoluteSectionTime(expectedTime);

          // Is the track number the same at the start and end of the gap?
          if (m_internalBuffer[errorStart].metadata.trackNumber() !=
              m_internalBuffer[errorEnd].metadata.trackNumber()) {
            ORC_LOG_DEBUG(
                "F2SectionCorrection::correctInternalBuffer(): Gap starts on "
                "track {} and ends on track {}",
                m_internalBuffer[errorStart].metadata.trackNumber(),
                m_internalBuffer[errorEnd].metadata.trackNumber());

            // Work out which side of the track boundary this gap section
            // falls on by projecting error_end's track-relative time backwards
            // to section i. This must be computed as a plain integer: a
            // negative result is meaningful here (it means i precedes the
            // boundary and therefore belongs to error_start's track), but
            // SectionTime rejects negative times and would throw, so we must
            // not build a SectionTime just to test its sign.
            const int32_t endRelativeFrames =
                m_internalBuffer[errorEnd].metadata.sectionTime().frames() -
                (errorEnd - i);

            if (endRelativeFrames >= 0) {
              // Section i is within error_end's (later) track
              m_internalBuffer[i].metadata.setTrackNumber(
                  m_internalBuffer[errorEnd].metadata.trackNumber());
              m_internalBuffer[i].metadata.setSectionTime(
                  SectionTime(endRelativeFrames));
            } else {
              // Section i is within error_start's (earlier) track
              m_internalBuffer[i].metadata.setTrackNumber(
                  m_internalBuffer[errorStart].metadata.trackNumber());
              m_internalBuffer[i].metadata.setSectionTime(
                  m_internalBuffer[errorStart].metadata.sectionTime() +
                  (i - errorStart));
            }

            ORC_LOG_DEBUG(
                "F2SectionCorrection::correctInternalBuffer(): Corrected "
                "section {} across a track boundary to track {} (time {}, "
                "absolute {}); gap spanned tracks {} -> {}",
                i, m_internalBuffer[i].metadata.trackNumber(),
                m_internalBuffer[i].metadata.sectionTime().toString(),
                m_internalBuffer[i].metadata.absoluteSectionTime().toString(),
                m_internalBuffer[errorStart].metadata.trackNumber(),
                m_internalBuffer[errorEnd].metadata.trackNumber());
          } else {
            // The track number is the same, so we can correct the track number
            // by just copying it
            m_internalBuffer[i].metadata.setTrackNumber(
                m_internalBuffer[errorStart].metadata.trackNumber());

            // Same thing for the section time
            m_internalBuffer[i].metadata.setSectionTime(
                m_internalBuffer[errorStart].metadata.sectionTime() +
                (i - errorStart));
          }

          // Mark the corrected metadata as valid
          m_internalBuffer[i].metadata.setValid(true);

          m_correctedSections++;
          ORC_LOG_DEBUG(
              "F2SectionCorrection::correctInternalBuffer(): Corrected section "
              "{} with absolute time {}, Track number {} and track time {} "
              "from original metadata with absolute time {}",
              i, m_internalBuffer[i].metadata.absoluteSectionTime().toString(),
              m_internalBuffer[i].metadata.trackNumber(),
              m_internalBuffer[i].metadata.sectionTime().toString(),
              originalMetadata.absoluteSectionTime().toString());
        }
      } else {
        // R-2: the gap cannot be reconciled precisely (too large, or the
        // section count disagrees with the bracketing timestamps). Rather than
        // aborting the entire decode, degrade: fill the gap with monotonic
        // best-effort metadata derived from the last known-good section so the
        // pipeline keeps flowing. The frame payloads in these sections are
        // already flagged as errors, so downstream treatment is unaffected.
        ORC_LOG_WARN(
            "F2SectionCorrection::processInternalBuffer(): Uncorrectable gap "
            "between {} and {} (gap length {}, time difference {}); filling "
            "with best-effort metadata.",
            m_internalBuffer[errorStart]
                .metadata.absoluteSectionTime()
                .toString(),
            m_internalBuffer[errorEnd]
                .metadata.absoluteSectionTime()
                .toString(),
            gapLength, timeDifference);

        for (int i = errorStart + 1; i < errorEnd; ++i) {
          m_internalBuffer[i].metadata = m_internalBuffer[errorStart].metadata;
          m_internalBuffer[i].metadata.setTimelineResync(false);
          m_internalBuffer[i].metadata.setAbsoluteSectionTime(
              m_internalBuffer[errorStart].metadata.absoluteSectionTime() +
              (i - errorStart));
          m_internalBuffer[i].metadata.setTrackNumber(
              m_internalBuffer[errorStart].metadata.trackNumber());
          m_internalBuffer[i].metadata.setSectionTime(
              m_internalBuffer[errorStart].metadata.sectionTime() +
              (i - errorStart));
          m_internalBuffer[i].metadata.setValid(true);
          m_uncorrectableSections++;
        }
      }
    }
  }

  outputSections();
}

// Drain corrected sections to the output buffer, retaining a small lookahead
// window so later gap corrections still have a "last known good" anchor.
//
// R-3: this previously popped exactly one section per input push, so any large
// gap backlogged the internal buffer until flush() and forced a full-deque
// rescan on every push (O(N) per section, quadratic over gap-heavy captures).
void F2SectionCorrection::outputSections() {
  const size_t lookahead = static_cast<size_t>(m_maximumGapSize) + 2;
  while (m_internalBuffer.size() > lookahead &&
         m_internalBuffer.front().metadata.isValid()) {
    emitSection();
  }
}

// Move the front section to the output buffer and update statistics.
void F2SectionCorrection::emitSection() {
  F2Section section = std::move(m_internalBuffer.front());
  m_internalBuffer.pop_front();

  m_totalSections++;

  // Statistics generation...
  uint8_t trackNumber = section.metadata.trackNumber();
  SectionTime sectionTime = section.metadata.sectionTime();
  SectionTime absoluteTime = section.metadata.absoluteSectionTime();

  if (section.metadata.qMode() == SectionMetadata::QMode::QMode1) {
    m_qmode1Sections++;
  }
  if (section.metadata.qMode() == SectionMetadata::QMode::QMode2) {
    m_qmode2Sections++;
  }
  if (section.metadata.qMode() == SectionMetadata::QMode::QMode3) {
    m_qmode3Sections++;
  }
  if (section.metadata.qMode() == SectionMetadata::QMode::QMode4) {
    m_qmode4Sections++;
  }

  // Set the absolute start and end times
  if (absoluteTime <= m_absoluteStartTime) m_absoluteStartTime = absoluteTime;
  if (absoluteTime > m_absoluteEndTime) m_absoluteEndTime = absoluteTime;

  const SectionMetadata& meta = section.metadata;
  const bool isUserTrack = (trackNumber != 0 && trackNumber != 0xAA);
  const bool isMode1or4 = (meta.qMode() == SectionMetadata::QMode1 ||
                           meta.qMode() == SectionMetadata::QMode4);

  // Media catalogue number (Q-mode 2) is disc-global; keep the first non-empty
  // value seen.
  if (meta.qMode() == SectionMetadata::QMode2 && m_catalogueNumber.empty() &&
      !meta.upcEanCode().empty()) {
    m_catalogueNumber = meta.upcEanCode();
  }

  // Remember the current real user track so a mode-3 ISRC block - whose own
  // track number is a placeholder (Q-2) - is attributed to the track it plays
  // within.
  if (isMode1or4 && isUserTrack) m_currentTrack = trackNumber;

  // ISRC (Q-mode 3) belongs to the current track.
  if (meta.qMode() == SectionMetadata::QMode3 && !meta.isrcCode().empty()) {
    int idx = trackIndex(m_currentTrack);
    if (idx >= 0 && m_trackIsrc[idx].empty()) {
      m_trackIsrc[idx] = meta.isrcCode();
    }
  }

  // Per-track time and control statistics come from real mode-1/4 user tracks;
  // mode-0/2/3 blocks carry placeholder track/time values (Q-1/Q-2).
  if (isMode1or4 && isUserTrack) {
    int idx = trackIndex(trackNumber);
    if (idx < 0) {
      // New track - append to every index-aligned per-track vector.
      m_trackNumbers.push_back(trackNumber);
      m_trackStartTimes.push_back(sectionTime);
      m_trackEndTimes.push_back(sectionTime);
      m_trackAbsStartTimes.push_back(absoluteTime);
      m_trackAbsEndTimes.push_back(absoluteTime);
      m_trackPreemphasis.push_back(meta.hasPreemphasis());
      m_trackPreemphasisVaried.push_back(false);
      m_trackCopyProhibited.push_back(meta.isCopyProhibited());
      m_trackIsAudio.push_back(meta.isAudio());
      m_track2Channel.push_back(meta.is2Channel());
      m_trackIsrc.push_back(std::string());

      ORC_LOG_DEBUG(
          "F2SectionCorrection::emitSection(): New track {} detected with "
          "start time {} (absolute {})",
          trackNumber, sectionTime.toString(), absoluteTime.toString());
    } else {
      if (sectionTime < m_trackStartTimes[idx]) {
        m_trackStartTimes[idx] = sectionTime;
      }
      if (sectionTime >= m_trackEndTimes[idx]) {
        m_trackEndTimes[idx] = sectionTime;
      }
      if (absoluteTime < m_trackAbsStartTimes[idx]) {
        m_trackAbsStartTimes[idx] = absoluteTime;
      }
      if (absoluteTime >= m_trackAbsEndTimes[idx]) {
        m_trackAbsEndTimes[idx] = absoluteTime;
      }
      // Track a per-track pre-emphasis change (rare, but the flag can toggle at
      // a track boundary and, in principle, mid-track).
      if (m_trackPreemphasis[idx] != meta.hasPreemphasis()) {
        m_trackPreemphasisVaried[idx] = true;
        m_trackPreemphasis[idx] = meta.hasPreemphasis();  // last seen
      }
    }
  } else if (!isUserTrack) {
    const auto section_type = section.metadata.sectionType().type();
    const char* type_name = (section_type == SectionType::LeadIn)    ? "LeadIn"
                            : (section_type == SectionType::LeadOut) ? "LeadOut"
                            : (section_type == SectionType::UserData)
                                ? "UserData"
                                : "UNKNOWN";
    ORC_LOG_DEBUG(
        "F2SectionCorrection::emitSection(): {} section with start time {}",
        type_name, sectionTime.toString());
  }

  // Push the section (moved so no deep copy) after the statistics above have
  // finished reading from it.
  m_outputBuffer.push(std::move(section));
}

// Give the trailing invalid sections best-effort metadata continued from the
// last known-good section.
//
// Precise correction brackets a run of invalid sections between two valid ones,
// so a run at the very end of the stream can never be corrected: there is no
// following valid section and never will be. Those sections still hold their
// CRC-failure defaults (UserData, track 0, absolute time 00:00:00 - see
// Subcode::fromData()), and emitting them unchanged pushes that phantom
// position through the rest of the pipeline, where it reaches the WAV
// timeline, the Audacity labels and the decode report.
//
// Forward-fill instead, using the same degradation already applied to an
// over-long or unreconcilable gap in processInternalBuffer(): copy the last
// valid section's metadata and advance the timeline one section at a time.
void F2SectionCorrection::forwardFillTrailingInvalidSections() {
  if (m_internalBuffer.empty()) return;
  if (m_internalBuffer.back().metadata.isValid()) return;

  int lastValid = -1;
  for (int i = static_cast<int>(m_internalBuffer.size()) - 1; i >= 0; --i) {
    if (m_internalBuffer[i].metadata.isValid()) {
      lastValid = i;
      break;
    }
  }

  if (lastValid < 0) {
    // Nothing in the buffer ever decoded, so there is no timeline to continue
    // from. Leave the sections as they are: their metadata stays flagged
    // invalid, which is what downstream needs in order to disregard it.
    ORC_LOG_WARN(
        "F2SectionCorrection::forwardFillTrailingInvalidSections(): {} "
        "trailing section(s) have invalid metadata and there is no valid "
        "section to continue the timeline from; leaving them flagged invalid.",
        m_internalBuffer.size());
    return;
  }

  const SectionMetadata anchor = m_internalBuffer[lastValid].metadata;
  const int filled = static_cast<int>(m_internalBuffer.size()) - 1 - lastValid;
  for (int i = lastValid + 1; i < static_cast<int>(m_internalBuffer.size());
       ++i) {
    const int offset = i - lastValid;
    m_internalBuffer[i].metadata = anchor;
    m_internalBuffer[i].metadata.setTimelineResync(false);
    m_internalBuffer[i].metadata.setAbsoluteSectionTime(
        anchor.absoluteSectionTime() + offset);
    m_internalBuffer[i].metadata.setSectionTime(anchor.sectionTime() + offset);
    m_internalBuffer[i].metadata.setValid(true);
  }

  // Deliberately NOT counted as uncorrectable sections. A run longer than the
  // correctable gap has already been forward-filled and charged as
  // uncorrectable by processInternalBuffer(), so whatever survives to here is
  // short by construction and is an artefact of the stream ending rather than
  // a defect in the disc. It is reported separately so it stays visible.
  m_tailFilledSections += static_cast<uint32_t>(filled);
  ORC_LOG_DEBUG(
      "F2SectionCorrection::forwardFillTrailingInvalidSections(): "
      "forward-filled {} trailing section(s) from absolute time {}",
      filled, anchor.absoluteSectionTime().toString());
}

void F2SectionCorrection::flush() {
  // The end-of-stream tail may still hold sections whose metadata never became
  // valid; give them a continued timeline before they are emitted.
  forwardFillTrailingInvalidSections();

  // Flush the entire internal buffer.
  while (!m_internalBuffer.empty()) {
    emitSection();
  }
}

void F2SectionCorrection::setNoTimecodes(bool noTimecodes) {
  m_noTimecodes = noTimecodes;
}

void F2SectionCorrection::showStatistics() const {
  ORC_LOG_INFO("F2 Section Metadata Correction statistics:");
  ORC_LOG_INFO("  F2 Sections:");
  ORC_LOG_INFO("    Total: {} ({} F2)", m_totalSections, m_totalSections * 98);
  ORC_LOG_INFO("    Corrected: {}", m_correctedSections);
  ORC_LOG_INFO("    Uncorrectable: {}", m_uncorrectableSections);
  ORC_LOG_INFO("    Pre-Leadin: {}", m_preLeadinSections);
  ORC_LOG_INFO("    Missing: {}", m_missingSections);
  ORC_LOG_INFO("    Padding: {}", m_paddingSections);
  ORC_LOG_INFO("    Out of order: {}", m_outOfOrderSections);
  ORC_LOG_INFO("    Timeline resyncs: {} ({} section(s) skipped, {} discarded)",
               m_timelineResyncs, m_resyncSkippedSections,
               m_resyncDiscardedSections);

  ORC_LOG_INFO("  QMode Sections:");
  ORC_LOG_INFO("    QMode 1 (CD Data): {}", m_qmode1Sections);
  ORC_LOG_INFO("    QMode 2 (Catalogue No.): {}", m_qmode2Sections);
  ORC_LOG_INFO("    QMode 3 (ISO 3901 ISRC): {}", m_qmode3Sections);
  ORC_LOG_INFO("    QMode 4 (LD Data): {}", m_qmode4Sections);

  // Q-6: lead-in TOC (IEC 60908 §17.5.1)
  ORC_LOG_INFO("  Lead-in TOC:");
  ORC_LOG_INFO("    Lead-in sections: {}", m_leadinSections);
  if (m_hasToc) {
    ORC_LOG_INFO("    First track: {}", m_tocFirstTrack);
    ORC_LOG_INFO("    Last track: {}", m_tocLastTrack);
    ORC_LOG_INFO("    Lead-out start: {}", m_tocLeadOutStart.toString());
    for (size_t i = 0; i < m_tocTrackNumbers.size(); ++i) {
      ORC_LOG_INFO("    Track {} start: {}", m_tocTrackNumbers[i],
                   m_tocTrackStartTimes[i].toString());
    }
  } else {
    ORC_LOG_INFO("    No lead-in TOC decoded (capture starts after lead-in).");
  }

  ORC_LOG_INFO("  Absolute Time:");
  ORC_LOG_INFO("    Start time: {}", m_absoluteStartTime.toString());
  ORC_LOG_INFO("    End time: {}", m_absoluteEndTime.toString());
  // Only calculate duration if we have valid sections (otherwise start time >
  // end time)
  if (m_totalSections > 0 && m_absoluteEndTime >= m_absoluteStartTime) {
    ORC_LOG_INFO("    Duration: {}",
                 (m_absoluteEndTime - m_absoluteStartTime).toString());
  } else {
    ORC_LOG_INFO("    Duration: N/A");
  }

  // Show each track in order of appearance
  for (int i = 0; i < m_trackNumbers.size(); i++) {
    ORC_LOG_INFO("  Track {}:", m_trackNumbers[i]);
    ORC_LOG_INFO("    Start time: {}", m_trackStartTimes[i].toString());
    ORC_LOG_INFO("    End time: {}", m_trackEndTimes[i].toString());
    // Only calculate duration if end time is valid
    if (m_trackEndTimes[i] >= m_trackStartTimes[i]) {
      ORC_LOG_INFO("    Duration: {}",
                   (m_trackEndTimes[i] - m_trackStartTimes[i]).toString());
    } else {
      ORC_LOG_INFO("    Duration: N/A");
    }
  }
}
