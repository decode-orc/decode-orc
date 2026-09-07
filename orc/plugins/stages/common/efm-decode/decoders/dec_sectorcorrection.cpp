/*
 * File:        dec_sectorcorrection.cpp
 * Purpose:     efm-decoder-data - EFM Data24 to data decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dec_sectorcorrection.h"

#include <orc/support/logging.h>

#include <map>
#include <utility>
#include <vector>

namespace {
// A missing-sector gap is filled with one fabricated 2048-byte sector each, so
// an uncorroborated sector address could otherwise request an unbounded fill
// and exhaust memory. Cap the fill as a last-resort backstop. 4500 sectors =
// 1 minute at 75 sectors/s.
//
// R-5: this is now only reachable for a gap with no Q-channel reference to
// check it against. Where a reference exists the decision is made on evidence
// rather than on size - see processQueue() - because gap size alone cannot
// tell a real run of missing sectors from a corrupt address, and guessing
// wrong either shifts the whole rest of the image or fabricates hundreds of
// megabytes of phantom sectors.
constexpr int32_t kMaxSectorGapFill = 4500;

// R-5: how many sectors to hold before committing to a header-to-Q offset.
// Only the opening window needs this: after it, a verified address is checked
// against an established offset and can be believed on its own. Small enough
// that the buffered payload is negligible, large enough that a handful of
// corrupt headers cannot outvote the truth.
constexpr size_t kAnchorWindow = 16;

// The image is written by appending sectors in emission order, so a sector's
// byte offset is its position in the stream, not its address. Filling every
// genuine gap is what keeps offset and address in step; where they are made to
// diverge deliberately (an address-space discontinuity) that must be reported,
// never silent.
Sector makeMissingSector(const SectorAddress& address) {
  Sector missingSector;
  missingSector.dataValid(false);
  missingSector.setAddress(address);
  missingSector.setMode(1);
  missingSector.pushData(std::vector<uint8_t>(2048, 0));
  missingSector.pushErrorData(std::vector<uint8_t>(2048, 1));
  return missingSector;
}
}  // namespace

SectorCorrection::SectorCorrection()
    : m_haveLastSectorInfo(false),
      m_lastSectorAddress(0),
      m_lastSectorMode(0),
      m_lastQSectionFrames(-1),
      m_anchorResolved(false),
      m_haveAddressOffset(false),
      m_addressOffset(0),
      m_anchorOutliers(0),
      m_goodSectors(0),
      m_missingLeadingSectors(0),
      m_missingSectors(0),
      m_repairedAddresses(0),
      m_addressDiscontinuities(0),
      m_backwardAddresses(0),
      m_uncorroboratedFills(0) {}

void SectorCorrection::pushSector(const Sector& sector) {
  // Add the data to the input buffer
  m_inputBuffer.push_back(sector);

  // Process the queue
  processQueue();
}

Sector SectorCorrection::popSector() {
  // Return the first item in the output buffer
  Sector sector = m_outputBuffer.front();
  m_outputBuffer.pop_front();
  return sector;
}

// R-5: reconcile a sector's header address with the Q-channel timeline.
//
// A passing EDC covers the header, so a trusted address is authoritative: it
// defines the header-to-Q offset, and it is never overridden. An untrusted
// address (the EDC failed and the header was taken on the strength of unset C2
// error flags, which a mis-correction leaves clear) is replaced by the offset's
// prediction. That single rule is what separates a corrupt header from a real
// address jump - the two are indistinguishable by size alone.
//
// Returns true if the sector's address was replaced.
bool SectorCorrection::repairAddress(Sector& sector) {
  const int32_t qFrames = sector.qSectionFrames();

  if (sector.isAddressTrusted()) {
    if (qFrames >= 0) {
      // A verified address re-learns the offset, so a genuine step in the
      // disc's address space is adopted rather than fought.
      m_addressOffset = sector.address().address() - qFrames;
      m_haveAddressOffset = true;
    }
    return false;
  }

  if (qFrames < 0 || !m_haveAddressOffset) {
    // Nothing to check the header against - leave it alone.
    return false;
  }

  const int32_t predicted = qFrames + m_addressOffset;
  if (predicted < 0 || sector.address().address() == predicted) return false;

  ORC_LOG_DEBUG(
      "SectorCorrection::repairAddress(): Untrusted header address {} replaced "
      "with {} predicted from the Q-channel timeline",
      sector.address().address(), predicted);
  sector.setAddress(SectorAddress(predicted));
  m_repairedAddresses++;
  return true;
}

// R-5: decide the header-to-Q offset from the opening window, then bring every
// sector in it into line before any of them is placed.
//
// This is the one place a verified address may be overruled. Everywhere else a
// passing EDC settles the matter, but here there is no established offset to
// judge a disagreement against, and a 32-bit EDC does admit a rare false-valid.
// A lone dissenter among the first sectors is far more likely to be that than a
// real step in the disc's address space one sector into the capture - and
// believing it anchors the entire image in the wrong place.
void SectorCorrection::resolveAnchor() {
  if (m_anchorResolved) return;
  m_anchorResolved = true;

  // Modal offset among the sectors whose address is verified.
  std::map<int32_t, int> votes;
  for (const Sector& sector : m_anchorBuffer) {
    if (!sector.isAddressTrusted() || sector.qSectionFrames() < 0) continue;
    ++votes[sector.address().address() - sector.qSectionFrames()];
  }

  if (!votes.empty()) {
    auto best = votes.begin();
    for (auto it = votes.begin(); it != votes.end(); ++it) {
      if (it->second > best->second) best = it;
    }
    m_addressOffset = best->first;
    m_haveAddressOffset = true;
    ORC_LOG_DEBUG(
        "SectorCorrection::resolveAnchor(): Header-to-Q offset {} agreed by {} "
        "of {} verified sector(s) in the opening window",
        m_addressOffset, best->second, votes.size());

    // Overrule the dissenters, verified or not.
    for (Sector& sector : m_anchorBuffer) {
      if (sector.qSectionFrames() < 0) continue;
      const int32_t predicted = sector.qSectionFrames() + m_addressOffset;
      if (predicted < 0 || sector.address().address() == predicted) continue;
      ORC_LOG_WARN(
          "SectorCorrection::resolveAnchor(): Opening sector address {} "
          "disagrees with the offset the rest of the window agrees on; "
          "using {} instead.",
          sector.address().toString(), SectorAddress(predicted).toString());
      sector.setAddress(SectorAddress(predicted));
      sector.addressTrusted(true);
      m_anchorOutliers++;
    }
  }

  for (Sector& sector : m_anchorBuffer) placeSector(sector);
  m_anchorBuffer.clear();
}

void SectorCorrection::flush() { resolveAnchor(); }

void SectorCorrection::processQueue() {
  while (!m_inputBuffer.empty()) {
    // Get the first item in the input buffer
    Sector sector = m_inputBuffer.front();
    m_inputBuffer.pop_front();

    if (!m_anchorResolved) {
      m_anchorBuffer.push_back(std::move(sector));
      if (m_anchorBuffer.size() >= kAnchorWindow) resolveAnchor();
      continue;
    }

    placeSector(sector);
  }
}

// Place one sector: reconcile its address, fill any gap it opens, emit it.
// The sector is moved from, so the caller must not use it afterwards.
void SectorCorrection::placeSector(Sector& sector) {
  repairAddress(sector);

  if (!m_haveLastSectorInfo) {
    // This is the first sector - fill the missing leading sectors so the
    // image starts at address 0. The fill is capped for the same reason as
    // the gap fill below.
    const int32_t firstAddress = sector.address().address();
    if (firstAddress > 0 && firstAddress <= kMaxSectorGapFill) {
      ORC_LOG_DEBUG(
          "SectorCorrection::placeSector(): First received sector address "
          "is {} ({}); filling {} missing leading sector(s)",
          firstAddress, sector.address().toString(), firstAddress);
      for (int32_t i = 0; i < firstAddress; ++i) {
        m_outputBuffer.push_back(makeMissingSector(SectorAddress(i)));
        m_missingLeadingSectors++;
      }
    } else if (firstAddress > kMaxSectorGapFill) {
      ORC_LOG_WARN(
          "SectorCorrection::placeSector(): Capture starts at sector "
          "address {} ({}), beyond the leading-fill cap ({}); the image "
          "starts at that address rather than at 0.",
          firstAddress, sector.address().toString(), kMaxSectorGapFill);
    }

    m_haveLastSectorInfo = true;
  } else if (sector.address() != m_lastSectorAddress + 1) {
    const int32_t headerGap =
        sector.address().address() - m_lastSectorAddress.address() - 1;

    // R-5: the Q-channel timeline says how many sections actually elapsed
    // between these two sectors. If the two agree, the gap is a real run of
    // missing sectors and filling it keeps the image aligned. If they
    // disagree, the sections were adjacent on the disc and no data is
    // missing - the address space itself steps - so filling would fabricate
    // sectors that never existed.
    const bool haveQPair =
        m_lastQSectionFrames >= 0 && sector.qSectionFrames() >= 0;
    const int32_t qGap =
        haveQPair ? sector.qSectionFrames() - m_lastQSectionFrames - 1 : -1;

    ORC_LOG_DEBUG(
        "SectorCorrection::placeSector(): Address step. Last: {} ({}) "
        "current: {} ({}) header gap: {} Q gap: {}",
        m_lastSectorAddress.address(), m_lastSectorAddress.toString(),
        sector.address().address(), sector.address().toString(), headerGap,
        haveQPair ? qGap : -1);

    int32_t gap = 0;
    if (haveQPair && headerGap != qGap) {
      // Not corroborated: an address-space discontinuity, not missing data.
      if (headerGap < 0) m_backwardAddresses++;
      m_addressDiscontinuities++;
      ORC_LOG_WARN(
          "SectorCorrection::placeSector(): Sector address steps by {} from "
          "{} to {} but the Q-channel timeline shows {} section(s) between "
          "them; treating as an address discontinuity (no data missing, "
          "nothing filled).",
          headerGap + 1, m_lastSectorAddress.toString(),
          sector.address().toString(), qGap + 1);
    } else if (headerGap < 0) {
      // No Q reference and the address went backwards or repeated. Nothing
      // can be filled; re-anchor and record it rather than letting the
      // baseline walk backwards unnoticed.
      m_backwardAddresses++;
      m_addressDiscontinuities++;
      ORC_LOG_WARN(
          "SectorCorrection::placeSector(): Sector address went backwards, "
          "from {} to {}, with no Q-channel reference to check it against; "
          "treating as an address discontinuity.",
          m_lastSectorAddress.toString(), sector.address().toString());
    } else if (!haveQPair && headerGap > kMaxSectorGapFill) {
      // Uncorroborated and implausibly large - the backstop.
      m_addressDiscontinuities++;
      ORC_LOG_WARN(
          "SectorCorrection::placeSector(): Gap of {} sectors exceeds the "
          "fill cap ({}) and has no Q-channel reference to corroborate it; "
          "treating as an address discontinuity.",
          headerGap, kMaxSectorGapFill);
    } else {
      gap = headerGap;
      if (!haveQPair) m_uncorroboratedFills++;
    }

    for (int32_t i = 0; i < gap; ++i) {
      m_outputBuffer.push_back(makeMissingSector(m_lastSectorAddress + 1 + i));
      m_missingSectors++;
    }
  }

  // Add the sector to the output buffer
  m_lastQSectionFrames = sector.qSectionFrames();
  m_lastSectorAddress = sector.address();
  m_lastSectorMode = sector.mode();
  m_outputBuffer.push_back(std::move(sector));
  m_goodSectors++;
}

bool SectorCorrection::isReady() const {
  // Return true if the output buffer is not empty
  return !m_outputBuffer.empty();
}

void SectorCorrection::showStatistics() const {
  ORC_LOG_INFO("Sector gap correction:");
  ORC_LOG_INFO("  Good sectors: {}", m_goodSectors);
  ORC_LOG_INFO("  Missing leading sectors: {}", m_missingLeadingSectors);
  ORC_LOG_INFO("  Missing/Gap sectors: {}", m_missingSectors);
  ORC_LOG_INFO("  Total sectors: {}",
               m_goodSectors + m_missingLeadingSectors + m_missingSectors);
  ORC_LOG_INFO("  Address integrity:");
  ORC_LOG_INFO("    Repaired from Q timeline: {}", m_repairedAddresses);
  ORC_LOG_INFO("    Address discontinuities: {} (of which backwards: {})",
               m_addressDiscontinuities, m_backwardAddresses);
  ORC_LOG_INFO("    Uncorroborated gap fills: {}", m_uncorroboratedFills);
}
