/*
 * File:        nabts_record_catalogue.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Bounded catalogue of the teletext records an analysed range
 *              carried, merged from assembled messages
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_RECORD_CATALOGUE_H
#define ORC_NABTS_RECORD_CATALOGUE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "nabts_record.h"
#include "naplps_interpreter.h"
#include "naplps_lint.h"
#include "vbi-services/vbi_analysis_results.h"
#include "vbi-services/vbi_identity_attestation.h"

namespace orc {

/**
 * @brief One copy of a record's data, with the holes it arrived with
 *
 * The two run index for index: |present| says, for each byte of |data|, whether
 * that byte arrived or merely stands in for one a lost packet carried, and is
 * empty when nothing was lost. Holding the holes open rather than closing them
 * up is what keeps a damaged copy comparable with the others (see
 * NabtsDataGroup::present).
 */
struct NabtsRecordCopy {
  std::vector<uint8_t> data;
  std::vector<uint8_t> present;
  /// How sure the detector was of each byte, 0-255 (see
  /// NabtsDataGroup::confidence). Empty where nothing measured this copy, which
  /// is read as full confidence — a detector that cannot express doubt has not
  /// expressed any.
  std::vector<uint8_t> confidence;
};

/**
 * @brief What lint-directed repair made of a whole catalogue of records
 *
 * Aggregated over every presentation record, so the stage's report can say how
 * much of what a reader will see was recovered rather than received. The
 * per-page detail is in each page's own diagnostics.
 */
struct NabtsLintTotals {
  uint32_t records_linted = 0;
  /// Records the linter faulted before any repair.
  uint32_t records_faulted = 0;
  /// Records still faulted after repair — what is left to look at.
  uint32_t records_faulted_after = 0;

  uint64_t errors_before = 0;
  uint64_t warnings_before = 0;
  uint64_t errors_after = 0;
  uint64_t warnings_after = 0;

  uint64_t suspect_bytes = 0;
  /// Doubted bytes that also failed their parity, which is what the repair
  /// needs before it may change one. The gap between the two is how much of
  /// the doubt is being reported rather than acted on.
  uint64_t bytes_offered = 0;
  uint64_t bytes_repaired = 0;
  uint64_t bytes_ambiguous = 0;
  uint64_t changes_declined_by_reach = 0;
  uint64_t pdis_resynchronised = 0;
  uint64_t coordinate_words_dropped = 0;

  /// Records the pass altered, and those of them whose *page* came out
  /// different. The fault counts below say whether the records parse better;
  /// only this says whether what a reader is shown was changed.
  uint32_t records_altered = 0;
  uint32_t records_drawing_changed = 0;

  /// Human-readable summary for the stage report. Empty where nothing was
  /// linted and nothing was found.
  std::string summary() const;
};

/**
 * @brief Lint and repair every presentation record, for the totals alone
 *
 * Does not change @p records: it exists so a run can report how much repair the
 * recording needed without a reader having to open every page. Records are
 * repaired in the same order nabts_interpret_records() repairs them — Support
 * Record first, per Data Channel — so the totals describe the same readings a
 * reader will be shown.
 */
NabtsLintTotals nabts_lint_records(
    const std::vector<NabtsCataloguedRecord>& records);

/**
 * @brief Run every presentation record's code into its page
 *
 * §6.1: a presentation record's data is NAPLPS, so this is where it becomes
 * something a viewer can draw. Each record is presented the way a receiver
 * would present the page — the general reset of CEA-516 §8.5, the channel's
 * Support Record where the Support-Needed Flag asks for it (§5.2.7.9), the
 * caption preset of §5.2.7.3 where the Caption Flag does, and the record drawn
 * over its own More chain (§5.2.7.8) — so the chains and the support records
 * have to be resolved across the whole catalogue rather than record by record.
 *
 * Separate from recovery because it depends on @p grid, the receiver being
 * emulated: X3.110 §6.2.3 sizes a DRCS character's storage buffer from the
 * physical resolution its character field covers, so interpreting during
 * recovery would pin part of the result to whatever receiver was configured
 * when the recording was read. Records whose type is not a presentation one
 * are left alone.
 *
 * @param repair Whether to run each record's presentation code through
 *               lint-directed repair before interpreting it (see
 *               naplps_lint_repair.h). What the repair changes is how the
 *               record *reads*; @ref NabtsCataloguedRecord::data keeps the
 *               bytes as they were recovered, so the packet stream and the
 *               record files a run exports are unaffected. Each page's
 *               diagnostics carry what the repair did to it.
 */
