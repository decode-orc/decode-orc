/*
 * File:        naplps_dialect_model.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     How a service writes NAPLPS, learned from the records it sent,
 *              for reading what a recording lost
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "naplps_dialect_model.h"

#include <algorithm>
#include <cmath>

#include "naplps_interpreter.h"

namespace orc {

namespace {

/// Add-alpha smoothing of the byte-pair counts, so a pair never seen is
/// unlikely rather than impossible.
constexpr double kPairSmoothing = 0.5;

bool arrived(const std::vector<uint8_t>& data,
             const std::vector<uint8_t>& present, size_t index) {
  // 0x00 is how a lost packet's filler reads, and carries nothing a page shows.
  return index < data.size() && data[index] != 0 &&
         (present.empty() || (index < present.size() && present[index] != 0));
}

}  // namespace

void NaplpsDialectModel::learn(const std::vector<uint8_t>& data,
                               const std::vector<uint8_t>& present) {
  if (data.empty()) {
    return;
  }
  ++records_;
  const std::vector<NaplpsByteRole> roles = naplps_read_roles(data);

  // Byte pairs within unbroken stretches of text and of PDI data. A stretch is
  // broken by anything else — a control, an escape — and by a byte that never
  // arrived, which the pair across it would misrepresent.
  enum class Stretch { kNone, kText, kPdi };
  Stretch stretch = Stretch::kNone;
  int previous = -1;
  for (size_t i = 0; i < roles.size(); ++i) {
    Stretch kind = Stretch::kNone;
    if (roles[i] == NaplpsByteRole::kText) {
      kind = Stretch::kText;
    } else if (roles[i] == NaplpsByteRole::kPdi) {
      kind = Stretch::kPdi;
    }
    if (kind != stretch) {
      previous = -1;
      stretch = kind;
    }
    if (kind == Stretch::kNone || !arrived(data, present, i)) {
      previous = -1;
      continue;
    }
    std::vector<uint32_t>& pairs =
        kind == Stretch::kText ? text_pairs_ : pdi_pairs_;
    std::array<uint64_t, kAlphabet>& firsts =
        kind == Stretch::kText ? text_firsts_ : pdi_firsts_;
    const int byte = data[i] & 0x7F;
    if (previous >= 0) {
      ++pairs[static_cast<size_t>(previous) * kAlphabet +
              static_cast<size_t>(byte)];
      ++firsts[static_cast<size_t>(previous)];
    }
    previous = byte;
  }
}

double NaplpsDialectModel::score(const std::vector<uint32_t>& pairs,
                                 const std::array<uint64_t, kAlphabet>& firsts,
                                 const uint8_t* bytes, size_t count) const {
  if (count < 2) {
    return 0.0;
  }
  double total = 0.0;
  for (size_t i = 1; i < count; ++i) {
    const size_t from = bytes[i - 1] & 0x7F;
    const size_t to = bytes[i] & 0x7F;
    const double seen = pairs[from * kAlphabet + to];
    total += std::log((seen + kPairSmoothing) /
                      (static_cast<double>(firsts[from]) +
                       kPairSmoothing * static_cast<double>(kAlphabet)));
  }
  return total / static_cast<double>(count - 1);
}

double NaplpsDialectModel::text_score(const uint8_t* bytes,
                                      size_t count) const {
  return score(text_pairs_, text_firsts_, bytes, count);
}

double NaplpsDialectModel::pdi_score(const uint8_t* bytes, size_t count) const {
  return score(pdi_pairs_, pdi_firsts_, bytes, count);
}

}  // namespace orc
