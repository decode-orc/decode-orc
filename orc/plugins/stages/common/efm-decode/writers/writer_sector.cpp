/*
 * File:        writer_sector.cpp
 * Purpose:     efm-decoder-data - EFM Data24 to data decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "writer_sector.h"

#include <orc/support/logging.h>

// This writer class writes raw data to a file directly from the Data24 sections
// This is (generally) used when the output is not stereo audio data

WriterSector::~WriterSector() {
  if (m_file.is_open()) {
    m_file.close();
  }
}

bool WriterSector::open(const std::string& filename) {
  m_file.open(filename, std::ios::binary);
  if (!m_file.is_open()) {
    ORC_LOG_CRITICAL(
        "WriterSector::open() - Could not open file {} for writing", filename);
    return false;
  }
  ORC_LOG_DEBUG("WriterSector::open() - Opened file {} for data writing",
                filename);
  return true;
}

void WriterSector::write(const Sector& sector) {
  if (!m_file.is_open()) {
    ORC_LOG_CRITICAL("WriterSector::write() - File is not open for writing");
    return;
  }

  // Each sector contains 2048 bytes that we need to write to the output file
  m_file.write(reinterpret_cast<const char*>(sector.data().data()),
               static_cast<std::streamsize>(sector.size() * sizeof(uint8_t)));
}

void WriterSector::close() {
  if (m_file.is_open()) {
    ORC_LOG_DEBUG("WriterSector::close(): Closed the data file");
    m_file.close();
  }
}

int64_t WriterSector::size() {
  if (m_file.is_open()) {
    auto currentPos = m_file.tellp();
    return static_cast<int64_t>(currentPos);
  }

  return 0;
}
