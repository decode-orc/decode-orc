/*
 * File:        teletext_row_squasher.h
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     Combine repeated copies of a teletext page row into one
 *              best-estimate row ("squashing")
 *
 * The squashing technique implemented here — grouping every copy of a
 * sub-page recovered from a recording and combining them per byte position —
 * is taken from vhs-teletext by Alistair Buxton (ali1234),
 * https://github.com/ali1234/vhs-teletext, with thanks. No code is shared:
 * this is an independent implementation of the idea, extended to weight the
 * vote by the odd-parity check the standard already provides (see the class
 * comment below).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_ROW_SQUASHER_H
#define ORC_TELETEXT_ROW_SQUASHER_H

// Shared plugin-side library, NOT part of the SDK contract: it compiles
// against the public SDK headers only and is linked privately into the stage
// plugins that need it. Nothing here crosses the plugin boundary, so changes
// never force an ABI bump.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "teletext_slicer.h"

namespace orc {

// ETSI EN 300 706 §9.3.2: a displayable row packet carries 40 bytes.
constexpr size_t kTeletextRowBytes = 40;

using TeletextRowBytes = std::array<uint8_t, kTeletextRowBytes>;

// How sure the recovery chain was of each byte of a row, 0 … 1 (see
// TeletextLineResult::byte_confidence). A copy recorded without one counts as
// wholly confident, which is what makes the weighting invisible to a caller
// that has nothing to say about it.
using TeletextRowConfidence = std::array<float, kTeletextRowBytes>;

// Which byte positions of a squashed row any copy actually covered. A service
// that sends a row's columns in more than one packet (see add_row()'s column
// range) can leave part of a row with nothing to vote on, and a position no
// copy covered is not the same as one every copy agreed was zero.
using TeletextRowCoverage = std::array<bool, kTeletextRowBytes>;

// Identity of the page a row belongs to. Sub-code is part of the key because
// a different sub-code is a different page (ETSI EN 300 706 §9.3.1.2) —
// merging rotating sub-pages would blend unrelated content.
struct TeletextPageKey {
  int magazine = 8;
  int page_number = 0;
  int subcode = 0;
  // Which run of this page's content the row belongs to. Where the service
  // replaced the content — ETSI EN 300 706 Annex A.2 calls what it showed
  // before "the previous version of the page" — copies either side are copies
  // of different versions and must not be combined; TeletextPageDecoder
  // decides where that happened (see its set_row_squasher()). Counting the
  // runs rather than deleting the earlier copies keeps them all addressable: a
  // consumer replaying a recovered stream can ask for the run each packet
  // actually belonged to, which a consumer that only ever wants the newest run
  // gets for free by keying on the decoder's current key.
  int erase_epoch = 0;

  bool operator<(const TeletextPageKey& other) const {
    if (magazine != other.magazine) return magazine < other.magazine;
    if (page_number != other.page_number)
      return page_number < other.page_number;
    if (subcode != other.subcode) return subcode < other.subcode;
    return erase_epoch < other.erase_epoch;
  }
  bool operator==(const TeletextPageKey& other) const {
    return magazine == other.magazine && page_number == other.page_number &&
           subcode == other.subcode && erase_epoch == other.erase_epoch;
  }
};

/**
 * @brief Combines repeated transmissions of a page row into one row
 *
 * Teletext is a carousel: any recording longer than one carousel cycle holds
 * several copies of every row, damaged in different places. Comparing the
 * copies recovers a row cleaner than any single copy of it. The technique,
 * and the name, come from vhs-teletext by Alistair Buxton (ali1234) —
 * https://github.com/ali1234/vhs-teletext — where "squashing" compares every
 * instance of a sub-page and takes the most frequent value at each position.
 *
 * The combination here is likewise per byte position. Where that project
 * takes the plain mode (most frequent value), this one first restricts the
 * vote to candidates that pass odd parity (ETSI EN 300 706 §8.1): parity
 * detects every single-bit error, so a byte that fails it is known to be
 * corrupt and should never win over one that passes, however often it was
 * seen. The plain mode is the fallback when no copy of a byte is
 * parity-clean.
 *
 * Within that restriction the vote is by weight rather than by head count: a
 * copy recorded with per-byte confidences (see add_row()) contributes its
 * confidence in the byte instead of one vote. Parity and confidence answer
 * different questions and neither subsumes the other — parity is certain and
 * blind (it catches every single-bit error but says nothing about which byte to
 * believe among those that pass), while confidence is graded and fallible (a
 * bit the detector was sure of can still be wrong). Ordering them this way
 * keeps the certainty first: a value known to be corrupt cannot win however
 * confidently it was recovered, and among values that might be right the one
 * the detector nearly misread does not outvote the one it read cleanly.
 *
 * Not every copy filed under a row is a copy of that row: the MRAG is Hamming
 * 8/4 and mis-corrects on a burst, and a page whose header was lost leaves the
 * one before it open, so packets of other pages arrive here looking like
 * content. Combining them is worse than not combining at all — the row becomes
 * a per-character blend of every page mis-addressed into it, and the blend gets
 * worse the more copies are combined. So the vote is taken twice: once to find
 * what the copies mostly say, and again without the copies that agree with
 * less than half of it. Only a minority may be dropped this way, since where
 * most copies disagree there is no row for the rest to be outliers of.
 *
 * Copies are keyed by an opaque source id so a consumer that re-reads the
 * same recovered line — as a sliding-window previewer does every time its
 * window is rebuilt — replaces its earlier copy instead of stuffing the
 * ballot.
 *
 * A header packet's own display bytes are deliberately not squashed: they carry
 * a real-time clock (§9.3.1.4) that legitimately differs between transmissions
 * of the same page, so a copy of row 0 starting at column 0 is refused. Columns
 * further along row 0 are accepted — where a service carries part of its header
 * row in a separate packet (525-line WST's row extensions, which put the
 * service name at columns 32-39) that part holds still and is worth combining.
 *
 * Thread safety: none; confine an instance to one thread.
 */
