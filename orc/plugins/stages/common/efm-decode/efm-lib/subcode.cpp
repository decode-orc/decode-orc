/*
 * File:        subcode.cpp
 * Purpose:     EFM-library - Convert subcode data to FrameMetadata and back
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "subcode.h"

#include <orc/support/logging.h>

#include <cstdio>
#include <cstdlib>

#include "efm_exception.h"
#include "hex_utils.h"

// Takes 98 bytes of subcode data and returns a FrameMetadata object
SectionMetadata Subcode::fromData(const std::vector<uint8_t>& data) {
  // Ensure the data is 98 bytes long
  if (data.size() != 98) {
    ORC_LOG_ERROR(
        "Subcode::fromData(): Data size of {} does not match 98 bytes",
        static_cast<int>(data.size()));
    throw efm::EfmDecodeError(__func__);
  }

  // Extract the p-channel data and q-channel data
  std::vector<uint8_t> pChannelData;
  std::vector<uint8_t> qChannelData;
  pChannelData.resize(12);
  qChannelData.resize(12);

  // Note: index 0 and 1 are sync0 and sync1 bytes
  // so we get 96 bits of data per channel from 98 bytes input
  for (int index = 2; index < data.size(); ++index) {
    setBit(pChannelData, index - 2, data[index] & 0x80);
    setBit(qChannelData, index - 2, data[index] & 0x40);
  }

  // Create the SectionMetadata object
  SectionMetadata sectionMetadata;

  // Set the p-channel (p-channel is just repeating flag)
  // For correction purposes we will count the number of 0s and 1s
  // and set the flag to the majority value
  // C-3: pChannelData is the already-extracted 12-byte (96-bit) P bitfield -
  // the S0/S1 sync symbols were stripped during extraction above. The loop must
  // count all 12 bytes; starting at index 2 skipped the first 16 P bits (a
  // second, erroneous "skip S0/S1" offset) and biased the 96/2 majority vote.
  int oneCount = 0;
  for (int index = 0; index < pChannelData.size(); ++index) {
    // Count the number of bits set to 1 in pChannelData[index]
    oneCount += countBits(static_cast<uint8_t>(pChannelData[index]));
  }

  // if (oneCount != 96 && oneCount != 0) {
  //     if (m_showDebug) {
  //         tbcDebugStream() << "Subcode::fromData(): P channel data contains"
  //         << 96-oneCount << "zeros and"
  //                  << oneCount << "ones - indicating some p-channel
  //                  corruption";
  //     }
  // }

  if (oneCount > (96 / 2)) {
    sectionMetadata.setPFlag(true);
  } else {
    sectionMetadata.setPFlag(false);
  }

  // Set the q-channel
  // If the q-channel CRC is not valid, attempt to repair the data
  if (!isCrcValid(qChannelData)) {
    sectionMetadata.setRepaired(repairData(qChannelData));
  }

  if (isCrcValid(qChannelData)) {
    // Set the q-channel data from the subcode data

    // Get the address and control nybbles
    uint8_t controlNybble = qChannelData[0] >> 4;
    uint8_t modeNybble = qChannelData[0] & 0x0F;

    // Validate the mode nybble before processing (it is an unsigned nybble, so
    // the only bound that matters is the upper one).
    bool validMode = (modeNybble <= 0x4);

    if (!validMode) {
      // Invalid mode nybble - treat as corrupted data even though CRC passed
      ORC_LOG_DEBUG(
          "Subcode::fromData(): Invalid Q-mode nybble! Must be 0-4, got {} - Q "
          "channel data is: {}",
          modeNybble, HexUtils::vectorToHex(qChannelData));

      // Set the q-channel data to invalid and use default values
      // Extract absolute time for diagnostic purposes
      int32_t minutes = bcd2ToInt(qChannelData[7]);
      int32_t seconds = bcd2ToInt(qChannelData[8]);
      int32_t frames = bcd2ToInt(qChannelData[9]);

      if (minutes < 0) minutes = 0;
      if (minutes > 99) minutes = 99;  // C-1: minutes are BCD 00-99
      if (seconds < 0) seconds = 0;
      if (seconds > 59) seconds = 59;
      if (frames < 0) frames = 0;
      if (frames > 74) frames = 74;

      SectionTime badAbsTime = SectionTime(minutes, seconds, frames);
      ORC_LOG_DEBUG(
          "Subcode::fromData(): Potentially corrupt absolute time is: {}",
          badAbsTime.toString());
      sectionMetadata.setValid(false);
      return sectionMetadata;
    }

    // Q-1: Q-mode 0 must NOT be decoded as a mode-1 lead-in. IEC 60908 §17.5.4:
    // mode 0 "shall contain, if used, only the CONTROL and CRC bits, all other
    // bits are zero"; the NOTE says it MAY replace mode 1 on non-CD channels,
    // not that it should be decoded AS mode 1. Decoding all-zero DATA-Q as mode
    // 1 yields TNO=0 -> a "valid" lead-in at 00:00:00, so a mid-program mode-0
    // block (relevant to LaserDisc) was dropped as out-of-order and its audio
    // discarded. We still parse the control bits below (they are meaningful in
    // mode 0), then mark the section invalid so its audio is preserved via the
    // interpolation path like a CRC-failed section.
    bool isQMode0 = false;

    // Set the q-channel mode
    switch (modeNybble) {
      case 0x0:
        isQMode0 = true;
        // Placeholder mode so the control-nybble decode below runs; the section
        // is marked invalid before any mode-1 field decode.
        sectionMetadata.setQMode(SectionMetadata::QMode1);
        break;
      case 0x1:
        sectionMetadata.setQMode(SectionMetadata::QMode1);
        break;
      case 0x2:
        sectionMetadata.setQMode(SectionMetadata::QMode2);
        break;
      case 0x3:
        sectionMetadata.setQMode(SectionMetadata::QMode3);
        break;
      case 0x4:
        sectionMetadata.setQMode(SectionMetadata::QMode4);
        break;
      default:
        break;
    }

    // Validate the control nybble before processing.
    // C-4: control values 8-11 (1XXX) are decoded below as 4-channel audio.
    // IEC 60908-1999 §17.5 NOTE 2 redefines 1XXX as "broadcasting use" (the
    // 4-channel encoding is from the earlier Red Book / ECMA-130 edition); the
    // legacy mapping is kept deliberately as it is more useful for real legacy
    // discs. Values 12-15 (0xC-0xF) are treated as corrupt.
    bool validControl = (controlNybble <= 0x4) || (controlNybble == 0x6) ||
                        (controlNybble >= 0x8 && controlNybble <= 0xB);

    if (!validControl) {
      // Invalid control nybble - treat as corrupted data even though CRC passed
      ORC_LOG_DEBUG(
          "Subcode::fromData(): Invalid control nybble! Must be 0-4, 6, or "
          "8-11, got {} - Q channel data is: {}",
          controlNybble, HexUtils::vectorToHex(qChannelData));

      // Set the q-channel data to invalid
      int32_t minutes = bcd2ToInt(qChannelData[7]);
      int32_t seconds = bcd2ToInt(qChannelData[8]);
      int32_t frames = bcd2ToInt(qChannelData[9]);

      if (minutes < 0) minutes = 0;
      if (minutes > 99) minutes = 99;  // C-1: minutes are BCD 00-99
      if (seconds < 0) seconds = 0;
      if (seconds > 59) seconds = 59;
      if (frames < 0) frames = 0;
      if (frames > 74) frames = 74;

      SectionTime badAbsTime = SectionTime(minutes, seconds, frames);
      ORC_LOG_DEBUG(
          "Subcode::fromData(): Potentially corrupt absolute time is: {}",
          badAbsTime.toString());
      sectionMetadata.setValid(false);
      return sectionMetadata;
    }

    // Set the q-channel control settings.
    //
    // Q-8: the switch does not cover the whole nybble - IEC 60908 SS17.5.1
    // leaves 0x5, 0x7 and 0xC-0xF unassigned - so record whether the value was
    // one the standard defines. Without that, a corrupt nybble landing on an
    // unassigned value falls through to the constructor's defaults and reports
    // itself as 2-channel audio with the same confidence as a real reading.
    bool controlValid = true;
    switch (controlNybble) {
      case 0x0:
        // AUDIO_2CH_NO_PREEMPHASIS_COPY_PROHIBITED
        sectionMetadata.setAudio(true);
        sectionMetadata.setCopyProhibited(true);
        sectionMetadata.setPreemphasis(false);
        sectionMetadata.set2Channel(true);
        break;
      case 0x1:
        // AUDIO_2CH_PREEMPHASIS_COPY_PROHIBITED
        sectionMetadata.setAudio(true);
        sectionMetadata.setCopyProhibited(true);
        sectionMetadata.setPreemphasis(true);
        sectionMetadata.set2Channel(true);
        break;
      case 0x2:
        // AUDIO_2CH_NO_PREEMPHASIS_COPY_PERMITTED
        sectionMetadata.setAudio(true);
        sectionMetadata.setCopyProhibited(false);
        sectionMetadata.setPreemphasis(false);
        sectionMetadata.set2Channel(true);
        break;
      case 0x3:
        // AUDIO_2CH_PREEMPHASIS_COPY_PERMITTED
        sectionMetadata.setAudio(true);
        sectionMetadata.setCopyProhibited(false);
        sectionMetadata.setPreemphasis(true);
        sectionMetadata.set2Channel(true);
        break;
      case 0x4:
        // DIGITAL_COPY_PROHIBITED
        sectionMetadata.setAudio(false);
        sectionMetadata.setCopyProhibited(true);
        sectionMetadata.setPreemphasis(false);
        sectionMetadata.set2Channel(true);
        break;
      case 0x6:
        // DIGITAL_COPY_PERMITTED
        sectionMetadata.setAudio(false);
        sectionMetadata.setCopyProhibited(false);
        sectionMetadata.setPreemphasis(false);
        sectionMetadata.set2Channel(true);
        break;
      case 0x8:
        // AUDIO_4CH_NO_PREEMPHASIS_COPY_PROHIBITED
        sectionMetadata.setAudio(true);
        sectionMetadata.setCopyProhibited(true);
        sectionMetadata.setPreemphasis(false);
        sectionMetadata.set2Channel(false);
        break;
      case 0x9:
        // AUDIO_4CH_PREEMPHASIS_COPY_PROHIBITED
        sectionMetadata.setAudio(true);
        sectionMetadata.setCopyProhibited(true);
        sectionMetadata.setPreemphasis(true);
        sectionMetadata.set2Channel(false);
        break;
      case 0xA:
        // AUDIO_4CH_NO_PREEMPHASIS_COPY_PERMITTED
        sectionMetadata.setAudio(true);
        sectionMetadata.setCopyProhibited(false);
        sectionMetadata.setPreemphasis(false);
        sectionMetadata.set2Channel(false);
        break;
      case 0xB:
        // AUDIO_4CH_PREEMPHASIS_COPY_PERMITTED
        sectionMetadata.setAudio(true);
        sectionMetadata.setCopyProhibited(false);
        sectionMetadata.setPreemphasis(true);
        sectionMetadata.set2Channel(false);
        break;
      default:
        ORC_LOG_DEBUG(
            "Subcode::fromData(): Q-channel control nybble {:#x} is not "
            "assigned by IEC 60908; the control flags for this section carry "
            "no information",
            controlNybble);
        controlValid = false;
        break;
    }
    sectionMetadata.setControlValid(controlValid);

    // Q-1: mode 0 carries no track/time fields; keep the parsed control bits
    // but mark the section invalid so it is reconstructed by interpolation
    // (audio preserved) instead of being misclassified as a valid 00:00:00
    // lead-in.
    if (isQMode0) {
      ORC_LOG_DEBUG(
          "Subcode::fromData(): Q-mode 0 section - control bits kept, marking "
          "invalid for interpolation.");
      sectionMetadata.setValid(false);
      return sectionMetadata;
    }

    if (sectionMetadata.qMode() == SectionMetadata::QMode1 ||
        sectionMetadata.qMode() == SectionMetadata::QMode4) {
      // Get the track number
      uint8_t trackNumber = bcd2ToInt(qChannelData[1]);

      // If the track number is 0, then this is a lead-in frame
      // If the track number is 0xAA, then this is a lead-out frame
      // If the track number is 1-99, then this is a user data frame
      bool isLeadin = false;
      if (trackNumber == 0) {
        isLeadin = true;
        sectionMetadata.setSectionType(SectionType(SectionType::LeadIn), 0);
        ORC_LOG_DEBUG(
            "Subcode::fromData(): Q-Mode 1/4 has track number 0 - this is a "
            "lead-in frame");
      } else if (trackNumber == 0xAA) {
        sectionMetadata.setSectionType(SectionType(SectionType::LeadOut), 0);
        ORC_LOG_DEBUG(
            "Subcode::fromData(): Q-Mode 1/4 has track number 0xAA - this is a "
            "lead-out frame");
      } else {
        sectionMetadata.setSectionType(SectionType(SectionType::UserData),
                                       trackNumber);
      }

      // Q-6: in the lead-in the Q-channel layout differs from the program area.
      // Byte 2 is POINT (a TOC pointer) and bytes 7-9 are PMIN/PSEC/PFRAME (the
      // TOC entry POINT refers to) rather than the running absolute time; only
      // bytes 3-5 (MIN/SEC/FRAME) are the running lead-in time and they DO
      // increment monotonically. Storing PMIN/PSEC/PFRAME as absolute time made
      // the time repeat/jump between TOC items so the timeline could never
      // settle inside a lead-in. Interpret the lead-in fields as TOC data and
      // use the running MIN/SEC/FRAME as both section and absolute time so the
      // lead-in has a monotonic timeline; the TOC itself is assembled
      // downstream (F2SectionCorrection) from POINT/PMIN/PSEC/PFRAME.
      if (isLeadin) {
        int32_t leadinMinutes = validateAndClampTimeValue(
            bcd2ToInt(qChannelData[3]), 99, "lead-in minutes", sectionMetadata);
        int32_t leadinSeconds = validateAndClampTimeValue(
            bcd2ToInt(qChannelData[4]), 59, "lead-in seconds", sectionMetadata);
        int32_t leadinFrames = validateAndClampTimeValue(
            bcd2ToInt(qChannelData[5]), 74, "lead-in frames", sectionMetadata);
        SectionTime leadinTime(static_cast<uint8_t>(leadinMinutes),
                               static_cast<uint8_t>(leadinSeconds),
                               static_cast<uint8_t>(leadinFrames));
        sectionMetadata.setSectionTime(leadinTime);
        sectionMetadata.setAbsoluteSectionTime(leadinTime);

        // POINT is kept raw (A0/A1/A2 are not BCD); PMIN/PSEC/PFRAME are
        // decoded BCD integers. A0/A1 reuse the "seconds"/"frames" slots for
        // the first/last track number and disc type, so they are not clamped as
        // a time here.
        sectionMetadata.setLeadinToc(
            static_cast<uint8_t>(qChannelData[2]), bcd2ToInt(qChannelData[7]),
            bcd2ToInt(qChannelData[8]), bcd2ToInt(qChannelData[9]));
      } else {
        // Program area: byte 2 is INDEX (00 = pause). Record the raw value so
        // downstream can distinguish pauses from audio.
        sectionMetadata.setIndex(bcd2ToInt(qChannelData[2]));

        // Set the frame time q_data_channel[3-5]
        // Validate BCD values to handle edge case where CRC passes but data is
        // corrupt. C-1: minutes are BCD 00-99 (IEC 60908 §17.5.1), not capped
        // at 59 - real discs exceed 60 minutes.
        int32_t sectionMinutes = validateAndClampTimeValue(
            bcd2ToInt(qChannelData[3]), 99, "section minutes", sectionMetadata);
        int32_t sectionSeconds = validateAndClampTimeValue(
            bcd2ToInt(qChannelData[4]), 59, "section seconds", sectionMetadata);
        int32_t sectionFrames = validateAndClampTimeValue(
            bcd2ToInt(qChannelData[5]), 74, "section frames", sectionMetadata);
        sectionMetadata.setSectionTime(
            SectionTime(sectionMinutes, sectionSeconds, sectionFrames));

        // Set the zero byte q_data_channel[6] - Not used at the moment

        // Set the ap time q_data_channel[7-9]
        // Validate BCD values to handle edge case where CRC passes but data is
        // corrupt
        int32_t absMinutes =
            validateAndClampTimeValue(bcd2ToInt(qChannelData[7]), 99,
                                      "absolute minutes", sectionMetadata);
        int32_t absSeconds =
            validateAndClampTimeValue(bcd2ToInt(qChannelData[8]), 59,
                                      "absolute seconds", sectionMetadata);
        int32_t absFrames = validateAndClampTimeValue(
            bcd2ToInt(qChannelData[9]), 74, "absolute frames", sectionMetadata);
        sectionMetadata.setAbsoluteSectionTime(
            SectionTime(absMinutes, absSeconds, absFrames));
      }
    } else if (sectionMetadata.qMode() == SectionMetadata::QMode2) {
      // Extract the 52-bit (13-digit) UPC/EAN media catalogue number.
      // The digits are packed as 13 BCD nibbles across q-channel bytes 1-7
      // (byte 9 holds the frame number, bytes 10-11 the CRC). We must read
      // nibbles rather than whole bytes: reading qChannelData[i + 1] for
      // i up to 12 ran past the end of the 12-byte buffer.
      uint64_t upc = 0;
      for (int digit = 0; digit < 13; ++digit) {
        const int byteIndex = 1 + (digit / 2);
        const uint8_t nibble =
            (digit % 2 == 0)
                ? static_cast<uint8_t>(qChannelData[byteIndex] >> 4)
                : static_cast<uint8_t>(qChannelData[byteIndex] & 0x0F);
        upc = upc * 10 + nibble;
      }
      // Q-3: store the full 13-digit catalogue number as a string so leading
      // zeros are preserved and the value is not truncated modulo 2^32.
      std::string upcString = std::to_string(upc);
      while (upcString.size() < 13) {
        upcString = "0" + upcString;
      }
      sectionMetadata.setUpcEanCode(upcString);

      ORC_LOG_DEBUG("Subcode::fromData(): Q-Mode 2 has UPC/EAN code of: {}",
                    upcString);

      // Only the absolute frame number is included for Q mode 2. Track number
      // and track-relative time are reconstructed from the surrounding mode-1
      // timeline in F2SectionCorrection (Q-2), so the values set here are just
      // placeholders.
      sectionMetadata.setSectionType(SectionType(SectionType::UserData), 1);
      sectionMetadata.setSectionTime(SectionTime(0, 0, 0));

      // Validate BCD value to handle edge case where CRC passes but data is
      // corrupt
      int32_t absFrames = validateAndClampTimeValue(
          bcd2ToInt(qChannelData[9]), 74, "absolute frames (QMode2)",
          sectionMetadata);
      sectionMetadata.setAbsoluteSectionTime(SectionTime(0, 0, absFrames));
    } else if (sectionMetadata.qMode() == SectionMetadata::QMode3) {
      // Q-4: Q-mode 3 (ISRC). Like mode 2 it encodes only a catalogue-style
      // field (the ISRC) plus AFRAME; TNO/INDEX/track-time are reconstructed
      // from the surrounding mode-1 timeline in F2SectionCorrection. Keep the
      // section VALID (extract AFRAME for continuity and so the statistics
      // count it) rather than discarding it - discarding forced its audio down
      // the all-error interpolation path and left the mode-3 correction branch
      // and statistic permanently dead.
      sectionMetadata.setSectionType(SectionType(SectionType::UserData), 1);
      sectionMetadata.setSectionTime(SectionTime(0, 0, 0));

      // Decode the ISRC characters (IEC 60908 §17.5.3): I1-I5 as 6-bit
      // graphic characters, I6-I12 as BCD digits.
      std::string isrc = decodeIsrc(qChannelData);
      if (!isrc.empty()) sectionMetadata.setIsrcCode(isrc);

      int32_t absFrames = validateAndClampTimeValue(
          bcd2ToInt(qChannelData[9]), 74, "absolute frames (QMode3)",
          sectionMetadata);
      sectionMetadata.setAbsoluteSectionTime(SectionTime(0, 0, absFrames));

      ORC_LOG_DEBUG(
          "Subcode::fromData(): Q-Mode 3 (ISRC) section, ISRC '{}', absolute "
          "frame {}",
          isrc, absFrames);
    } else {
      ORC_LOG_ERROR("Subcode::fromData(): Invalid Q-mode {}",
                    static_cast<int>(sectionMetadata.qMode()));
      throw efm::EfmDecodeError(__func__);
    }

    sectionMetadata.setValid(true);
  } else {
    // Set the q-channel data to invalid leaving the rest of
    // the metadata as default values
    char crcBuf1[32], crcBuf2[32];
    snprintf(crcBuf1, sizeof(crcBuf1), "%x",
             static_cast<int>(getQChannelCrc(qChannelData)));
    snprintf(crcBuf2, sizeof(crcBuf2), "%x",
             static_cast<int>(calculateQChannelCrc16(qChannelData)));
    ORC_LOG_DEBUG(
        "Subcode::fromData(): Invalid CRC in Q-channel data - expected: {} "
        "calculated: {}",
        crcBuf1, crcBuf2);

    // Range check the absolute time - as it is potentially corrupt and could be
    // out of range
    int32_t minutes = bcd2ToInt(qChannelData[7]);
    int32_t seconds = bcd2ToInt(qChannelData[8]);
    int32_t frames = bcd2ToInt(qChannelData[9]);

    if (minutes < 0) minutes = 0;
    if (minutes > 99) minutes = 99;  // C-1: minutes are BCD 00-99
    if (seconds < 0) seconds = 0;
    if (seconds > 59) seconds = 59;
    if (frames < 0) frames = 0;
    if (frames > 74) frames = 74;

    SectionTime badAbsTime = SectionTime(minutes, seconds, frames);
    ORC_LOG_DEBUG(
        "Subcode::fromData(): Q channel data is: {} potentially corrupt "
        "absolute time is: {}",
        HexUtils::vectorToHex(qChannelData), badAbsTime.toString());
    sectionMetadata.setValid(false);
  }

  // Sanity check the track number and frame type
  //
  // If the track number is 0, then this is a lead-in frame
  // If the track number is 0xAA, then this is a lead-out frame
  // If the track number is 1-99, then this is a user data frame
  const auto track_num = sectionMetadata.trackNumber();
  const auto section_type = sectionMetadata.sectionType().type();
  // Q-10: lead-out sections are internally track 0 (the 0xAA on the disc is
  // mapped to track 0 by setSectionType), so track 0 is legitimate for both
  // LeadIn and LeadOut. Only flag track 0 outside of those.
  const bool track0_mismatch =
      (track_num == 0 && section_type != SectionType::LeadIn &&
       section_type != SectionType::LeadOut);
  const bool trackAA_mismatch =
      (track_num == 0xAA && section_type != SectionType::LeadOut);
  const bool track_out_of_range = (track_num > 99);
  if (track0_mismatch) {
    ORC_LOG_DEBUG(
        "Subcode::fromData(): Track number 0 is only valid for lead-in/out "
        "frames");
  }
  if (trackAA_mismatch) {
    ORC_LOG_DEBUG(
        "Subcode::fromData(): Track number 0xAA is only valid for lead-out "
        "frames");
  }
  if (track_out_of_range) {
    ORC_LOG_DEBUG("Subcode::fromData(): Track number {} is out of range",
                  track_num);
  }

  if (sectionMetadata.isRepaired()) {
    ORC_LOG_DEBUG(
        "Subcode::fromData(): Q-channel repaired for section with absolute "
        "time: {} track number: {} and section time: {}",
        sectionMetadata.absoluteSectionTime().toString(),
        sectionMetadata.trackNumber(),
        sectionMetadata.sectionTime().toString());
  }

  // All done!
  return sectionMetadata;
}

uint8_t Subcode::countBits(uint8_t byteValue) {
  uint8_t count = 0;
  for (int i = 0; i < 8; ++i) {
    if (byteValue & (1 << i)) count++;
  }
  return count;
}

// Set a bit in a byte array
void Subcode::setBit(std::vector<uint8_t>& data, uint8_t bitPosition,
                     bool value) {
  // Check to ensure the bit position is valid
  if (bitPosition >= data.size() * 8) {
    ORC_LOG_ERROR(
        "Subcode::setBit(): Bit position {} is out of range for data size {}",
        bitPosition, static_cast<int>(data.size()));
    throw efm::EfmDecodeError(__func__);
  }

  // We need to convert this to a byte number and bit number within that byte
  uint8_t byteNumber = bitPosition / 8;
  uint8_t bitNumber = 7 - (bitPosition % 8);

  // Set the bit
  if (value) {
    data[byteNumber] =
        static_cast<uint8_t>(data[byteNumber] | (1 << bitNumber));  // Set bit
  } else {
    data[byteNumber] = static_cast<uint8_t>(data[byteNumber] &
                                            ~(1 << bitNumber));  // Clear bit
  }
}

bool Subcode::isCrcValid(const std::vector<uint8_t>& qChannelData) {
  // Get the CRC from the data
  uint16_t dataCrc = getQChannelCrc(qChannelData);

  // Calculate the CRC
  uint16_t calculatedCrc = calculateQChannelCrc16(qChannelData);

  // Check if the CRC is valid
  return dataCrc == calculatedCrc;
}

uint16_t Subcode::getQChannelCrc(const std::vector<uint8_t>& qChannelData) {
  // Get the CRC from the data
  return static_cast<uint16_t>(static_cast<uint8_t>(qChannelData[10]) << 8 |
                               static_cast<uint8_t>(qChannelData[11]));
}

// Generate a 16-bit CRC for the subcode data
// Adapted from http://mdfs.net/Info/Comp/Comms/CRC16.htm
uint16_t Subcode::calculateQChannelCrc16(
    const std::vector<uint8_t>& qChannelData) {
  std::size_t i;
  uint32_t crc = 0;

  // Create a copy excluding the last 2 bytes
  std::vector<uint8_t> data(qChannelData.begin(), qChannelData.end() - 2);

  for (int pos = 0; pos < data.size(); ++pos) {
    crc = crc ^ static_cast<uint32_t>(static_cast<uint8_t>(data[pos]) << 8);
    for (i = 0; i < 8; ++i) {
      crc = crc << 1;
      if (crc & 0x10000) crc = (crc ^ 0x1021) & 0xFFFF;
    }
  }

  // Invert the CRC
  crc = ~crc & 0xFFFF;

  return static_cast<uint16_t>(crc);
}

// Because of the way Q-channel data is spread over many frames, the most
// likely cause of a CRC error is a single bit error in the data. We can
// attempt to repair the data by flipping each bit in turn and checking
// the CRC.
//
// Perhaps there is some more effective way to repair the data, but this
// will do for now.
bool Subcode::repairData(std::vector<uint8_t>& qChannelData) {
  std::vector<uint8_t> dataCopy = qChannelData;

  // Q-10(b): trial-flip every one of the 96 Q-channel bits, including the 16
  // stored-CRC bits (80..95). A single-bit error that landed in the CRC field
  // itself is ~17% of all single-bit errors; flipping it back makes the stored
  // CRC match the (already-correct) data again, so the whole 96-bit word is
  // recoverable rather than only the 80 data bits.
  for (int i = 0; i < 96; ++i) {
    dataCopy = qChannelData;
    dataCopy[i / 8] =
        static_cast<uint8_t>(dataCopy[i / 8] ^ (1 << (7 - (i % 8))));

    if (isCrcValid(dataCopy)) {
      qChannelData = dataCopy;
      return true;
    }
  }

  return false;
}

// Validate and clamp time component values, marking section as repaired if
// needed Returns the clamped value
int32_t Subcode::validateAndClampTimeValue(int32_t value, int32_t maxValue,
                                           const std::string& valueName,
                                           SectionMetadata& sectionMetadata) {
  if (value > maxValue) {
    ORC_LOG_DEBUG(
        "Subcode::validateAndClampTimeValue(): Invalid {} value {} - marking "
        "section as repaired",
        valueName, value);
    sectionMetadata.setRepaired(true);
    return maxValue;
  }
  return value;
}

// Q-4: decode the 12-character ISRC from a Q-mode 3 subcode block.
//
// IEC 60908 §17.5.3 layout of the 72 data bits that follow the CONTROL/ADR
// byte (qChannelData[0]); bit positions below are absolute from the MSB of
// byte 0:
//   I1..I5  : 6-bit graphic characters   (bits  8..37)  -> country + owner
//   (2 zero bits)                        (bits 38..39)
//   I6..I12 : 4-bit BCD digits           (bits 40..67)  -> year + designation
//   (4 zero bits)                        (bits 68..71)
//   AFRAME  : BCD                        (bits 72..79, qChannelData[9])
//
// The 6-bit characters are encoded as (ASCII - 0x30): digits '0'-'9' map to
// 0x00-0x09 and letters 'A'-'Z' map to 0x11-0x2A. A real ISRC begins with
// alphanumeric characters, so a blank or corrupt field (any invalid character
// or out-of-range BCD digit) yields an empty string rather than a bogus code.
//
// Validated against real mode-3 subcode (T-Square disc, owner "SONY").
std::string Subcode::decodeIsrc(const std::vector<uint8_t>& qChannelData) {
  auto readBits = [&](int pos, int count) -> uint32_t {
    uint32_t value = 0;
    for (int i = 0; i < count; ++i) {
      const int bit = pos + i;
      const int byteIndex = bit / 8;
      const int bitIndex = 7 - (bit % 8);
      value = (value << 1) |
              ((static_cast<uint8_t>(qChannelData[byteIndex]) >> bitIndex) & 1);
    }
    return value;
  };

  auto decode6bit = [](uint32_t v) -> char {
    // CD subchannel ISRC characters are encoded as (ASCII - 0x30): digits
    // '0'-'9' -> 0x00-0x09, letters 'A'-'Z' -> 0x11-0x2A.
    char c = static_cast<char>(v + 0x30);
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) return c;
    return '\0';  // invalid / blank
  };

  std::string isrc;

  // I1..I5: five 6-bit alphanumeric characters starting at absolute bit 8.
  for (int i = 0; i < 5; ++i) {
    char c = decode6bit(readBits(8 + i * 6, 6));
    if (c == '\0') return "";  // blank/corrupt field
    isrc.push_back(c);
  }

  // I6..I12: seven BCD digits starting at absolute bit 40.
  for (int i = 0; i < 7; ++i) {
    uint32_t digit = readBits(40 + i * 4, 4);
    if (digit > 9) return "";  // not a valid BCD digit
    isrc.push_back(static_cast<char>('0' + digit));
  }

  return isrc;
}

// Convert BCD (Binary Coded Decimal) to integer
uint8_t Subcode::bcd2ToInt(uint8_t bcd) {
  uint16_t value = 0;
  uint16_t factor = 1;

  // Check for the lead out track exception of 0xAA
  // (See ECMA-130 22.3.3.1)
  if (bcd == 0xAA) {
    return 0xAA;
  }

  while (bcd > 0) {
    value += (bcd & 0x0F) * factor;
    bcd >>= 4;
    factor *= 10;
  }

  return value;
}
