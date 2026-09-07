/*
 * File:        dec_sectorcorrection.h
 * Purpose:     efm-decoder-data - EFM Data24 to data decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef DEC_SECTORCORRECTION_H
#define DEC_SECTORCORRECTION_H

#include <deque>

#include "decoders.h"
#include "sector.h"

class SectorCorrection : public Decoder {
 public:
  SectorCorrection();
  void pushSector(const Sector& sector);
  Sector popSector();
  bool isReady() const;

  void showStatistics() const;

  // Accessors for the curated decode report (sector-gap correction).
  uint32_t goodSectors() const { return m_goodSectors; }
  uint32_t missingSectors() const { return m_missingSectors; }
  uint32_t missingLeadingSectors() const { return m_missingLeadingSectors; }

  // R-5: address integrity.
  //   repairedAddresses()      - untrusted header addresses replaced from the
  //                              Q-channel timeline.
  //   addressDiscontinuities() - jumps the Q timeline does not corroborate:
  //                              the disc's address space steps, but no data is
  //                              missing, so nothing is filled and the image
  //                              offset stops tracking the address from here.
  //   backwardAddresses()      - of those, the ones that went backwards or
  //                              repeated an address.
  //   uncorroboratedFills()    - gaps filled on header evidence alone because
  //                              no Q reference was available.
  uint32_t repairedAddresses() const { return m_repairedAddresses; }
  // Sectors in the opening window whose address disagreed with the majority
  // and was overruled - including a corrupt first sector that would otherwise
  // have anchored the image in the wrong place.
  uint32_t anchorOutliers() const { return m_anchorOutliers; }
  uint32_t addressDiscontinuities() const { return m_addressDiscontinuities; }
  uint32_t backwardAddresses() const { return m_backwardAddresses; }
  uint32_t uncorroboratedFills() const { return m_uncorroboratedFills; }

  // R-5: release the anchor window at end of stream, for a capture too short
  // to fill it. Safe to call more than once.
  void flush();

 private:
  void processQueue();
  void resolveAnchor();
  void placeSector(Sector& sector);
  bool repairAddress(Sector& sector);

  std::deque<Sector> m_inputBuffer;
  std::deque<Sector> m_outputBuffer;

  // R-5: the first sector's address anchors the whole image - it sets where
  // the leading fill stops - yet it is the one address with nothing to check
  // it against, because the header-to-Q offset is learned from the sectors
  // themselves. A corrupt first header therefore used to fabricate thousands
  // of phantom leading sectors and leave the rest of the image offset. Hold a
  // short window of sectors at the start, take the offset the majority of them
  // agree on, and only then place any of them.
  std::deque<Sector> m_anchorBuffer;
  bool m_anchorResolved;

  bool m_haveLastSectorInfo;
  SectorAddress m_lastSectorAddress;
  int32_t m_lastSectorMode;
  int32_t m_lastQSectionFrames;

  // R-5: header address minus Q-channel section time, learned from the most
  // recent EDC-verified sector. Constant across a contiguously-addressed run,
  // and it re-learns by itself wherever the disc's address space genuinely
  // steps, because only a verified header ever sets it.
  bool m_haveAddressOffset;
  int32_t m_addressOffset;
  uint32_t m_anchorOutliers;

  // Statistics
  uint32_t m_goodSectors;
  uint32_t m_missingLeadingSectors;
  uint32_t m_missingSectors;
  uint32_t m_repairedAddresses;
  uint32_t m_addressDiscontinuities;
  uint32_t m_backwardAddresses;
  uint32_t m_uncorroboratedFills;
};

#endif  // DEC_SECTORCORRECTION_H
