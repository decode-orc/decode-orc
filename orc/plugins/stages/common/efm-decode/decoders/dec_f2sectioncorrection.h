/*
 * File:        dec_f2sectioncorrection.h
 * Purpose:     efm-decoder-f2 - EFM T-values to F2 Section decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef DEC_F2SECTIONCORRECTION_H
#define DEC_F2SECTIONCORRECTION_H

#include <cstdint>
#include <deque>
#include <queue>
#include <string>
#include <vector>

#include "decoders.h"
#include "section_metadata.h"

class F2SectionCorrection : public Decoder {
 public:
  F2SectionCorrection();
  void pushSection(const F2Section& data);
  void pushSection(F2Section&& data);
  F2Section popSection();
  bool isReady() const;
  bool isValid() const;
  void flush();
  void setNoTimecodes(bool noTimecodes);
  void showStatistics() const;

  // Diagnostics used to build a meaningful error when lead-in never locks:
  //   receivedSections()      - total F2 sections handed to pushSection().  If
  //                             zero, EFM/F3 frame sync never happened at all.
  //   validMetadataSections() - of those, how many carried valid subcode
  //                             metadata.  If zero (but sections were received)
  //                             the stream decoded but has no usable timecodes.
  int32_t receivedSections() const { return m_receivedSections; }
  int32_t validMetadataSections() const { return m_validMetadataSections; }

  // Accessors used by EfmProcessor to assemble the curated decode report.
  uint32_t totalSections() const { return m_totalSections; }
  uint32_t correctedSections() const { return m_correctedSections; }
  uint32_t uncorrectableSections() const { return m_uncorrectableSections; }
  uint32_t preLeadinSections() const { return m_preLeadinSections; }
  uint32_t missingSections() const { return m_missingSections; }
  uint32_t paddingSections() const { return m_paddingSections; }
  uint32_t outOfOrderSections() const { return m_outOfOrderSections; }

  // Trailing sections whose Q-channel never decoded and which the stream ended
  // before they could be bracketed for correction. Their metadata is
  // forward-filled from the last valid section at flush() time. Structural
  // (a consequence of where the capture stops), so counted separately from
  // uncorrectableSections().
  uint32_t tailFilledSections() const { return m_tailFilledSections; }

  // R-3: timeline re-baselining. A forward jump larger than the fill cap is
  // not a run of missing sections that can be reconstructed - the timeline is
  // simply restarted at the new absolute time.
  //   timelineResyncs()         - how many times that happened.
  //   resyncSkippedSections()   - total sections of absolute time stepped over
  //                               and therefore absent from the output. These
  //                               are the reason totalSections() can be less
  //                               than (absoluteEndTime - absoluteStartTime).
  //   resyncDiscardedSections() - buffered sections thrown away at a resync
  //                               because their correction anchor went with it.
  uint32_t timelineResyncs() const { return m_timelineResyncs; }
  uint32_t resyncSkippedSections() const { return m_resyncSkippedSections; }
  uint32_t resyncDiscardedSections() const { return m_resyncDiscardedSections; }

  uint32_t qmode1Sections() const { return m_qmode1Sections; }
  uint32_t qmode2Sections() const { return m_qmode2Sections; }
  uint32_t qmode3Sections() const { return m_qmode3Sections; }
  uint32_t qmode4Sections() const { return m_qmode4Sections; }

  SectionTime absoluteStartTime() const { return m_absoluteStartTime; }
  SectionTime absoluteEndTime() const { return m_absoluteEndTime; }

  // Q-mode 2 media catalogue number (UPC/EAN), disc-global; empty if none seen.
  const std::string& catalogueNumber() const { return m_catalogueNumber; }

  // Per-track Q-channel aggregation (index-aligned; user tracks only, in order
  // of appearance).
  const std::vector<uint8_t>& trackNumbers() const { return m_trackNumbers; }
  const std::vector<SectionTime>& trackStartTimes() const {
    return m_trackStartTimes;
  }
  const std::vector<SectionTime>& trackEndTimes() const {
    return m_trackEndTimes;
  }
  const std::vector<SectionTime>& trackAbsStartTimes() const {
    return m_trackAbsStartTimes;
  }
  const std::vector<SectionTime>& trackAbsEndTimes() const {
    return m_trackAbsEndTimes;
  }
  const std::vector<bool>& trackPreemphasis() const {
    return m_trackPreemphasis;
  }
  const std::vector<bool>& trackPreemphasisVaried() const {
    return m_trackPreemphasisVaried;
  }
  const std::vector<bool>& trackCopyProhibited() const {
    return m_trackCopyProhibited;
  }
  const std::vector<bool>& trackIsAudio() const { return m_trackIsAudio; }
  const std::vector<bool>& trackIs2Channel() const { return m_track2Channel; }
  const std::vector<std::string>& trackIsrc() const { return m_trackIsrc; }

  // Q-6: lead-in TOC (IEC 60908 §17.5.1), assembled from the POINT/PMIN/PSEC/
  // PFRAME fields of the lead-in Q-channel. hasToc() is false when the capture
  // did not include any decodable lead-in (e.g. it starts in the program area).
  bool hasToc() const { return m_hasToc; }
  uint8_t tocFirstTrack() const { return m_tocFirstTrack; }
  uint8_t tocLastTrack() const { return m_tocLastTrack; }
  uint8_t tocDiscType() const { return m_tocDiscType; }
  SectionTime tocLeadOutStart() const { return m_tocLeadOutStart; }
  const std::vector<uint8_t>& tocTrackNumbers() const {
    return m_tocTrackNumbers;
  }
  const std::vector<SectionTime>& tocTrackStartTimes() const {
    return m_tocTrackStartTimes;
  }
  uint32_t leadinSections() const { return m_leadinSections; }

 private:
  void processQueue();
  int trackIndex(uint8_t trackNumber) const;
  void recordTocEntry(const SectionMetadata& metadata);

  void waitForInputToSettle(F2Section& f2Section);
  void waitingForSection(F2Section& f2Section);
  SectionTime getExpectedAbsoluteTime() const;

  void processInternalBuffer();
  void outputSections();
  void emitSection();
  void forwardFillTrailingInvalidSections();

  std::queue<F2Section> m_inputBuffer;
  std::deque<F2Section> m_leadinBuffer;
  std::queue<F2Section> m_outputBuffer;

  std::deque<F2Section> m_internalBuffer;

  bool m_leadinComplete;

  int32_t m_maximumGapSize;
  int32_t m_paddingWatermark;

  // Statistics
  uint32_t m_totalSections;
  uint32_t m_correctedSections;
  uint32_t m_uncorrectableSections;
  uint32_t m_preLeadinSections;
  uint32_t m_missingSections;
  uint32_t m_paddingSections;
  uint32_t m_outOfOrderSections;
  uint32_t m_tailFilledSections;
  uint32_t m_timelineResyncs;
  uint32_t m_resyncSkippedSections;
  uint32_t m_resyncDiscardedSections;

  uint32_t m_qmode1Sections;
  uint32_t m_qmode2Sections;
  uint32_t m_qmode3Sections;
  uint32_t m_qmode4Sections;

  // Time statistics
  SectionTime m_absoluteStartTime;
  SectionTime m_absoluteEndTime;
  std::vector<uint8_t> m_trackNumbers;
  std::vector<SectionTime> m_trackStartTimes;
  std::vector<SectionTime> m_trackEndTimes;

  // Per-track Q-channel aggregation (index-aligned with m_trackNumbers)
  std::vector<SectionTime> m_trackAbsStartTimes;
  std::vector<SectionTime> m_trackAbsEndTimes;
  std::vector<bool> m_trackPreemphasis;        // last-seen pre-emphasis flag
  std::vector<bool> m_trackPreemphasisVaried;  // flag changed within the track
  std::vector<bool> m_trackCopyProhibited;
  std::vector<bool> m_trackIsAudio;
  std::vector<bool> m_track2Channel;
  std::vector<std::string> m_trackIsrc;  // ISRC (Q-mode 3), if present

  // Q-6: lead-in TOC assembled from POINT/PMIN/PSEC/PFRAME (IEC 60908 §17.5.1).
  // m_tocTrackNumbers / m_tocTrackStartTimes are index-aligned (POINT 01-99 ->
  // that track's start time). The A0/A1/A2 pointers populate the scalars.
  bool m_hasToc;
  uint8_t m_tocFirstTrack;        // POINT A0 (PMIN)
  uint8_t m_tocLastTrack;         // POINT A1 (PMIN)
  uint8_t m_tocDiscType;          // POINT A0 (PSEC)
  SectionTime m_tocLeadOutStart;  // POINT A2 (PMIN:PSEC:PFRAME)
  std::vector<uint8_t> m_tocTrackNumbers;
  std::vector<SectionTime> m_tocTrackStartTimes;
  uint32_t m_leadinSections;

  // Q-mode 2 media catalogue number (UPC/EAN), disc-global
  std::string m_catalogueNumber;
  // Last real (mode-1/4) user track seen, used to attribute a mode-3 ISRC block
  // to the track it belongs to.
  uint8_t m_currentTrack;

  // Timecode handling
  bool m_noTimecodes;

  // Diagnostic counters (see receivedSections() / validMetadataSections())
  int32_t m_receivedSections;
  int32_t m_validMetadataSections;
};

#endif  // DEC_F2SECTIONCORRECTION_H