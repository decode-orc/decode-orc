/*
 * File:        dec_data24torawsector.h
 * Purpose:     efm-decoder-data - EFM Data24 to data decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef DEC_DATA24TORAWSECTOR_H
#define DEC_DATA24TORAWSECTOR_H

#include <deque>
#include <vector>

#include "decoders.h"
#include "sector.h"

class Data24ToRawSector : public Decoder {
 public:
  Data24ToRawSector();
  void pushSection(const Data24Section& data24Section);
  void pushSection(Data24Section&& data24Section);
  RawSector popSector();
  bool isReady() const;

  void showStatistics() const;

 private:
  void processStateMachine();

  // R-5: the Data24 stream is a flat byte stream here - sector boundaries are
  // found by sync search and need not align with section boundaries - so the
  // Q-channel time of the section a sector was cut from can only be recovered
  // by tracking byte positions. m_streamOffset is the absolute offset of
  // m_sectorData.begin() in the concatenated stream; m_sectionQTimes holds one
  // absolute time per appended section, the first of which is stream section
  // m_firstSectionIndex. Every buffer erase must go through eraseFront() so the
  // two stay in step.
  void eraseFront(size_t bytes);
  int32_t qTimeAtStreamStart() const;

  std::deque<int32_t> m_sectionQTimes;
  int64_t m_firstSectionIndex;
  int64_t m_streamOffset;

  std::deque<Data24Section> m_inputBuffer;
  std::deque<RawSector> m_outputBuffer;

  // State machine states
  enum State { WaitingForSync, InSync, LostSync };

  State m_currentState;

  // 12 byte sync pattern
  const std::vector<uint8_t> m_syncPattern{0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                           0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

  // Sector data buffer
  std::vector<uint8_t> m_sectorData;
  std::vector<uint8_t> m_sectorErrorData;
  std::vector<uint8_t> m_sectorPaddedData;

  // State machine state processing functions
  State waitingForSync();
  State inSync();
  State lostSync();

  uint32_t m_missedSyncPatternCount;
  uint32_t m_goodSyncPatternCount;
  uint32_t m_badSyncPatternCount;

  // Statistics
  uint32_t m_validSectorCount;
  uint32_t m_discardedBytes;
  uint32_t m_discardedPaddingBytes;
  uint32_t m_syncLostCount;
};

#endif  // DEC_DATA24TORAWSECTOR_H