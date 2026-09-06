/*
 * File:        delay_lines.cpp
 * Purpose:     EFM-library - Delay line functions
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "delay_lines.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <cstdlib>

#include "efm_exception.h"

DelayLines::DelayLines(std::vector<int32_t> delayLengths) {
  m_delayLines.reserve(delayLengths.size());
  for (int32_t i = 0; i < static_cast<int32_t>(delayLengths.size()); ++i) {
    m_delayLines.push_back(DelayLine(delayLengths[i]));
  }
}

void DelayLines::push(std::vector<uint8_t>& data,
                      std::vector<uint8_t>& errorData,
                      std::vector<uint8_t>& paddedData,
                      std::vector<uint8_t>& doubtData) {
  if (data.size() != m_delayLines.size()) {
    ORC_LOG_ERROR("Input data size does not match the number of delay lines.");
    throw efm::EfmDecodeError(__func__);
  }

  // Process each input value through its corresponding delay line
  for (int32_t i = 0; i < static_cast<int32_t>(m_delayLines.size()); ++i) {
    uint8_t datum = data[i];
    bool datum_error = (errorData[i] != 0);
    bool datum_padded = (paddedData[i] != 0);
    uint8_t datum_doubt = doubtData[i];

    m_delayLines[i].push(datum, datum_error, datum_padded, datum_doubt);

    data[i] = datum;
    errorData[i] = datum_error ? 1 : 0;
    paddedData[i] = datum_padded ? 1 : 0;
    doubtData[i] = datum_doubt;
  }

  // Clear the vector if delay lines aren't ready (in order to
  // return empty data vectors)
  if (!isReady()) {
    data.clear();
    errorData.clear();
    paddedData.clear();
    doubtData.clear();
  }
}

bool DelayLines::isReady() {
  for (int32_t i = 0; i < static_cast<int32_t>(m_delayLines.size()); ++i) {
    if (!m_delayLines[i].isReady()) {
      return false;
    }
  }
  return true;
}

void DelayLines::flush() {
  for (int32_t i = 0; i < static_cast<int32_t>(m_delayLines.size()); ++i) {
    m_delayLines[i].flush();
  }
}

// DelayLine class implementation
DelayLine::DelayLine(int32_t delayLength)
    : m_head(0), m_ready(false), m_pushCount(0) {
  m_buffer.resize(delayLength);
  m_delayLength = delayLength;

  flush();
}

// DelayLine class implementation
DelayLine::DelayLine() : DelayLine(0) {}

void DelayLine::push(uint8_t& datum, bool& datumError, bool& datumPadded,
                     uint8_t& datumDoubt) {
  if (m_delayLength == 0) {
    return;
  }

  // Store the input value temporarily
  uint8_t tempInput = datum;
  bool tempInputError = datumError;
  bool tempInputPadded = datumPadded;
  uint8_t tempInputDoubt = datumDoubt;

  // Ring-buffer: m_head points to the oldest slot (the output value).
  // Read the output, overwrite the slot with the new input, then advance the
  // head. This is O(1) and avoids the O(N) erase(begin()) shift.
  datum = m_buffer[m_head].datum;
  datumError = m_buffer[m_head].error;
  datumPadded = m_buffer[m_head].padded;
  datumDoubt = m_buffer[m_head].doubt;

  m_buffer[m_head].datum = tempInput;
  m_buffer[m_head].error = tempInputError;
  m_buffer[m_head].padded = tempInputPadded;
  m_buffer[m_head].doubt = tempInputDoubt;

  m_head = (m_head + 1 >= m_delayLength) ? 0 : m_head + 1;

  // Check if the delay line is ready
  if (m_pushCount >= m_delayLength) {
    m_ready = true;
  } else {
    ++m_pushCount;
  }
}

bool DelayLine::isReady() { return m_ready; }

void DelayLine::flush() {
  if (m_delayLength > 0) {
    DelayContents_t temp;
    temp.datum = 0;
    temp.error = false;
    temp.padded = false;
    temp.doubt = 0;
    std::fill(m_buffer.begin(), m_buffer.end(), temp);

    m_head = 0;
    m_ready = false;
  } else {
    m_ready = true;
  }
  m_pushCount = 0;
}
