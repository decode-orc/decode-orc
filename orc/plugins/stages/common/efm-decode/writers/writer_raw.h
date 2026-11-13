/*
 * File:        writer_raw.h
 * Purpose:     efm-decoder-audio - EFM Data24 to Audio decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef WRITER_RAW_H
#define WRITER_RAW_H

#include <cstdint>
#include <fstream>
#include <string>

#include "audio_head_offset.h"
#include "section.h"

class WriterRaw {
 public:
  WriterRaw() = default;
  ~WriterRaw();

  bool open(const std::string& filename);
  void write(const AudioSection& audioSection);
  void close();
  int64_t size();
  bool isOpen() const { return m_file.is_open(); };

  // Head alignment (issue #231): positive prepends that many silent stereo
  // pairs before the first decoded pair; negative drops the first |pairs|
  // decoded pairs. Must be set before the first write().
  void setHeadOffsetPairs(int64_t pairs) { m_headOffset.setOffsetPairs(pairs); }

 private:
  std::ofstream m_file;
  AudioHeadOffset m_headOffset;
};

#endif  // WRITER_RAW_H