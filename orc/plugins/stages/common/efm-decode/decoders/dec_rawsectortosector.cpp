/*
 * File:        dec_rawsectortosector.cpp
 * Purpose:     efm-decoder-data - EFM Data24 to data decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dec_rawsectortosector.h"

#include <orc/support/logging.h>

#include <utility>
#include <vector>

#include "../efm-lib/efm_constants.h"
#include "../efm-lib/efm_exception.h"

// This table is the CRC32 look-up for the EDC data
static constexpr uint32_t crc32Lut[256] = {
    0x00000000, 0x90910101, 0x91210201, 0x01B00300, 0x92410401, 0x02D00500,
    0x03600600, 0x93F10701, 0x94810801, 0x04100900, 0x05A00A00, 0x95310B01,
    0x06C00C00, 0x96510D01, 0x97E10E01, 0x07700F00, 0x99011001, 0x09901100,
    0x08201200, 0x98B11301, 0x0B401400, 0x9BD11501, 0x9A611601, 0x0AF01700,
    0x0D801800, 0x9D111901, 0x9CA11A01, 0x0C301B00, 0x9FC11C01, 0x0F501D00,
    0x0EE01E00, 0x9E711F01, 0x82012001, 0x12902100, 0x13202200, 0x83B12301,
    0x10402400, 0x80D12501, 0x81612601, 0x11F02700, 0x16802800, 0x86112901,
    0x87A12A01, 0x17302B00, 0x84C12C01, 0x14502D00, 0x15E02E00, 0x85712F01,
    0x1B003000, 0x8B913101, 0x8A213201, 0x1AB03300, 0x89413401, 0x19D03500,
    0x18603600, 0x88F13701, 0x8F813801, 0x1F103900, 0x1EA03A00, 0x8E313B01,
    0x1DC03C00, 0x8D513D01, 0x8CE13E01, 0x1C703F00, 0xB4014001, 0x24904100,
    0x25204200, 0xB5B14301, 0x26404400, 0xB6D14501, 0xB7614601, 0x27F04700,
    0x20804800, 0xB0114901, 0xB1A14A01, 0x21304B00, 0xB2C14C01, 0x22504D00,
    0x23E04E00, 0xB3714F01, 0x2D005000, 0xBD915101, 0xBC215201, 0x2CB05300,
    0xBF415401, 0x2FD05500, 0x2E605600, 0xBEF15701, 0xB9815801, 0x29105900,
    0x28A05A00, 0xB8315B01, 0x2BC05C00, 0xBB515D01, 0xBAE15E01, 0x2A705F00,
    0x36006000, 0xA6916101, 0xA7216201, 0x37B06300, 0xA4416401, 0x34D06500,
    0x35606600, 0xA5F16701, 0xA2816801, 0x32106900, 0x33A06A00, 0xA3316B01,
    0x30C06C00, 0xA0516D01, 0xA1E16E01, 0x31706F00, 0xAF017001, 0x3F907100,
    0x3E207200, 0xAEB17301, 0x3D407400, 0xADD17501, 0xAC617601, 0x3CF07700,
    0x3B807800, 0xAB117901, 0xAAA17A01, 0x3A307B00, 0xA9C17C01, 0x39507D00,
    0x38E07E00, 0xA8717F01, 0xD8018001, 0x48908100, 0x49208200, 0xD9B18301,
    0x4A408400, 0xDAD18501, 0xDB618601, 0x4BF08700, 0x4C808800, 0xDC118901,
    0xDDA18A01, 0x4D308B00, 0xDEC18C01, 0x4E508D00, 0x4FE08E00, 0xDF718F01,
    0x41009000, 0xD1919101, 0xD0219201, 0x40B09300, 0xD3419401, 0x43D09500,
    0x42609600, 0xD2F19701, 0xD5819801, 0x45109900, 0x44A09A00, 0xD4319B01,
    0x47C09C00, 0xD7519D01, 0xD6E19E01, 0x46709F00, 0x5A00A000, 0xCA91A101,
    0xCB21A201, 0x5BB0A300, 0xC841A401, 0x58D0A500, 0x5960A600, 0xC9F1A701,
    0xCE81A801, 0x5E10A900, 0x5FA0AA00, 0xCF31AB01, 0x5CC0AC00, 0xCC51AD01,
    0xCDE1AE01, 0x5D70AF00, 0xC301B001, 0x5390B100, 0x5220B200, 0xC2B1B301,
    0x5140B400, 0xC1D1B501, 0xC061B601, 0x50F0B700, 0x5780B800, 0xC711B901,
    0xC6A1BA01, 0x5630BB00, 0xC5C1BC01, 0x5550BD00, 0x54E0BE00, 0xC471BF01,
    0x6C00C000, 0xFC91C101, 0xFD21C201, 0x6DB0C300, 0xFE41C401, 0x6ED0C500,
    0x6F60C600, 0xFFF1C701, 0xF881C801, 0x6810C900, 0x69A0CA00, 0xF931CB01,
    0x6AC0CC00, 0xFA51CD01, 0xFBE1CE01, 0x6B70CF00, 0xF501D001, 0x6590D100,
    0x6420D200, 0xF4B1D301, 0x6740D400, 0xF7D1D501, 0xF661D601, 0x66F0D700,
    0x6180D800, 0xF111D901, 0xF0A1DA01, 0x6030DB00, 0xF3C1DC01, 0x6350DD00,
    0x62E0DE00, 0xF271DF01, 0xEE01E001, 0x7E90E100, 0x7F20E200, 0xEFB1E301,
    0x7C40E400, 0xECD1E501, 0xED61E601, 0x7DF0E700, 0x7A80E800, 0xEA11E901,
    0xEBA1EA01, 0x7B30EB00, 0xE8C1EC01, 0x7850ED00, 0x79E0EE00, 0xE971EF01,
    0x7700F000, 0xE791F101, 0xE621F201, 0x76B0F300, 0xE541F401, 0x75D0F500,
    0x7460F600, 0xE4F1F701, 0xE381F801, 0x7310F900, 0x72A0FA00, 0xE231FB01,
    0x71C0FC00, 0xE151FD01, 0xE0E1FE01, 0x7070FF00};

RawSectorToSector::RawSectorToSector()
    : m_validSectors(0),
      m_invalidSectors(0),
      m_correctedSectors(0),
      m_mode0Sectors(0),
      m_mode1Sectors(0),
      m_mode2Sectors(0),
      m_invalidModeSectors(0),
      m_implausibleHeaders(0),
      m_rspcQCleanCodewords(0),
      m_rspcQCorrectedCodewords(0),
      m_rspcPCleanCodewords(0),
      m_rspcPCorrectedCodewords(0) {}

void RawSectorToSector::pushSector(const RawSector& rawSector) {
  // Add the data to the input buffer
  m_inputBuffer.push_back(rawSector);

  // Process the queue
  processQueue();
}

Sector RawSectorToSector::popSector() {
  // Move the first item out of the output buffer to avoid a deep copy.
  Sector sector = std::move(m_outputBuffer.front());
  m_outputBuffer.pop_front();
  return sector;
}

bool RawSectorToSector::isReady() const {
  // Return true if the output buffer is not empty
  return !m_outputBuffer.empty();
}

// Note: Mode 0 and Mode 2 support missing
// Note: Does not fill missing sectors
void RawSectorToSector::processQueue() {
  while (!m_inputBuffer.empty()) {
    // Get the first item in the input buffer (move to avoid a deep copy)
    RawSector rawSector = std::move(m_inputBuffer.front());
    m_inputBuffer.pop_front();
    bool rawSectorValid = false;

    // Verify the data sizes (sanity check)
    if (rawSector.data().size() != efm::kRawSectorSize) {
      ORC_LOG_DEBUG(
          "RawSectorToSector::processQueue(): Sector data size is incorrect. "
          "Expected 2352 bytes, got {} bytes",
          rawSector.data().size());
      ORC_LOG_CRITICAL(
          "RawSectorToSector::processQueue(): Sector data size is incorrect");
      throw efm::EfmDecodeError(__func__);
    }

    if (rawSector.errorData().size() != efm::kRawSectorSize) {
      ORC_LOG_DEBUG(
          "RawSectorToSector::processQueue(): Sector error data size is "
          "incorrect. Expected 2352 bytes, got {} bytes",
          rawSector.errorData().size());
      ORC_LOG_CRITICAL(
          "RawSectorToSector::processQueue(): Sector error data size is "
          "incorrect");
      throw efm::EfmDecodeError(__func__);
    }

    if (rawSector.paddedData().size() != efm::kRawSectorSize) {
      ORC_LOG_DEBUG(
          "RawSectorToSector::processQueue(): Sector padded data size is "
          "incorrect. Expected 2352 bytes, got {} bytes",
          rawSector.paddedData().size());
      ORC_LOG_CRITICAL(
          "RawSectorToSector::processQueue(): Sector padded data size is "
          "incorrect");
      throw efm::EfmDecodeError(__func__);
    }

    // Determine the sector mode (for modes 0 and 2 there is no correction
    // available)
    int32_t mode = 0;

    // Is the mode byte valid (not error or padding)?
    if (static_cast<uint8_t>(rawSector.errorData()[15]) != 0) {
      // Mode byte is invalid
      ORC_LOG_DEBUG(
          "RawSectorToSector::processQueue(): Sector mode byte is invalid. "
          "Assuming it's mode 1");
      mode = -1;
    } else {
      // Extract the sector mode data
      if (static_cast<uint8_t>(rawSector.data()[15]) == 0) {
        mode = 0;
      } else if (static_cast<uint8_t>(rawSector.data()[15]) == 1) {
        mode = 1;
      } else if (static_cast<uint8_t>(rawSector.data()[15]) == 2) {
        mode = 2;
      } else {
        mode = -1;
      }

      if (mode != 1) {
        ORC_LOG_DEBUG(
            "RawSectorToSector::processQueue(): Sector mode byte is valid, but "
            "mode isn't? Mode reported as {}",
            static_cast<uint8_t>(rawSector.data()[15]));
      }
    }

    // If the mode is invalid, we try to treat the sector as mode 1 to see if
    // the error correction makes the mode metadata valid.  If it doesn't we
    // discard the sector as error
    if (mode == 1 || mode == -1) {
      // Compute the CRC32 of the sector data based on the EDC word
      uint32_t originalEdcWord =
          ((static_cast<uint32_t>(static_cast<uint8_t>(rawSector.data()[2064])))
           << 0) |
          ((static_cast<uint32_t>(static_cast<uint8_t>(rawSector.data()[2065])))
           << 8) |
          ((static_cast<uint32_t>(static_cast<uint8_t>(rawSector.data()[2066])))
           << 16) |
          ((static_cast<uint32_t>(static_cast<uint8_t>(rawSector.data()[2067])))
           << 24);

      uint32_t edcWord = crc32(rawSector.data(), 2064);

      // If the CRC32 of the sector data is incorrect, attempt to correct it
      // using Q and P parity
      if (originalEdcWord != edcWord) {
        ORC_LOG_DEBUG(
            "RawSectorToSector::processQueue(): CRC32 error - sector data is "
            "corrupt. EDC: {} Calculated: {} attempting to correct",
            originalEdcWord, edcWord);

        // Attempt Q and P parity error correction on the sector data.
        //
        // E-3: RSPC is a product code - P corrections create newly-correctable
        // Q words and vice versa - so iterate {Q pass; P pass} until the EDC
        // passes, a pass makes no change, or an iteration cap is reached
        // (standard CD-ROM decoders iterate 2-4 rounds). E-4 makes the flag
        // maintenance across passes effective.
        Rspc rspc;

        std::vector<uint8_t> correctedData = rawSector.data();
        std::vector<uint8_t> correctedErrorData = rawSector.errorData();

        constexpr int kMaxRspcIterations = 4;
        uint32_t correctedEdcWord = 0;
        bool edcNowValid = false;
        for (int iteration = 0; iteration < kMaxRspcIterations; ++iteration) {
          const std::vector<uint8_t> beforeData = correctedData;
          const std::vector<uint8_t> beforeErrors = correctedErrorData;

          rspc.qParityEcc(correctedData, correctedErrorData);
          rspc.pParityEcc(correctedData, correctedErrorData);

          correctedEdcWord =
              (static_cast<uint32_t>(correctedData[2064]) << 0) |
              (static_cast<uint32_t>(correctedData[2065]) << 8) |
              (static_cast<uint32_t>(correctedData[2066]) << 16) |
              (static_cast<uint32_t>(correctedData[2067]) << 24);
          edcWord = crc32(correctedData, 2064);

          if (correctedEdcWord == edcWord) {
            edcNowValid = true;
            break;
          }
          // Convergence check: if a full round changed nothing, further passes
          // cannot help.
          if (correctedData == beforeData &&
              correctedErrorData == beforeErrors) {
            break;
          }
        }

        // E-8(e): fold this sector's codeword-level RSPC activity into the
        // cumulative statistics before the rspc instance goes out of scope.
        m_rspcQCleanCodewords += rspc.qCleanCodewords();
        m_rspcQCorrectedCodewords += rspc.qCorrectedCodewords();
        m_rspcPCleanCodewords += rspc.pCleanCodewords();
        m_rspcPCorrectedCodewords += rspc.pCorrectedCodewords();

        // Copy the (best-effort) corrected data back to the raw sector
        rawSector.pushData(correctedData);
        rawSector.pushErrorData(correctedErrorData);

        // Is the CRC now correct?
        if (!edcNowValid) {
          // Error correction failed - sector is invalid and there's nothing
          // more we can do

          if (mode == 1) {
            ORC_LOG_DEBUG(
                "RawSectorToSector::processQueue(): CRC32 error - sector data "
                "cannot be recovered. EDC: {} Calculated: {} post correction",
                correctedEdcWord, edcWord);
            rawSectorValid = false;
          } else {
            // Mode was invalid as the sector is completely invalid.  This is
            // probably padding of some sort
            ORC_LOG_DEBUG(
                "RawSectorToSector::processQueue(): Sector mode was invalid "
                "and the sector doesn't appear to be mode 1");
            m_invalidModeSectors++;
            rawSectorValid = false;
          }
        } else {
          // Sector was invalid, but now corrected
          ORC_LOG_DEBUG(
              "RawSectorToSector::processQueue(): Sector data corrected. EDC: "
              "{} Calculated: {}",
              correctedEdcWord, edcWord);
          m_correctedSectors++;
          m_mode1Sectors++;  // E-8: count the successfully-corrected mode-1
          rawSectorValid = true;
          mode = 1;  // If error correction worked... this a mode 1 sector
        }
      } else {
        // Original sector data is valid
        m_validSectors++;
        rawSectorValid = true;

        // It's possible that the original mode byte was marked as error, but
        // the RSCP error correction was able to correct the data.  In this
        // case, we need to update the mode byte
        if (static_cast<uint8_t>(rawSector.data()[15]) == 0) {
          mode = 0;
        } else if (static_cast<uint8_t>(rawSector.data()[15]) == 1) {
          mode = 1;
        } else if (static_cast<uint8_t>(rawSector.data()[15]) == 2) {
          mode = 2;
        } else {
          mode = -1;
        }

        if (mode == 0) {
          m_mode0Sectors++;
        } else if (mode == 1) {
          m_mode1Sectors++;
        } else if (mode == 2) {
          m_mode2Sectors++;
        } else {
          // R-2: a 32-bit EDC admits a rare false-valid, so an out-of-range
          // mode byte here is a disc condition (corrupt sector that happened to
          // pass EDC), not an invariant violation. Discard this sector - the
          // same treatment as the failed-EDC invalid-mode path above - rather
          // than aborting the entire decode.
          ORC_LOG_DEBUG(
              "RawSectorToSector::processQueue(): EDC passed (EDC: {} "
              "Calculated: {}) but mode byte {} is out of range - discarding "
              "sector.",
              originalEdcWord, edcWord,
              static_cast<uint8_t>(rawSector.data()[15]));
          m_validSectors--;  // undo the increment above - not a valid sector
          m_invalidModeSectors++;
          rawSectorValid = false;
        }
      }
    } else {
      // Mode 0 and Mode 2 sectors carry no P/Q EDC, so they are not corrected.
      if (mode == 0) {
        m_mode0Sectors++;
        // C-5: ECMA-130 §14.2 - a mode-0 sector's bytes 16-2351 shall all be
        // (00). Verify and warn if not, so genuinely non-zero mode-0 data is
        // surfaced rather than silently accepted.
        const std::vector<uint8_t>& d = rawSector.data();
        bool allZero = true;
        for (size_t i = 16; i < efm::kRawSectorSize && allZero; ++i) {
          if (d[i] != 0) allZero = false;
        }
        if (!allZero) {
          ORC_LOG_WARN(
              "RawSectorToSector::processQueue(): Mode 0 sector contains "
              "non-zero user data (ECMA-130 §14.2 requires all-zero).");
        }
      } else if (mode == 2) {
        m_mode2Sectors++;
        // C-5: ECMA-130 §14.2 - a mode-2 sector's user data is 2336 bytes
        // (16-2351). These are now emitted in full below (sized by mode); the
        // sink writes sector.size() bytes so the larger payload round-trips.
      }
      rawSectorValid = true;
    }

    // E-5 / R-5: even a sector that still fails EDC is more useful emitted with
    // per-byte flags (dataValid=false) than dropped and replaced downstream by
    // an all-zero dummy. Its address, however, is only as good as its 4-byte
    // header (bytes 12-15).
    //
    // A passing EDC covers bytes 0-2063, the header included, so it verifies
    // the address outright - that is the only condition under which the address
    // may be treated as trusted. Clear C2 error flags are much weaker evidence:
    // a mis-corrected symbol is unflagged and wrong, which is exactly how a
    // single bad sector used to claim a six-figure address jump. Such a sector
    // is still emitted, marked untrusted, for SectorCorrection to place from
    // the Q-channel timeline.
    const std::vector<uint8_t>& sectorData = rawSector.data();
    const std::vector<uint8_t>& sectorErrorData = rawSector.errorData();
    const bool headerUnflagged =
        sectorErrorData[12] == 0 && sectorErrorData[13] == 0 &&
        sectorErrorData[14] == 0 && sectorErrorData[15] == 0;

    // C-6: lead-in-area data sector headers use MIN + 0xA0 (ECMA-130 §14.2(a)).
    // Such sectors are not program-area data; discard them rather than emitting
    // a bogus address. Checked before plausibility so a lead-in header is not
    // reported as corrupt.
    if (sectorData[12] >= 0xA0) {
      ORC_LOG_DEBUG(
          "RawSectorToSector::processQueue(): Lead-in sector header (MIN byte "
          "{:#04x}) - discarding.",
          static_cast<uint8_t>(sectorData[12]));
      if (rawSectorValid) m_invalidSectors++;
      continue;
    }

    // R-5: the header MSF is BCD (ECMA-130 §14.2). bcdToInt() cannot represent
    // a nibble above 9, and SectorAddress::setTime() clamps out-of-range fields
    // rather than rejecting them, so without this check a corrupt header
    // silently yields a plausible-looking but wrong address. SEC and FRAME have
    // hard ceilings of 59 and 74.
    const bool headerPlausible =
        isBcd(sectorData[12]) && isBcd(sectorData[13]) &&
        isBcd(sectorData[14]) && bcdToInt(sectorData[13]) <= 59 &&
        bcdToInt(sectorData[14]) <= 74;

    // The address may be believed on its own only when EDC verified it.
    const bool addressTrusted = rawSectorValid && headerPlausible;

    // A sector can be emitted if there is any sound way to place it: its own
    // EDC-verified address, a Q-channel reference to predict one from, or -
    // failing both - the legacy best-effort header (unflagged and well-formed,
    // which is weak evidence but better than dropping real data). With none of
    // those, discard it and let SectorCorrection fill the gap.
    const bool haveQReference = rawSector.qSectionFrames() >= 0;
    const bool placeable = addressTrusted || haveQReference ||
                           (headerUnflagged && headerPlausible);
    if (!placeable) {
      m_invalidSectors++;
      continue;
    }
    if (!headerPlausible) m_implausibleHeaders++;
    if (!rawSectorValid) m_invalidSectors++;

    // Extract the sector address data. An implausible header still needs *some*
    // address to construct the sector with; it is marked untrusted, so
    // SectorCorrection replaces it from the Q timeline before it is used.
    int32_t min = headerPlausible ? bcdToInt(sectorData[12]) : 0;
    int32_t sec = headerPlausible ? bcdToInt(sectorData[13]) : 0;
    int32_t frame = headerPlausible ? bcdToInt(sectorData[14]) : 0;

    SectorAddress sectorAddress(static_cast<uint8_t>(min),
                                static_cast<uint8_t>(sec),
                                static_cast<uint8_t>(frame));

    // Extract the sector mode data
    if (sectorData[15] == 0) {
      mode = 0;
    } else if (sectorData[15] == 1) {
      mode = 1;
    } else if (sectorData[15] == 2) {
      mode = 2;
    } else {
      mode = -1;
    }

    // Create an output sector
    Sector sector;
    sector.dataValid(rawSectorValid);
    sector.setAddress(sectorAddress);
    sector.setMode(mode);
    sector.setQSectionFrames(rawSector.qSectionFrames());
    sector.addressTrusted(addressTrusted);

    // C-5: size the emitted user-data payload by sector mode (ECMA-130 §14).
    // Mode 1 carries 2048 user bytes (16..2063); mode 2 (CD-ROM XA) carries
    // 2336 (16..2351). Mode 0 / unknown fall back to the 2048 layout so the
    // mode-1 output length is unchanged. The downstream sink writes
    // sector.size() bytes, so a variable-length payload round-trips intact.
    // data()/errorData() now return const references, so we index them
    // directly (P-2: no defensive whole-sector copies).
    constexpr std::vector<uint8_t>::difference_type kUserDataOffset = 16;
    constexpr std::vector<uint8_t>::difference_type kUserDataSizeMode1 =
        2048;  // ECMA-130 §14 (Mode 1 user data)
    constexpr std::vector<uint8_t>::difference_type kUserDataSizeMode2 =
        2336;  // ECMA-130 §14 (Mode 2 user data)
    const std::vector<uint8_t>::difference_type userDataSize =
        (mode == 2) ? kUserDataSizeMode2 : kUserDataSizeMode1;

    sector.pushData(std::vector<uint8_t>(
        sectorData.begin() + kUserDataOffset,
        sectorData.begin() + kUserDataOffset + userDataSize));
    sector.pushErrorData(std::vector<uint8_t>(
        sectorErrorData.begin() + kUserDataOffset,
        sectorErrorData.begin() + kUserDataOffset + userDataSize));

    // Add the sector to the output buffer
    m_outputBuffer.push_back(sector);
  }
}

// Convert 1 byte BCD to integer. Only meaningful for a value isBcd() accepts.
// static
uint8_t RawSectorToSector::bcdToInt(uint8_t bcd) {
  return (bcd >> 4) * 10 + (bcd & 0x0F);
}

// R-5: true when both nibbles are decimal digits, i.e. the byte is genuine BCD.
// static
bool RawSectorToSector::isBcd(uint8_t value) {
  return (value >> 4) <= 9 && (value & 0x0F) <= 9;
}

// CRC code adapted and used under GPLv3 from:
// https://github.com/claunia/edccchk/blob/master/edccchk.c
uint32_t RawSectorToSector::crc32(const std::vector<uint8_t>& src,
                                  int32_t size) {
  uint32_t crc = 0;
  const uint8_t* data = src.data();

  while (size--) {
    crc = (crc >> 8) ^ crc32Lut[(crc ^ (*data++)) & 0xFF];
  }

  return crc;
}

void RawSectorToSector::showStatistics() const {
  ORC_LOG_INFO("Raw Sector to Sector (RSPC error-correction):");
  ORC_LOG_INFO("  Valid sectors: {} (corrected: {})",
               m_validSectors + m_correctedSectors, m_correctedSectors);
  ORC_LOG_INFO("  Invalid sectors: {}", m_invalidSectors);

  ORC_LOG_INFO("  Sector metadata:");
  ORC_LOG_INFO("    Mode 0 sectors: {}", m_mode0Sectors);
  ORC_LOG_INFO("    Mode 1 sectors: {}", m_mode1Sectors);
  ORC_LOG_INFO("    Mode 2 sectors: {}", m_mode2Sectors);
  ORC_LOG_INFO("    Invalid mode sectors: {}", m_invalidModeSectors);
  ORC_LOG_INFO("    Implausible header addresses: {}", m_implausibleHeaders);

  // E-8(e): report genuine RSPC repair activity separately from codewords that
  // decoded clean, so "already valid" is not conflated with "repaired".
  ORC_LOG_INFO("  RSPC codewords (on corrected sectors):");
  ORC_LOG_INFO("    Q-parity: {} corrected, {} clean",
               m_rspcQCorrectedCodewords, m_rspcQCleanCodewords);
  ORC_LOG_INFO("    P-parity: {} corrected, {} clean",
               m_rspcPCorrectedCodewords, m_rspcPCleanCodewords);
}
