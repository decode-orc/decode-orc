/*
 * File:        dec_f2sectiontof1section.cpp
 * Purpose:     ld-efm-decoder - EFM data decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dec_f2sectiontof1section.h"

#include <fmt/core.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cstdlib>

#include "../efm-lib/efm_constants.h"
#include "../efm-lib/efm_exception.h"

F2SectionToF1Section::F2SectionToF1Section()
    : m_delayLine1({0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
                    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1}),
      m_delayLine2({0, 0, 0, 0, 2, 2, 2, 2, 0, 0, 0, 0,
                    2, 2, 2, 2, 0, 0, 0, 0, 2, 2, 2, 2}),
      m_delayLineM({108, 104, 100, 96, 92, 88, 84, 80, 76, 72, 68, 64, 60, 56,
                    52,  48,  44,  40, 36, 32, 28, 24, 20, 16, 12, 8,  4,  0}),
      m_invalidInputF2FramesCount(0),
      m_validInputF2FramesCount(0),
      m_invalidOutputF1FramesCount(0),
      m_validOutputF1FramesCount(0),
      m_dlLostFramesCount(0),
      m_warmupLostFramesCount(0),
      m_drainLostFramesCount(0),
      m_continuityErrorCount(0),
      m_inputByteErrors(0),
      m_outputByteErrors(0),
      m_invalidPaddedF1FramesCount(0),
      m_invalidNonPaddedF1FramesCount(0),
      m_lastFrameNumber(-1),
      m_haveSectionMetadata(false),
      m_draining(false) {}

void F2SectionToF1Section::pushSection(const F2Section& f2Section) {
  // Add the data to the input buffer
  m_inputBuffer.push_back(f2Section);

  // Process the queue
  processQueue();
}

void F2SectionToF1Section::pushSection(F2Section&& f2Section) {
  // Move the data into the input buffer to avoid a deep copy.
  m_inputBuffer.push_back(std::move(f2Section));

  // Process the queue
  processQueue();
}

F1Section F2SectionToF1Section::popSection() {
  // Move the first item out of the output buffer to avoid a deep copy.
  F1Section section = std::move(m_outputBuffer.front());
  m_outputBuffer.pop_front();
  return section;
}

bool F2SectionToF1Section::isReady() const {
  // Return true if the output buffer is not empty
  return !m_outputBuffer.empty();
}

// Note: The F2 frames will not be correct until the delay lines are full
// So lead-in is required to prevent loss of the input date.  For now we will
// just discard the data until the delay lines are full.
void F2SectionToF1Section::processQueue() {
  // Process the input buffer
  while (!m_inputBuffer.empty()) {
    F2Section f2Section = m_inputBuffer.front();
    m_inputBuffer.pop_front();
    F1Section f1Section;

    // Sanity check the F2 section
    if (!f2Section.isComplete()) {
      ORC_LOG_CRITICAL(
          "F2SectionToF1Section::processQueue - F2 Section is not complete");
      ORC_LOG_CRITICAL(
          "This usually indicates a stream error or EOF while reading from "
          "stdin in a pipeline");
      ORC_LOG_CRITICAL(
          "Check that the input EFM stream is properly formatted and complete");
      throw efm::EfmDecodeError(__func__);
    }

    // Check section continuity
    if (m_lastFrameNumber != -1) {
      if (f2Section.metadata.absoluteSectionTime().frames() !=
          m_lastFrameNumber + 1) {
        ORC_LOG_WARN(
            "F2 Section continuity error last frame: {} current frame: {}",
            m_lastFrameNumber,
            f2Section.metadata.absoluteSectionTime().frames());
        ORC_LOG_WARN("Last section time: {}",
                     f2Section.metadata.absoluteSectionTime().toString());
        ORC_LOG_WARN(
            "This is a bug in the F2 Metadata correction and should be "
            "reported");
        m_continuityErrorCount++;
      }
      m_lastFrameNumber = f2Section.metadata.absoluteSectionTime().frames();
    } else {
      m_lastFrameNumber = f2Section.metadata.absoluteSectionTime().frames();
    }

    for (int index = 0; index < efm::kFramesPerSection; index++) {
      const F2Frame& f2Frame = f2Section.frame(index);
      std::vector<uint8_t> data = f2Frame.data();
      std::vector<uint8_t> errorData = f2Frame.errorData();
      std::vector<uint8_t> paddedData = f2Frame.paddedData();
      // Issue #307: an empty doubt vector means the producer trusted every
      // symbol (see Frame::doubtData()); the CIRC needs one entry per symbol
      // either way, because the delay lines must carry it in lockstep.
      const std::vector<uint8_t>& frameDoubt = f2Frame.doubtData();
      if (frameDoubt.size() == data.size()) {
        m_doubtScratch.assign(frameDoubt.begin(), frameDoubt.end());
      } else {
        m_doubtScratch.assign(data.size(), 0);
      }

      // Check F2 frame for errors (counts only when errorData = 1)
      uint32_t inFrameErrors = f2Frame.countErrors();
      if (inFrameErrors == 0) {
        m_validInputF2FramesCount++;
      } else {
        m_invalidInputF2FramesCount++;
        m_inputByteErrors += inFrameErrors;
      }

      processF2FrameData(std::move(data), std::move(errorData),
                         std::move(paddedData), m_doubtScratch, f1Section);
    }

    // All frames in the section are processed
    f1Section.metadata = f2Section.metadata;

    // Remember the metadata so flush() can synthesise continuing metadata for
    // the tail sections it drains out of the delay lines (E-7).
    m_lastSectionMetadata = f2Section.metadata;
    m_haveSectionMetadata = true;

    // Add the section to the output buffer
    m_outputBuffer.push_back(f1Section);
  }
}

void F2SectionToF1Section::pushSubstituteF1Frame(F1Section& f1Section) {
  // E-7: fabricated filler emitted while the CIRC delay lines fill (warm up) or
  // as trailing padding once the genuine tail has been drained. Not genuine
  // decoded data, so mark it padded=1 rather than passing zeros off as valid
  // data downstream.
  F1Frame f1Frame;
  f1Frame.setData(std::vector<uint8_t>(24, 0));
  f1Frame.setErrorData(std::vector<uint8_t>(24, 0));
  f1Frame.setPaddedData(std::vector<uint8_t>(24, 1));
  f1Section.pushFrame(f1Frame);
  m_dlLostFramesCount++;
  // Substitutes are only ever emitted while the delay lines are filling: by the
  // time flush() runs they are full, so every frame it produces is a decoded
  // (but filler-contaminated) frame rather than a substitute. Drain frames are
  // therefore counted in flush() itself, not here.
  if (!m_draining) m_warmupLostFramesCount++;
}

void F2SectionToF1Section::processF2FrameData(std::vector<uint8_t> data,
                                              std::vector<uint8_t> errorData,
                                              std::vector<uint8_t> paddedData,
                                              std::vector<uint8_t>& doubtData,
                                              F1Section& f1Section) {
  m_delayLine1.push(data, errorData, paddedData, doubtData);
  if (data.empty()) {
    pushSubstituteF1Frame(f1Section);
    return;
  }

  // Process the data
  // Note: We will only get valid data if the delay lines are all full
  m_inverter.invertParity(data);

  m_circ.c1Decode(data, errorData, paddedData, doubtData);

  m_delayLineM.push(data, errorData, paddedData, doubtData);
  if (data.empty()) {
    pushSubstituteF1Frame(f1Section);
    return;
  }

  // Only perform C2 decode if delay line 1 is full and delay line M is full
  m_circ.c2Decode(data, errorData, paddedData, doubtData);

  if (std::any_of(errorData.begin(), errorData.end(),
                  [](uint8_t value) { return value != 0; })) {
    ORC_LOG_DEBUG("F2SectionToF1Section - F2 Frame: C2 Failed");
  }

  m_interleave.deinterleave(data, errorData, paddedData, doubtData);

  m_delayLine2.push(data, errorData, paddedData, doubtData);
  if (data.empty()) {
    pushSubstituteF1Frame(f1Section);
    return;
  }

  // Put the resulting data (and error data) into an F1 frame and
  // push it to the output buffer
  F1Frame f1Frame;
  f1Frame.setData(data);
  f1Frame.setErrorData(errorData);
  f1Frame.setPaddedData(paddedData);

  // Check F1 frame for errors
  // Note: The error C2 count will differ from the overall error F1 count
  // due to the the interleaving which will distribute the errors over more
  // than on frame (potentially)
  uint32_t outFrameErrors = f1Frame.countErrors();
  uint32_t outFramePadding = f1Frame.countPadded();

  if (outFrameErrors == 0 && outFramePadding == 0) {
    m_validOutputF1FramesCount++;
  } else {
    m_invalidOutputF1FramesCount++;
    m_outputByteErrors += outFrameErrors;

    // Invalid with or without padding?
    if (outFramePadding > 0) {
      m_invalidPaddedF1FramesCount++;
    } else {
      m_invalidNonPaddedF1FramesCount++;
    }
  }

  f1Section.pushFrame(f1Frame);
}

void F2SectionToF1Section::flush() {
  // E-7: the CIRC delay lines introduce a latency of
  // delayLine1 (max 1) + delayLineM (max 108) + delayLine2 (max 2) = 111 F2
  // frames between input and output. At end of stream that many genuine frames
  // are still trapped inside the delay lines; without a flush they are silently
  // dropped (the truncated-capture tail). Push padding frames through the chain
  // so the trapped genuine frames are carried out as F1 frames. The last frames
  // to emerge are our own padding, marked padded=1 (concealed as silence).
  constexpr int32_t kFramesPerSection = efm::kFramesPerSection;
  // Round up to whole sections so downstream always receives complete sections.
  constexpr int32_t kFlushSections =
      (efm::kDeinterleaveLatencyF1Frames + kFramesPerSection - 1) /
      kFramesPerSection;  // 2

  // Nothing to drain if no genuine section was ever processed (e.g. a stream
  // too short to fill the delay lines) — there is no tail to recover and no
  // metadata to continue from.
  if (!m_haveSectionMetadata) {
    return;
  }

  // Continue the section timeline from the last real section.
  SectionMetadata metadata = m_lastSectionMetadata;

  m_draining = true;
  for (int32_t section = 0; section < kFlushSections; ++section) {
    // Advance the (absolute) section time by one 1/75 s frame per section so
    // the flushed sections remain contiguous with the decoded stream.
    metadata.setAbsoluteSectionTime(metadata.absoluteSectionTime() + 1);
    metadata.setSectionTime(metadata.sectionTime() + 1);

    F1Section f1Section;
    for (int32_t index = 0; index < kFramesPerSection; ++index) {
      // Feed an all-padding F2 frame (32 symbols) into the chain.
      m_doubtScratch.assign(32, 0);
      processF2FrameData(
          std::vector<uint8_t>(32, 0), std::vector<uint8_t>(32, 0),
          std::vector<uint8_t>(32, 1), m_doubtScratch, f1Section);
    }
    f1Section.metadata = metadata;
    m_outputBuffer.push_back(f1Section);
    // Every frame emitted by the drain is filler-contaminated: it was assembled
    // from codewords that contained the padding pushed through above.
    m_drainLostFramesCount += kFramesPerSection;
  }
  m_draining = false;
}

void F2SectionToF1Section::showData(const std::string& description,
                                    int32_t index,
                                    const std::string& timeString,
                                    std::vector<uint8_t>& data,
                                    std::vector<uint8_t>& dataError) {
  // Early return if no errors to avoid string processing
  bool hasError = false;
  for (uint8_t error : dataError) {
    if (error) {
      hasError = true;
      break;
    }
  }
  if (!hasError) return;

  // Pre-allocate string with approximate size needed
  std::string dataString;
  dataString.reserve(data.size() * 3);

  // Process data only if we know we need to display it
  for (size_t i = 0; i < data.size(); ++i) {
    if (!dataError[i]) {
      dataString.append(fmt::format("{:02x} ", data[i]));
    } else {
      dataString.append("XX ");
    }
  }

  ORC_LOG_DEBUG("F2SectionToF1Section - {}[{:02d}]: ({}) {}XX=ERROR",
                description, index, timeString, dataString);
}

void F2SectionToF1Section::showStatistics() const {
  ORC_LOG_INFO("F2 Section to F1 Section statistics:");
  ORC_LOG_INFO("  Input F2 Frames:");
  ORC_LOG_INFO("    Valid frames: {}", m_validInputF2FramesCount);
  ORC_LOG_INFO("    Corrupt frames: {} frames containing {} byte errors",
               m_invalidInputF2FramesCount, m_inputByteErrors);
  ORC_LOG_INFO("    Delay line lost frames: {}", m_dlLostFramesCount);
  ORC_LOG_INFO("      of which warm-up: {}", m_warmupLostFramesCount);
  ORC_LOG_INFO("      of which drain: {}", m_drainLostFramesCount);
  ORC_LOG_INFO("    Continuity errors: {}", m_continuityErrorCount);

  ORC_LOG_INFO("  Output F1 Frames (after CIRC):");
  ORC_LOG_INFO("    Valid frames: {}", m_validOutputF1FramesCount);
  ORC_LOG_INFO("    Invalid frames due to padding: {}",
               m_invalidPaddedF1FramesCount);
  ORC_LOG_INFO("    Invalid frames without padding: {}",
               m_invalidNonPaddedF1FramesCount);
  ORC_LOG_INFO("    Invalid frames (total): {}", m_invalidOutputF1FramesCount);
  ORC_LOG_INFO("    Output byte errors: {}", m_outputByteErrors);

  ORC_LOG_INFO("  C1 decoder (fully-populated codewords):");
  ORC_LOG_INFO("    Valid C1s: {}", m_circ.validC1s());
  ORC_LOG_INFO("    Fixed C1s: {}", m_circ.fixedC1s());
  ORC_LOG_INFO("    Error C1s: {}", m_circ.errorC1s());
  ORC_LOG_INFO("    Padded C1s (warm-up/drain, not scored): {}",
               m_circ.paddedC1s());

  ORC_LOG_INFO("  C2 decoder (fully-populated codewords):");
  ORC_LOG_INFO("    Valid C2s: {}", m_circ.validC2s());
  ORC_LOG_INFO("    Fixed C2s: {}", m_circ.fixedC2s());
  ORC_LOG_INFO("    Error C2s: {}", m_circ.errorC2s());
  ORC_LOG_INFO("    Padded C2s (warm-up/drain, not scored): {}",
               m_circ.paddedC2s());

  // Issue #307: erasures raised on the producer's doubt alone.
  ORC_LOG_INFO("  Producer-doubt erasures:");
  ORC_LOG_INFO("    C1: {} in {} codewords", m_circ.doubtErasuresC1(),
               m_circ.doubtSeededC1s());
  ORC_LOG_INFO("    C2: {} in {} codewords", m_circ.doubtErasuresC2(),
               m_circ.doubtSeededC2s());
}
