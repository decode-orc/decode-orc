/*
 * File:        sector.cpp
 * Purpose:     EFM-library - EFM Section classes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "sector.h"

#include <orc/support/logging.h>

#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include "efm_constants.h"
#include "efm_exception.h"

// ECMA-130 §14.2 / IEC 60908 §17.5.1: sector addresses are MIN:SEC:FRAME with
// MIN a two-digit BCD value (00-99). The maximum representable address is
// 99:59:74 = (99*60 + 59)*75 + 74 = 449999, so a valid address is strictly
// below one past that value.
namespace {
constexpr int32_t kMaxSectorFrames = 450000;  // 99:59:74 + 1
constexpr uint8_t kMaxMinutes = 99;
constexpr uint8_t kMaxSeconds = 59;
constexpr uint8_t kMaxFrameOfSecond = 74;
}  // namespace

// Sector address class
// ---------------------------------------------------------------------------------------------------
SectorAddress::SectorAddress() : m_address(0) {
  if (m_address < 0 || m_address >= kMaxSectorFrames) {
    ORC_LOG_ERROR("SectorAddress::SectionTime(): Invalid address value of {}",
                  m_address);
    throw efm::EfmDecodeError(__func__);
  }
}

SectorAddress::SectorAddress(int32_t address) : m_address(address) {
  if (m_address < 0 || m_address >= kMaxSectorFrames) {
    ORC_LOG_ERROR("SectorAddress::SectionTime(): Invalid address value of {}",
                  m_address);
    throw efm::EfmDecodeError(__func__);
  }
}

SectorAddress::SectorAddress(uint8_t minutes, uint8_t seconds, uint8_t frames) {
  setTime(minutes, seconds, frames);
}

void SectorAddress::setAddress(int32_t address) {
  if (address < 0 || address >= kMaxSectorFrames) {
    ORC_LOG_ERROR("SectorAddress::setFrames(): Invalid address value of {}",
                  address);
    throw efm::EfmDecodeError(__func__);
  }

  m_address = address;
}

void SectorAddress::setTime(uint8_t minutes, uint8_t seconds, uint8_t frames) {
  // Set the address in minutes, seconds, and frames

  // Ensure the time is sane (IEC 60908 §17.5.1: MIN is BCD 00-99, SEC modulo
  // 60, FRAME modulo 75 -> true ceilings are 99:59:74).
  if (minutes > kMaxMinutes) {
    ORC_LOG_DEBUG(
        "SectorAddress::setTime(): Invalid minutes value {}, setting to {}",
        minutes, kMaxMinutes);
    minutes = kMaxMinutes;
  }
  if (seconds > kMaxSeconds) {
    ORC_LOG_DEBUG(
        "SectorAddress::setTime(): Invalid seconds value {}, setting to {}",
        seconds, kMaxSeconds);
    seconds = kMaxSeconds;
  }
  if (frames > kMaxFrameOfSecond) {
    ORC_LOG_DEBUG(
        "SectorAddress::setTime(): Invalid frames value {}, setting to {}",
        frames, kMaxFrameOfSecond);
    frames = kMaxFrameOfSecond;
  }

  m_address = (minutes * 60 + seconds) * 75 + frames;
}

std::string SectorAddress::toString() const {
  // Return the time in the format MM:SS:FF
  std::ostringstream stream;
  stream << std::setfill('0') << std::setw(2)
         << static_cast<int>(m_address / (75 * 60)) << ":" << std::setw(2)
         << static_cast<int>((m_address / 75) % 60) << ":" << std::setw(2)
         << static_cast<int>(m_address % 75);
  return stream.str();
}

uint8_t SectorAddress::intToBcd(uint32_t value) {
  if (value > 99) {
    ORC_LOG_ERROR(
        "SectorAddress::intToBcd(): Value must be in the range 0 to 99.");
    throw efm::EfmDecodeError(__func__);
  }

  uint16_t bcd = 0;
  uint16_t factor = 1;

  while (value > 0) {
    bcd += (value % 10) * factor;
    value /= 10;
    factor *= 16;
  }

  // Ensure the result is always 1 byte (00-99)
  return bcd & 0xFF;
}

// Raw sector class
// The raw sector is 2352 bytes (unscrambled) and contains user data and error
// correction data
RawSector::RawSector()
    : m_data(std::vector<uint8_t>(efm::kRawSectorSize, 0)),
      m_errorData(std::vector<uint8_t>(efm::kRawSectorSize, 0)),
      m_qSectionFrames(-1) {}

void RawSector::pushData(const std::vector<uint8_t>& inData) {
  m_data = inData;
}

void RawSector::pushErrorData(const std::vector<uint8_t>& inData) {
  m_errorData = inData;
}

void RawSector::pushPaddedData(const std::vector<uint8_t>& inData) {
  m_paddedData = inData;
}

const std::vector<uint8_t>& RawSector::data() const { return m_data; }

const std::vector<uint8_t>& RawSector::errorData() const { return m_errorData; }

const std::vector<uint8_t>& RawSector::paddedData() const {
  return m_paddedData;
}

uint32_t RawSector::size() const {
  return static_cast<uint32_t>(m_data.size());
}

void RawSector::showData() {
  // The per-line hex strings below are expensive to build; skip entirely when
  // the trace level this logs at is not active.
  if (!orc::get_logger()->should_log(spdlog::level::trace)) return;

  const int bytesPerLine = 48;
  bool hasError = false;

  // Extract the sector address data (note: this is not verified as correct)
  int32_t min = bcdToInt(m_data[12]);
  int32_t sec = bcdToInt(m_data[13]);
  int32_t frame = bcdToInt(m_data[14]);
  SectorAddress address(static_cast<uint8_t>(min), static_cast<uint8_t>(sec),
                        static_cast<uint8_t>(frame));

  for (int offset = 0; offset < static_cast<int>(m_data.size());
       offset += bytesPerLine) {
    // Print offset
    std::string line = "RawSector::showData() - [" + address.toString() + "] ";
    char offsetStr[16];
    snprintf(offsetStr, sizeof(offsetStr), "%06x: ", offset);
    line += offsetStr;

    // Print hex values
    for (int i = 0;
         i < bytesPerLine && (offset + i) < static_cast<int>(m_data.size());
         ++i) {
      if (m_errorData[offset + i] == 0) {
        char hexStr[4];
        snprintf(hexStr, sizeof(hexStr), "%02x ", m_data[offset + i]);
        line += hexStr;
      } else {
        line += "XX ";
        hasError = true;
      }
    }

    ORC_LOG_TRACE("{}", line);
  }

  if (hasError) {
    ORC_LOG_TRACE("RawSector contains errors");
  }
}

uint8_t RawSector::bcdToInt(uint8_t bcd) {
  return static_cast<uint8_t>((bcd >> 4) * 10 + (bcd & 0x0F));
}

// Sector class
// The sector is 2048 bytes and contains user data only (post error correction)
Sector::Sector()
    : m_data(std::vector<uint8_t>(2048, 0)),
      m_errorData(std::vector<uint8_t>(2048, 0)),
      m_mode(1),
      m_validData(false),
      m_qSectionFrames(-1),
      m_addressTrusted(false) {}

void Sector::pushData(const std::vector<uint8_t>& inData) { m_data = inData; }

void Sector::pushErrorData(const std::vector<uint8_t>& inData) {
  m_errorData = inData;
}

void Sector::pushPaddedData(const std::vector<uint8_t>& inData) {
  m_paddedData = inData;
}

const std::vector<uint8_t>& Sector::data() const { return m_data; }

const std::vector<uint8_t>& Sector::errorData() const { return m_errorData; }

const std::vector<uint8_t>& Sector::paddedData() const { return m_paddedData; }

uint32_t Sector::size() const { return static_cast<uint32_t>(m_data.size()); }

void Sector::setAddress(SectorAddress address) { m_address = address; }

SectorAddress Sector::address() const { return m_address; }

void Sector::setMode(int32_t mode) {
  // -1 is invalid/unknown
  // 0 is mode 0
  // 1 is mode 1
  // 2 is mode 2

  if (mode < -1 || mode > 2) {
    ORC_LOG_ERROR("Sector::setMode(): Invalid mode value of {}", mode);
    throw efm::EfmDecodeError(__func__);
  }
  m_mode = mode;
}

int32_t Sector::mode() const { return m_mode; }
