/*
 * File:        nabts_salvage.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Salvage of the groups whose synchronizing packet was lost
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_SALVAGE_H
#define ORC_NABTS_SALVAGE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nabts_data_group.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

// Record addresses are nine hexadecimal digits, 36 bits (§5.2.5). A page
// salvaged without its record header is listed at this address plus its
// number, above anything a record header can carry, so no address arithmetic
// — a More Record's algorithmic successor (§5.2.7.6), a keypad page number —
// can ever reach it from a page that was named.
constexpr uint64_t kNabtsIdentityLostAddressBase = uint64_t{1} << 40;

// Bytes a loose run and a record must both hold, received and passing their
// parity, at the alignment that agrees best, before the run is judged a copy
// of the record: a block and a half, enough that a page template two pages
// share does not decide it alone.
constexpr size_t kNabtsSalvageMinJudged = 40;

// Of those bytes, the share that must agree. Loose runs come from the
// recordings that lose the most, but the bytes compared are the ones passing
// their parity, of which a damaged tape's copies agree on nearly all; a run of
// another page on the same template agrees on the template and nothing after.
constexpr size_t kNabtsSalvageMinAgreePercent = 85;

// Loose runs held for salvage over one pass, and the bytes they may hold
// between them. The most damaged recording measured hands on about four
// thousand runs of some fifteen packets; the bounds sit well above that and
// keep a capture that loses every synchronizing packet to tens of megabytes.
constexpr size_t kNabtsSalvageMaxRuns = 65536;
constexpr size_t kNabtsSalvageMaxBytes = size_t{32} << 20;

/// A loose run as salvage holds it: the run, and the frame it ended in.
struct NabtsHeldRun {
  NabtsLooseRun run;
  uint64_t frame = 0;
};

/// What salvage made of the loose runs (see NabtsSalvage::apply()).
struct NabtsSalvageStats {
  /// Runs taken in, and runs refused once the bounds were met.
  uint64_t runs_held = 0;
  uint64_t runs_refused = 0;
  /// Runs found to be copies of a catalogued record.
  uint64_t runs_matched = 0;
  /// Bytes of those records no copy had delivered, filled from them, and the
  /// records filled.
  uint64_t bytes_filled = 0;
  uint64_t records_filled = 0;
  /// Runs no catalogued record holds, and those of them that make up the
  /// pages listed without their identity.
  uint64_t runs_unmatched = 0;
  uint64_t runs_in_pages = 0;
  /// Pages listed without their identity
  /// (NabtsCataloguedRecord::identity_lost).
  uint64_t pages_found = 0;
  /// Groups of runs refused as pages because their code reads unlike the
  /// service's own records.
  uint64_t pages_refused = 0;

  bool acted() const { return bytes_filled > 0 || pages_found > 0; }

  /// One line for the stage report.
  std::string summary() const;
};

/**
 * @brief What becomes of the groups whose synchronizing packet was lost
 *
 * A group's first packet carries its group header and its record header, and
 * a recording that loses a line in every field loses it whenever a group
 * happens to start there: the rest of the group then arrives on a channel with
 * no group open, and the group assembler can do nothing with it but count it
 * (NabtsLooseRun). One recording measured lost the start of a third of its
 * groups this way, and a few dozen of its pages never once arrived with their
 * start, so they were never listed at all.
 *
 * The bytes are received bytes all the same, and the record they belong to can
 * be found from them: a run is set against the catalogued records of its
 * channel at every alignment their shared stretches of six bytes suggest,
 * and judged a copy of the one it agrees with best where both hold enough
 * bytes passing their parity (kNabtsSalvageMinJudged) and nearly all of them
 * agree (kNabtsSalvageMinAgreePercent). Its bytes then fill the record's holes
 * — the positions no copy delivered — and never a byte the vote settled: a run
 * is one reading, the vote many. That matters where a recording loses the same
 * line in every field, since a group whose start fell on that line lost the
 * blocks every other copy kept, and kept the ones they lost.
 *
 * A run no record holds is a page none of its copies named. Runs are grouped
 * by the same test, and a group of two or more — or one run clean enough to
 * stand alone — is voted into a page of its own
 * (NabtsCataloguedRecord::identity_lost), listed after the named records at
 * an address no record header can carry (kNabtsIdentityLostAddressBase). Its
 * head is a hole, since the record's start went with the synchronizing packet,
 * and its bytes begin at the first shift or escape the run holds, before
 * which the presentation code is the tail of a command whose start was lost.
 *
 * Only channels with a record named as transmitted are salvaged: a run on a
 * misread packet address has no records to be a copy of, and would otherwise
 * be listed as pages of a channel the service never used.
 *
 * The runs placed nowhere — on such a channel, a copy of no listed record and
 * part of no listed page — are received bytes all the same, and are kept for
 * the caller to write out as they arrived (unplaced(),
 * nabts_unplaced_runs_text()).
 */
class NabtsSalvage {
 public:
  /// Hold |run|, which ended in frame |frame|. Runs past the bounds
  /// (kNabtsSalvageMaxRuns, kNabtsSalvageMaxBytes) are counted and dropped.
  void add(const NabtsLooseRun& run, uint64_t frame);

  /// Runs held so far.
  std::size_t size() const { return runs_.size(); }

  /**
   * @brief Fill |records|' holes from the runs that are copies of them, and
   *        append the pages the rest make up
   *
   * |records| are the catalogue's, voted (NabtsRecordCatalogue::records()).
   * A record's bytes that a run fills are marked as arrived in its
   * data_present. The pages appended carry identity_lost and are interpreted
   * with the rest.
   */
  NabtsSalvageStats apply(std::vector<NabtsCataloguedRecord>& records);

  /// The runs apply() placed nowhere, in the order they were held: on a
  /// channel no record was named on, or a copy of no listed record and part
  /// of no listed page. Empty before apply().
  const std::vector<NabtsHeldRun>& unplaced() const { return unplaced_; }

 private:
  using Held = NabtsHeldRun;

  std::vector<Held> runs_;
  std::vector<Held> unplaced_;
  std::size_t bytes_ = 0;
  NabtsSalvageStats stats_;
};

/**
 * @brief The runs salvage placed nowhere, as text
 *
 * One block a run: its channel, the frame it ended in and the packets it
 * holds, then its bytes as they arrived, 64 to a line — a 7-bit character
 * where the byte passes its odd parity, '~' where it fails, '_' for a byte of
 * a packet the continuity index says was lost, {SO}, {SI} and {ESC} for the
 * shifts and the escape that say how the bytes after them read (X3.110
 * §6.1.3), and '.' for any other control. Empty when |runs| is.
 */
std::string nabts_unplaced_runs_text(const std::vector<NabtsHeldRun>& runs);

}  // namespace orc

#endif  // ORC_NABTS_SALVAGE_H
