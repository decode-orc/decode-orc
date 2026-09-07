/*
 * File:        writer_sector_metadata.h
 * Purpose:     efm-decoder-data - EFM Data24 to data decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef WRITER_SECTOR_METADATA_H
#define WRITER_SECTOR_METADATA_H

#include <cstdint>
#include <fstream>
#include <string>

#include "sector.h"

// Writes the bad-sector map (.bsm) that accompanies a data image: one entry
// per sector the decode could not recover.
//
// R-8: entries are the sector's position in the image, not its disc address.
// The two are equal only while every gap is filled; an address-space
// discontinuity makes the image offset stop tracking the address, and an
// address-keyed map then mis-indexes every entry past that point. Consumers
// pair the map with the image, so the map has to speak the image's coordinates.
class WriterSectorMetadata {
 public:
  WriterSectorMetadata();
  ~WriterSectorMetadata();

  bool open(const std::string& filename);
  void write(const Sector& sector);
  void close();
  int64_t size();
  bool isOpen() const { return m_file.is_open(); };

 private:
  std::ofstream m_file;

  // R-8: sectors written so far. write() is called once per sector emitted to
  // the image, in the same order, so this is the sector's image offset.
  int64_t m_sectorIndex;
};

#endif  // WRITER_SECTOR_METADATA_H