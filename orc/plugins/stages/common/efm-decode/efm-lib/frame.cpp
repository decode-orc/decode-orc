/*
 * File:        frame.cpp
 * Purpose:     EFM-library - EFM Frame type classes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "frame.h"

#include <orc/support/logging.h>

#include <cstdio>
#include <cstdlib>

#include "efm_exception.h"
#include "hex_utils.h"

// Frame class
// --------------------------------------------------------------------------------------------------------

// Set the data for the frame, ensuring it matches the frame size
void Frame::setData(const std::vector<uint8_t>& data) {
  if (static_cast<int>(data.size()) != frameSize()) {
    ORC_LOG_ERROR(
        "Frame::setData(): Data size of {} does not match frame size of {}",
        data.size(), frameSize());
    throw efm::EfmDecodeError(__func__);
  }
  m_frameData = data;
}

// Get the data for the frame, returning a zero-filled vector if empty
const std::vector<uint8_t>& Frame::data() const {
  static const std::vector<uint8_t> emptyVec;
  if (m_frameData.empty()) {
    ORC_LOG_DEBUG(
        "Frame::getData(): Frame is empty, returning zero-filled vector");
    return emptyVec;
  }
  return m_frameData;
}

// Set the error data for the frame, ensuring it matches the frame size
// Note: This is a vector of uint8_t, where 0 is no error and non-zero is an
// error
void Frame::setErrorData(const std::vector<uint8_t>& errorData) {
  if (static_cast<int>(errorData.size()) != frameSize()) {
    ORC_LOG_ERROR(
        "Frame::setErrorData(): Error data size of {} does not match frame "
        "size of {}",
        errorData.size(), frameSize());
    throw efm::EfmDecodeError(__func__);
  }

  m_frameErrorData = errorData;
}

// Get the error_data for the frame, returning a zero-filled vector if empty
// Note: This is a vector of uint8_t, where 0 is no error and non-zero is an
// error
const std::vector<uint8_t>& Frame::errorData() const {
  static const std::vector<uint8_t> emptyVec;
  if (m_frameErrorData.empty()) {
    ORC_LOG_DEBUG(
        "Frame::getErrorData(): Error frame is empty, returning zero-filled "
        "vector");
    return emptyVec;
  }
  return m_frameErrorData;
}

// Set the per-symbol doubt data for the frame, ensuring it matches the frame
// size. Note: this is a vector of uint8_t in the range 0-15, where 0 means the
// producer fully trusted the symbol and 15 that it positively distrusted it.
void Frame::setDoubtData(const std::vector<uint8_t>& doubtData) {
  if (static_cast<int>(doubtData.size()) != frameSize()) {
    ORC_LOG_ERROR(
        "Frame::setDoubtData(): Doubt data size of {} does not match frame "
        "size of {}",
        doubtData.size(), frameSize());
    throw efm::EfmDecodeError(__func__);
  }

  m_frameDoubtData = doubtData;
}

// Get the doubt data for the frame. An EMPTY vector is returned when no doubt
// was ever set, which the caller must read as "every symbol fully trusted" -
// the ordinary case for a producer that emits no confidence information. It is
// deliberately not materialised as a zero-filled vector: the CIRC path runs
// ~26M frames per stereo disc and would pay an allocation per frame for
// information it already has.
const std::vector<uint8_t>& Frame::doubtData() const {
  return m_frameDoubtData;
}

// Count the number of errors in the frame
uint32_t Frame::countErrors() const {
  uint32_t errorCount = 0;
  for (uint8_t e : m_frameErrorData) {
    if (e) errorCount++;
  }
  return errorCount;
}

// Set the padded data for the frame, ensuring it matches the frame size
// Note: This is a vector of uint8_t, where 0 is no padding and non-zero is
// padding
void Frame::setPaddedData(const std::vector<uint8_t>& paddedData) {
  if (paddedData.size() != static_cast<size_t>(frameSize())) {
    ORC_LOG_ERROR(
        "Frame::setPaddedData(): Padded data size of {} does not match frame "
        "size of {}",
        static_cast<int>(paddedData.size()), frameSize());
    throw efm::EfmDecodeError(__func__);
  }

  m_framePaddedData = paddedData;
}

// Get the padded data for the frame, returning a zero-filled vector if empty
// Note: This is a vector of uint8_t, where 0 is no padding and non-zero is
// padding
const std::vector<uint8_t>& Frame::paddedData() const {
  static const std::vector<uint8_t> emptyVec;
  if (m_framePaddedData.empty()) {
    ORC_LOG_DEBUG(
        "Frame::paddedData(): Padded data is empty, returning zero-filled "
        "vector");
    return emptyVec;
  }
  return m_framePaddedData;
}

// Count the number of padded bytes in the frame
uint32_t Frame::countPadded() const {
  uint32_t paddingCount = 0;
  for (uint8_t p : m_framePaddedData) {
    if (p) paddingCount++;
  }
  return paddingCount;
}

// Check if the frame is full (i.e., has data)
bool Frame::isFull() const { return !m_frameData.empty(); }

// Check if the frame is empty (i.e., has no data)
bool Frame::isEmpty() const { return m_frameData.empty(); }

// Constructor for Data24, initializes data to the frame size
Data24::Data24() {
  m_frameData.resize(frameSize());
  m_frameErrorData.resize(frameSize());
  std::fill(m_frameErrorData.begin(), m_frameErrorData.end(), false);
  m_framePaddedData.resize(frameSize());
  std::fill(m_framePaddedData.begin(), m_framePaddedData.end(), false);
}

// We override the set_data function to ensure the data is 24 bytes
// since it's possible to have less than 24 bytes of data
void Data24::setData(const std::vector<uint8_t>& data) {
  m_frameData = data;

  // Data24 tolerates a short final frame (fewer than 24 bytes), padding it with
  // zeros; a longer input is truncated. resize() does both. (P-11: the previous
  // manual pad loop was a no-op - it started at size()==24 - and silently
  // accepted oversized input.)
  m_frameData.resize(24, 0);
}

void Data24::setErrorData(const std::vector<uint8_t>& errorData) {
  m_frameErrorData = errorData;

  // If there are less than 24 values, pad data with 0 to 24 values
  if (m_frameErrorData.size() < 24) {
    m_frameErrorData.resize(24, 0);
  }
}

// Get the frame size for Data24
int Data24::frameSize() const { return 24; }

void Data24::showData() {
  if (!orc::get_logger()->should_log(spdlog::level::trace)) return;

  std::string dataString;
  bool hasError = false;
  char buffer[4];
  for (int i = 0; i < m_frameData.size(); ++i) {
    if (m_frameErrorData[i] == false && m_framePaddedData[i] == false) {
      snprintf(buffer, sizeof(buffer), "%02X ", m_frameData[i]);
      dataString.append(buffer);
    } else {
      if (m_framePaddedData[i] == true) {
        dataString.append("PP ");
      } else {
        dataString.append("XX ");
        hasError = true;
      }
    }
  }
  ORC_LOG_TRACE("Data24: {}{}", HexUtils::trim(dataString),
                hasError ? " ERROR" : "");
}

// Constructor for F1Frame, initializes data to the frame size
F1Frame::F1Frame() {
  m_frameData.resize(frameSize());
  m_frameErrorData.resize(frameSize());
  std::fill(m_frameErrorData.begin(), m_frameErrorData.end(), false);
  m_framePaddedData.resize(frameSize());
  std::fill(m_framePaddedData.begin(), m_framePaddedData.end(), false);
}

// Get the frame size for F1Frame
int F1Frame::frameSize() const { return 24; }

void F1Frame::showData() {
  if (!orc::get_logger()->should_log(spdlog::level::trace)) return;

  std::string dataString;
  bool hasError = false;
  char buffer[4];
  for (int i = 0; i < m_frameData.size(); ++i) {
    if (m_frameErrorData[i] == false && m_framePaddedData[i] == false) {
      snprintf(buffer, sizeof(buffer), "%02X ", m_frameData[i]);
      dataString.append(buffer);
    } else {
      if (m_framePaddedData[i] == true) {
        dataString.append("PP ");
      } else {
        dataString.append("XX ");
        hasError = true;
      }
    }
  }
  ORC_LOG_TRACE("F1Frame: {}{}", HexUtils::trim(dataString),
                hasError ? " ERROR" : "");
}

// Constructor for F2Frame, initializes data to the frame size
F2Frame::F2Frame() {
  m_frameData.resize(frameSize());
  m_frameErrorData.resize(frameSize());
  std::fill(m_frameErrorData.begin(), m_frameErrorData.end(), false);
  m_framePaddedData.resize(frameSize());
  std::fill(m_framePaddedData.begin(), m_framePaddedData.end(), false);
}

// Get the frame size for F2Frame
int F2Frame::frameSize() const { return 32; }

void F2Frame::showData() {
  // P-5: skip the per-frame hex string build when the level this logs at is
  // off.
  if (!orc::get_logger()->should_log(spdlog::level::info)) return;
  std::string dataString;
  bool hasError = false;
  char buffer[4];
  for (int i = 0; i < m_frameData.size(); ++i) {
    if (m_frameErrorData[i] == false && m_framePaddedData[i] == false) {
      snprintf(buffer, sizeof(buffer), "%02X ", m_frameData[i]);
      dataString.append(buffer);
    } else {
      if (m_framePaddedData[i] == true) {
        dataString.append("PP ");
      } else {
        dataString.append("XX ");
        hasError = true;
      }
    }
  }
  if (hasError) {
    ORC_LOG_INFO("F2Frame: {} ERROR", HexUtils::trim(dataString));
  } else {
    ORC_LOG_INFO("F2Frame: {}", HexUtils::trim(dataString));
  }
}

// Constructor for F3Frame, initializes data to the frame size
F3Frame::F3Frame() {
  m_frameData.resize(frameSize());
  m_subcodeByte = 0;
  m_f3FrameType = Subcode;
}

// Get the frame size for F3Frame
int F3Frame::frameSize() const { return 32; }

// Set the frame type as subcode and set the subcode value
void F3Frame::setFrameTypeAsSubcode(uint8_t subcodeValue) {
  m_f3FrameType = Subcode;
  m_subcodeByte = subcodeValue;
}

// Set the frame type as sync0 and set the subcode value to 0
void F3Frame::setFrameTypeAsSync0() {
  m_f3FrameType = Sync0;
  m_subcodeByte = 0;
}

// Set the frame type as sync1 and set the subcode value to 0
void F3Frame::setFrameTypeAsSync1() {
  m_f3FrameType = Sync1;
  m_subcodeByte = 0;
}

// Get the F3 frame type
F3Frame::F3FrameType F3Frame::f3FrameType() const { return m_f3FrameType; }

// Get the F3 frame type as a string
std::string F3Frame::f3FrameTypeAsString() const {
  switch (m_f3FrameType) {
    case Subcode:
      return "Subcode";
    case Sync0:
      return "Sync0";
    case Sync1:
      return "Sync1";
    default:
      return "UNKNOWN";
  }
}

// Get the subcode value
uint8_t F3Frame::subcodeByte() const { return m_subcodeByte; }

void F3Frame::showData() {
  // P-5: skip the per-frame hex string build when the level this logs at is
  // off.
  if (!orc::get_logger()->should_log(spdlog::level::info)) return;
  std::string dataString;
  bool hasError = false;
  for (size_t i = 0; i < m_frameData.size(); ++i) {
    if (m_frameErrorData[i] == false) {
      char buf[4];
      snprintf(buf, sizeof(buf), "%02x ", m_frameData[i]);
      dataString += buf;
    } else {
      dataString += "XX ";
      hasError = true;
    }
  }

  std::string errorString = hasError ? "ERROR" : "";

  char subcodeStr[16];
  snprintf(subcodeStr, sizeof(subcodeStr), "0x%02x", m_subcodeByte);

  if (m_f3FrameType == Subcode) {
    ORC_LOG_INFO("F3Frame: {} subcode: {} {}", dataString, subcodeStr,
                 errorString);
  } else if (m_f3FrameType == Sync0) {
    ORC_LOG_INFO("F3Frame: {} Sync0 {}", dataString, errorString);
  } else if (m_f3FrameType == Sync1) {
    ORC_LOG_INFO("F3Frame: {} Sync1 {}", dataString, errorString);
  } else {
    ORC_LOG_INFO("F3Frame: {} UNKNOWN {}", dataString, errorString);
  }
}
