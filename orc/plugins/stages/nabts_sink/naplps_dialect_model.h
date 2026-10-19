/*
 * File:        naplps_dialect_model.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     How a service writes NAPLPS, learned from the records it sent,
 *              for reading what a recording lost
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NAPLPS_DIALECT_MODEL_H
#define ORC_NAPLPS_DIALECT_MODEL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace orc {

/// Bytes of a record the model reads at once when it weighs how a stretch is
/// to be read: enough for a word or two of text, or a command or two.
constexpr size_t kNaplpsDialectWindowBytes = 32;

/**
 * @brief How a service writes NAPLPS, learned from records it sent whole
 *
 * X3.110 says what a record may contain; a service's own encoder decides what
 * its records do contain, and that is what the reading of a damaged record can
 * lean on where the standard leaves more than one reading open. What is
 * learned, from stretches of record that arrived whole, is which bytes follow
 * which, separately in text (a character G-set in GL) and in PDI data (the PDI
 * set in GL): English words and page furniture on one side, opcodes and their
 * operands on the other. A stretch the reading is unsure how to take is scored
 * both ways.
 *
 * Nothing about any service is built in: the model is only what the records
 * given to learn() show, so it follows the network and era of the recordings
 * it is given.
 *
 * Learning is linear in the bytes given; scoring is linear in the bytes
 * scored. Not thread safe while learning; the const members may be called from
 * several threads afterwards.
 */
class NaplpsDialectModel {
 public:
  /// Learn from one record. |present| is parallel to |data| and empty when
  /// every byte arrived; a stretch touching a byte that did not is skipped.
  void learn(const std::vector<uint8_t>& data,
             const std::vector<uint8_t>& present);

  /// Records learned from.
  size_t records() const { return records_; }

  /// Average log-likelihood per byte of |bytes| read as text, and as PDI data.
  /// Comparable with each other; zero bytes score zero.
  double text_score(const uint8_t* bytes, size_t count) const;
  double pdi_score(const uint8_t* bytes, size_t count) const;

 private:
  /// 7-bit bytes only: parity is gone by the time a record is read.
  static constexpr size_t kAlphabet = 128;

  double score(const std::vector<uint32_t>& pairs,
               const std::array<uint64_t, kAlphabet>& firsts,
               const uint8_t* bytes, size_t count) const;

  std::vector<uint32_t> text_pairs_ =
      std::vector<uint32_t>(kAlphabet * kAlphabet);
  std::vector<uint32_t> pdi_pairs_ =
      std::vector<uint32_t>(kAlphabet * kAlphabet);
  std::array<uint64_t, kAlphabet> text_firsts_{};
  std::array<uint64_t, kAlphabet> pdi_firsts_{};
  size_t records_ = 0;
};

}  // namespace orc

#endif  // ORC_NAPLPS_DIALECT_MODEL_H