void nabts_interpret_records(std::vector<NabtsCataloguedRecord>& records,
                             NaplpsRenderGrid grid, bool repair = false);

// ---------------------------------------------------------------------------
// The vote
// ---------------------------------------------------------------------------

/**
 * @brief How close in weight two candidates have to be to count as tied
 *
 * A percentage of the leader's weight. Exact ties are the case this is really
 * for — two copies of a record saying different things once each — but a lead
 * of a few per cent is one copy's detector being marginally happier than
 * another's about a byte they disagree on, which is not evidence enough to
 * settle it either. Anything further behind than this lost on the weights, and
 * the grammar is not asked.
 */
constexpr unsigned kNabtsVoteTiePercent = 90;

/**
 * @brief How much of a record a copy has to agree with to be a copy of it
 *
 * A percentage of the positions both it and the vote have a byte for. Copies of
 * one record differ only where they were damaged, which on a recovered
 * recording is a small fraction of a record; a copy of some other record —
 * assembled here by a mis-corrected packet address (§3.2.2), or folded in under
 * an identity the recording never named — agrees only by coincidence, which
 * over a record's length is nowhere near half of it. Anywhere between the two
 * settles the same copies either way.
 */
constexpr unsigned kNabtsOutlierAgreementPercent = 50;

/// Fewest positions a copy must be judged over before it may be ruled out. A
/// record of a handful of bytes can agree with another by chance.
constexpr std::size_t kNabtsOutlierMinJudgedPositions = 16;

/**
 * @brief Furthest a copy's data may be slid to line it up with the others
 *
 * Where a record's data starts is read from its header: RD and the announcers
 * after it say which optional bytes follow (§5.2.3, §5.2.7), and each is a
 * Hamming 8/4 byte a burst can mis-correct. A misread announcer moves the start
 * of the data by the bytes it claimed or disowned — a classification sequence
 * or a short header extension — and with it every byte of the copy, so a copy
 * of the record reads as a copy of nothing. Those are a few bytes; this is the
 * widest slip looked for.
 */
constexpr std::size_t kNabtsMaxRecordSlip = 8;

/**
 * @brief Contested positions in one record the grammar is asked about
 *
 * Each one costs a lint pass per candidate, and a record whose copies disagree
 * in more places than this is one the vote is not going to rescue by argument.
 * The count is reported either way, so a capped record says so rather than
 * looking decisive.
 */
constexpr size_t kNabtsMaxAdjudicatedPositions = 32;

/// Candidates put to the grammar at one contested position, heaviest first.
/// Every copy retained could in principle say something different, and a
/// position where four of them are still level is one no argument is going to
/// settle.
constexpr size_t kNabtsMaxVoteCandidates = 4;

/// What the vote is allowed to do beyond weighing the copies.
struct NabtsVoteOptions {
  /**
   * Ask the grammar about positions the weights could not separate: lint the
   * whole record with each leading candidate in place and keep the one that
   * leaves it most grammatical (see NaplpsLintGrade). Off here and on at the
   * stage, which is where the reader decides — this is recovery rather than
   * presentation, so it changes @ref NabtsCataloguedRecord::data and everything
   * downstream of it, the exported record files included.
   *
   * Meaningful only for a presentation record: an application record's data is
   * function descriptors (§7.2.2), and grading it as NAPLPS would be reading it
   * in a language it is not written in.
   */
  bool grammar_assisted = false;

  /**
   * The service state the record will be read in — the channel's Support
   * Record, chiefly, whose macros §5.2.7.9 has the pages of that channel
   * invoke. Optional: without it every such invocation lints as an undefined
   * macro, which costs the tie-break nothing directly (every candidate carries
   * the same finding) but leaves it grading a record against a grammar it is
   * only half in possession of.
   *
   * Never advanced: each trial forks it, so nothing a candidate byte would
   * define outlives the trial that considered it.
   */
  NaplpsLinter* context = nullptr;
};

