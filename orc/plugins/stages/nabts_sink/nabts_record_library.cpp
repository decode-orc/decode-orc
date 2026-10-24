/*
 * File:        nabts_record_library.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Recorded NABTS record data, indexed so bytes a recording lost
 *              can be looked up by the bytes either side of them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_record_library.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <utility>

namespace orc {

namespace {

/// Multiplier of the polynomial hash the anchors are keyed on. Odd, so every
/// byte position contributes.
constexpr uint64_t kHashBase = 0x100000001b3ULL;

/// SplitMix64's finaliser: spreads a polynomial hash's low bits, which the
/// sampling rule and the sort both lean on.
uint64_t mix(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

uint64_t anchor_hash(const uint8_t* bytes) {
  uint64_t h = 0;
  for (size_t i = 0; i < kNabtsLibraryAnchorBytes; ++i) {
    h = h * kHashBase + bytes[i] + 1;
  }
  return mix(h);
}

/// A byte that can stand in a context or a fill: delivered, and not the 0x00
/// a lost packet's filler reads as. A NUL the sender really did transmit draws
/// nothing (X3.110 §5.3.1 makes it transparent), so nothing is lost by never
/// matching on one — and a reference read from a record file carries no mask,
/// so its filler is recognisable only by being 0x00.
bool usable(const std::vector<uint8_t>& data,
            const std::vector<uint8_t>& present, size_t index) {
  return index < data.size() && data[index] != 0 &&
         nabts_library_present(present, index);
}

}  // namespace

void NabtsRecordLibrary::add(NabtsLibraryRecord record) {
  records_.push_back(std::move(record));
}

void NabtsRecordLibrary::build() {
  anchors_.clear();
  for (size_t r = 0; r < records_.size(); ++r) {
    const NabtsLibraryRecord& record = records_[r];
    const std::vector<uint8_t>& data = record.data;
    if (data.size() < kNabtsLibraryAnchorBytes) {
      continue;
    }
    // Unusable bytes in the window ending at i, so an anchor is taken only
    // where all of its bytes arrived.
    size_t unusable = 0;
    for (size_t i = 0; i < data.size(); ++i) {
      unusable += usable(data, record.present, i) ? 0 : 1;
      if (i >= kNabtsLibraryAnchorBytes) {
        unusable -=
            usable(data, record.present, i - kNabtsLibraryAnchorBytes) ? 0 : 1;
      }
      if (i + 1 < kNabtsLibraryAnchorBytes || unusable != 0) {
        continue;
      }
      const size_t start = i + 1 - kNabtsLibraryAnchorBytes;
      const uint64_t hash = anchor_hash(&data[start]);
      if (hash % kSampleStride == 0) {
        anchors_.push_back(Anchor{hash, static_cast<uint32_t>(r),
                                  static_cast<uint32_t>(start)});
      }
    }
  }
  std::sort(anchors_.begin(), anchors_.end());
}

NabtsLibraryAnswer NabtsRecordLibrary::fill(
    const std::vector<uint8_t>& data, const std::vector<uint8_t>& present,
    size_t start, size_t end, size_t exclude,
    const NabtsLibraryPolicy& policy) const {
  return search(data, present, nullptr, start, end, exclude, policy);
}

NabtsLibraryAnswer NabtsRecordLibrary::correct(
    const std::vector<uint8_t>& data, const std::vector<uint8_t>& present,
    const std::vector<uint8_t>& wrong, size_t start, size_t end, size_t exclude,
    const NabtsLibraryPolicy& policy) const {
  return search(data, present, &wrong, start, end, exclude, policy);
}

NabtsLibraryAnswer NabtsRecordLibrary::search(
    const std::vector<uint8_t>& data, const std::vector<uint8_t>& present,
    const std::vector<uint8_t>* wrong, size_t start, size_t end, size_t exclude,
    const NabtsLibraryPolicy& policy) const {
  NabtsLibraryAnswer answer;
  if (start >= end || end > data.size()) {
    return answer;
  }
  const size_t length = end - start;
  // A byte of the query known to be wrong: stepped over, neither agreeing nor
  // ending the context. Never one for fill().
  const auto is_wrong = [wrong](size_t index) {
    return wrong != nullptr && index < wrong->size() && (*wrong)[index] != 0;
  };

  // Where the query's usable context runs to on each side: up to the next byte
  // that never arrived, and no further than the context is ever measured —
  // counted in usable bytes, the wrong ones stepped over (and never more than
  // twice as far again, however many there are).
  size_t left_reach = 0;
  size_t left_usable = 0;
  while (left_usable < kNabtsLibraryMaxContextBytes && left_reach < start &&
         left_reach < 2 * kNabtsLibraryMaxContextBytes) {
    const size_t at = start - 1 - left_reach;
    if (!is_wrong(at)) {
      if (!usable(data, present, at)) {
        break;
      }
      ++left_usable;
    }
    ++left_reach;
  }
  size_t right_reach = 0;
  size_t right_usable = 0;
  while (right_usable < kNabtsLibraryMaxContextBytes &&
         end + right_reach < data.size() &&
         right_reach < 2 * kNabtsLibraryMaxContextBytes) {
    const size_t at = end + right_reach;
    if (!is_wrong(at)) {
      if (!usable(data, present, at)) {
        break;
      }
      ++right_usable;
    }
    ++right_reach;
  }
  if (left_usable < kNabtsLibraryAnchorBytes ||
      right_usable < kNabtsLibraryAnchorBytes) {
    return answer;  // Not enough context on one side to anchor anything.
  }

  // Alignments proposed by any sampled anchor in either context: the record,
  // and where in it the hole's first byte would fall.
  std::set<std::pair<uint32_t, int64_t>> alignments;
  const auto propose = [&](size_t from, size_t to) {
    for (size_t q = from; q + kNabtsLibraryAnchorBytes <= to; ++q) {
      const uint64_t hash = anchor_hash(&data[q]);
      if (hash % kSampleStride != 0) {
        continue;
      }
      auto range = std::equal_range(anchors_.begin(), anchors_.end(),
                                    Anchor{hash, 0, 0});
      for (auto it = range.first; it != range.second; ++it) {
        if (it->record == exclude) {
          continue;
        }
        alignments.emplace(it->record, static_cast<int64_t>(it->offset) +
                                           static_cast<int64_t>(start) -
                                           static_cast<int64_t>(q));
      }
    }
  };
  propose(start - left_reach, start);
  propose(end, end + right_reach);

  // Each alignment, verified byte by byte: the fill must have arrived whole in
  // that record, and the agreement around it is measured outward from the hole.
  struct Candidate {
    size_t record = 0;
    size_t left = 0;
    size_t right = 0;
    size_t total() const { return left + right; }
  };
  std::map<std::vector<uint8_t>, std::vector<Candidate>> by_fill;
  bool matched = false;
  for (const auto& [record_index, ref_start_signed] : alignments) {
    const NabtsLibraryRecord& record = records_[record_index];
    if (ref_start_signed < 0) {
      continue;
    }
    const size_t ref_start = static_cast<size_t>(ref_start_signed);
    const size_t ref_end = ref_start + length;
    if (ref_end > record.data.size()) {
      continue;
    }
    bool whole = true;
    for (size_t i = ref_start; i < ref_end && whole; ++i) {
      whole = usable(record.data, record.present, i);
    }
    if (!whole) {
      continue;
    }
    Candidate c;
    c.record = record_index;
    // Outward from the hole, byte by byte; a byte of the query known to be
    // wrong is stepped over, and the first real difference ends the side.
    size_t left_span = 0;
    while (left_span < left_reach && left_span < ref_start) {
      const size_t at = start - 1 - left_span;
      const size_t there = ref_start - 1 - left_span;
      if (!is_wrong(at)) {
        if (!usable(record.data, record.present, there) ||
            record.data[there] != data[at]) {
          break;
        }
        ++c.left;
      }
      ++left_span;
    }
    size_t right_span = 0;
    while (right_span < right_reach &&
           ref_end + right_span < record.data.size()) {
      const size_t at = end + right_span;
      const size_t there = ref_end + right_span;
      if (!is_wrong(at)) {
        if (!usable(record.data, record.present, there) ||
            record.data[there] != data[at]) {
          break;
        }
        ++c.right;
      }
      ++right_span;
    }
    if (c.left < kNabtsLibraryAnchorBytes ||
        c.right < kNabtsLibraryAnchorBytes) {
      continue;
    }
    matched = true;
    // A side that is padding, or one operand repeated, matches anywhere.
    const auto distinct_in = [&](size_t from, size_t to) {
      std::array<bool, 256> seen{};
      size_t distinct = 0;
      for (size_t i = from; i < to; ++i) {
        if (!is_wrong(i) && !seen[data[i]]) {
          seen[data[i]] = true;
          ++distinct;
        }
      }
      return distinct;
    };
    if (distinct_in(start - left_span, start) < policy.min_distinct_bytes ||
        distinct_in(end, end + right_span) < policy.min_distinct_bytes) {
      continue;
    }
    by_fill[std::vector<uint8_t>(
                record.data.begin() + static_cast<std::ptrdiff_t>(ref_start),
                record.data.begin() + static_cast<std::ptrdiff_t>(ref_end))]
        .push_back(c);
  }

  if (by_fill.empty()) {
    answer.refusal = matched ? NabtsLibraryRefusal::kTooLittleContext
                             : NabtsLibraryRefusal::kNoMatch;
    return answer;
  }

  // The best-supported fill, and how close the best of the others comes.
  const std::vector<uint8_t>* best_bytes = nullptr;
  Candidate best;
  size_t best_count = 0;
  size_t runner_up = 0;
  // Each fill with its strongest agreement and how many records offer it, for
  // the dominant-fill rule below.
  struct Offer {
    const std::vector<uint8_t>* bytes = nullptr;
    Candidate strongest;
    size_t records = 0;
  };
  std::vector<Offer> offers;
  offers.reserve(by_fill.size());
  for (const auto& [bytes, candidates] : by_fill) {
    const Candidate strongest =
        *std::max_element(candidates.begin(), candidates.end(),
                          [](const Candidate& a, const Candidate& b) {
                            return a.total() < b.total();
                          });
    std::set<size_t> distinct_records;
    for (const Candidate& c : candidates) {
      distinct_records.insert(c.record);
    }
    offers.push_back({&bytes, strongest, distinct_records.size()});
    if (best_bytes == nullptr || strongest.total() > best.total()) {
      if (best_bytes != nullptr) {
        runner_up = std::max(runner_up, best.total());
      }
      best_bytes = &bytes;
      best = strongest;
      best_count = distinct_records.size();
    } else {
      runner_up = std::max(runner_up, strongest.total());
    }
  }

  if (best.total() < policy.trusted_context_bytes) {
    answer.refusal = NabtsLibraryRefusal::kTooLittleContext;
    return answer;
  }
  if (by_fill.size() > 1 &&
      best.total() < runner_up + policy.preference_margin_bytes) {
    // Records that agree around the hole disagree inside it. One fill offered
    // by far more records than any other is still the one sent (see
    // kNabtsLibraryDominantRatio).
    const auto top = std::max_element(
        offers.begin(), offers.end(),
        [](const Offer& a, const Offer& b) { return a.records < b.records; });
    size_t next = 0;
    for (auto it = offers.begin(); it != offers.end(); ++it) {
      if (it != top) {
        next = std::max(next, it->records);
      }
    }
    const bool dominant =
        policy.dominant_ratio > 0 &&
        top->records >= policy.dominant_min_records &&
        top->records >= policy.dominant_ratio * next &&
        top->strongest.total() >= policy.dominant_context_bytes;
    if (!dominant) {
      answer.refusal = NabtsLibraryRefusal::kAmbiguous;
      return answer;
    }
    best_bytes = top->bytes;
    best = top->strongest;
    best_count = top->records;
  }

  NabtsLibraryFill fill;
  fill.bytes = *best_bytes;
  fill.record = best.record;
  fill.context_left = best.left;
  fill.context_right = best.right;
  fill.agreeing_records = static_cast<uint32_t>(best_count);
  answer.fill = std::move(fill);
  return answer;
}

}  // namespace orc
