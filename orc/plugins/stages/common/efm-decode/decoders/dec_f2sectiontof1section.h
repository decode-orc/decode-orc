/*
 * File:        dec_f2sectiontof1section.h
 * Purpose:     ld-efm-decoder - EFM data decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef DEC_F2SECTIONTOF1SECTION_H
#define DEC_F2SECTIONTOF1SECTION_H

#include <string>

#include "decoders.h"
#include "delay_lines.h"
#include "interleave.h"
#include "inverter.h"
#include "reedsolomon.h"

class F2SectionToF1Section : public Decoder {
 public:
  F2SectionToF1Section();
  void pushSection(const F2Section& f2Section);
  void pushSection(F2Section&& f2Section);
  F1Section popSection();
  bool isReady() const;

  // E-7: at end of stream the newest ~111 genuine F2 frames are still held
  // inside the CIRC delay lines. flush() pushes padding frames through the
  // chain to carry that trapped tail out as F1 sections (normally hidden by
  // lead-out, but real data for a truncated capture).
  void flush();

  // Issue #307: the smallest producer doubt (0 trusted - 15 distrusted) that
  // makes an otherwise-clean symbol a C1/C2 erasure candidate. 0 disables
  // doubt-derived erasures, which is the bit-exact legacy behaviour.
  void setDoubtErasureThreshold(uint8_t threshold) {
    m_circ.setDoubtErasureThreshold(threshold);
  }

  void showStatistics() const;

  // Accessors for the curated decode report (CIRC C1/C2 health). valid/fixed/
  // error cover fully-populated codewords only; padded counts the words that
  // contained warm-up or drain filler and so could not be scored.
  int32_t validC1s() const { return m_circ.validC1s(); }
  int32_t fixedC1s() const { return m_circ.fixedC1s(); }
  int32_t errorC1s() const { return m_circ.errorC1s(); }
  int32_t paddedC1s() const { return m_circ.paddedC1s(); }
  int32_t validC2s() const { return m_circ.validC2s(); }
  int32_t fixedC2s() const { return m_circ.fixedC2s(); }
  int32_t errorC2s() const { return m_circ.errorC2s(); }
  int32_t paddedC2s() const { return m_circ.paddedC2s(); }

  // Issue #307: erasures raised on the strength of the producer's doubt alone,
  // and the codewords that received at least one.
  int64_t doubtErasuresC1() const { return m_circ.doubtErasuresC1(); }
  int64_t doubtErasuresC2() const { return m_circ.doubtErasuresC2(); }
  int64_t doubtSeededC1s() const { return m_circ.doubtSeededC1s(); }
  int64_t doubtSeededC2s() const { return m_circ.doubtSeededC2s(); }

  // Substitute F1 frames emitted while the delay lines were still filling
  // (warm-up) versus those emitted by flush() once the genuine tail had been
  // carried out (drain). Both are structural, not input defects.
  uint64_t warmupLostFrames() const { return m_warmupLostFramesCount; }
  uint64_t drainLostFrames() const { return m_drainLostFramesCount; }

 private:
  void processQueue();

  // Push a single F2 frame's data through the CIRC delay-line / Reed-Solomon
  // chain, appending exactly one F1 frame to f1Section (a padded substitute
  // while the delay lines are still filling, otherwise the decoded frame).
  // doubtData is taken by reference (unlike the by-value buffers above) so
  // the caller can hand it the same scratch vector every frame; the CIRC runs
  // ~11.8M frames per disc side and an owned vector here would be an
  // allocation per frame for what is usually a run of zeros.
  void processF2FrameData(std::vector<uint8_t> data,
                          std::vector<uint8_t> errorData,
                          std::vector<uint8_t> paddedData,
                          std::vector<uint8_t>& doubtData,
                          F1Section& f1Section);

  // Append a padded substitute F1 frame (fabricated filler emitted while the
  // CIRC delay lines fill; marked padded=1 so downstream never mistakes the
  // zeros for genuine data).
  void pushSubstituteF1Frame(F1Section& f1Section);

  void showData(const std::string& description, int32_t index,
                const std::string& timeString, std::vector<uint8_t>& data,
                std::vector<uint8_t>& dataError);

  std::deque<F2Section> m_inputBuffer;
  std::deque<F1Section> m_outputBuffer;

  ReedSolomon m_circ;

  DelayLines m_delayLine1;
  DelayLines m_delayLine2;
  DelayLines m_delayLineM;

  Interleave m_interleave;
  Inverter m_inverter;

  // Reusable per-frame doubt buffer for processF2FrameData(); see the note on
  // that declaration.
  std::vector<uint8_t> m_doubtScratch;

  // Statistics
  uint64_t m_invalidInputF2FramesCount;
  uint64_t m_validInputF2FramesCount;
  uint64_t m_invalidOutputF1FramesCount;
  uint64_t m_validOutputF1FramesCount;
  uint64_t m_dlLostFramesCount;
  uint64_t m_warmupLostFramesCount;
  uint64_t m_drainLostFramesCount;
  uint64_t m_continuityErrorCount;

  uint64_t m_inputByteErrors;
  uint64_t m_outputByteErrors;

  uint64_t m_invalidPaddedF1FramesCount;
  uint64_t m_invalidNonPaddedF1FramesCount;

  // Continuity check
  int32_t m_lastFrameNumber;

  // Metadata of the most recently processed F2 section; used to synthesise
  // continuing metadata for the sections emitted by flush(). m_haveSection
  // Metadata stays false until at least one real section has been processed.
  SectionMetadata m_lastSectionMetadata;
  bool m_haveSectionMetadata;

  // Set for the duration of flush() so that substitute frames emitted while
  // draining the delay lines are attributed to the drain rather than warm-up.
  bool m_draining;
};

#endif  // DEC_F2SECTIONTOF1SECTION_H