/// One record's data as the vote settled it, with what the vote learned on the
/// way — see @ref NabtsCataloguedRecord::data_present for what the two masks
/// mean and why they are worth keeping.
struct NabtsVoteResult {
  std::vector<uint8_t> data;
  std::vector<uint8_t> present;
  std::vector<uint8_t> confidence;
  /// Positions where the leading candidates were within
  /// kNabtsVoteTiePercent of one another.
  uint32_t positions_contested = 0;
  /// Contested positions the grammar decided. The rest kept the recency pick,
  /// either because the grammar was not asked or because it had no preference.
  uint32_t positions_adjudicated = 0;
};

/**
 * @brief Combine repeated copies of one record's data into one best estimate
 *
 * A cyclic service (§7.1.2) brings every record round for the length of the
 * recording, so a capture holds many copies of each. Copies differ only where
 * they were damaged, and a vote across them recovers bytes that no single copy
 * has right — which matters for NAPLPS, where the data is a stateful opcode
 * stream and one wrong byte changes how the several after it are read.
 *
 * The vote is held per byte position. CEA-516 §3.3 gives every data byte of a
 * type-zero group odd parity in b8, and §4.3 makes a type-zero group the only
 * one carrying a teletext record — so here, unlike at the packet layer where
 * the group type is not yet known, every byte is known to be parity-coded and a
 * byte failing the check is known to be corrupt. Parity-clean candidates
 * therefore win outright wherever a position has any; a position every copy
 * damaged falls back to a vote among all of them.
 *
 * Within a class, each copy contributes how sure the detector was of its byte
 * rather than one vote apiece, so a value read cleanly outweighs the same
 * number of copies of one the detector nearly decided the other way. A copy
 * nothing measured contributes full weight, which is the reading the row
 * squasher gives an unmeasured copy and the only honest one for a detector with
 * no way of saying it is unsure. Ties go to the most recent copy, which is what
 * keeping a single copy would have shown — unless @ref
 * NabtsVoteOptions::grammar_assisted asks the grammar first.
 *
 * A copy abstains at the positions its own lost packets took from it, so a
 * recording that never received any one copy whole can still have every
 * position of the record decided by whichever copies did receive it.
 *
 * A copy that would be ruled out below is first tried a few bytes either side
 * (up to kNabtsMaxRecordSlip), since a misread record header starts the data
 * in the wrong place; where some slide makes it agree with the others, it
 * votes slid. A copy that agrees as it stands is never moved.
 *
 * Not every copy held for a record is a copy of it: a mis-corrected packet
 * address assembles a packet of some other record into this one, and
 * reconcile_identities() folds in the copies of identities the recording never
 * named. Such a copy is a clean read of real data and neither parity nor
 * confidence can tell it apart, so the vote is taken twice — once to find what
 * the copies mostly say, and again without the copies agreeing with less than
 * @ref kNabtsOutlierAgreementPercent of it. Only a minority may be dropped that
 * way, since where most copies disagree there is no record for the rest to be
 * outliers of.
 *
 * |copies| must be alignable position for position, oldest first — see
 * NabtsMessage::aligned, which is what admits a copy to the vote. The result is
 * as long as the length most copies agree on, and the longest of them where
 * there is no agreement at all; a copy shorter than that votes only over the
 * bytes it has.
 *
 * Linear in the record length and the number of copies, plus — only where the
 * grammar is asked and only for the positions it is asked about — a bounded
 * number of lint passes over the record (kNabtsMaxAdjudicatedPositions).
 */
NabtsVoteResult nabts_vote_record(const std::vector<NabtsRecordCopy>& copies,
                                  const NabtsVoteOptions& options = {});

/// The voted data alone, for a caller with no use for the evidence.
std::vector<uint8_t> nabts_vote_record_data(
    const std::vector<NabtsRecordCopy>& copies);

