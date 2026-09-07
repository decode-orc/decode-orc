/*
 * File:        dec_rawsectortosector.h
 * Purpose:     efm-decoder-data - EFM Data24 to data decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef DEC_RAWSECTORTOSECTOR_H
#define DEC_RAWSECTORTOSECTOR_H

#include <deque>
#include <vector>

#include "decoders.h"
#include "rspc.h"
#include "sector.h"

class RawSectorToSector : public Decoder {
 public:
  RawSectorToSector();
  void pushSector(const RawSector& rawSector);
  Sector popSector();
  bool isReady() const;

  void showStatistics() const;

  // Accessors for the curated decode report (RSPC sector recovery).
  uint32_t validSectors() const { return m_validSectors; }
  uint32_t correctedSectors() const { return m_correctedSectors; }
  uint32_t invalidSectors() const { return m_invalidSectors; }

  // R-5: sectors whose header MSF was not well-formed BCD (or was out of
  // range). Their address is unusable and is replaced downstream from the
  // Q-channel timeline.
  uint32_t implausibleHeaders() const { return m_implausibleHeaders; }

 private:
  void processQueue();
  static uint8_t bcdToInt(uint8_t bcd);
  static bool isBcd(uint8_t value);
  uint32_t crc32(const std::vector<uint8_t>& src, int32_t size);

  std::deque<RawSector> m_inputBuffer;
  std::deque<Sector> m_outputBuffer;

  // Statistics
  uint32_t m_validSectors;
  uint32_t m_invalidSectors;
  uint32_t m_correctedSectors;

  uint32_t m_mode0Sectors;
  uint32_t m_mode1Sectors;
  uint32_t m_mode2Sectors;
  uint32_t m_invalidModeSectors;
  uint32_t m_implausibleHeaders;

  // E-8(e): cumulative codeword-level RSPC activity across all corrected
  // sectors - clean (already valid) vs genuinely repaired P/Q codewords.
  uint64_t m_rspcQCleanCodewords;
  uint64_t m_rspcQCorrectedCodewords;
  uint64_t m_rspcPCleanCodewords;
  uint64_t m_rspcPCorrectedCodewords;
};

#endif  // DEC_RAWSECTORTOSECTOR_H