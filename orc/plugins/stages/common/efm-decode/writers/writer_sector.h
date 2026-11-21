/*
 * File:        writer_sector.h
 * Purpose:     efm-decoder-data - EFM Data24 to data decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef WRITER_SECTOR_H
#define WRITER_SECTOR_H

#include <cstdint>
#include <fstream>
#include <string>

#include "sector.h"

class WriterSector {
 public:
  WriterSector() = default;
  ~WriterSector();

  bool open(const std::string& filename);
  void write(const Sector& sector);
  void close();
  int64_t size();
  bool isOpen() const { return m_file.is_open(); };

 private:
  std::ofstream m_file;
};

#endif  // WRITER_SECTOR_H