/*
 * File:        writer_sector_metadata.cpp
 * Purpose:     efm-decoder-data - EFM Data24 to data decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "writer_sector_metadata.h"

#include <fmt/format.h>
#include <orc/support/logging.h>

// This writer class writes metadata about sector data to a file

WriterSectorMetadata::WriterSectorMetadata() : m_sectorIndex(0) {}

WriterSectorMetadata::~WriterSectorMetadata() {
  if (m_file.is_open()) {
    m_file.close();
  }
}

bool WriterSectorMetadata::open(const std::string& filename) {
  m_sectorIndex = 0;
  m_file.open(filename);
  if (!m_file.is_open()) {
    ORC_LOG_CRITICAL(
        "WriterSectorMetadata::open() - Could not open file {} for writing",
        filename);
    return false;
  }
  ORC_LOG_DEBUG(
      "WriterSectorMetadata::open() - Opened file {} for metadata writing",
      filename);

  return true;
}

void WriterSectorMetadata::write(const Sector& sector) {
  // R-8: every sector emitted to the image passes through here in image order,
  // so the running index is the entry's offset in that image. Recording the
  // sector's own address instead would silently stop indexing the image at the
  // first address-space discontinuity. Counted before any early return so the
  // index cannot drift out of step with the image.
  const int64_t index = m_sectorIndex++;

  if (!m_file.is_open()) {
    ORC_LOG_CRITICAL(
        "WriterSectorMetadata::write() - File is not open for writing");
    return;
  }

  // If the sector is not valid, write a metadata entry for it
  if (!sector.isDataValid()) {
    // Write a metadata entry for the sector
    m_file << index << "\n";
  }
}

void WriterSectorMetadata::close() {
  if (!m_file.is_open()) {
    return;
  }

  m_file.close();
  ORC_LOG_DEBUG(
      "WriterSectorMetadata::close(): Closed the bad sector map metadata file");
}

int64_t WriterSectorMetadata::size() {
  if (m_file.is_open()) {
    auto currentPos = m_file.tellp();
    return static_cast<int64_t>(currentPos);
  }

  return 0;
}
