/*
 * File:        dec_channeltof3frame.cpp
 * Purpose:     efm-decoder-f2 - EFM T-values to F2 Section decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dec_channeltof3frame.h"

#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>
#include <queue>

#include "efm_constants.h"
#include "efm_exception.h"

namespace {

// Channel-frame layout (IEC 60908 §18, and the diagram in createF3Frame
// below): the 32 data symbols start at channel bit 44 and repeat every 17 bits
// - 14 symbol bits followed by 3 merging bits.
constexpr int kFirstDataBit = 44;
constexpr int kBitsPerSymbol = 17;
constexpr int kSymbolBits = 14;
constexpr int kDataSymbolsPerFrame = 32;

// Index of the first data symbol whose 14 bits reach bit `bitPos` or beyond.
int firstSymbolReaching(int bitPos) {
  const int offset = bitPos - (kFirstDataBit + kSymbolBits - 1);
  if (offset <= 0) return 0;
  return (offset + kBitsPerSymbol - 1) / kBitsPerSymbol;
}

// Index of the last data symbol that starts at or before bit `bitPos`; -1 when
// the first symbol has not started yet.
int lastSymbolStartingBy(int bitPos) {
  const int offset = bitPos - kFirstDataBit;
  if (offset < 0) return -1;
  return offset / kBitsPerSymbol;
}

}  // namespace

ChannelToF3Frame::ChannelToF3Frame() {
  // Statistics
  m_goodFrames = 0;
  m_undershootFrames = 0;
  m_overshootFrames = 0;

  m_validEfmSymbols = 0;
  m_invalidEfmSymbols = 0;

  m_validSubcodeSymbols = 0;
  m_invalidSubcodeSymbols = 0;
}

void ChannelToF3Frame::pushFrame(const std::vector<uint8_t>& data) {
  // Add the data to the input buffer
  m_inputBuffer.push(data);

  // Process queue
  processQueue();
}

void ChannelToF3Frame::pushFrame(std::vector<uint8_t>&& data) {
  // Move the data into the input buffer to avoid a deep copy.
  m_inputBuffer.push(std::move(data));

  // Process queue
  processQueue();
}

F3Frame ChannelToF3Frame::popFrame() {
  // Move the first item out of the output buffer to avoid a deep copy.
  F3Frame frame = std::move(m_outputBuffer.front());
  m_outputBuffer.pop();
  return frame;
}

bool ChannelToF3Frame::isReady() const {
  // Return true if the output buffer is not empty
  return !m_outputBuffer.empty();
}

void ChannelToF3Frame::processQueue() {
  while (!m_inputBuffer.empty()) {
    // Extract the first item in the input buffer
    std::vector<uint8_t> frameData = m_inputBuffer.front();
    m_inputBuffer.pop();

    // Count the number of bits in the frame. The frame carries *packed* EFM
    // bytes (t-value in the low nibble, producer doubt in the high nibble), so
    // only the t-value contributes to the bit count.
    int bitCount = 0;
    for (int i = 0; i < frameData.size(); ++i) {
      bitCount += orc::efm_tvalue(frameData.at(i));
    }

    // Generate statistics
    if (bitCount != efm::kEfmFrameChannelBits) {
      ORC_LOG_DEBUG(
          "ChannelToF3Frame::processQueue() - Frame data is {} bits (should be "
          "588)",
          bitCount);
    }
    if (bitCount == efm::kEfmFrameChannelBits) m_goodFrames++;
    if (bitCount < efm::kEfmFrameChannelBits) m_undershootFrames++;
    if (bitCount > efm::kEfmFrameChannelBits) m_overshootFrames++;

    // Create an F3 frame
    F3Frame f3Frame = createF3Frame(frameData);

    // Place the frame into the output buffer
    m_outputBuffer.push(f3Frame);
  }
}

F3Frame ChannelToF3Frame::createF3Frame(const std::vector<uint8_t>& tValues) {
  F3Frame f3Frame;

  // The channel frame data is:
  //   Sync Header: 24 bits (bits 0-23)
  //   Merging bits: 3 bits (bits 24-26)
  //   Subcode: 14 bits (bits 27-40)
  //   Merging bits: 3 bits (bits 41-43)
  //   Then 32x 17-bit data values (bits 44-587)
  //     Data: 14 bits
  //     Merging bits: 3 bits
  //
  // Giving a total of 588 bits

  // Convert the T-values to data, attributing each data symbol the doubt of
  // the T-values that produced it (issue #307).
  std::vector<uint8_t> frameData;
  const bool anyDoubt = tvaluesToData(tValues, frameData, m_symbolDoubt);

  // Extract the subcode in bits 27-40
  uint16_t subcode = m_efm.fourteenToEight(getBits(frameData, 27, 40));
  if (subcode == 300) {
    subcode = 0;
    m_invalidSubcodeSymbols++;
  } else {
    m_validSubcodeSymbols++;
  }

  // Extract the data values in bits 44-587 ignoring the merging bits
  std::vector<uint8_t> dataValues;
  std::vector<uint8_t> errorValues;
  dataValues.reserve(32);  // P-7: 32 data symbols per frame
  errorValues.reserve(32);
  for (int i = 44; i < (frameData.size() * 8) - 13; i += 17) {
    uint16_t dataValue = m_efm.fourteenToEight(getBits(frameData, i, i + 13));

    if (dataValue < 256) {
      dataValues.push_back(dataValue);
      errorValues.push_back(0);
      m_validEfmSymbols++;
    } else {
      dataValues.push_back(0);
      errorValues.push_back(1);
      m_invalidEfmSymbols++;
    }
  }

  // If the data values are not a multiple of 32 (due to undershoot), pad with
  // zeros. The padding is already flagged as an erasure, and no channel bits
  // were emitted for it, so its doubt is 0 in m_symbolDoubt already.
  while (dataValues.size() < 32) {
    dataValues.push_back(0);
    errorValues.push_back(1);
  }

  // Create an F3 frame...

  // Determine the frame type
  if (subcode == 256) {
    f3Frame.setFrameTypeAsSync0();
  } else if (subcode == 257) {
    f3Frame.setFrameTypeAsSync1();
  } else {
    f3Frame.setFrameTypeAsSubcode(subcode);
  }

  // Set the frame data
  f3Frame.setData(dataValues);
  f3Frame.setErrorData(errorValues);
  // Left unset (empty) when the producer trusted every symbol, which is the
  // whole-stream case for an .efm with no confidence information - see
  // Frame::doubtData().
  if (anyDoubt) f3Frame.setDoubtData(m_symbolDoubt);

  return f3Frame;
}

bool ChannelToF3Frame::tvaluesToData(const std::vector<uint8_t>& tValues,
                                     std::vector<uint8_t>& outputData,
                                     std::vector<uint8_t>& symbolDoubt) {
  // P-7: each T-value emits 3-11 *bits* (not 1), so a full 588-bit frame is ~74
  // bytes. Reserving tValues.size()/8 under-reserved ~4x and caused several
  // reallocations per frame; reserve for the maximum 11 bits per T-value.
  outputData.clear();
  outputData.reserve((tValues.size() * 11 + 7) / 8);
  if (m_collectDoubt) symbolDoubt.assign(kDataSymbolsPerFrame, 0);

  uint32_t bitBuffer = 0;  // Use 32-bit buffer to avoid frequent byte writes
  int bitsInBuffer = 0;

  // Issue #307: the doubt is attributed to symbols as the bits are laid down,
  // from the running bit position, rather than by building a per-channel-bit
  // vector and reducing it afterwards. Both give the same answer - a symbol
  // takes the greatest doubt of the T-values overlapping its 14 bits - but
  // this one is proportional to T-values (~74 per frame) instead of channel
  // bits (588), and this runs ~11.8M times per disc side.
  int bitPos = 0;
  bool anyDoubt = false;

  for (uint8_t packed : tValues) {
    // The input carries packed EFM bytes: t-value in the low nibble, the
    // producer's doubt about it in the high nibble.
    uint8_t tValue = orc::efm_tvalue(packed);
    const uint8_t doubt = m_collectDoubt ? orc::efm_doubt(packed) : 0;

    // R-1: an out-of-range T-value on this path is dirty-capture data, not an
    // invariant violation, so clamp into the valid IEC 60908 §13 range [3, 11]
    // instead of aborting the whole decode. A clamped symbol corrupts the local
    // bit alignment, which the downstream 14->8 EFM decode flags as an invalid
    // symbol (erasure) - the correct degradation for a bad capture.
    if (tValue < 3 || tValue > 11) {
      ORC_LOG_DEBUG(
          "ChannelToF3Frame::tvaluesToData(): T-value {} out of range [3,11], "
          "clamping.",
          tValue);
      tValue = std::clamp<uint8_t>(tValue, 3, 11);
    }

    // Shift in 1 followed by (tValue-1) zeros
    bitBuffer = (bitBuffer << tValue) | (1U << (tValue - 1));
    bitsInBuffer += tValue;

    // Attribute this T-value's doubt to every data symbol its bits fall in. A
    // T-value landing wholly in the 3 merging bits between two symbols reaches
    // neither, which the empty [first, last] range expresses naturally.
    if (m_collectDoubt && doubt != 0) {
      const int first = firstSymbolReaching(bitPos);
      const int last = std::min(lastSymbolStartingBy(bitPos + tValue - 1),
                                kDataSymbolsPerFrame - 1);
      for (int symbol = first; symbol <= last; ++symbol) {
        if (doubt > symbolDoubt[symbol]) {
          symbolDoubt[symbol] = doubt;
          anyDoubt = true;
        }
      }
    }
    bitPos += tValue;

    // Write complete bytes when we have 8 or more bits
    while (bitsInBuffer >= 8) {
      outputData.push_back(static_cast<char>(bitBuffer >> (bitsInBuffer - 8)));
      bitsInBuffer -= 8;
    }
  }

  // Handle remaining bits
  if (bitsInBuffer > 0) {
    bitBuffer <<= (8 - bitsInBuffer);
    outputData.push_back(static_cast<char>(bitBuffer));
  }

  return anyDoubt;
}

uint16_t ChannelToF3Frame::getBits(const std::vector<uint8_t>& data,
                                   int startBit, int endBit) {
  // Validate input
  if (startBit < 0 || startBit > 587 || endBit < 0 || endBit > 587 ||
      startBit > endBit) {
    ORC_LOG_ERROR("ChannelToF3Frame::getBits(): Invalid bit range {}-{}",
                  startBit, endBit);
    throw efm::EfmDecodeError(__func__);
  }

  int startByte = startBit / 8;
  int endByte = endBit / 8;

  if (endByte >= data.size()) {
    ORC_LOG_ERROR(
        "ChannelToF3Frame::getBits(): Byte index {} exceeds data size {}",
        endByte, data.size());
    throw efm::EfmDecodeError(__func__);
  }

  // Fast path for bits within a single byte
  if (startByte == endByte) {
    uint8_t mask = (0xFF >> (startBit % 8)) & (0xFF << (7 - (endBit % 8)));
    return (data[startByte] & mask) >> (7 - (endBit % 8));
  }

  // Handle multi-byte case
  uint16_t result = 0;
  int bitsRemaining = endBit - startBit + 1;

  // Handle first byte
  int firstByteBits = 8 - (startBit % 8);
  uint8_t mask = 0xFF >> (startBit % 8);
  result = (data[startByte] & mask) << (bitsRemaining - firstByteBits);

  // Handle middle bytes
  for (int i = startByte + 1; i < endByte; i++) {
    result |= (data[i] & 0xFF)
              << (bitsRemaining - firstByteBits - 8 * (i - startByte));
  }

  // Handle last byte
  int lastByteBits = (endBit % 8) + 1;
  mask = 0xFF << (8 - lastByteBits);
  result |= (data[endByte] & mask) >> (8 - lastByteBits);

  return result;
}

void ChannelToF3Frame::showStatistics() const {
  ORC_LOG_INFO("Channel to F3 Frame statistics:");
  ORC_LOG_INFO("  Channel Frames:");
  ORC_LOG_INFO("    Total: {}",
               m_goodFrames + m_undershootFrames + m_overshootFrames);
  ORC_LOG_INFO("    Good: {}", m_goodFrames);
  ORC_LOG_INFO("    Undershoot: {}", m_undershootFrames);
  ORC_LOG_INFO("    Overshoot: {}", m_overshootFrames);
  ORC_LOG_INFO("  EFM symbols:");
  ORC_LOG_INFO("    Valid: {}", m_validEfmSymbols);
  ORC_LOG_INFO("    Invalid: {}", m_invalidEfmSymbols);
  ORC_LOG_INFO("  Subcode symbols:");
  ORC_LOG_INFO("    Valid: {}", m_validSubcodeSymbols);
  ORC_LOG_INFO("    Invalid: {}", m_invalidSubcodeSymbols);
}