/**
 * @brief Accumulates assembled messages into a bounded record catalogue
 *
 * A trigger run reads the whole frame range in one pass, and a cyclic service
 * (CEA-516 §7.1.2) sends every record of its magazine round and round for the
 * length of the recording. Catalogueing them — one entry per {channel, record
 * address, version}, the identity §5.2.1 gives a record — bounds what the run
 * holds by the size of the service rather than by the length of the recording.
 *
 * The version is part of the identity because §5.2.1 says it is: a service that
 * revises a page increments Y16 (§5.2.7.2), and the old and new versions are
 * different records that happen to share an address. Keying on the address
 * alone would show whichever the recording happened to end on.
 *
 * Which copy is kept is decided by how good it is rather than by how recent:
 * a complete, undamaged copy is never replaced by a damaged one, so a service
 * recorded off air keeps the copy that arrived cleanly rather than the last one
 * before the tape ran out. Among copies of equal quality the longer wins, and
 * among equals the first — replacing like with like would only churn.
 *
 * Where no copy ever arrives whole — which is the ordinary case for a recording
 * off tape — the damaged copies are kept and combined instead of one of them
 * being chosen, so the record data is a vote across the carousel rather than
 * whichever copy happened to be longest. See nabts_vote_record_data() for the
 * vote, and NabtsMessage::aligned for which copies are eligible to take part.
 * The copies are discarded the moment an undamaged one arrives: there is then
 * nothing left for a vote to improve on.
 *
 * Messages must be merged in ascending temporal order, which is the order the
 * record assembler emits them in.
 *
 * Thread safety: none; confine an instance to one thread.
 */
class NabtsRecordCatalogue {
 public:
  /**
   * Upper bound on catalogued records.
   *
   * §7.1.1 organises a service into magazines of pages, and §3.2.3 allows 4096
   * channels of them; a real broadcast service ran to a few hundred pages. This
   * sits above that with room for several channels to share the recording,
   * which is what a capture of a whole VBI actually contains. When full, the
   * least recently seen record is dropped and the dataset flagged truncated.
   */
  static constexpr std::size_t kMaxCataloguedRecords = 1024;

  /**
   * Copies of a record's data retained for the vote.
   *
   * Beyond a handful of copies the vote rarely changes, and a recording of any
   * length would otherwise hold every copy of every record it carried. Record
   * data is capped at 1904 bytes by §8.4.2.5, so the two bounds together put
   * the worst case a little over 30 MB — reached only by a capture that fills
   * the catalogue with full-length records none of which ever arrives
   * undamaged. A record that does arrive undamaged holds no copies at all.
   */
  static constexpr std::size_t kMaxCopiesPerRecord = 16;

  explicit NabtsRecordCatalogue(std::size_t max_records = kMaxCataloguedRecords,
                                std::size_t max_copies = kMaxCopiesPerRecord);

  /**
   * @brief Merge one assembled message
   *
   * @param message  Message as joined by NabtsRecordAssembler
   * @param frame_id Frame carrying the last packet that completed it
   *
   * Every copy counts as an appearance, including a partial one: how often a
   * record came round is a property of the service, and a reader comparing
   * times_seen against times_intact is exactly how the recording's condition
   * shows up.
   */
  void merge(const NabtsMessage& message, uint64_t frame_id);

  /**
   * @brief Remove the record identities the recording never actually named
   *
   * CEA-516 §5.2.1 identifies a record by its data channel, its record address
   * and its version, and every digit of all three is carried by its own Hamming
   * 8/4 byte. ETSI EN 300 706 §8.2 gives that code minimum distance 4, so a
   * burst of three bit errors — the ordinary error on a band-limited recording,
   * not the exotic one — resolves silently to a neighbouring codeword. The
   * damage that does is not to the record: it is a *copy* of the record, filed
   * under a channel, address or version the service never transmitted, which
   * this catalogue then holds beside the original with no way to tell them
   * apart.
   *
   * So the catalogue counts, per entry, how often the identity arrived as
   * transmitted (NabtsCataloguedRecord::times_attested), and this pass acts on
   * it. An entry no arrival ever attested is not a record:
   *
   *  - where exactly one attested identity differs from it in a single
   *    hexadecimal digit, it is a misreading of that one. Its copies join that
   *    record's vote — which is the point of folding rather than discarding:
   *    the bytes were received and they are that record's — and its appearances
   *    and frame extent are added to it.
   *  - otherwise it is removed. Several attested neighbours make the misreading
   *    ambiguous, and none at all is a false lock on noise; neither can be
   *    attributed to anything.
   *
   * Withheld entirely on a catalogue where nothing was ever attested, which
   * offers no baseline to judge the rest against (see
   * vbi_identity_reconciliation_applies()). On an undamaged source every byte
   * arrives as transmitted, so the pass finds nothing and costs one walk of the
   * catalogue.
   *
   * Call once, after the last merge() and before records(): the fold has to
   * happen while the copies are still held, since records() is where the vote
   * across them is taken.
   */
  VbiIdentityReconciliation reconcile_identities();

