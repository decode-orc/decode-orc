/*
 * File:        teletext_squash_stats.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Accumulates what combining repeated page rows ("squashing")
 *              changed, as a diagnostic profile of the rewrite pass
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_squash_stats.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

// Bucket a copy count; see TeletextSquashStats::kCopyBuckets.
size_t copy_bucket(size_t copies) {
  if (copies <= 1) return 0;
  if (copies == 2) return 1;
  if (copies < 8) return 2;
  return 3;
}

const char* copy_bucket_name(size_t bucket) {
  switch (bucket) {
    case 0:
      return "1 copy";
    case 1:
      return "2 copies";
    case 2:
      return "3-7 copies";
    default:
      return "8+ copies";
  }
}

double percent(uint64_t part, uint64_t whole) {
  return whole == 0
             ? 0.0
             : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

}  // namespace

std::string teletext_grouped(uint64_t value) {
  const std::string digits = std::to_string(value);
  std::string text;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i > 0 && (digits.size() - i) % 3 == 0) {
      text.push_back(',');
    }
    text.push_back(digits[i]);
  }
  return text;
}

void TeletextSquashStats::add_row(const TeletextRowBytes& before,
                                  const TeletextRowBytes& after, size_t copies,
                                  size_t columns) {
  ++rows_;
  if (copies > 0) {
    ++rows_attributed_;
    ++copies_[copy_bucket(copies)];
  }

  const size_t examined = std::min(columns, kTeletextRowBytes);
  bool rewritten = false;
  for (size_t position = 0; position < examined; ++position) {
    ++bytes_total_;
    if (before[position] != after[position]) {
      ++bytes_changed_;
      rewritten = true;
    }
    if (!teletext_odd_parity_valid(before[position])) {
      ++parity_before_;
    }
    if (!teletext_odd_parity_valid(after[position])) {
      ++parity_after_;
    }
  }
  if (rewritten) {
    ++rows_rewritten_;
  }
}

uint64_t TeletextSquashStats::copies_in_bucket(size_t bucket) const {
  return bucket < copies_.size() ? copies_[bucket] : 0;
}

std::string TeletextSquashStats::character_loss_summary() const {
  if (rows_ == 0) {
    return {};
  }
  std::string text = fmt::format(
      "Data loss {:.2f}% — {} of {} recovered characters are damaged",
      percent(parity_after_, bytes_total_), teletext_grouped(parity_after_),
      teletext_grouped(bytes_total_));

  // What the run would have read without combining, so the figure above lands
  // as an outcome rather than just a level. Only worth saying when the vote
  // actually moved something.
  if (parity_before_ > parity_after_) {
    const uint64_t mended = parity_before_ - parity_after_;
    text += fmt::format(
        "\n  Combining repeated rows mended {} of the {} characters that "
        "arrived damaged ({:.1f}%); without it the loss would be {:.2f}%",
        teletext_grouped(mended), teletext_grouped(parity_before_),
        percent(mended, parity_before_), percent(parity_before_, bytes_total_));
  }
  return text;
}

std::string TeletextSquashStats::summary() const {
  if (rows_ == 0) {
    return "Teletext squashing: no row packets to combine";
  }

  std::string text = fmt::format(
      "Teletext squashing: {} row packets over {} page run{}; {} rewritten "
      "({:.1f}%), {} of {} display bytes replaced ({:.2f}%)",
      teletext_grouped(rows_), teletext_grouped(page_runs_),
      page_runs_ == 1 ? "" : "s", teletext_grouped(rows_rewritten_),
      percent(rows_rewritten_, rows_), teletext_grouped(bytes_changed_),
      teletext_grouped(bytes_total_), percent(bytes_changed_, bytes_total_));

  text += fmt::format(
      "\n  Odd-parity failures: {} before ({:.2f}%), {} after ({:.2f}%)",
      teletext_grouped(parity_before_), percent(parity_before_, bytes_total_),
      teletext_grouped(parity_after_), percent(parity_after_, bytes_total_));
  if (parity_after_ > parity_before_) {
    // The vote can only raise the failure count by preferring a damaged byte
    // to a clean one, which the parity-first rule forbids unless no copy of
    // that position was clean. Worth saying out loud when it happens.
    text +=
        " — the vote found no parity-clean copy at some positions and fell "
        "back to the plain majority";
  }

  if (rows_ > rows_attributed_) {
    // What the vote never saw, which is the other half of reading the parity
    // figures: a packet no page could be held responsible for is written as it
    // was recovered, damage and all. Either no page of its magazine was open
    // when it arrived, or its address had been corrected — the MRAG is Hamming
    // 8/4 and mis-corrects on a burst, and a packet moved onto another page's
    // row is not a copy of that row (see teletext_page_decoder.cpp's
    // address_attested()).
    text += fmt::format(
        "\n  {} row packet{} reached no page and could not be voted on "
        "({:.1f}%): no page of the magazine was open, or the packet's address "
        "had been corrected and was not evidence of where it belonged",
        teletext_grouped(rows_ - rows_attributed_),
        rows_ - rows_attributed_ == 1 ? "" : "s",
        percent(rows_ - rows_attributed_, rows_));
  }

  // Empty buckets are left out: the distribution is read for its shape, and
  // three "0 (0.0%)" entries bury the one that carries it. Rows that belonged
  // to no open page are not in it at all — no vote could reach them.
  if (rows_attributed_ > 0) {
    text += "\n  Copies per row packet:";
    for (size_t bucket = 0; bucket < kCopyBuckets; ++bucket) {
      if (copies_in_bucket(bucket) == 0) {
        continue;
      }
      text += fmt::format(" {} {} ({:.1f}%);", copy_bucket_name(bucket),
                          teletext_grouped(copies_in_bucket(bucket)),
                          percent(copies_in_bucket(bucket), rows_attributed_));
    }
    text.pop_back();
  }

  if (rows_attributed_ > 0 && copies_in_bucket(0) == rows_attributed_) {
    // Every row stood alone, so nothing could be corrected. The usual cause
    // is a recording shorter than one carousel cycle; the other is a service
    // that rewrites each page as it re-sends it, which makes every
    // transmission a run of its own (see set_page_runs()).
    text +=
        "\n  No row was transmitted more than once in its page run, so "
        "squashing had nothing to combine";
  }

  return text;
}

}  // namespace orc
