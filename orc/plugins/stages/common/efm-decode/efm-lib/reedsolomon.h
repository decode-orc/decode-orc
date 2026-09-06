/*
 * File:        reedsolomon.h
 * Purpose:     EFM-library - Reed-Solomon CIRC functions
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef REEDSOLOMON_H
#define REEDSOLOMON_H

#include <cstdint>
#include <utility>
#include <vector>

// ezpwd __RS() macro parameters are:
// NAME, TYPE, SYMBOLS, PAYLOAD, POLY, FCR, PRIM, DUAL
// (the ECMA-130 values below are POLY=0x11D, FCR=0, PRIM=1, DUAL=false -
// mislabelling FCR/PRIM as INIT/AGR would invite a future mis-edit of the
// safety-critical first-consecutive-root value.)

// To find the integer representation of the polynomial P(x)=x^8+x^4+x^3+x^2+1
// treat the coefficients as binary digits, where each coefficient corresponds
// to a power of x, starting from x^0 on the rightmost side. If there is no term
// for a specific power of x, its coefficient is 0.
//
// Steps:
//     Write the polynomial in terms of its binary representation:
//     P(x)=x^8+x^4+x^3+x^2+1
//
//     The coefficients from x^8 down to x^0 are: 1,0,0,0,1,1,1,0,1.
//
//     Form the binary number from the coefficients:
//     Binary representation: 100011101

//     Convert the binary number to its decimal (integer) equivalent:
//     0b100011101 = 0x11D = 285

class ReedSolomon {
 public:
  ReedSolomon();

  // Both CIRC codes have minimum distance 5, so either corrects any
  // combination of e located errors and s supplied erasures satisfying
  // 2e + s <= 4. Four is therefore the hard erasure capacity of a codeword.
  // (IEC 60908 §16.3 / ECMA-130 Annex C.)
  static constexpr int kErasureCapacity = 4;

  // Issue #307: the smallest producer doubt (0 trusted - 15 distrusted) that
  // makes a symbol an erasure candidate. 0 disables doubt-derived erasures
  // entirely, which is the bit-exact legacy behaviour.
  void setDoubtErasureThreshold(uint8_t threshold);

  void c1Decode(std::vector<uint8_t>& inputData,
                std::vector<uint8_t>& errorData,
                std::vector<uint8_t>& paddedData,
                std::vector<uint8_t>& doubtData);
  void c2Decode(std::vector<uint8_t>& inputData,
                std::vector<uint8_t>& errorData,
                std::vector<uint8_t>& paddedData,
                std::vector<uint8_t>& doubtData);

  // Symbols that were made erasures on the strength of the producer's doubt
  // alone (i.e. the 14->8 EFM decode found nothing wrong with them), and the
  // codewords that received at least one such erasure. Reported so the
  // threshold's cost can be seen in the decode report.
  int64_t doubtErasuresC1() const { return m_doubtErasuresC1; }
  int64_t doubtErasuresC2() const { return m_doubtErasuresC2; }
  int64_t doubtSeededC1s() const { return m_doubtSeededC1s; }
  int64_t doubtSeededC2s() const { return m_doubtSeededC2s; }

  // Codewords that were fully populated with received disc data. These are the
  // only ones whose outcome says anything about the input's quality.
  int32_t validC1s() const;
  int32_t fixedC1s() const;
  int32_t errorC1s() const;

  int32_t validC2s() const;
  int32_t fixedC2s() const;
  int32_t errorC2s() const;

  // Codewords containing at least one padding symbol, i.e. assembled partly
  // from the decoder's own de-interleave warm-up fill or end-of-stream drain
  // rather than from the disc. Such a word cannot satisfy its parity check, so
  // counting its failure as an input defect would misreport a clean stream.
  // Tallied separately and excluded from the valid/fixed/error figures.
  int32_t paddedC1s() const;
  int32_t paddedC2s() const;

 private:
  // Append doubt-derived erasure positions to m_erasures, most-doubted first,
  // stopping at the code's erasure capacity. Positions already flagged by
  // errorData are skipped (they are erasures already). Returns the number of
  // positions added.
  int seedDoubtErasures(const std::vector<uint8_t>& errorData,
                        const std::vector<uint8_t>& doubtData);

  // P-6: reusable scratch buffers so the CIRC hot path (decode() is called
  // ~26M times per stereo disc) does not allocate per call. Each pipeline owns
  // its own ReedSolomon and calls c1/c2Decode sequentially, so plain members
  // are safe (the shared ezpwd codecs are const; this object is not shared).
  std::vector<uint8_t> m_scratchData;
  std::vector<int> m_erasures;
  std::vector<int> m_position;

  // Reusable (doubt, position) candidate scratch for seedDoubtErasures(), kept
  // out of the hot path's allocation budget for the same reason as the buffers
  // above.
  std::vector<std::pair<uint8_t, int>> m_doubtCandidates;

  uint8_t m_doubtErasureThreshold;

  int64_t m_doubtErasuresC1;
  int64_t m_doubtErasuresC2;
  int64_t m_doubtSeededC1s;
  int64_t m_doubtSeededC2s;

  int32_t m_validC1s;
  int32_t m_fixedC1s;
  int32_t m_errorC1s;
  int32_t m_paddedC1s;

  int32_t m_validC2s;
  int32_t m_fixedC2s;
  int32_t m_errorC2s;
  int32_t m_paddedC2s;
};

#endif  // REEDSOLOMON_H