  /// Records catalogued so far — what the cap bounds.
  std::size_t size() const { return records_.size(); }

  /// True once the cap has dropped at least one record.
  bool truncated() const { return truncated_; }

  /**
   * @brief Catalogue contents, ascending by {channel, address, version}
   *
   * Where a record's copies were combined rather than chosen among, the vote is
   * held here, so each record carries the data a receiver would have presented.
   * Running that data is nabts_interpret_records()' business and deliberately
   * not done here: what a page looks like depends on the receiver it is drawn
   * for, and recovery has no business fixing that.
   *
   * @param grammar_assisted_vote Let the grammar settle the positions the
   * vote's weights could not (NabtsVoteOptions::grammar_assisted). Each Data
   *        Channel's Support Record is voted first and read into the state its
   *        pages are then graded against, for the reason
   *        nabts_interpret_records() presents it first: a page invoking a macro
   *        defined there is otherwise graded as invoking an undefined one.
   */
  std::vector<NabtsCataloguedRecord> records(
      bool grammar_assisted_vote = false) const;

 private:
  /// The classification flags (§5.2.7) a record carries into the catalogue, in
  /// the order Entry's tallies are kept.
  enum ClassificationFlag : std::size_t {
    kCaption,
    kCyclicMarker,
    kPriority,
    kAlarm,
    kUpdate,
    kSupportRecord,
    kSupportNeeded,
    kIndex,
    kMore,
    kClassificationFlags
  };

  struct Entry {
    NabtsCataloguedRecord record;
    /// Whether the kept copy was complete and undamaged, which is what a later
    /// copy has to beat to replace it.
    bool kept_copy_intact = false;
    /// Monotonic counter of the last update, for the cap.
    uint64_t last_touched = 0;
    /// Damaged copies of the record data eligible to vote, oldest first. Empty
    /// once an undamaged copy has arrived, and for a record that only ever
    /// arrived misaligned.
    std::vector<NabtsRecordCopy> copies;
    /// How many appearances arrived with each classification flag set, and how
    /// many of those named the record as transmitted — the evidence the flags
    /// are voted from (see voted_flags()), indexed as ClassificationFlag.
    std::array<uint32_t, kClassificationFlags> flags_set{};
    std::array<uint32_t, kClassificationFlags> flags_set_attested{};
  };

  /// Identity of a record (§5.2.1). Ordered so iteration is channel order, then
  /// address, then version — which is how a reader would list a service.
  using Key = std::tuple<uint16_t, uint64_t, uint8_t>;

  /// Whether |message| is a better copy than the one |entry| is holding.
  static bool copy_is_better(const Entry& entry, const NabtsMessage& message);

  /// Overwrite |entry|'s kept copy from |message|.
  static void take_copy(Entry& entry, const NabtsMessage& message);

  /// Retain |message|'s data for the vote, if it can take part in one.
  void add_copy(Entry& entry, const NabtsMessage& message) const;

  /// Count |message|'s classification flags into |entry|'s tallies.
  static void tally_flags(Entry& entry, const NabtsMessage& message);

  /// Set |record|'s classification flags from |entry|'s tallies.
  ///
  /// Every flag is a bit of a Hamming 8/4 byte (§5.2.7), so a three-bit burst
  /// sets or clears one as silently as it moves an address — and a single
  /// flag is enough to move a page into the caption track or make it its
  /// channel's Support Record. Taking the flags from whichever copy was kept
  /// lets one misreading decide that for every copy; a flag is instead held
  /// set when most appearances that named the record as transmitted had it
  /// set, or, where none did, most appearances of any kind.
  static void voted_flags(const Entry& entry, NabtsCataloguedRecord& record);

  /// Move what |misread| knows into |target|, the record it was a misreading
  /// of: its appearances, its frame extent and its copies. |misread| is left
  /// gutted, and its caller erases it.
  void fold_into(Entry& target, Entry& misread) const;

  void enforce_bounds();

  std::size_t max_records_;
  std::size_t max_copies_;
  std::map<Key, Entry> records_;
  uint64_t touch_counter_ = 0;
  bool truncated_ = false;
};

}  // namespace orc

#endif  // ORC_NABTS_RECORD_CATALOGUE_H
