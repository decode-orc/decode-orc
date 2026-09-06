/*
 * File:        dec_f3frametof2section.cpp
 * Purpose:     efm-decoder-f2 - EFM T-values to F2 Section decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dec_f3frametof2section.h"

#include <orc/support/logging.h>

#include <cmath>
#include <cstddef>

#include "efm_constants.h"
#include "efm_exception.h"

F3FrameToF2Section::F3FrameToF2Section()
    : m_badSyncCounter(0),
      m_lastSectionMetadata(SectionMetadata()),
      m_currentState(ExpectingInitialSync),
      m_inputF3Frames(0),
      m_presyncDiscardedF3Frames(0),
      m_goodSync0(0),
      m_missingSync0(0),
      m_undershootSync0(0),
      m_overshootSync0(0),
      m_discardedF3Frames(0),
      m_paddedF3Frames(0),
      m_lostSyncCounter(0),
      m_headLostF3Frames(0),
      m_firstSectionOutput(false) {}

void F3FrameToF2Section::noteHeadLoss(uint64_t frames) {
  if (m_firstSectionOutput) return;
  m_headLostF3Frames += frames;
}

void F3FrameToF2Section::pushFrame(const F3Frame& data) {
  m_internalBuffer.push_back(data);
  m_inputF3Frames++;
  processStateMachine();
}

void F3FrameToF2Section::pushFrame(F3Frame&& data) {
  // Move the frame into the internal buffer to avoid a deep copy.
  m_internalBuffer.push_back(std::move(data));
  m_inputF3Frames++;
  processStateMachine();
}

F2Section F3FrameToF2Section::popSection() {
  // Move the first item out of the output buffer to avoid a deep copy.
  F2Section section = std::move(m_outputBuffer.front());
  m_outputBuffer.pop();
  return section;
}

bool F3FrameToF2Section::isReady() const { return !m_outputBuffer.empty(); }

void F3FrameToF2Section::processStateMachine() {
  if (m_internalBuffer.size() > 1) {
    switch (m_currentState) {
      case ExpectingInitialSync:
        m_currentState = expectingInitialSync();
        break;
      case ExpectingSync:
        m_currentState = expectingSync();
        break;
      case HandleValid:
        m_currentState = handleValid();
        break;
      case HandleUndershoot:
        m_currentState = handleUndershoot();
        break;
      case HandleOvershoot:
        m_currentState = handleOvershoot();
        break;
      case LostSync:
        m_currentState = lostSync();
        break;
    }
  }
}

F3FrameToF2Section::State F3FrameToF2Section::expectingInitialSync() {
  State nextState = ExpectingInitialSync;

  // Does the internal buffer contain a sync0 frame?
  // Note: For the initial sync we are only using sync0 frames
  bool foundSync0 = false;
  for (size_t i = 0; i < m_internalBuffer.size(); ++i) {
    if (m_internalBuffer[i].f3FrameType() == F3Frame::Sync0) {
      m_presyncDiscardedF3Frames += i;
      noteHeadLoss(i);
      // Discard all frames before the sync0 frame
      m_internalBuffer = std::vector<F3Frame>(
          m_internalBuffer.begin() + static_cast<std::ptrdiff_t>(i),
          m_internalBuffer.end());
      foundSync0 = true;
      break;
    }
  }

  if (foundSync0) {
    ORC_LOG_DEBUG(
        "F3FrameToF2Section::expectingInitialSync - Found sync0 frame after "
        "discarding {} frames",
        m_presyncDiscardedF3Frames);
    m_presyncDiscardedF3Frames = 0;
    nextState = ExpectingSync;
  } else {
    m_presyncDiscardedF3Frames += m_internalBuffer.size();
    noteHeadLoss(m_internalBuffer.size());
    m_internalBuffer.clear();
  }

  return nextState;
}

F3FrameToF2Section::State F3FrameToF2Section::expectingSync() {
  State nextState = ExpectingSync;

  // Did we receive a sync0 frame?
  if (m_internalBuffer.back().f3FrameType() == F3Frame::Sync0) {
    // Extract the section frames and remove them from the internal buffer
    m_sectionFrames = std::vector<F3Frame>(m_internalBuffer.begin(),
                                           m_internalBuffer.end() - 1);
    m_internalBuffer = std::vector<F3Frame>(m_internalBuffer.end() - 1,
                                            m_internalBuffer.end());
  } else if (m_internalBuffer.back().f3FrameType() == F3Frame::Sync1) {
    // Is the previous frame a sync0 frame?
    if (m_internalBuffer.size() > 1 &&
        m_internalBuffer[m_internalBuffer.size() - 2].f3FrameType() ==
            F3Frame::Sync0) {
      // Keep waiting for a sync0 frame
      nextState = ExpectingSync;
      return nextState;
    } else {
      // Looks like we got a sync1 frame without a sync0 frame - make the
      // previous frame sync0 and process
      m_missingSync0++;
      m_internalBuffer[m_internalBuffer.size() - 2].setFrameTypeAsSync0();

      // Extract the section frames and remove them from the internal buffer,
      // keeping both the repaired sync0 frame and the sync1 frame.
      m_sectionFrames = std::vector<F3Frame>(m_internalBuffer.begin(),
                                             m_internalBuffer.end() - 2);
      m_internalBuffer = std::vector<F3Frame>(m_internalBuffer.end() - 2,
                                              m_internalBuffer.end());
      ORC_LOG_DEBUG(
          "F3FrameToF2Section::expectingSync - Got sync1 frame without a sync0 "
          "frame - section frame size is {}",
          m_sectionFrames.size());
    }
  } else {
    // Keep waiting for a sync0 frame
    nextState = ExpectingSync;
    return nextState;
  }

  // Do we have a valid number of frames in the section?
  // Or do we have overshoot or undershoot?
  if (m_sectionFrames.size() == efm::kFramesPerSection) {
    m_goodSync0++;
    nextState = HandleValid;
  } else if (m_sectionFrames.size() < efm::kFramesPerSection) {
    m_undershootSync0++;
    nextState = HandleUndershoot;
  } else if (m_sectionFrames.size() > efm::kFramesPerSection) {
    m_overshootSync0++;
    nextState = HandleOvershoot;
  }

  // Have we hit the bad sync limit?
  if (m_badSyncCounter > 3) {
    nextState = LostSync;
  }

  return nextState;
}

F3FrameToF2Section::State F3FrameToF2Section::handleValid() {
  State nextState = ExpectingSync;

  // Output the section
  outputSection(false);

  // Reset the bad sync counter
  m_badSyncCounter = 0;

  nextState = ExpectingSync;
  return nextState;
}

F3FrameToF2Section::State F3FrameToF2Section::handleUndershoot() {
  State nextState = HandleUndershoot;
  m_badSyncCounter++;

  // How much undershoot do we have?
  int padding =
      efm::kFramesPerSection - static_cast<int>(m_sectionFrames.size());

  if (padding > 4) {
    ORC_LOG_DEBUG(
        "F3FrameToF2Section::handleUndershoot - Undershoot is {} frames; "
        "ignoring sync0 frame",
        padding);
    // Put the section frames back into the internal buffer, BEFORE the
    // pending sync frame(s) already there: the put-back frames precede the
    // sync in the frame stream.
    m_internalBuffer.insert(m_internalBuffer.begin(), m_sectionFrames.begin(),
                            m_sectionFrames.end());
    m_sectionFrames.clear();
    nextState = ExpectingSync;
  } else {
    m_paddedF3Frames += padding;
    ORC_LOG_DEBUG(
        "F3FrameToF2Section::handleUndershoot - Padding section with {} frames",
        padding);

    // If we are padding, we are introducing errors that the CIRC must later
    // absorb.
    F3Frame emptyFrame;
    emptyFrame.setData(std::vector<uint8_t>(32, 0));
    emptyFrame.setErrorData(std::vector<uint8_t>(32, 1));
    emptyFrame.setPaddedData(std::vector<uint8_t>(32, 0));
    emptyFrame.setFrameTypeAsSubcode(0);

    // The padding frames are inserted as one contiguous block starting at
    // position 4 (immediately after the sync0 and sync1 frames, which must stay
    // at the front of the section).
    for (int i = 0; i < padding; ++i) {
      m_sectionFrames.insert(m_sectionFrames.begin() + 4 + i, emptyFrame);
    }

    outputSection(true);
  }

  nextState = ExpectingSync;
  return nextState;
}

F3FrameToF2Section::State F3FrameToF2Section::handleOvershoot() {
  State nextState = HandleOvershoot;

  // How many sections worth of data do we have?
  int frameCount =
      static_cast<int>(m_sectionFrames.size() / efm::kFramesPerSection);
  int remainder =
      static_cast<int>(m_sectionFrames.size() % efm::kFramesPerSection);
  ORC_LOG_DEBUG(
      "F3FrameToF2Section::handleOvershoot - Got {} frames, which is {} "
      "sections with a remainder of {} frames",
      m_sectionFrames.size(), frameCount, remainder);

  if (frameCount == 1) {
    // Delete frames from the start of the section buffer to make it 98 frames
    m_discardedF3Frames += remainder;
    noteHeadLoss(static_cast<uint64_t>(remainder));
    m_sectionFrames = std::vector<F3Frame>(m_sectionFrames.begin() + remainder,
                                           m_sectionFrames.end());
    outputSection(true);
  } else {
    // Remove any frames that are not part of a complete section from the
    // beginning of the section buffer
    m_discardedF3Frames += remainder;
    noteHeadLoss(static_cast<uint64_t>(remainder));
    m_sectionFrames = std::vector<F3Frame>(m_sectionFrames.begin() + remainder,
                                           m_sectionFrames.end());

    // Break the section buffer into 98 frame sections and output them
    std::vector<F3Frame> tempSectionFrames = m_sectionFrames;
    for (int i = 0; i < frameCount; ++i) {
      m_sectionFrames = std::vector<F3Frame>(
          tempSectionFrames.begin(),
          tempSectionFrames.begin() + efm::kFramesPerSection);
      tempSectionFrames = std::vector<F3Frame>(
          tempSectionFrames.begin() + efm::kFramesPerSection,
          tempSectionFrames.end());
      outputSection(true);
    }
  }

  // Each missed sync is a bad sync
  m_badSyncCounter += frameCount;

  nextState = ExpectingSync;
  return nextState;
}

F3FrameToF2Section::State F3FrameToF2Section::lostSync() {
  State nextState = ExpectingInitialSync;
  ORC_LOG_DEBUG("F3FrameToF2Section::lostSync - Lost section sync");
  m_lostSyncCounter++;
  m_badSyncCounter = 0;
  noteHeadLoss(m_internalBuffer.size() + m_sectionFrames.size());
  m_internalBuffer.clear();
  m_sectionFrames.clear();
  return nextState;
}

void F3FrameToF2Section::outputSection(bool showAddress) {
  if (m_sectionFrames.size() != efm::kFramesPerSection) {
    ORC_LOG_ERROR(
        "F3FrameToF2Section::outputSection - Section size is {} not 98",
        m_sectionFrames.size());
    throw efm::EfmDecodeError(__func__);
  }

  Subcode subcode;

  std::vector<uint8_t> subcodeData;
  subcodeData.reserve(
      efm::kFramesPerSection);  // P-7: one subcode byte per frame
  for (size_t i = 0; i < efm::kFramesPerSection; ++i) {
    subcodeData.push_back(m_sectionFrames[i].subcodeByte());
  }
  SectionMetadata sectionMetadata = subcode.fromData(subcodeData);

  F2Section f2Section;
  for (uint32_t index = 0; index < efm::kFramesPerSection; ++index) {
    F2Frame f2Frame;
    f2Frame.setData(m_sectionFrames[index].data());
    f2Frame.setErrorData(m_sectionFrames[index].errorData());
    // Issue #307: carry the producer's per-symbol doubt down to the CIRC,
    // which seeds C1/C2 erasures from it. Empty means "all trusted" and is
    // passed on as such (see Frame::doubtData()).
    const std::vector<uint8_t>& doubtData = m_sectionFrames[index].doubtData();
    if (!doubtData.empty()) f2Frame.setDoubtData(doubtData);
    f2Section.pushFrame(f2Frame);
  }

  // There is an edge case where a repaired Q-channel will pass CRC, but the
  // data is still invalid This is a sanity check for that case
  if (sectionMetadata.isRepaired()) {
    // Check the absolute time is within 10 frames of the last section
    // Compare raw frame values directly to avoid creating invalid SectionTime
    // objects through subtraction
    int32_t currentFrames = sectionMetadata.absoluteSectionTime().frames();
    int32_t lastFrames = m_lastSectionMetadata.absoluteSectionTime().frames();
    int32_t timeDiff = std::abs(currentFrames - lastFrames);
    if (timeDiff > 10) {
      ORC_LOG_DEBUG(
          "WARNING: F3FrameToF2Section::outputSection - Repaired section has a "
          "large time difference from the last section - marking as invalid");
      sectionMetadata.setValid(false);
    }
  }

  f2Section.metadata = sectionMetadata;
  // Only update last section metadata if this section is valid
  if (sectionMetadata.isValid()) {
    m_lastSectionMetadata = sectionMetadata;
  }
  m_firstSectionOutput = true;
  m_outputBuffer.push(f2Section);

  if (showAddress) {
    ORC_LOG_DEBUG(
        "F3FrameToF2Section::outputSection - Outputting F2 section with "
        "address {}",
        sectionMetadata.absoluteSectionTime().toString());
  }
}

void F3FrameToF2Section::showStatistics() const {
  ORC_LOG_INFO("F3 Frame to F2 Section statistics:");
  ORC_LOG_INFO("  F3 Frames:");
  ORC_LOG_INFO("    Input frames: {}", m_inputF3Frames);
  ORC_LOG_INFO("    Good sync0 frames: {}", m_goodSync0);
  ORC_LOG_INFO("    Missing sync0 frames: {}", m_missingSync0);
  ORC_LOG_INFO("    Undershoot sync0 frames: {}", m_undershootSync0);
  ORC_LOG_INFO("    Overshoot sync0 frames: {}", m_overshootSync0);
  ORC_LOG_INFO("    Lost sync: {}", m_lostSyncCounter);
  ORC_LOG_INFO("  Frame loss:");
  ORC_LOG_INFO("    Presync discarded F3 frames: {}",
               m_presyncDiscardedF3Frames);
  ORC_LOG_INFO("    Discarded F3 frames: {}", m_discardedF3Frames);
  ORC_LOG_INFO("    Padded F3 frames: {}", m_paddedF3Frames);
}
