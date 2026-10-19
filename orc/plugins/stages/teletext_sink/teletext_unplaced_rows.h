/*
 * File:        teletext_unplaced_rows.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     The display packets no page could be held responsible for, as
 *              text
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_TELETEXT_UNPLACED_ROWS_H
#define ORC_TELETEXT_UNPLACED_ROWS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "vbi-services/teletext_slicer.h"

namespace orc {

// Packets kept for the text, past which they are counted and not kept: about
// fifteen megabytes of text, ten times what the most damaged recording
// measured leaves unplaced.
constexpr uint64_t kTeletextUnplacedMaxRows = 200000;

/**
 * @brief The display packets no page could be held responsible for, as text
 *
 * The decoder places each display packet on the page whose transmission it
 * belongs to (ETSI EN 300 706 §7.2.1). One that arrives with no page of its
 * magazine open — its page's header lost, on a service whose headers close
 * the page in progress in every magazine — or whose address had to be
 * corrected reaches no page: it is written to the stream as recovered, and no
 * page shows it (TeletextSquashStats). Its characters arrived all the same,
 * and many are rows no page that did arrive holds: a version of a changing
 * page whose header went, rows a service sent without a page. This keeps
 * them readable: one line a packet, in the order they arrived, with the field
 * it came in, its magazine and packet number, and its display bytes as
 * characters — a control code (a spacing attribute, which a set shows as a
 * space) as a space, a byte failing its odd parity (§8.1) as '~'.
 *
 * Pure: no I/O, no clock.
 *
 * Thread safety: none; confine an instance to one thread.
 */
class TeletextUnplacedRows {
 public:
  /// Note |packet|, which reached no page, recovered in field |field_index|
  /// of the range and carrying |display_bytes| display bytes after its
  /// address (32 on a 525-line service, 40 on a 625-line one).
  void add(int64_t field_index,
           const std::array<uint8_t, kTeletextPacketBytes>& packet,
           size_t display_bytes);

  /// Packets noted, and those of them kept for the text.
  uint64_t rows() const { return rows_; }
  uint64_t rows_kept() const { return rows_ - rows_dropped_; }

  /// The document: a header saying what it holds, then a line a packet.
  /// Empty when no packet was noted.
  std::string text() const;

 private:
  std::string lines_;
  uint64_t rows_ = 0;
  uint64_t rows_dropped_ = 0;
};

}  // namespace orc

#endif  // ORC_TELETEXT_UNPLACED_ROWS_H
