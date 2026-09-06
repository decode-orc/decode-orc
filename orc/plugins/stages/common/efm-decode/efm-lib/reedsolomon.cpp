/*
 * File:        reedsolomon.cpp
 * Purpose:     EFM-library - Reed-Solomon CIRC functions
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "reedsolomon.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <cstdlib>

#include "efm_exception.h"
#include "ezpwd_compat.h"

// ezpwd ECMA-130 CIRC configuration. Both the C1 (32,28) and C2 (28,24)
// shortened codes carry 4 parity symbols over GF(2^8) with POLY=0x11D, FCR=0,
// PRIM=1, so a single RS(255,251) codec (255 - 4 = 251 payload) decodes both -
// ezpwd treats the shorter received words as shortened code words. (P-12: the
// former byte-identical C1RS/C2RS pair collapsed to one type.)
template <size_t SYMBOLS, size_t PAYLOAD>
struct CircRS;
template <size_t PAYLOAD>
struct CircRS<255, PAYLOAD>
    : public __RS(CircRS, uint8_t, 255, PAYLOAD, 0x11D, 0, 1, false);

// P-12: the RS codec carries precomputed Galois-field tables. ezpwd's decode()
// is a const method - it only reads those tables and works in local scratch -
// so a single shared instance is safe for concurrent decode() calls from
// multiple pipelines. A file-scope instance (constructed once) is used rather
// than thread_local because this CIRC path calls decode() ~26M times per stereo
// disc and is sensitive to per-access thread-local storage overhead.
CircRS<255, 255 - 4> circRs;

ReedSolomon::ReedSolomon() {
  // Doubt-derived erasures are off until a caller opts in (issue #307).
  m_doubtErasureThreshold = 0;

  m_doubtErasuresC1 = 0;
  m_doubtErasuresC2 = 0;
  m_doubtSeededC1s = 0;
  m_doubtSeededC2s = 0;

  // Initialise statistics
  m_validC1s = 0;
  m_fixedC1s = 0;
  m_errorC1s = 0;
  m_paddedC1s = 0;

  m_validC2s = 0;
  m_fixedC2s = 0;
  m_errorC2s = 0;
  m_paddedC2s = 0;
}

namespace {

// True if any symbol of the received word is decoder-supplied padding (CIRC
// warm-up fill or end-of-stream drain) rather than data read from the disc.
// Such a word is not fully populated: its parity check is meaningless, so its
// outcome must not be scored as an input defect.
bool containsPadding(const std::vector<uint8_t>& paddedData) {
  return std::any_of(paddedData.begin(), paddedData.end(),
                     [](uint8_t value) { return value != 0; });
}

// Drop the four C2 parity positions (12-15) from a 28-entry per-symbol
// vector, leaving the 24 payload entries in payload order.
void dropC2Parity(std::vector<uint8_t>& values) {
  values.erase(values.begin() + 12, values.begin() + 16);
}

}  // namespace

void ReedSolomon::setDoubtErasureThreshold(uint8_t threshold) {
  m_doubtErasureThreshold = threshold;
}

// Issue #307: the errors that dominate data-disc sector loss are invisible to
// every check the decoder has of its own - the frame is 588 bits, the symbols
// are legal EFM codewords, the 14->8 decode raises nothing - so C1/C2 are left
// correcting unknown errors instead of erasures, roughly halving their
// correction power. The producer knows better: the .efm carries its per-t-value
// doubt, which arrives here as a per-symbol doubt.
//
// Positions are taken most-doubted first and stopped at the code's capacity.
// Ranking rather than a bare threshold is what makes this safe: a bad stretch
// can put more than four symbols over any fixed threshold, and the "> 4
// supplied erasures" early-out in c1Decode/c2Decode would then convert words
// the code could have corrected into outright failures. Rank-and-cap cannot
// exceed capacity by construction.
int ReedSolomon::seedDoubtErasures(const std::vector<uint8_t>& errorData,
                                   const std::vector<uint8_t>& doubtData) {
  if (m_doubtErasureThreshold == 0) return 0;
  if (static_cast<int>(m_erasures.size()) >= kErasureCapacity) return 0;

  m_doubtCandidates.clear();
  for (int index = 0; index < static_cast<int>(doubtData.size()); ++index) {
    // Symbols the EFM decode already condemned are erasures in their own
    // right; seeding them again would double-count against the capacity.
    if (errorData[index] != 0) continue;
    if (doubtData[index] < m_doubtErasureThreshold) continue;
    m_doubtCandidates.emplace_back(doubtData[index], index);
  }
  if (m_doubtCandidates.empty()) return 0;

  // Most-doubted first; ties broken by position so the choice is deterministic.
  std::sort(
      m_doubtCandidates.begin(), m_doubtCandidates.end(),
      [](const std::pair<uint8_t, int>& a, const std::pair<uint8_t, int>& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
      });

  int added = 0;
  for (const auto& candidate : m_doubtCandidates) {
    if (static_cast<int>(m_erasures.size()) >= kErasureCapacity) break;
    m_erasures.push_back(candidate.second);
    ++added;
  }

  // ezpwd does not require sorted erasure positions, but keeping the list in
  // position order matches what the errorData scan produces and keeps the
  // decoder's inputs reproducible.
  std::sort(m_erasures.begin(), m_erasures.end());
  return added;
}

// Perform a C1 Reed-Solomon decoding operation on the input data
// This is a (32,28) Reed-Solomon encode - 32 bytes in, 28 bytes out
void ReedSolomon::c1Decode(std::vector<uint8_t>& inputData,
                           std::vector<uint8_t>& errorData,
                           std::vector<uint8_t>& paddedData,
                           std::vector<uint8_t>& doubtData) {
  // Ensure input data is 32 bytes long
  if (inputData.size() != 32) {
    ORC_LOG_ERROR("ReedSolomon::c1Decode - Input data must be 32 bytes long");
    throw efm::EfmDecodeError(__func__);
  }

  if (doubtData.size() != 32) {
    ORC_LOG_ERROR("ReedSolomon::c1Decode - Doubt data must be 32 bytes long");
    throw efm::EfmDecodeError(__func__);
  }

  // Classify the received word before the parity symbols are trimmed away, so
  // padding carried in positions 28-31 is still visible.
  const bool partiallyPopulated = containsPadding(paddedData);

  // Trim the parity bytes from the padded data (32 → 28)
  paddedData.resize(paddedData.size() - 4);

  // A word that contains any filler cannot be validated as a whole, so none of
  // its output symbols can be trusted - not even the ones that did come from
  // the disc. Mark them all as filler so downstream attributes them to the
  // decode boundary rather than treating them as recovered disc data that
  // happened to fail. Without this the genuine symbols of a drain codeword
  // arrive flagged error=1, padded=0 and are scored as input defects.
  if (partiallyPopulated) {
    std::fill(paddedData.begin(), paddedData.end(), 1);
  }

  // Copy input into reusable scratch for the ezpwd decoder (which modifies in
  // place). assign() retains the scratch buffer's capacity (P-6).
  m_scratchData.assign(inputData.begin(), inputData.end());
  m_erasures.clear();
  m_position.clear();

  // Convert the errorData into a list of erasure positions
  for (int index = 0; index < static_cast<int>(errorData.size()); ++index) {
    if (errorData[index]) m_erasures.push_back(index);
  }

  // E-2: the (32,28) C1 code has minimum distance 5, so it can attempt an
  // in-capacity erasure decode for up to 4 supplied erasures. C1 erasure flags
  // come from the EFM demodulator (invalid 14-bit symbols - reliable erasures),
  // so passing 3-4 of them straight through as uncorrectable without attempting
  // the decode discarded correctable words. More than 4 exceeds capacity.
  //
  // Tested before the doubt-derived erasures are added, so a doubt seed can
  // never be what tips a word over capacity (see seedDoubtErasures()).
  if (static_cast<int>(m_erasures.size()) > kErasureCapacity) {
    inputData.resize(inputData.size() - 4);  // keep received bytes 0..27
    errorData.assign(inputData.size(), 1);
    doubtData.resize(doubtData.size() - 4);  // drop the parity doubt (32 -> 28)
    if (partiallyPopulated) {
      ++m_paddedC1s;
    } else {
      ++m_errorC1s;
    }
    return;
  }

  // Issue #307: top up the erasure list from the producer's doubt.
  const int doubtErasures = seedDoubtErasures(errorData, doubtData);
  if (doubtErasures > 0) {
    m_doubtErasuresC1 += doubtErasures;
    ++m_doubtSeededC1s;
  }

  // Snapshot the supplied erasures before decode prunes them (see c2Decode).
  const std::vector<int> suppliedErasures = m_erasures;

  // Decode the data
  int result = circRs.decode(m_scratchData, m_erasures, &m_position);

  // Accept combinations satisfying 2e + s <= 4 (see c2Decode for the
  // rationale).
  bool accept = false;
  if (result >= 0) {
    int locatedErrors = 0;
    for (int p : m_position) {
      if (std::find(suppliedErasures.begin(), suppliedErasures.end(), p) ==
          suppliedErasures.end()) {
        ++locatedErrors;
      }
    }
    if (2 * locatedErrors + static_cast<int>(suppliedErasures.size()) <=
        kErasureCapacity) {
      accept = true;
    }
  }

  if (accept) {
    // Doubt describes a specific received byte, so a byte the decoder actually
    // replaced carries none: clear it rather than let a stale value seed a C2
    // erasure for a symbol C1's parity has already settled. Positions ezpwd
    // reports but left unchanged keep their doubt - the producer's distrust
    // still describes the byte that is there. Done before inputData is
    // overwritten, while it still holds the received word.
    for (int p : m_position) {
      if (p >= 0 && p < static_cast<int>(inputData.size()) &&
          m_scratchData[p] != inputData[p]) {
        doubtData[p] = 0;
      }
    }

    // Strip the parity bytes (32 → 28) from the corrected data
    inputData.assign(m_scratchData.begin(), m_scratchData.end() - 4);
    errorData.assign(inputData.size(), 0);
    doubtData.resize(doubtData.size() - 4);

    if (partiallyPopulated) {
      ++m_paddedC1s;
    } else if (result == 0) {
      ++m_validC1s;
    } else {
      ++m_fixedC1s;
    }
    return;
  }

  // Rejected: propagate the RECEIVED bytes (first 28) and flag all as corrupt.
  inputData.resize(inputData.size() - 4);
  errorData.assign(inputData.size(), 1);
  doubtData.resize(doubtData.size() - 4);
  if (partiallyPopulated) {
    ++m_paddedC1s;
  } else {
    ++m_errorC1s;
  }
  return;
}

// Perform a C2 Reed-Solomon decoding operation on the input data
// This is a (28,24) Reed-Solomon encode - 28 bytes in, 24 bytes out
void ReedSolomon::c2Decode(std::vector<uint8_t>& inputData,
                           std::vector<uint8_t>& errorData,
                           std::vector<uint8_t>& paddedData,
                           std::vector<uint8_t>& doubtData) {
  // Ensure input data is 28 bytes long
  if (inputData.size() != 28) {
    ORC_LOG_ERROR("ReedSolomon::c2Decode - Input data must be 28 bytes long");
    throw efm::EfmDecodeError(__func__);
  }

  if (errorData.size() != 28) {
    ORC_LOG_ERROR("ReedSolomon::c2Decode - Error data must be 28 bytes long");
    throw efm::EfmDecodeError(__func__);
  }

  if (doubtData.size() != 28) {
    ORC_LOG_ERROR("ReedSolomon::c2Decode - Doubt data must be 28 bytes long");
    throw efm::EfmDecodeError(__func__);
  }

  // Classify the received word before the parity symbols are removed, so
  // padding carried in positions 12-15 is still visible.
  const bool partiallyPopulated = containsPadding(paddedData);

  // Remove parity positions 12-15 from paddedData (28 → 24)
  paddedData.erase(paddedData.begin() + 12, paddedData.begin() + 16);

  // As in c1Decode: if the word was not fully populated, every symbol it
  // produces is filler-contaminated and must be flagged as such.
  if (partiallyPopulated) {
    std::fill(paddedData.begin(), paddedData.end(), 1);
  }

  // Copy input into reusable scratch for the ezpwd decoder (which modifies in
  // place). assign() retains the scratch buffer's capacity (P-6).
  m_scratchData.assign(inputData.begin(), inputData.end());
  m_erasures.clear();
  m_position.clear();

  // Convert the errorData into a list of erasure positions
  for (int index = 0; index < static_cast<int>(errorData.size()); ++index) {
    if (errorData[index] != 0) m_erasures.push_back(index);
  }

  // The (28,24) C2 code has minimum distance 5, so more than 4 supplied
  // erasures already exceeds capacity and cannot be corrected. Tested before
  // the doubt-derived erasures are added (see c1Decode).
  if (static_cast<int>(m_erasures.size()) > kErasureCapacity) {
    // Remove parity byte positions 12-15 and assign result
    inputData.erase(inputData.begin() + 12, inputData.begin() + 16);
    errorData.assign(inputData.size(), 1);
    dropC2Parity(doubtData);
    if (partiallyPopulated) {
      ++m_paddedC2s;
    } else {
      ++m_errorC2s;
    }
    return;
  }

  // Issue #307: top up the erasure list from the producer's doubt. This is
  // where the doubt earns its keep - after C1 has passed a word the EFM decode
  // could find nothing wrong with, the doubt is the only remaining evidence
  // that some of its symbols are not what the disc carried.
  const int doubtErasures = seedDoubtErasures(errorData, doubtData);
  if (doubtErasures > 0) {
    m_doubtErasuresC2 += doubtErasures;
    ++m_doubtSeededC2s;
  }

  // Snapshot the supplied erasures: ezpwd's decode() prunes erasures it finds
  // were actually correct, so we need the original list to classify the
  // corrections it reports in 'position'.
  const std::vector<int> suppliedErasures = m_erasures;

  // Decode the data
  int result = circRs.decode(m_scratchData, m_erasures, &m_position);

  // E-1: accept erasure-dominated corrections up to the code's full capacity.
  // IEC 60908 §16.3 / ECMA-130 Annex C: the (28,24) C2 code corrects any
  // combination of e located errors and s supplied erasures with 2e + s <= 4.
  // After a C1 burst failure all 28 inputs are flagged and the cross-interleave
  // spreads them so 3-4 erasures per C2 word is the *normal* burst case; the
  // previous "reject any decode that changed > 2 symbols" clamp discarded those
  // guaranteed-valid corrections, roughly halving CIRC's designed burst
  // tolerance. 'position' lists the positions ezpwd actually changed; any that
  // was not a supplied erasure is a located error.
  bool accept = false;
  if (result >= 0) {
    int locatedErrors = 0;
    for (int p : m_position) {
      if (std::find(suppliedErasures.begin(), suppliedErasures.end(), p) ==
          suppliedErasures.end()) {
        ++locatedErrors;
      }
    }
    if (2 * locatedErrors + static_cast<int>(suppliedErasures.size()) <=
        kErasureCapacity) {
      accept = true;
    }
  }

  if (accept) {
    // As in c1Decode: doubt about a byte the decoder replaced is spent, and
    // must be cleared while inputData still holds the received word.
    for (int p : m_position) {
      if (p >= 0 && p < static_cast<int>(inputData.size()) &&
          m_scratchData[p] != inputData[p]) {
        doubtData[p] = 0;
      }
    }

    // Keep the 24 payload bytes of the decoded (corrected) data, dropping
    // parity byte positions 12-15. Written straight into inputData (reusing its
    // capacity) so the scratch buffer is left intact for the next call.
    inputData.resize(24);
    std::copy(m_scratchData.begin(), m_scratchData.begin() + 12,
              inputData.begin());
    std::copy(m_scratchData.begin() + 16, m_scratchData.begin() + 28,
              inputData.begin() + 12);
    errorData.assign(inputData.size(), 0);
    dropC2Parity(doubtData);

    if (partiallyPopulated) {
      ++m_paddedC2s;
    } else if (result == 0) {
      ++m_validC2s;
    } else {
      ++m_fixedC2s;
    }
    return;
  }

  // Rejected: propagate the RECEIVED bytes (not the decoder-modified tmpData,
  // which may hold a miscorrection) and flag all outputs as corrupt.
  inputData.erase(inputData.begin() + 12, inputData.begin() + 16);
  errorData.assign(inputData.size(), 1);
  dropC2Parity(doubtData);
  if (partiallyPopulated) {
    ++m_paddedC2s;
  } else {
    ++m_errorC2s;
  }
  return;
}

// Getter functions for the statistics
int32_t ReedSolomon::validC1s() const { return m_validC1s; }

int32_t ReedSolomon::fixedC1s() const { return m_fixedC1s; }

int32_t ReedSolomon::errorC1s() const { return m_errorC1s; }

int32_t ReedSolomon::validC2s() const { return m_validC2s; }

int32_t ReedSolomon::fixedC2s() const { return m_fixedC2s; }

int32_t ReedSolomon::errorC2s() const { return m_errorC2s; }

int32_t ReedSolomon::paddedC1s() const { return m_paddedC1s; }

int32_t ReedSolomon::paddedC2s() const { return m_paddedC2s; }
