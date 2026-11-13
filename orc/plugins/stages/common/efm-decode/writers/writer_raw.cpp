/*
 * File:        writer_raw.cpp
 * Purpose:     efm-decoder-audio - EFM Data24 to Audio decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "writer_raw.h"

#include <orc/support/logging.h>

// This writer class writes audio data to a file in raw format (no header)
// This is used when the output is stereo audio data without WAV header

WriterRaw::~WriterRaw() {
  if (m_file.is_open()) {
    m_file.close();
  }
}

bool WriterRaw::open(const std::string& filename) {
  m_file.open(filename, std::ios::binary);
  if (!m_file.is_open()) {
    ORC_LOG_CRITICAL("WriterRaw::open() - Could not open file {} for writing",
                     filename);
    return false;
  }
  ORC_LOG_DEBUG("WriterRaw::open() - Opened file {} for raw audio data writing",
                filename);
  return true;
}

void WriterRaw::write(const AudioSection& audioSection) {
  if (!m_file.is_open()) {
    ORC_LOG_CRITICAL("WriterRaw::write() - File is not open for writing");
    return;
  }

  // Each Audio section contains 98 frames. P-7: reference each frame (no
  // per-frame Audio copy) and assemble one section-sized interleaved buffer so
  // the whole section is written in a single call.
  std::vector<int16_t> sectionData;
  sectionData.reserve(static_cast<size_t>(98) * 12);
  for (int index = 0; index < 98; ++index) {
    const Audio& audio = audioSection.frame(index);
    const std::vector<int16_t>& data = audio.data();
    sectionData.insert(sectionData.end(), data.begin(), data.end());
  }
  m_headOffset.apply(sectionData);
  if (sectionData.empty()) return;

  const auto* bytes = reinterpret_cast<const char*>(sectionData.data());
  const auto byteCount =
      static_cast<std::streamsize>(sectionData.size() * sizeof(int16_t));
  m_file.write(bytes, byteCount);
}

void WriterRaw::close() {
  if (!m_file.is_open()) {
    return;
  }

  // For raw audio, we just close the file - no header to write
  m_file.close();
  ORC_LOG_DEBUG("WriterRaw::close(): Closed the raw audio file");
}

int64_t WriterRaw::size() {
  if (m_file.is_open()) {
    return m_file.tellp();
  }

  return 0;
}
