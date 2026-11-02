/*
 * File:        teletext_squash_stats.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Accumulates what combining repeated page rows ("squashing")
 *              changed, as a diagnostic profile of the rewrite pass
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_SQUASH_STATS_H
#define ORC_TELETEXT_SQUASH_STATS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "vbi-services/teletext_row_squasher.h"

namespace orc {

/**
 * @brief What combining repeated copies of a page row changed
 *
 * Fed the before and after form of every row packet the rewrite pass emits,
 * this answers the question the packet count cannot: whether squashing had
 * anything to work with, and whether what it did was an improvement.
 *
 * The reading is the odd-parity failure count either side of the rewrite.
 * ETSI EN 300 706 §8.1 gives every display byte odd parity, so a byte that
 * fails it is known to be damaged: parity failures falling is squashing
 * removing damage it could prove was there. They are not a full error count —
 * a byte damaged in two bits passes parity and is invisible here — but they
 * are the only measure available without the original transmission, and they
 * move in the right direction for the right reason.
 *
 * Read the parity figures beside the row packets that reached no page at all.
 * Those are written as recovered — nothing could be voted over them — so their
 * damage is in the total with no chance of ever having been mended, and a run
 * that refuses more addresses shows a higher loss for having refused to guess.
 *
 * The copies-per-row distribution is the other half. A row transmitted once
 * cannot be corrected however good the vote is, so a run whose rows are mostly
 * single-copy has not failed to squash — it had nothing to squash. The two
 * figures separate "the recording is too short, or the page came round too
 * rarely" from "the copies disagreed and the vote could not settle it".
 *
 * Pure accumulation: no I/O, no clock.
 *
 * Thread safety: none; confine an instance to one thread.
 */
/// |value| grouped in threes — 16645 as "16,645" — so a six-figure count can
/// be read at a glance. Done by hand rather than through a locale so a report
/// is identical wherever it is produced.
std::string teletext_grouped(uint64_t value);

class TeletextSquashStats {
 public:
  // Copy-count buckets. The boundaries are where the vote changes character:
  // one copy cannot be corrected at all, two can only disagree, and a majority
  // becomes possible from three.
  static constexpr size_t kCopyBuckets = 4;  // 1, 2, 3-7, 8+

  /**
   * @brief Record one row packet passing through the rewrite pass
   *
   * @param before Display bytes as recovered
   * @param after  Display bytes as written (identical when the vote agreed
   *               with this copy, or when the row had a single copy)
   * @param copies Copies of this row the vote was taken over, or 0 when the
   *               packet belonged to no open page — an orphan row, which no
   *               vote can reach. Orphans count towards the character figures
   *               (they are written, so their damage is in the output) but not
   *               towards the copies distribution, which describes what the
   *               vote had to work with.
   * @param columns Display bytes the packet actually carried: 40 on a 625-line
   *               service (ETSI EN 300 706 §9.3.2), 32 on a 525-line one
   *               (ITU-R BT.653 Table 1b). Positions past it were never
   *               transmitted, and counting them as damaged — a zero byte
   *               fails odd parity — would put a fault on every 525-line row.
   */
  void add_row(const TeletextRowBytes& before, const TeletextRowBytes& after,
               size_t copies, size_t columns = kTeletextRowBytes);

  /**
   * @brief Record one row packet written without the rewrite pass
   *
   * The character figures are the run's headline whether or not rows were
   * combined, so the pass-through configuration feeds them too. Nothing was
   * voted on, so the row counts as its own before and after.
   */
  void add_written_row(const TeletextRowBytes& row,
                       size_t columns = kTeletextRowBytes) {
    add_row(row, row, /*copies=*/0, columns);
  }

  /// Row packets recorded
  uint64_t rows() const { return rows_; }
  /// ... of which belonged to an open page and could be voted on
  uint64_t rows_attributed() const { return rows_attributed_; }
  /// ... of which were rewritten by the vote
  uint64_t rows_rewritten() const { return rows_rewritten_; }
  /// Display bytes the vote replaced
  uint64_t bytes_changed() const { return bytes_changed_; }
  /// Display bytes examined (40 per 625-line row packet, 32 per 525-line one)
  uint64_t bytes_total() const { return bytes_total_; }
  /// Display bytes failing odd parity before the rewrite
  uint64_t parity_failures_before() const { return parity_before_; }
  /// ... and after it
  uint64_t parity_failures_after() const { return parity_after_; }
  /// Row packets whose vote was taken over @p bucket copies (see kCopyBuckets)
  uint64_t copies_in_bucket(size_t bucket) const;

  /**
   * @brief Record how the recovered rows were spread across page runs
   *
   * @param runs Sub-page runs the squasher tracked — distinct
   *             {magazine, page, sub-code, erase epoch} identities. A run of
   *             a page is what its copies could be combined within, so a
   *             recording whose runs outnumber its pages is one whose pages
   *             were repeatedly erased (C4, ETSI EN 300 706 §9.3.1.3 Table 2)
   *             and could not be combined across the erases.
   */
  void set_page_runs(size_t runs) { page_runs_ = runs; }
  size_t page_runs() const { return page_runs_; }

  /**
   * @brief The run's headline: how much of what was recovered is damaged
   *
   * One sentence, meant to be the first thing read. Damage is counted by the
   * odd parity the standard already puts on every display byte (ETSI EN 300
   * 706 §8.1), over the display rows of the packet stream as written.
   *
   * It is a floor, not an exact figure: parity catches every single-bit error
   * but a byte damaged in two bits passes it and is counted as good. It also
   * says nothing about rows that never arrived — a page row lost outright is
   * absent from both sides of the ratio. Read it as "of the characters this
   * export produced, this share are known wrong", which is the question a
   * reader deciding whether to keep the capture is actually asking.
   *
   * Empty when no row packet was recorded.
   */
  std::string character_loss_summary() const;

  /**
   * @brief Human-readable multi-line summary of what the vote changed
   *
   * One line saying nothing happened when no row packet was recorded. Stable
   * for a given set of inputs: no timestamps, no ordering by hash.
   */
  std::string summary() const;

 private:
  uint64_t rows_ = 0;
  uint64_t rows_attributed_ = 0;
  uint64_t rows_rewritten_ = 0;
  uint64_t bytes_changed_ = 0;
  uint64_t bytes_total_ = 0;
  uint64_t parity_before_ = 0;
  uint64_t parity_after_ = 0;
  std::array<uint64_t, kCopyBuckets> copies_{};
  size_t page_runs_ = 0;
};

}  // namespace orc

#endif  // ORC_TELETEXT_SQUASH_STATS_H
