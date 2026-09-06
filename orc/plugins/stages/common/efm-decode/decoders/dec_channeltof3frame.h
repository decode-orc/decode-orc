/*
 * File:        dec_channeltof3frame.h
 * Purpose:     efm-decoder-f2 - EFM T-values to F2 Section decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef DEC_CHANNELTOF3FRAME_H
#define DEC_CHANNELTOF3FRAME_H

#include <cstdint>
#include <queue>
#include <vector>

#include "decoders.h"
#include "efm.h"

class ChannelToF3Frame : public Decoder {
 public:
  ChannelToF3Frame();
  void pushFrame(const std::vector<uint8_t>& data);
  void pushFrame(std::vector<uint8_t>&& data);
  F3Frame popFrame();
  bool isReady() const;

  // Issue #307: whether to attribute the producer's per-T-value doubt to the
  // symbols it produced. Off by default: it is per-T-value work in the hottest
  // loop of the decode, and nothing downstream reads the result unless the
  // CIRC has been asked to seed erasures from it.
  void setCollectDoubt(bool collectDoubt) { m_collectDoubt = collectDoubt; }

  void showStatistics() const;

 private:
  void processQueue();
  F3Frame createF3Frame(const std::vector<uint8_t>& data);

  // Expand packed T-values into the frame's channel bit stream. `outputData`
  // receives the bits packed 8-to-a-byte (MSB first); `symbolDoubt` receives
  // one entry per data symbol (32), holding the greatest doubt of the T-values
  // overlapping that symbol's 14 bits - a symbol is wrong if any one of its
  // bits is, so the maximum, not the mean, describes the producer's distrust
  // of it. Returns true when any symbol carries doubt; always false (and
  // `symbolDoubt` untouched) unless setCollectDoubt(true) was called.
  bool tvaluesToData(const std::vector<uint8_t>& tvalues,
                     std::vector<uint8_t>& outputData,
                     std::vector<uint8_t>& symbolDoubt);
  uint16_t getBits(const std::vector<uint8_t>& data, int startBit, int endBit);

  Efm m_efm;

  // See setCollectDoubt().
  bool m_collectDoubt = false;

  // Reusable per-symbol doubt scratch (32 entries). A member rather than a
  // local so the per-frame decode does not allocate; createF3Frame() is the
  // only user and is not re-entrant.
  std::vector<uint8_t> m_symbolDoubt;

  std::queue<std::vector<uint8_t>> m_inputBuffer;
  std::queue<F3Frame> m_outputBuffer;

  // Statistics
  uint32_t m_goodFrames;
  uint32_t m_undershootFrames;
  uint32_t m_overshootFrames;
  uint32_t m_validEfmSymbols;
  uint32_t m_invalidEfmSymbols;
  uint32_t m_validSubcodeSymbols;
  uint32_t m_invalidSubcodeSymbols;
};

#endif  // DEC_CHANNELTOF3FRAME_H