class TeletextRowSquasher {
 public:
  struct Options {
    // Copies retained per row. Beyond a handful the vote rarely changes, and
    // each copy costs 40 bytes plus its source id.
    size_t max_copies_per_row = 16;
    // Upper bound on tracked pages; the least recently updated is dropped
    // when the bound is reached.
    size_t max_pages = 1024;
  };

  TeletextRowSquasher() = default;
  explicit TeletextRowSquasher(Options options) : options_(options) {}

  /**
   * @brief Record one copy of a display row
   *
   * @param key    Page the row belongs to
   * @param row    Display row 1-24 (ETSI EN 300 706 §9.3.2)
   * @param bytes  The 40 display bytes in transmission coding
   * @param source Opaque identity of where this copy came from. Adding the
   *               same source twice replaces the earlier copy rather than
   *               counting it again.
   * @param confidence How sure the recovery chain was of each byte, or nullptr
   *               when it cannot say — which counts as wholly sure, so a caller
   *               with no confidences to offer votes exactly as it always did.
   * @param first_column First byte position of @p bytes this copy speaks for
   * @param column_count How many positions from there it speaks for
   *
   * The column range is for services that send one display row in more than
   * one packet — 525-line WST sends columns 32-39 separately (see
   * teletext_page_decoder.h). A copy votes only within its range, so the two
   * halves of a row are combined across repeats independently and neither
   * pollutes the other's positions. The default covers the whole row, which is
   * what a service carrying a row in one packet wants.
   */
  void add_row(const TeletextPageKey& key, int row,
               const TeletextRowBytes& bytes, int64_t source,
               const TeletextRowConfidence* confidence = nullptr,
               size_t first_column = 0,
               size_t column_count = kTeletextRowBytes);

  /**
   * @brief Best estimate of @p row, or nullopt when no copy has been recorded
   *
   * @param covered When non-null, receives which byte positions any copy spoke
   *                for. Positions no copy covered hold zero in the returned row
   *                and false here; a caller that mixes column ranges must read
   *                this rather than treat the zero as a recovered byte. A
   *                position only outlying copies covered is not covered: they
   *                are not copies of this row, so nothing spoke for it.
   */
  std::optional<TeletextRowBytes> squashed_row(
      const TeletextPageKey& key, int row,
      TeletextRowCoverage* covered = nullptr) const;

  /**
   * @brief Drop every copy held for one run of a page
   *
   * Combining copies assumes they are copies of the *same* content, so a page
   * whose content is replaced must not vote across the replacement. The key's
   * erase_epoch is what keeps the runs apart, and TeletextPageDecoder advances
   * it for you where the content changed — this is for a consumer keying the
   * squasher itself, and for reclaiming a run it knows it will not ask about
   * again.
   */
  void erase_page(const TeletextPageKey& key);

  /// Copies recorded for @p row (0 when none) that speak for @p column. The
  /// default counts the copies that speak for the row's first column — the
  /// packets carrying the row itself, rather than any that only extend it (see
  /// add_row()'s column range), which is what "how many times was this row
  /// transmitted" means. A column past the display packet's reach counts the
  /// packets that extend the row there instead.
  size_t copy_count(const TeletextPageKey& key, int row,
                    size_t column = 0) const;

  /// Times @p row arrived in this run, counting the packets carrying the row
  /// itself as copy_count() does but without its bound: copies beyond
  /// Options::max_copies_per_row replace the oldest held, and still arrived.
  /// A copy re-fed under a source already held is not a new arrival.
  size_t arrival_count(const TeletextPageKey& key, int row) const;

  /// Pages currently tracked
  size_t page_count() const { return pages_.size(); }

  void clear();

 private:
  struct RowCopies {
    std::vector<TeletextRowBytes> copies;
    std::vector<TeletextRowConfidence> confidences;
    std::vector<int64_t> sources;
    // Byte positions each copy speaks for (see add_row()). A row carried whole
    // by one packet has every copy at {0, kTeletextRowBytes}.
    std::vector<uint8_t> first_column;
    std::vector<uint8_t> column_count;
    // Insertion order of each slot. Once the copy bound is reached the oldest
    // slot is overwritten in place (erasing the front of the parallel vectors
    // would move every later element on every saturated add), so slot order no
    // longer says which copy is newest — this does. A replaced copy keeps its
    // sequence number: re-reading a line is not a new transmission.
    std::vector<uint64_t> seq;
    uint64_t next_seq = 0;
    // Copies of the row itself (first column 0) that were new rather than a
    // re-read, however many of them the bound let the row keep.
    uint64_t arrivals = 0;
  };
  struct PageRows {
    // Index 0 is unused: display rows are numbered from 1 (§9.3.2).
    std::array<RowCopies, 25> rows;
    // Monotonic counter of the last update, for the max_pages bound.
    uint64_t last_touched = 0;
  };

  // One pass of the vote over |copies|. |included| is either null — every copy
  // votes — or one flag per copy, zero for the copies the outlier pass ruled
  // out. Both outputs are overwritten in full.
  static void vote_row(const RowCopies& copies, const uint8_t* included,
                       TeletextRowBytes& result, TeletextRowCoverage& covered);

  void enforce_page_bound();

  Options options_;
  std::map<TeletextPageKey, PageRows> pages_;
  uint64_t touch_counter_ = 0;
};

}  // namespace orc

#endif  // ORC_TELETEXT_ROW_SQUASHER_H
