/*
 * File:        teletext_row_squasher.cpp
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     Combine repeated copies of a teletext page row into one
 *              best-estimate row ("squashing")
 *
 * Squashing is an idea from vhs-teletext by Alistair Buxton (ali1234),
 * https://github.com/ali1234/vhs-teletext, with thanks — independently
 * implemented here (see teletext_row_squasher.h).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_row_squasher.h"

#include <algorithm>
#include <array>

#include "teletext_page_decoder.h"

namespace orc {

void TeletextRowSquasher::add_row(const TeletextPageKey& key, int row,
                                  const TeletextRowBytes& bytes, int64_t source,
                                  const TeletextRowConfidence* confidence,
                                  size_t first_column, size_t column_count) {
  if (row < 0 || row >= static_cast<int>(
                            std::tuple_size<decltype(PageRows::rows)>::value)) {
    return;  // enhancement packets are not squashed
  }
  first_column = std::min(first_column, kTeletextRowBytes);
  column_count = std::min(column_count, kTeletextRowBytes - first_column);
  if (column_count == 0) {
    return;
  }
  if (row == 0 && first_column == 0) {
    // The header packet's own display bytes carry a live clock, which differs
    // between transmissions of the same page by design; only columns a separate
    // packet carries can be combined (see the class comment).
    return;
  }

  auto it = pages_.find(key);
  if (it == pages_.end()) {
    enforce_page_bound();
    it = pages_.emplace(key, PageRows{}).first;
  }
  it->second.last_touched = ++touch_counter_;

  RowCopies& copies = it->second.rows[static_cast<size_t>(row)];

  TeletextRowConfidence weights;
  if (confidence != nullptr) {
    weights = *confidence;
  } else {
    weights.fill(1.0F);
  }

  // Re-reading the same recovered line replaces its earlier copy: a previewer
  // that rebuilds a sliding window would otherwise let a long-resident frame
  // outvote the rest purely by being read more often. A source is only the same
  // copy when it speaks for the same columns — one packet of a service that
  // splits a row across two contributes to both halves under the same id.
  for (size_t index = 0; index < copies.sources.size(); ++index) {
    if (copies.sources[index] != source ||
        copies.first_column[index] != first_column) {
      continue;
    }
    copies.copies[index] = bytes;
    copies.confidences[index] = weights;
    copies.column_count[index] = static_cast<uint8_t>(column_count);
    return;  // recency deliberately unchanged: a re-read, not a transmission
  }

  if (first_column == 0) {
    ++copies.arrivals;
  }
  if (copies.sources.size() >= options_.max_copies_per_row &&
      !copies.sources.empty()) {
    // Overwrite the oldest slot in place (see RowCopies::seq), among the copies
    // speaking for these columns: the two halves of a split row each keep their
    // own history, so a busy half cannot evict the other's only copy.
    size_t oldest = copies.seq.size();
    for (size_t i = 0; i < copies.seq.size(); ++i) {
      if (copies.first_column[i] != first_column) {
        continue;
      }
      if (oldest == copies.seq.size() || copies.seq[i] < copies.seq[oldest]) {
        oldest = i;
      }
    }
    if (oldest < copies.seq.size()) {
      copies.sources[oldest] = source;
      copies.copies[oldest] = bytes;
      copies.confidences[oldest] = weights;
      copies.first_column[oldest] = static_cast<uint8_t>(first_column);
      copies.column_count[oldest] = static_cast<uint8_t>(column_count);
      copies.seq[oldest] = copies.next_seq++;
      return;
    }
    // No copy of these columns yet; let the row grow by one rather than drop
    // the contribution, so a half that arrives late is not locked out.
  }
  copies.sources.push_back(source);
  copies.copies.push_back(bytes);
  copies.confidences.push_back(weights);
  copies.first_column.push_back(static_cast<uint8_t>(first_column));
  copies.column_count.push_back(static_cast<uint8_t>(column_count));
  copies.seq.push_back(copies.next_seq++);
}

namespace {

// How much of a copy has to agree with the provisional row before the copy is
// taken to be a copy of that row. A copy of the row differs from it only where
// it was damaged, which on a recovered recording is a few bytes in forty; a
// copy of something else agrees only by coincidence, which over 32 or 40
// positions is nowhere near half of them. Anywhere between the two settles the
// same copies either way, so the boundary is put in the middle of the gap.
constexpr double kOutlierAgreementFraction = 0.5;

// Fewest positions a copy must be judged over before it may be called an
// outlier. A copy speaking for a handful of columns can agree on none of them
// by chance, and a row extension is eight columns wide.
constexpr size_t kOutlierMinJudgedPositions = 8;

}  // namespace

void TeletextRowSquasher::vote_row(const RowCopies& copies,
                                   const uint8_t* included,
                                   TeletextRowBytes& result,
                                   TeletextRowCoverage& covered) {
  // Pick the winning value at each byte position across the copies.
  //
  // Odd parity (ETSI EN 300 706 §8.1) detects every single-bit error, so a
  // byte that fails it is known to be corrupt: the vote is held among the
  // parity-clean candidates whenever there are any, and only falls back to all
  // candidates when every copy of this position is damaged. Within that, each
  // copy contributes how sure the recovery chain was of the byte (1 where it
  // cannot say), so a value read cleanly outweighs the same number of copies of
  // one the detector nearly decided the other way. Ties go to the value from
  // the most recently added copy: with only two differing copies there is no
  // majority to find, and the newer transmission is what a plain Level 1
  // decoder would be showing.
  //
  // This runs for every byte of every row of every page rendered — a previewer
  // renders per frame stepped, and the sink's rewrite pass asks once per
  // packet of the whole recording — so the tally is one pass over the copies
  // per position. The accumulators are per byte value, epoch-marked by
  // position so nothing is cleared between positions.
  const size_t copy_count = copies.copies.size();
  std::array<double, 256> weight_of;
  std::array<uint64_t, 256> newest_of;
  std::array<uint8_t, 256> seen_at{};  // epoch marks: position + 1, 0 = never
  std::array<uint8_t, 256> distinct;   // values seen at this position

  result.fill(0);
  covered.fill(false);
  for (size_t position = 0; position < kTeletextRowBytes; ++position) {
    const uint8_t epoch = static_cast<uint8_t>(position + 1);
    size_t distinct_count = 0;
    for (size_t j = 0; j < copy_count; ++j) {
      // A copy the outlier pass excluded is not a copy of this row (see
      // squashed_row()), so it neither votes nor covers a position.
      if (included != nullptr && included[j] == 0) {
        continue;
      }
      // A copy votes only within the columns it spoke for: the two halves of a
      // split row are combined independently.
      if (position < copies.first_column[j] ||
          position >= static_cast<size_t>(copies.first_column[j]) +
                          copies.column_count[j]) {
        continue;
      }
      const uint8_t value = copies.copies[j][position];
      if (seen_at[value] != epoch) {
        seen_at[value] = epoch;
        weight_of[value] = 0.0;
        newest_of[value] = 0;
        distinct[distinct_count++] = value;
      }
      weight_of[value] += static_cast<double>(copies.confidences[j][position]);
      newest_of[value] = std::max(newest_of[value], copies.seq[j]);
    }

    bool found = false;
    bool best_clean = false;
    uint8_t best = 0;
    double best_weight = 0.0;
    uint64_t best_newest = 0;
    for (size_t k = 0; k < distinct_count; ++k) {
      const uint8_t value = distinct[k];
      const bool clean = teletext_odd_parity_valid(value);
      if (found) {
        if (best_clean && !clean) {
          continue;  // a value known corrupt never beats a parity-clean one
        }
        if (clean == best_clean && !(weight_of[value] > best_weight ||
                                     (weight_of[value] == best_weight &&
                                      newest_of[value] > best_newest))) {
          continue;
        }
      }
      found = true;
      best = value;
      best_clean = clean;
      best_weight = weight_of[value];
      best_newest = newest_of[value];
    }
    result[position] = best;
    covered[position] = found;
  }
}

std::optional<TeletextRowBytes> TeletextRowSquasher::squashed_row(
    const TeletextPageKey& key, int row, TeletextRowCoverage* covered) const {
  if (covered != nullptr) {
    covered->fill(false);
  }
  const auto it = pages_.find(key);
  if (it == pages_.end() || row < 0 ||
      row >=
          static_cast<int>(std::tuple_size<decltype(PageRows::rows)>::value)) {
    return std::nullopt;
  }
  const RowCopies& copies = it->second.rows[static_cast<size_t>(row)];
  if (copies.copies.empty()) {
    return std::nullopt;
  }
  if (copies.copies.size() == 1) {
    // A vote of one is the copy itself.
    if (covered != nullptr) {
      const size_t first = copies.first_column.front();
      const size_t count = copies.column_count.front();
      std::fill(covered->begin() + static_cast<std::ptrdiff_t>(first),
                covered->begin() + static_cast<std::ptrdiff_t>(first + count),
                true);
    }
    return copies.copies.front();
  }

  TeletextRowBytes result{};
  TeletextRowCoverage coverage{};
  vote_row(copies, nullptr, result, coverage);

  // Not every copy filed under a row is a copy of that row. The MRAG is
  // Hamming 8/4 and mis-corrects on a burst, and a page whose header was lost
  // leaves the one before it open — either way a packet of some other page
  // arrives here looking like content, and the vote above has no way to tell
  // it from the copies it belongs among. A page that collects enough of them
  // becomes a per-character blend of every page mis-addressed into it, and the
  // blend is worse the more copies are combined, which is the opposite of what
  // combining them is for.
  //
  // What separates the two is how much of the row a copy agrees with. Copies of
  // one row differ only where they were damaged; a copy of something else
  // disagrees nearly everywhere. So the vote is taken twice: once to find what
  // the copies mostly say, and again without the copies that hardly say it.
  //
  // Only a minority may be dropped, counted among the copies speaking for the
  // same columns. Where most of them disagree with the provisional row there is
  // no row for the rest to be outliers of — the page is being assembled out of
  // intruders and no vote among them is worth more than another.
  const size_t copy_count = copies.copies.size();
  std::vector<uint8_t> included;
  std::array<uint16_t, kTeletextRowBytes + 1> group_copies{};
  std::array<uint16_t, kTeletextRowBytes + 1> group_outliers{};
  for (size_t j = 0; j < copy_count; ++j) {
    const size_t first = copies.first_column[j];
    const size_t last =
        std::min(first + copies.column_count[j], kTeletextRowBytes);
    size_t judged = 0;
    size_t agreed = 0;
    for (size_t position = first; position < last; ++position) {
      if (!coverage[position]) {
        continue;
      }
      ++judged;
      if (copies.copies[j][position] == result[position]) {
        ++agreed;
      }
    }
    ++group_copies[first];
    if (judged >= kOutlierMinJudgedPositions &&
        static_cast<double>(agreed) <
            kOutlierAgreementFraction * static_cast<double>(judged)) {
      if (included.empty()) {
        included.assign(copy_count, 1);
      }
      included[j] = 0;
      ++group_outliers[first];
    }
  }

  if (!included.empty()) {
    bool any = false;
    for (size_t j = 0; j < copy_count; ++j) {
      if (included[j] != 0) {
        continue;
      }
      const size_t first = copies.first_column[j];
      if (static_cast<size_t>(group_outliers[first]) * 2 <
          static_cast<size_t>(group_copies[first])) {
        any = true;
      } else {
        included[j] = 1;  // no majority to defend; the group votes as it was
      }
    }
    if (any) {
      vote_row(copies, included.data(), result, coverage);
    }
  }

  if (covered != nullptr) {
    *covered = coverage;
  }
  return result;
}

void TeletextRowSquasher::erase_page(const TeletextPageKey& key) {
  pages_.erase(key);
}

size_t TeletextRowSquasher::arrival_count(const TeletextPageKey& key,
                                          int row) const {
  const auto it = pages_.find(key);
  if (it == pages_.end() || row < 0 ||
      row >=
          static_cast<int>(std::tuple_size<decltype(PageRows::rows)>::value)) {
    return 0;
  }
  return static_cast<size_t>(
      it->second.rows[static_cast<size_t>(row)].arrivals);
}

size_t TeletextRowSquasher::copy_count(const TeletextPageKey& key, int row,
                                       size_t column) const {
  const auto it = pages_.find(key);
  if (it == pages_.end() || row < 0 ||
      row >=
          static_cast<int>(std::tuple_size<decltype(PageRows::rows)>::value)) {
    return 0;
  }
  const RowCopies& copies = it->second.rows[static_cast<size_t>(row)];
  size_t count = 0;
  for (size_t i = 0; i < copies.first_column.size(); ++i) {
    const size_t first = copies.first_column[i];
    count +=
        (column >= first && column < first + copies.column_count[i]) ? 1 : 0;
  }
  return count;
}

void TeletextRowSquasher::clear() {
  pages_.clear();
  touch_counter_ = 0;
}

void TeletextRowSquasher::enforce_page_bound() {
  if (pages_.size() < options_.max_pages) {
    return;
  }
  auto oldest = pages_.begin();
  for (auto it = pages_.begin(); it != pages_.end(); ++it) {
    if (it->second.last_touched < oldest->second.last_touched) {
      oldest = it;
    }
  }
  pages_.erase(oldest);
}

}  // namespace orc
