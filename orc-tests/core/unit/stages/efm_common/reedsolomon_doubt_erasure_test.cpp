/*
 * File:        reedsolomon_doubt_erasure_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for issue #307 - seeding CIRC C1/C2 erasures from
 *              the producer's per-symbol doubt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "reedsolomon.h"

namespace {

// The all-zero word is a valid Reed-Solomon codeword, so any symbol set to a
// non-zero value is an error the decoder has to find. That makes it the
// simplest way to build a word with a known number of errors in known places.
std::vector<uint8_t> zeroWord(size_t size) {
  return std::vector<uint8_t>(size, 0);
}

// The doubt threshold used throughout: the suggested production default.
constexpr uint8_t kThreshold = 13;

// ---------------------------------------------------------------------------
// Off by default
// ---------------------------------------------------------------------------

// A decoder that was never told a threshold must behave exactly as it did
// before issue #307, whatever doubt the producer supplied.
TEST(ReedSolomonDoubt_C1, IgnoresDoubtWhenThresholdIsUnset) {
  ReedSolomon circ;
  std::vector<uint8_t> data = zeroWord(32);
  std::vector<uint8_t> errors = zeroWord(32);
  std::vector<uint8_t> padded = zeroWord(32);
  std::vector<uint8_t> doubt(32, 15);

  circ.c1Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.doubtErasuresC1(), 0);
  EXPECT_EQ(circ.doubtSeededC1s(), 0);
  EXPECT_EQ(circ.validC1s(), 1);
}

// An explicit threshold of 0 is the documented "disabled" setting; doubt 0 is
// the fully-trusted value, so it must not be read as "flag everything".
TEST(ReedSolomonDoubt_C1, ThresholdZeroDisablesSeeding) {
  ReedSolomon circ;
  circ.setDoubtErasureThreshold(0);
  std::vector<uint8_t> data = zeroWord(32);
  std::vector<uint8_t> errors = zeroWord(32);
  std::vector<uint8_t> padded = zeroWord(32);
  std::vector<uint8_t> doubt = zeroWord(32);

  circ.c1Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.doubtErasuresC1(), 0);
}

TEST(ReedSolomonDoubt_C1, IgnoresDoubtBelowTheThreshold) {
  ReedSolomon circ;
  circ.setDoubtErasureThreshold(kThreshold);
  std::vector<uint8_t> data = zeroWord(32);
  std::vector<uint8_t> errors = zeroWord(32);
  std::vector<uint8_t> padded = zeroWord(32);
  std::vector<uint8_t> doubt = zeroWord(32);
  doubt[3] = kThreshold - 1;

  circ.c1Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.doubtErasuresC1(), 0);
  EXPECT_EQ(circ.doubtSeededC1s(), 0);
}

// ---------------------------------------------------------------------------
// The point of the exercise: doubt turns unknown errors into erasures
// ---------------------------------------------------------------------------

// Three symbol errors need 2e = 6 > 4 of capacity when their positions are
// unknown, so C1 cannot correct them. Told where they are, the same word needs
// s = 3 and decodes. This is the whole mechanism of issue #307 in one test:
// nothing else in the decoder can see these errors, because a wrong symbol
// that is still a legal EFM codeword raises no erasure of its own.
TEST(ReedSolomonDoubt_C1, CorrectsThreeErrorsThatAreUncorrectableUnlocated) {
  std::vector<uint8_t> corrupted = zeroWord(32);
  corrupted[2] = 0x5A;
  corrupted[9] = 0x33;
  corrupted[17] = 0xC7;

  // Without the doubt: too many unlocated errors, the word is rejected.
  {
    ReedSolomon circ;
    std::vector<uint8_t> data = corrupted;
    std::vector<uint8_t> errors = zeroWord(32);
    std::vector<uint8_t> padded = zeroWord(32);
    std::vector<uint8_t> doubt = zeroWord(32);

    circ.c1Decode(data, errors, padded, doubt);

    EXPECT_EQ(circ.errorC1s(), 1);
    EXPECT_EQ(circ.fixedC1s(), 0);
  }

  // With the doubt naming the same three positions: corrected.
  {
    ReedSolomon circ;
    circ.setDoubtErasureThreshold(kThreshold);
    std::vector<uint8_t> data = corrupted;
    std::vector<uint8_t> errors = zeroWord(32);
    std::vector<uint8_t> padded = zeroWord(32);
    std::vector<uint8_t> doubt = zeroWord(32);
    doubt[2] = 15;
    doubt[9] = 14;
    doubt[17] = 13;

    circ.c1Decode(data, errors, padded, doubt);

    EXPECT_EQ(circ.fixedC1s(), 1);
    EXPECT_EQ(circ.errorC1s(), 0);
    EXPECT_EQ(circ.doubtErasuresC1(), 3);
    EXPECT_EQ(circ.doubtSeededC1s(), 1);
    EXPECT_EQ(data, std::vector<uint8_t>(28, 0));
  }
}

TEST(ReedSolomonDoubt_C2, CorrectsThreeErrorsThatAreUncorrectableUnlocated) {
  std::vector<uint8_t> corrupted = zeroWord(28);
  corrupted[1] = 0x11;
  corrupted[8] = 0x22;
  corrupted[20] = 0x44;

  {
    ReedSolomon circ;
    std::vector<uint8_t> data = corrupted;
    std::vector<uint8_t> errors = zeroWord(28);
    std::vector<uint8_t> padded = zeroWord(28);
    std::vector<uint8_t> doubt = zeroWord(28);

    circ.c2Decode(data, errors, padded, doubt);

    EXPECT_EQ(circ.errorC2s(), 1);
  }

  {
    ReedSolomon circ;
    circ.setDoubtErasureThreshold(kThreshold);
    std::vector<uint8_t> data = corrupted;
    std::vector<uint8_t> errors = zeroWord(28);
    std::vector<uint8_t> padded = zeroWord(28);
    std::vector<uint8_t> doubt = zeroWord(28);
    doubt[1] = 15;
    doubt[8] = 15;
    doubt[20] = 13;

    circ.c2Decode(data, errors, padded, doubt);

    EXPECT_EQ(circ.fixedC2s(), 1);
    EXPECT_EQ(circ.errorC2s(), 0);
    EXPECT_EQ(circ.doubtErasuresC2(), 3);
    EXPECT_EQ(data, std::vector<uint8_t>(24, 0));
  }
}

// ---------------------------------------------------------------------------
// Rank and cap
// ---------------------------------------------------------------------------

// A bad stretch can put more symbols over the threshold than the code can
// erase. Taking the most-doubted four and stopping is what keeps a doubt seed
// from tipping a word over capacity - a bare threshold would supply 8 erasures
// here and trip the "> 4" early-out, turning a correctable word into a
// failure.
TEST(ReedSolomonDoubt_C1, NeverSuppliesMoreErasuresThanTheCodeCanCarry) {
  ReedSolomon circ;
  circ.setDoubtErasureThreshold(kThreshold);
  std::vector<uint8_t> data = zeroWord(32);
  data[4] = 0x77;  // one real error, at the most-doubted position
  std::vector<uint8_t> errors = zeroWord(32);
  std::vector<uint8_t> padded = zeroWord(32);
  std::vector<uint8_t> doubt = zeroWord(32);
  for (int i = 0; i < 8; ++i) doubt[i] = 13;
  doubt[4] = 15;

  circ.c1Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.doubtErasuresC1(), ReedSolomon::kErasureCapacity);
  EXPECT_EQ(circ.errorC1s(), 0);
  EXPECT_EQ(data, std::vector<uint8_t>(28, 0));
}

// Symbols the EFM decode already condemned are erasures in their own right, so
// the doubt may only top the list up to capacity - never past it.
TEST(ReedSolomonDoubt_C1, LeavesRoomForTheErasuresTheEfmDecodeAlreadyRaised) {
  ReedSolomon circ;
  circ.setDoubtErasureThreshold(kThreshold);
  std::vector<uint8_t> data = zeroWord(32);
  std::vector<uint8_t> errors = zeroWord(32);
  errors[0] = 1;
  errors[1] = 1;
  errors[2] = 1;
  std::vector<uint8_t> padded = zeroWord(32);
  std::vector<uint8_t> doubt(32, 15);

  circ.c1Decode(data, errors, padded, doubt);

  // Three erasures were already supplied, so exactly one doubt seed fits.
  EXPECT_EQ(circ.doubtErasuresC1(), 1);
}

// A word already beyond capacity on its own erasures takes the early-out; the
// doubt must not be consulted at all, or the counters would misreport why the
// word failed.
TEST(ReedSolomonDoubt_C1, DoesNotSeedAWordAlreadyBeyondCapacity) {
  ReedSolomon circ;
  circ.setDoubtErasureThreshold(kThreshold);
  std::vector<uint8_t> data = zeroWord(32);
  std::vector<uint8_t> errors = zeroWord(32);
  for (int i = 0; i < 5; ++i) errors[i] = 1;
  std::vector<uint8_t> padded = zeroWord(32);
  std::vector<uint8_t> doubt(32, 15);

  circ.c1Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.doubtErasuresC1(), 0);
  EXPECT_EQ(circ.errorC1s(), 1);
  EXPECT_EQ(doubt.size(), 28u);
}

// A symbol the EFM decode already flagged must not also be counted as a doubt
// seed: it is one erasure, not two, and double-counting it would understate
// the remaining capacity.
TEST(ReedSolomonDoubt_C1, DoesNotReseedASymbolAlreadyFlaggedAsAnError) {
  ReedSolomon circ;
  circ.setDoubtErasureThreshold(kThreshold);
  std::vector<uint8_t> data = zeroWord(32);
  std::vector<uint8_t> errors = zeroWord(32);
  errors[6] = 1;
  std::vector<uint8_t> padded = zeroWord(32);
  std::vector<uint8_t> doubt = zeroWord(32);
  doubt[6] = 15;

  circ.c1Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.doubtErasuresC1(), 0);
  EXPECT_EQ(circ.doubtSeededC1s(), 0);
}

// ---------------------------------------------------------------------------
// The doubt vector itself
// ---------------------------------------------------------------------------

// The doubt travels with the symbols, so it must be trimmed exactly as the
// data is: C1 drops the four parity symbols from the end, C2 drops positions
// 12-15. Anything else would misalign the doubt with its symbols downstream.
TEST(ReedSolomonDoubt_C1, TrimsTheParityDoubtWithTheParitySymbols) {
  ReedSolomon circ;
  circ.setDoubtErasureThreshold(kThreshold);
  std::vector<uint8_t> data = zeroWord(32);
  std::vector<uint8_t> errors = zeroWord(32);
  std::vector<uint8_t> padded = zeroWord(32);
  std::vector<uint8_t> doubt = zeroWord(32);
  doubt[27] = 9;
  doubt[31] = 9;

  circ.c1Decode(data, errors, padded, doubt);

  ASSERT_EQ(doubt.size(), 28u);
  EXPECT_EQ(doubt[27], 9);
}

TEST(ReedSolomonDoubt_C2, TrimsTheParityDoubtWithTheParitySymbols) {
  ReedSolomon circ;
  circ.setDoubtErasureThreshold(kThreshold);
  std::vector<uint8_t> data = zeroWord(28);
  std::vector<uint8_t> errors = zeroWord(28);
  std::vector<uint8_t> padded = zeroWord(28);
  std::vector<uint8_t> doubt = zeroWord(28);
  doubt[11] = 7;
  doubt[13] = 9;  // parity position, dropped
  doubt[16] = 8;

  circ.c2Decode(data, errors, padded, doubt);

  ASSERT_EQ(doubt.size(), 24u);
  EXPECT_EQ(doubt[11], 7);
  EXPECT_EQ(doubt[12], 8);  // was position 16
}

// Doubt about a symbol the decoder replaced describes a byte that is no longer
// there, so it is spent: leaving it set would spend C2's capacity on a value
// C1's parity has already settled.
TEST(ReedSolomonDoubt_C1, ClearsTheDoubtOfSymbolsItCorrected) {
  ReedSolomon circ;
  circ.setDoubtErasureThreshold(kThreshold);
  std::vector<uint8_t> data = zeroWord(32);
  data[5] = 0x9E;
  std::vector<uint8_t> errors = zeroWord(32);
  std::vector<uint8_t> padded = zeroWord(32);
  std::vector<uint8_t> doubt = zeroWord(32);
  doubt[5] = 15;
  doubt[6] = 15;  // untouched symbol: its doubt survives to C2

  circ.c1Decode(data, errors, padded, doubt);

  ASSERT_EQ(circ.fixedC1s(), 1);
  EXPECT_EQ(doubt[5], 0);
  EXPECT_EQ(doubt[6], 15);
}

}  // namespace
