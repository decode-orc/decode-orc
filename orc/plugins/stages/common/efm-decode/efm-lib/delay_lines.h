/*
 * File:        delay_lines.h
 * Purpose:     EFM-library - Delay line functions
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef DELAY_LINES_H
#define DELAY_LINES_H

#include <cstdint>
#include <vector>

class DelayLine {
 public:
  DelayLine(int32_t _delayLength);
  // Constructor taking no argument needed for std::vector in C++
  DelayLine();
  void push(uint8_t& datum, bool& datumError, bool& datumPadded,
            uint8_t& datumDoubt);
  bool isReady();
  void flush();

 private:
  struct DelayContents_t {
    uint8_t datum;
    bool error;
    bool padded;
    // Issue #307: the producer's doubt about this symbol. It must be delayed
    // in lockstep with the symbol itself, otherwise the CIRC would seed its
    // erasures from another symbol's confidence.
    uint8_t doubt;
  };

  std::vector<DelayContents_t> m_buffer;
  int32_t
      m_head;  // ring-buffer head: index of the oldest (next-to-output) element

  bool m_ready;
  int32_t m_pushCount;
  int32_t m_delayLength;
};

class DelayLines {
 public:
  DelayLines(std::vector<int32_t> _delayLengths);
  void push(std::vector<uint8_t>& data, std::vector<uint8_t>& errorData,
            std::vector<uint8_t>& paddedData, std::vector<uint8_t>& doubtData);
  bool isReady();
  void flush();

 private:
  std::vector<DelayLine> m_delayLines;
};

#endif  // DELAY_LINES_H
