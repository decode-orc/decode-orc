/*
 * File:        dec_tvaluestochannel.cpp
 * Purpose:     efm-decoder-f2 - EFM T-values to F2 Section decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dec_tvaluestochannel.h"

#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <queue>

#include "efm_constants.h"
#include "efm_exception.h"

namespace {

// The buffers handled here carry *packed* EFM bytes: the t-value in the low
// nibble and the producer's doubt in the high nibble (CVBS File Format
// Specification, EFM extension format - "Binary Data File"). Framing only ever
// needs the t-value, so every read of one masks the doubt away; the doubt is
// otherwise untouched and rides out with the frame it belongs to.

// True if the two packed bytes at `it` are the T11+T11 frame sync pair.
inline bool isSyncPair(std::vector<uint8_t>::const_iterator it) {
  return orc::efm_tvalue(*it) == efm::kSyncSymbolT11 &&
         orc::efm_tvalue(*(it + 1)) == efm::kSyncSymbolT11;
}

// Iterator to the first T11+T11 sync pair in [first, last), or `last` if there
// is none. Replaces std::search over a plain {T11, T11} pattern, which would
// match only bytes whose doubt nibble happened to be zero.
std::vector<uint8_t>::const_iterator findSyncPair(
    std::vector<uint8_t>::const_iterator first,
    std::vector<uint8_t>::const_iterator last) {
  if (std::distance(first, last) < 2) return last;
  for (auto it = first; it != last - 1; ++it) {
    if (isSyncPair(it)) return it;
  }
  return last;
}

}  // namespace

TvaluesToChannel::TvaluesToChannel() {
  // Statistics
  m_consumedTValues = 0;
  m_discardedTValues = 0;
  m_channelFrameCount = 0;
  m_headDiscardedBits = 0;

  m_perfectFrames = 0;
  m_longFrames = 0;
  m_shortFrames = 0;

  m_overshootSyncs = 0;
  m_undershootSyncs = 0;
  m_perfectSyncs = 0;

  // Set the initial state
  m_currentState = ExpectingInitialSync;

  m_tvalueDiscardCount = 0;
}

void TvaluesToChannel::pushFrame(const std::vector<uint8_t>& data) {
  // P-7: append straight into the internal T-value buffer rather than staging
  // through a one-element queue.
  m_internalBuffer.insert(m_internalBuffer.end(), data.begin(), data.end());

  // Process the state machine
  processStateMachine();
}

std::vector<uint8_t> TvaluesToChannel::popFrame() {
  // Move the first item out of the output buffer to avoid a deep copy.
  std::vector<uint8_t> frame = std::move(m_outputBuffer.front());
  m_outputBuffer.pop();
  return frame;
}

// Accumulate the channel-bit length of a discarded buffer range into the
// head-discard statistic. Only bits lost before the first channel frame count:
// they are what shifts the decoded stream's origin relative to the input
// timeline (issue #231); later discards are mid-stream damage, not an origin
// shift.
void TvaluesToChannel::noteHeadDiscard(int32_t startPosition,
                                       int32_t endPosition) {
  if (m_channelFrameCount != 0) return;
  m_headDiscardedBits +=
      countBits(m_internalBuffer, startPosition, endPosition);
}

bool TvaluesToChannel::isReady() const {
  // Return true if the output buffer is not empty
  return !m_outputBuffer.empty();
}

void TvaluesToChannel::processStateMachine() {
  // We need 588 bits to make a frame.  Every frame starts with T11+T11.
  // So the minimum number of t-values we need is 54 and
  // the maximum number of t-values we can have is 191.  This upper limit
  // is where we need to maintain the buffer size (at 382 for 2 frames).

  while (m_internalBuffer.size() > efm::kMaxTvalueBufferSize) {
    switch (m_currentState) {
      case ExpectingInitialSync:
        // ORC_LOG_DEBUG("TvaluesToChannel::processStateMachine() - State:
        // ExpectingInitialSync");
        m_currentState = expectingInitialSync();
        break;
      case ExpectingSync:
        // ORC_LOG_DEBUG("TvaluesToChannel::processStateMachine() - State:
        // ExpectingSync");
        m_currentState = expectingSync();
        break;
      case HandleOvershoot:
        // ORC_LOG_DEBUG("TvaluesToChannel::processStateMachine() - State:
        // HandleOvershoot");
        m_currentState = handleOvershoot();
        break;
      case HandleUndershoot:
        // ORC_LOG_DEBUG("TvaluesToChannel::processStateMachine() - State:
        // HandleUndershoot");
        m_currentState = handleUndershoot();
        break;
    }
  }
}

TvaluesToChannel::State TvaluesToChannel::expectingInitialSync() {
  State nextState = ExpectingInitialSync;

  // Does the buffer contain a T11+T11 sequence?
  auto it = findSyncPair(m_internalBuffer.cbegin(), m_internalBuffer.cend());
  int initialSyncIndex =
      (it != m_internalBuffer.cend())
          ? static_cast<int>(std::distance(m_internalBuffer.cbegin(), it))
          : -1;

  if (initialSyncIndex != -1) {
    ORC_LOG_DEBUG(
        "TvaluesToChannel::expectingInitialSync() - Initial sync header "
        "found{}{}",
        m_tvalueDiscardCount > 0 ? " after " : "",
        m_tvalueDiscardCount > 0
            ? std::to_string(m_tvalueDiscardCount) + " discarded T-values"
            : "");

    m_tvalueDiscardCount = 0;
    nextState = ExpectingSync;
  } else {
    // Drop all but the last T-value in the buffer
    noteHeadDiscard(0, static_cast<int32_t>(m_internalBuffer.size()) - 1);
    m_tvalueDiscardCount += static_cast<int32_t>(m_internalBuffer.size()) - 1;
    m_discardedTValues += static_cast<int32_t>(m_internalBuffer.size()) - 1;
    m_internalBuffer.erase(m_internalBuffer.begin(),
                           m_internalBuffer.end() - 1);
  }

  return nextState;
}

TvaluesToChannel::State TvaluesToChannel::expectingSync() {
  State nextState = ExpectingSync;

  // The internal buffer contains a valid sync at the start
  // Find the next sync header after it
  auto it =
      findSyncPair(m_internalBuffer.cbegin() + 2, m_internalBuffer.cend());
  int syncIndex =
      (it != m_internalBuffer.cend())
          ? static_cast<int>(std::distance(m_internalBuffer.cbegin(), it))
          : -1;

  // Do we have a valid second sync header?
  if (syncIndex != -1) {
    // Extract the frame data from (and including) the first sync header until
    // (but not including) the second sync header
    std::vector<uint8_t> frameData(m_internalBuffer.begin(),
                                   m_internalBuffer.begin() + syncIndex);

    // Do we have exactly 588 bits of data?  Count the T-values
    int bitCount = static_cast<int>(countBits(frameData));

    // If the frame data is 550 to 600 bits, we have a valid frame
    if (bitCount > efm::kFrameBitCountAcceptMin &&
        bitCount < efm::kFrameBitCountAcceptMax) {
      if (bitCount != efm::kEfmFrameChannelBits) {
        ORC_LOG_DEBUG(
            "TvaluesToChannel::expectingSync() - Got frame with {} bits - "
            "Treating as valid",
            bitCount);
        if (bitCount > efm::kEfmFrameChannelBits) {
          attemptToFixOvershootFrame(frameData);
        }
        if (bitCount < efm::kEfmFrameChannelBits) {
          attemptToFixUndershootFrame(0, syncIndex, frameData);
        }
      }

      // We have a valid frame
      // Place the frame data into the output buffer
      m_outputBuffer.push(frameData);

      m_consumedTValues += frameData.size();
      m_channelFrameCount++;
      m_perfectSyncs++;

      if (bitCount == efm::kEfmFrameChannelBits) m_perfectFrames++;
      if (bitCount > efm::kEfmFrameChannelBits) m_longFrames++;
      if (bitCount < efm::kEfmFrameChannelBits) m_shortFrames++;

      // Remove the frame data from the internal buffer
      m_internalBuffer.erase(m_internalBuffer.begin(),
                             m_internalBuffer.begin() + syncIndex);
      nextState = ExpectingSync;
    } else {
      // This is most likely a missing sync header issue rather than
      // one or more T-values being incorrect. So we'll handle that
      // separately.
      if (bitCount > efm::kEfmFrameChannelBits) {
        nextState = HandleOvershoot;
      } else {
        nextState = HandleUndershoot;
      }
    }
  } else {
    // The buffer does not contain a valid second sync header, so throw it away

    ORC_LOG_DEBUG(
        "TvaluesToChannel::expectingSync() - No second sync header found, sync "
        "lost - dropping {} T-values",
        m_internalBuffer.size());

    noteHeadDiscard(0, static_cast<int32_t>(m_internalBuffer.size()));
    m_discardedTValues += m_internalBuffer.size();
    m_internalBuffer.clear();
    nextState = ExpectingInitialSync;
  }

  return nextState;
}

TvaluesToChannel::State TvaluesToChannel::handleUndershoot() {
  State nextState = ExpectingSync;

  // The frame data is too short
  m_undershootSyncs++;

  // Find the second sync header
  auto it =
      findSyncPair(m_internalBuffer.cbegin() + 2, m_internalBuffer.cend());
  int secondSyncIndex =
      (it != m_internalBuffer.cend())
          ? static_cast<int>(std::distance(m_internalBuffer.cbegin(), it))
          : -1;

  // R-5(d): if there is no second sync header there cannot be a third one
  // bracketing a frame either. Guard against indexing the buffer with -1
  // (begin() + secondSyncIndex would be begin() - 1, an out-of-bounds iterator
  // that traps under the hardened libc++, and m_discardedTValues += -1 would
  // corrupt the statistic). Treat it as a lost sync: drop all but the last
  // T-value and re-hunt for an initial sync.
  if (secondSyncIndex == -1) {
    ORC_LOG_DEBUG(
        "TvaluesToChannel::handleUndershoot() - No second sync header found - "
        "Sync lost.  Dropping {} T-values",
        m_internalBuffer.size() - 1);

    noteHeadDiscard(0, static_cast<int32_t>(m_internalBuffer.size()) - 1);
    m_discardedTValues += static_cast<int32_t>(m_internalBuffer.size()) - 1;
    m_internalBuffer.erase(m_internalBuffer.begin(),
                           m_internalBuffer.end() - 1);
    return ExpectingInitialSync;
  }

  // Find the third sync header
  auto it3 = findSyncPair(m_internalBuffer.cbegin() + secondSyncIndex + 2,
                          m_internalBuffer.cend());
  int thirdSyncIndex =
      (it3 != m_internalBuffer.cend())
          ? static_cast<int>(std::distance(m_internalBuffer.cbegin(), it3))
          : -1;

  // So, unless the data is completely corrupt we should have 588 bits between
  // the first and third sync headers (i.e. the second was a corrupt sync
  // header) or 588 bits between the second and third sync headers (i.e. the
  // first was a corrupt sync header)
  //
  // If neither of these conditions are met, we have a corrupt frame data and we
  // have to drop it

  if (thirdSyncIndex != -1) {
    // Value of the Ts between the first and third sync header
    int fttBitCount =
        static_cast<int>(countBits(m_internalBuffer, 0, thirdSyncIndex));

    // Value of the Ts between the second and third sync header
    int sttBitCount = static_cast<int>(
        countBits(m_internalBuffer, secondSyncIndex, thirdSyncIndex));

    if (fttBitCount > efm::kFrameBitCountAcceptMin &&
        fttBitCount < efm::kFrameBitCountAcceptMax) {
      ORC_LOG_DEBUG(
          "TvaluesToChannel::handleUndershoot() - Undershoot frame - Value "
          "from first to third sync_header = {} bits - treating as valid",
          fttBitCount);
      // Valid frame between the first and third sync headers
      std::vector<uint8_t> frameData(m_internalBuffer.begin(),
                                     m_internalBuffer.begin() + thirdSyncIndex);
      int32_t bitCount = static_cast<int32_t>(countBits(frameData));
      if (bitCount != efm::kEfmFrameChannelBits) {
        ORC_LOG_DEBUG(
            "TvaluesToChannel::handleUndershoot1() - Got frame with {} bits - "
            "Treating as valid",
            sttBitCount);
        if (bitCount > efm::kEfmFrameChannelBits) {
          attemptToFixOvershootFrame(frameData);
        }
        if (bitCount < efm::kEfmFrameChannelBits) {
          attemptToFixUndershootFrame(0, thirdSyncIndex, frameData);
        }
      }
      m_outputBuffer.push(frameData);

      m_consumedTValues += frameData.size();
      m_channelFrameCount++;

      if (fttBitCount == efm::kEfmFrameChannelBits) m_perfectFrames++;
      if (fttBitCount > efm::kEfmFrameChannelBits) m_longFrames++;
      if (fttBitCount < efm::kEfmFrameChannelBits) m_shortFrames++;

      // Remove the frame data from the internal buffer
      m_internalBuffer.erase(m_internalBuffer.begin(),
                             m_internalBuffer.begin() + thirdSyncIndex);
      nextState = ExpectingSync;
    } else if (sttBitCount > efm::kFrameBitCountAcceptMin &&
               sttBitCount < efm::kFrameBitCountAcceptMax) {
      ORC_LOG_DEBUG(
          "TvaluesToChannel::handleUndershoot() - Undershoot frame - Value "
          "from second to third sync_header = {} bits - treating as valid",
          sttBitCount);
      // The prefix [0, secondSyncIndex) is discarded below; account for it
      // BEFORE the frame push flips m_channelFrameCount and closes the
      // head-discard gate (the prefix precedes the very first frame).
      noteHeadDiscard(0, secondSyncIndex);
      // Valid frame between the second and third sync headers
      std::vector<uint8_t> frameData(m_internalBuffer.begin() + secondSyncIndex,
                                     m_internalBuffer.begin() + thirdSyncIndex);
      int32_t bitCount = static_cast<int32_t>(countBits(frameData));
      if (bitCount != efm::kEfmFrameChannelBits) {
        ORC_LOG_DEBUG(
            "TvaluesToChannel::handleUndershoot2() - Got frame with {} bits - "
            "Treating as valid",
            sttBitCount);
        if (bitCount > efm::kEfmFrameChannelBits) {
          attemptToFixOvershootFrame(frameData);
        }
        if (bitCount < efm::kEfmFrameChannelBits) {
          attemptToFixUndershootFrame(secondSyncIndex, thirdSyncIndex,
                                      frameData);
        }
      }
      m_outputBuffer.push(frameData);

      m_consumedTValues += frameData.size();
      m_channelFrameCount++;

      if (sttBitCount == efm::kEfmFrameChannelBits) m_perfectFrames++;
      if (sttBitCount > efm::kEfmFrameChannelBits) m_longFrames++;
      if (sttBitCount < efm::kEfmFrameChannelBits) m_shortFrames++;

      // Remove the frame data from the internal buffer (head accounting for
      // the dropped prefix happened above, before the frame push)
      m_discardedTValues += secondSyncIndex;
      m_internalBuffer.erase(m_internalBuffer.begin(),
                             m_internalBuffer.begin() + thirdSyncIndex);
      nextState = ExpectingSync;
    } else {
      ORC_LOG_DEBUG(
          "TvaluesToChannel::handleUndershoot() - First to third sync is {} "
          "bits, second to third sync is {}. Dropping (what might be a) frame.",
          fttBitCount, sttBitCount);
      nextState = ExpectingSync;

      // Remove the frame data from the internal buffer (nothing was output, so
      // the whole erased range is head loss, not just the counted prefix)
      noteHeadDiscard(0, thirdSyncIndex);
      m_discardedTValues += secondSyncIndex;
      m_internalBuffer.erase(m_internalBuffer.begin(),
                             m_internalBuffer.begin() + thirdSyncIndex);
    }
  } else {
    // R-5(d): processStateMachine() only dispatches here when the buffer
    // already exceeds 382 T-values, so the former "size <= 382, wait for more
    // data" branch was unreachable and has been removed. With a second but no
    // third sync header the frame cannot be bracketed - drop all but the last
    // T-value and re-hunt for an initial sync.
    ORC_LOG_DEBUG(
        "TvaluesToChannel::handleUndershoot() - No third sync header found - "
        "Sync lost.  Dropping {} T-values",
        m_internalBuffer.size() - 1);

    m_discardedTValues += static_cast<int32_t>(m_internalBuffer.size()) - 1;
    m_internalBuffer.erase(m_internalBuffer.begin(),
                           m_internalBuffer.end() - 1);
    nextState = ExpectingInitialSync;
  }

  return nextState;
}

TvaluesToChannel::State TvaluesToChannel::handleOvershoot() {
  State nextState = ExpectingSync;

  // The frame data is too long
  m_overshootSyncs++;

  // Is the overshoot due to a missing/corrupt sync header?
  // Count the bits between the first and second sync headers, if they are
  // 588*2, split the frame data into two frames

  // Find the second sync header
  auto it =
      findSyncPair(m_internalBuffer.cbegin() + 2, m_internalBuffer.cend());
  int syncIndex =
      (it != m_internalBuffer.cend())
          ? static_cast<int>(std::distance(m_internalBuffer.cbegin(), it))
          : -1;

  // Do we have a valid second sync header?
  if (syncIndex != -1) {
    // Extract the frame data from (and including) the first sync header until
    // (but not including) the second sync header
    std::vector<uint8_t> frameData(m_internalBuffer.begin(),
                                   m_internalBuffer.begin() + syncIndex);

    // Remove the frame data from the internal buffer
    m_internalBuffer.erase(m_internalBuffer.begin(),
                           m_internalBuffer.begin() + syncIndex);

    // How many bits of data do we have?  Count the T-values
    int bitCount = static_cast<int>(countBits(frameData));

    // If the frame data is within the range of n frames, we have n frames
    // separated by corrupt sync headers
    const int frameSize = efm::kEfmFrameChannelBits;
    const int tolerance = 11;  // How close to 588 bits do we need to be?
    const int maxFrames =
        10;  // Define the maximum number of frames to check for
    bool validFrames = false;

    for (int n = 2; n <= maxFrames; ++n) {
      if (bitCount > frameSize * n - tolerance &&
          bitCount < frameSize * n + tolerance) {
        validFrames = true;
        int accumulatedBits = 0;
        int endOfFrameIndex = 0;

        for (int i = 0; i < n; ++i) {
          std::vector<uint8_t> singleFrameData;
          while (accumulatedBits < frameSize &&
                 endOfFrameIndex < frameData.size()) {
            accumulatedBits += orc::efm_tvalue(frameData.at(endOfFrameIndex));
            ++endOfFrameIndex;
          }

          singleFrameData = std::vector<uint8_t>(
              frameData.begin(), frameData.begin() + endOfFrameIndex);
          frameData = std::vector<uint8_t>(frameData.begin() + endOfFrameIndex,
                                           frameData.end());
          accumulatedBits = 0;
          endOfFrameIndex = 0;

          uint32_t singleFrameBitCount = countBits(singleFrameData);

          // Place the frame into the output buffer
          m_outputBuffer.push(singleFrameData);

          ORC_LOG_DEBUG(
              "TvaluesToChannel::handleOvershoot() - Overshoot frame split - "
              "{} bits - frame split #{}",
              singleFrameBitCount, i + 1);

          m_consumedTValues += singleFrameData.size();
          m_channelFrameCount++;

          // E-8: a frame with fewer bits than nominal is a *short* frame and
          // more bits is a *long* frame (matching the convention above); these
          // were inverted.
          if (singleFrameBitCount == frameSize) m_perfectFrames++;
          if (singleFrameBitCount > frameSize) m_longFrames++;
          if (singleFrameBitCount < frameSize) m_shortFrames++;
        }
        break;
      }
    }

    if (!validFrames) {
      ORC_LOG_DEBUG(
          "TvaluesToChannel::handleOvershoot() - Attempted overshoot recovery, "
          "but there were no sync headers in the data - are we processing "
          "noise?");
      ORC_LOG_DEBUG(
          "TvaluesToChannel::handleOvershoot() - Overshoot by {} bits, but no "
          "sync header found, dropping {} T-values",
          bitCount, m_internalBuffer.size() - 1);
      // The extracted frameData (bitCount bits) was dropped without output too.
      if (m_channelFrameCount == 0) m_headDiscardedBits += bitCount;
      noteHeadDiscard(0, static_cast<int32_t>(m_internalBuffer.size()) - 1);
      m_discardedTValues += static_cast<int32_t>(m_internalBuffer.size()) - 1;
      m_internalBuffer.erase(m_internalBuffer.begin(),
                             m_internalBuffer.end() - 1);
      nextState = ExpectingInitialSync;
    } else {
      nextState = ExpectingSync;
    }
  } else {
    ORC_LOG_ERROR(
        "TvaluesToChannel::handleOvershoot() - Overshoot frame detected but no "
        "second sync header found, even though it should have been there.");
    throw efm::EfmDecodeError(__func__);
  }

  return nextState;
}

// This function tries some basic tricks to fix a frame that is more than 588
// bits long
void TvaluesToChannel::attemptToFixOvershootFrame(
    std::vector<uint8_t>& frameData) {
  int32_t bitCount = static_cast<int32_t>(countBits(frameData));

  if (bitCount > efm::kEfmFrameChannelBits) {
    // We have too many bits, so we'll try to remove some
    // We'll remove the first T-value in the frame
    std::vector<uint8_t> lframeData(frameData.begin(), frameData.end() - 1);
    // ... and the last T-value in the frame
    std::vector<uint8_t> rframeData(frameData.begin() + 1, frameData.end());
    int32_t lbitCount = static_cast<int32_t>(countBits(lframeData));
    int32_t rbitCount = static_cast<int32_t>(countBits(rframeData));

    if (lbitCount == efm::kEfmFrameChannelBits) {
      frameData = lframeData;
      ORC_LOG_DEBUG(
          "TvaluesToChannel::attemptToFixOvershootFrame() - Removed first "
          "T-value to fix frame");
    } else if (rbitCount == efm::kEfmFrameChannelBits) {
      frameData = rframeData;
      ORC_LOG_DEBUG(
          "TvaluesToChannel::attemptToFixOvershootFrame() - Removed last "
          "T-value to fix frame");
    }
  }
}

// This function tries some basic tricks to fix a frame that is less than 588
// bits long Note: the start and end indexes refer to m_internalBuffer
void TvaluesToChannel::attemptToFixUndershootFrame(
    uint32_t startIndex, uint32_t endIndex, std::vector<uint8_t>& frameData) {
  int32_t bitCount = static_cast<int32_t>(countBits(frameData));

  if (bitCount < efm::kEfmFrameChannelBits) {
    std::vector<uint8_t> lframeData(
        m_internalBuffer.begin() + static_cast<std::ptrdiff_t>(startIndex),
        m_internalBuffer.begin() + static_cast<std::ptrdiff_t>(endIndex) + 1);
    int32_t lbitCount = static_cast<int32_t>(countBits(lframeData));

    if (lbitCount == efm::kEfmFrameChannelBits) {
      frameData = lframeData;
      ORC_LOG_DEBUG(
          "TvaluesToChannel::attemptToFixUndershootFrame() - Added additional "
          "last T-value to fix frame");
      return;
    }

    if (startIndex > 0) {
      std::vector<uint8_t> rframeData(
          m_internalBuffer.begin() + static_cast<std::ptrdiff_t>(startIndex) -
              1,
          m_internalBuffer.begin() + static_cast<std::ptrdiff_t>(endIndex));
      int32_t rbitCount = static_cast<int32_t>(countBits(rframeData));

      if (rbitCount == efm::kEfmFrameChannelBits) {
        frameData = rframeData;
        ORC_LOG_DEBUG(
            "TvaluesToChannel::attemptToFixUndershootFrame() - Added "
            "additional first T-value to fix frame");
      }
    }
  }
}

// Count the number of bits in the array of T-values
uint32_t TvaluesToChannel::countBits(const std::vector<uint8_t>& data,
                                     int32_t startPosition,
                                     int32_t endPosition) {
  if (endPosition == -1) endPosition = static_cast<int32_t>(data.size());

  uint32_t bitCount = 0;
  for (int i = startPosition; i < endPosition; i++) {
    bitCount += orc::efm_tvalue(data.at(i));
  }
  return bitCount;
}

void TvaluesToChannel::showStatistics() const {
  ORC_LOG_INFO("T-values to Channel Frame statistics:");
  ORC_LOG_INFO("  T-Values:");
  ORC_LOG_INFO("    Consumed: {}", m_consumedTValues);
  ORC_LOG_INFO("    Discarded: {}", m_discardedTValues);
  ORC_LOG_INFO("  Channel frames:");
  ORC_LOG_INFO("    Total: {}", m_channelFrameCount);
  ORC_LOG_INFO("    588 bits: {}", m_perfectFrames);
  ORC_LOG_INFO("    >588 bits: {}", m_longFrames);
  ORC_LOG_INFO("    <588 bits: {}", m_shortFrames);
  ORC_LOG_INFO("  Sync headers:");
  ORC_LOG_INFO("    Good syncs: {}", m_perfectSyncs);
  ORC_LOG_INFO("    Overshoots: {}", m_overshootSyncs);
  ORC_LOG_INFO("    Undershoots: {}", m_undershootSyncs);

  // When we overshoot and split the frame, we are guessing the sync header...
  ORC_LOG_INFO("    Guessed: {}", m_channelFrameCount - m_perfectSyncs -
                                      m_overshootSyncs - m_undershootSyncs);
}
