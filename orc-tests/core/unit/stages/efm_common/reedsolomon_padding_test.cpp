/*
 * File:        reedsolomon_padding_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for CIRC codeword scoring: codewords containing
 *              de-interleave warm-up or drain padding must not be scored as
 *              input defects
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "reedsolomon.h"

namespace {

// The all-zero word is a valid Reed-Solomon codeword, so it decodes cleanly
// without any correction. That makes it the simplest possible "good" input.
std::vector<uint8_t> zeroWord(size_t size) {
  return std::vector<uint8_t>(size, 0);
}

// Flag more symbols as erasures than the code's capacity (2e + s <= 4), which
// forces the decoder down its uncorrectable path.
std::vector<uint8_t> beyondCapacityErasures(size_t size) {
  std::vector<uint8_t> erasures(size, 0);
  for (int i = 0; i < 5; ++i) erasures[i] = 1;
  return erasures;
}

TEST(ReedSolomonScoring_C1, CountsCleanFullyPopulatedWordAsValid) {
  ReedSolomon circ;
  std::vector<uint8_t> data = zeroWord(32);
  std::vector<uint8_t> errors = zeroWord(32);
  std::vector<uint8_t> padded = zeroWord(32);
  std::vector<uint8_t> doubt = zeroWord(32);

  circ.c1Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.validC1s(), 1);
  EXPECT_EQ(circ.errorC1s(), 0);
  EXPECT_EQ(circ.paddedC1s(), 0);
}

TEST(ReedSolomonScoring_C1, CountsUncorrectableFullyPopulatedWordAsError) {
  ReedSolomon circ;
  std::vector<uint8_t> data = zeroWord(32);
  std::vector<uint8_t> errors = beyondCapacityErasures(32);
  std::vector<uint8_t> padded = zeroWord(32);
  std::vector<uint8_t> doubt = zeroWord(32);

  circ.c1Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.errorC1s(), 1);
  EXPECT_EQ(circ.paddedC1s(), 0);
}

// A word that failed only because it was assembled partly from decoder-supplied
// filler says nothing about the disc, so it must land in the padded tally and
// leave the scored figures untouched.
TEST(ReedSolomonScoring_C1, ExcludesUncorrectableWordContainingPadding) {
  ReedSolomon circ;
  std::vector<uint8_t> data = zeroWord(32);
  std::vector<uint8_t> errors = beyondCapacityErasures(32);
  std::vector<uint8_t> padded = zeroWord(32);
  padded[0] = 1;
  std::vector<uint8_t> doubt = zeroWord(32);

  circ.c1Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.paddedC1s(), 1);
  EXPECT_EQ(circ.errorC1s(), 0);
  EXPECT_EQ(circ.validC1s(), 0);
  EXPECT_EQ(circ.fixedC1s(), 0);
}

// Padding in the parity symbols alone still makes the word partially populated;
// it is trimmed away early in c1Decode, so this guards the ordering.
TEST(ReedSolomonScoring_C1, DetectsPaddingCarriedInTheParitySymbols) {
  ReedSolomon circ;
  std::vector<uint8_t> data = zeroWord(32);
  std::vector<uint8_t> errors = zeroWord(32);
  std::vector<uint8_t> padded = zeroWord(32);
  padded[31] = 1;
  std::vector<uint8_t> doubt = zeroWord(32);

  circ.c1Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.paddedC1s(), 1);
  EXPECT_EQ(circ.validC1s(), 0);
}

TEST(ReedSolomonScoring_C2, CountsCleanFullyPopulatedWordAsValid) {
  ReedSolomon circ;
  std::vector<uint8_t> data = zeroWord(28);
  std::vector<uint8_t> errors = zeroWord(28);
  std::vector<uint8_t> padded = zeroWord(28);
  std::vector<uint8_t> doubt = zeroWord(28);

  circ.c2Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.validC2s(), 1);
  EXPECT_EQ(circ.errorC2s(), 0);
  EXPECT_EQ(circ.paddedC2s(), 0);
}

TEST(ReedSolomonScoring_C2, CountsUncorrectableFullyPopulatedWordAsError) {
  ReedSolomon circ;
  std::vector<uint8_t> data = zeroWord(28);
  std::vector<uint8_t> errors = beyondCapacityErasures(28);
  std::vector<uint8_t> padded = zeroWord(28);
  std::vector<uint8_t> doubt = zeroWord(28);

  circ.c2Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.errorC2s(), 1);
  EXPECT_EQ(circ.paddedC2s(), 0);
}

TEST(ReedSolomonScoring_C2, ExcludesUncorrectableWordContainingPadding) {
  ReedSolomon circ;
  std::vector<uint8_t> data = zeroWord(28);
  std::vector<uint8_t> errors = beyondCapacityErasures(28);
  std::vector<uint8_t> padded = zeroWord(28);
  padded[27] = 1;
  std::vector<uint8_t> doubt = zeroWord(28);

  circ.c2Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.paddedC2s(), 1);
  EXPECT_EQ(circ.errorC2s(), 0);
  EXPECT_EQ(circ.validC2s(), 0);
}

// The parity symbols removed by c2Decode are positions 12-15, so padding there
// must be seen before the erase.
TEST(ReedSolomonScoring_C2, DetectsPaddingCarriedInTheParitySymbols) {
  ReedSolomon circ;
  std::vector<uint8_t> data = zeroWord(28);
  std::vector<uint8_t> errors = zeroWord(28);
  std::vector<uint8_t> padded = zeroWord(28);
  padded[13] = 1;
  std::vector<uint8_t> doubt = zeroWord(28);

  circ.c2Decode(data, errors, padded, doubt);

  EXPECT_EQ(circ.paddedC2s(), 1);
  EXPECT_EQ(circ.validC2s(), 0);
}

}  // namespace
