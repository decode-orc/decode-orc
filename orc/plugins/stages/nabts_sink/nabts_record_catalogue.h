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
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "nabts_record.h"
#include "naplps_interpreter.h"
#include "naplps_reconstruct.h"
#include "vbi-services/vbi_analysis_results.h"
#include "vbi-services/vbi_identity_attestation.h"

namespace orc {

class NabtsCaptionStream;

/**
 * @brief One copy of a record's data, with the holes it arrived with
 *
 * The two run index for index: |present| says, for each byte of |data|, whether
 * that byte arrived or merely stands in for one a lost packet carried. Holding
 * the holes open rather than closing them up is what keeps a damaged copy
 * comparable with the others (see NabtsDataGroup::present).
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
 * @brief A record's key as the files a run exports name it
 *
 * "CCC-AAA-vN" — channel, record address and version (§5.2.1), in the forms
 * NabtsCataloguedRecord::channel_text has them — with "-K" after it for the
 * K-th content carried under that identity (NabtsCataloguedRecord::content),
 * from the second on. The records viewer's own names for a saved page reduce
 * to the same, so a picture pairs with its record file.
 */
std::string nabts_record_key(const NabtsCataloguedRecord& record);

/**
 * @brief A record's identity as the records viewer lists it
 *
 * "CCC/AAA vN" — channel and record address as NabtsCataloguedRecord::
 * channel_text has them, in capitals, and the version — with " (K)" after it
 * for the K-th content carried under that identity, from the second on.
 */
std::string nabts_record_identity(const NabtsCataloguedRecord& record);

/**
 * @brief Catalogue a packet stream the sink wrote, as the sink would have
 *
 * Replays @p stream — whole 33-byte packets, as a run's .t33 holds them —
 * through the same group reassembly, record assembly, identity reconciliation
 * and vote a run does, for a recording that is known only by its packet
 * stream: the reference recordings a reconstruction draws on (see
 * NabtsReconstructionInput). The stream carries no line positions and no
 * per-byte confidence, so a continuity gap is bounded by the packets rather
 * than the lines since, and every copy weighs the same in the vote; a record
 * that lost bytes still says which (NabtsCataloguedRecord::data_present).
 *
 * Linear in the stream's length, plus the vote.
 */
std::vector<NabtsCataloguedRecord> nabts_catalogue_stream(
    const std::vector<uint8_t>& stream);

/// What rebuilding the bytes records lost needs beyond the records themselves.
struct NabtsReconstructionInput {
  /// Rebuild at all. Off reads every record as recovered.
  bool enabled = false;
  /// Other recordings' presentation records to fill from, as the sink's
  /// reference_records parameter loaded them; may be null. The recording's
  /// own records are always drawn on.
  const std::vector<NabtsReferenceRecord>* references = nullptr;
  /// What the reconstruction may do, and the thresholds its fills are held to.
  NaplpsReconstructionOptions options;
};

/**
 * @brief A page played on the keys it links, for nabts_interpret_records()
 *
 * The record to present, as an index into the records passed alongside, and
 * the digits pressed on it in order (NaplpsInterpreter::press_key()).
 */
struct NabtsKeyPlay {
  std::size_t record = 0;
  std::vector<uint8_t> keys;
};

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
 * A record that starts on the screen it was asked from, neither clearing the
 * display nor filling it, is drawn over the page a viewer came to it from as
 * well, as the Panasonic TU-2000x drew it (nabts_presentation_contexts(),
 * NabtsCataloguedRecord::presented_over).
 *
 * Separate from recovery because it depends on @p grid, the receiver being
 * emulated: X3.110 §6.2.3 sizes a DRCS character's storage buffer from the
 * physical resolution its character field covers, so interpreting during
 * recovery would pin part of the result to whatever receiver was configured
 * when the recording was read. Records whose type is not a presentation one
 * are left alone.
 *
 * @param reconstruction Whether, and from what, to rebuild the bytes each
 *               record lost before it is interpreted (see
 *               naplps_reconstruct.h). What it changes is how the record
 *               *reads*; @ref NabtsCataloguedRecord::data keeps the bytes as
 *               they were recovered, so the packet stream and the record files
 *               a run exports are unaffected. Each page's diagnostics say what
 *               it did.
 * @param receiver_pace Play each page at the pace the receiver it names drew
 *               it (NaplpsInterpreter::set_receiver_pace()). What is drawn is
 *               the same either way; only when each figure appears moves.
 * @param play Where given, present only the record it names, exactly as above,
 *               and then press its keys in turn: that record's page is the
 *               display after the last of them, timed from the last key. The
 *               other records are left as they were.
 *
 * Each presentation record's NabtsCataloguedRecord::linked_keys is set from
 * what its own presentation defined (NaplpsInterpreter::linked_keys()).
 */
void nabts_interpret_records(
    std::vector<NabtsCataloguedRecord>& records, NaplpsRenderGrid grid,
    const NabtsReconstructionInput& reconstruction = {},
    NaplpsReceiverPace receiver_pace = NaplpsReceiverPace::kOff,
    const NabtsKeyPlay* play = nullptr);

// ---------------------------------------------------------------------------
// The vote
// ---------------------------------------------------------------------------

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
 * @brief Settled positions either side of a byte few copies hold that show
 *        whether the copy holding it closed up over a block it lost
 *
 * Where fewer than half the copies reaching a position hold a byte — the
 * stretch most of them lost — a copy does not vote there if, at the nearest
 * this many positions on one side that most copies hold, it agrees with the
 * vote at fewer than half and with the vote one to three packets' blocks
 * further on at half or more (a side with fewer than half as many judged says
 * nothing). A copy of the record agrees nearly everywhere it is lined up, and
 * by coincidence, a few percent, where it is not. See nabts_vote_record().
 */
constexpr std::size_t kNabtsAlignmentWitnesses = 8;

/**
 * @brief How much more often another version of an address must have been
 *        seen for a version seen only inside its span to be a misreading of it
 *
 * §5.2.7.2 has a revision replace the version before it, so a version that
 * came and went while another went on being sent, and carries its bytes
 * (kNabtsMisreadVersionAgreement), is that version with its version nibble
 * misread — however cleanly its header arrived: two damaged sightings of a
 * version inside the span of one seen hundreds of times. Versions a carousel
 * sends in turn are
 * seen about equally often, and a revision is not seen inside its
 * predecessor's span. See reconcile_identities().
 */
constexpr uint64_t kNabtsMisreadVersionRatio = 20;

/// Fewest appearances the version a misreading is folded into must have, so a
/// carousel of rare pages is never read as one page and its misreadings.
constexpr uint64_t kNabtsMisreadVersionMinMain = 20;

/**
 * @brief How much more often a page of the service's data channel must have
 *        been seen than a record of the same address and version on another
 *        channel for that record to be the page's packets misaddressed
 *
 * Every digit of the packet address is a Hamming 8/4 byte (CEA-516 §3.2.1),
 * and four wrong bits carry one codeword exactly onto another — P3 0x15 onto
 * 0xFD — so a misaddressed packet can arrive with its address as transmitted.
 * Where the packet is a page's synchronizing packet, it opens a group on a
 * data channel the service never used, under the page's own record header,
 * and the group then takes in whatever else is misaddressed there until its
 * block count is reached: blocks of other pages, or of the page, each at the
 * wrong offset, each verbatim from another page. So a record on a channel other
 * than the one most of the recording's attested identities are on, none of
 * whose copies arrived intact, whose address and version a record of that
 * channel carries seen at least this many times as often, is removed
 * (reconcile_identities()). Such records are seen once to three times, against
 * tens or hundreds of sightings of their namesakes. A service's records on a
 * second channel of its own, such as caption records, have no namesake and
 * stay.
 */
constexpr uint64_t kNabtsMisaddressedSeenRatio = 4;

/// Share of the bytes both versions received cleanly that must agree, at the
/// best slip (vote_agreement()), for the rare version to be a misreading.
constexpr double kNabtsMisreadVersionAgreement = 0.9;

/**
 * @brief How much of a content already held under an identity a copy has to
 *        agree with to be another copy of it
 *
 * A percentage of the positions both hold a byte for that passes its parity,
 * at the slip (up to kNabtsMaxRecordSlip) that agrees best. One identity can
 * carry more than one content over a recording (see
 * NabtsCataloguedRecord::content): a carousel's sixteen versions (§5.2.7.2
 * Y16) come round again with new pages, and a page is replaced without its
 * version moving. A copy that agrees with none of the contents held starts a
 * content of its own, catalogued as a record of its own; voted into another,
 * the two would come out as a per-byte blend of both. Across the recordings,
 * damaged copies of one content agree on 99.8 % of such positions at the
 * median and fall below half in fewer than one case in a hundred, while two
 * different contents under one identity agree on 6 % at the median — though
 * a page re-sent with a few scores changed agrees with the one before it on
 * nearly everything, and is the same page for this purpose. Lower, the
 * threshold lets pages of one carousel through as one: at 30 % seven of the
 * contents confirmed by an intact copy or another recording of the same day
 * are lost; higher, it splits damaged copies of one page apart. Two pages
 * built on one template agree on it as copies of one page do, and are told
 * apart by what each carries that the other does not
 * (kNabtsPageDifferenceBytes).
 *
 * A copy is judged against every copy a content holds for its vote as well as
 * the one it keeps (NabtsRecordCatalogue::content_of()). The kept copy is the
 * longest rather than the best: it can hold a few dozen of its record's
 * bytes, and a copy judged against it alone is judged on a handful of
 * positions — or against a copy whose damage happens to fall
 * where the new one's does not.
 *
 * Length settles it only between copies that arrived intact (§3.4's check
 * passed on every block of every group): those are known to be as long as
 * their record, so two of them more than a misread header's slip
 * (kNabtsMaxRecordSlip) apart are different contents whatever they agree on —
 * two editions of a magazine page can agree on half of their bytes and differ
 * in length by a hundred. A complete copy is not known to
 * be as long: a group whose last packets were lost ends at the next
 * synchronizing packet on its channel, and a misread group header (§4.2.5)
 * declares a block count a block or more out. Judging complete copies by
 * length opens hundreds of times as many contents as judging them by what
 * they carry, and nine in ten of those would be the same page as another.
 */
constexpr unsigned kNabtsSameContentPercent = 50;

/**
 * @brief The windows a copy is compared in when no one slip lines it up
 *
 * A copy whose packet was lost without the continuity index showing it
 * (§3.2.4 wraps at 16), or whose bytes were read at another block width, has
 * every byte after the loss moved by a block or more — further than a misread
 * header moves a whole copy (kNabtsMaxRecordSlip). So a copy that agrees with a
 * content at no single slip is compared again window by window: each
 * kNabtsContentWindowBytes of the content at its own best slip within
 * kNabtsContentWindowSlip, a window counting only where it judged at least
 * kNabtsContentWindowMinJudged positions, and the copy is the content's where
 * kNabtsContentWindowPercent of the positions judged that way agree. Two
 * blocks a window and a block either way find a copy one packet out of step:
 * on the damaged tapes such copies agree 0.89-0.91, while the pages of one
 * carousel that are known to differ — intact copies of both — agree 0.59 at
 * most. Per window, the best of 65 slips flatters two unrelated pages a little,
 * which is why the share asked is higher than kNabtsSameContentPercent.
 *
 * The windows are laid over the held content and, failing that, over the
 * copy: a copy that kept few of its bytes — one given a hole for two packets
 * it never lost, and the rest of its group marked absent, keeping a hundred
 * bytes or so — lends its
 * few bytes to several of the content's windows at slips that do not line
 * them up, where laid over the copy each of them is read once.
 */
constexpr std::size_t kNabtsContentWindowBytes = 54;
constexpr std::ptrdiff_t kNabtsContentWindowSlip = 32;
constexpr std::size_t kNabtsContentWindowMinJudged = 8;
constexpr unsigned kNabtsContentWindowPercent = 70;

/**
 * @brief When two contents that agree are two pages built on one layout
 *
 * A service builds many pages on one template, and two of them agree on every
 * byte of it, which can be most of the record: two ratings pages, or two
 * promotions on one layout, can agree on three quarters of the bytes both
 * arrived with or more. The agreement
 * tests (kNabtsSameContentPercent, kNabtsContentWindowPercent) and the
 * folding of identities the recording never named take such a pair for one
 * page, and the vote comes out a per-byte blend of both, rows of one page
 * among rows of the other.
 *
 * What tells two pages apart is what each carries that the other does not —
 * other text, another picture — and damage does not do that. A garbled
 * stretch fails parity in every other byte; the commonest misreading, two
 * neighbouring bits of one byte, passes parity but leaves single bytes wrong;
 * a lost or misplaced block moves bytes without changing them. So two copies,
 * or two votes, are two pages where each of them, read against the other at
 * the slip they agree best at, carries
 *
 *  - kNabtsPageDifferenceBytes bytes or more in runs of kNabtsPageDifferenceRun
 *    or more where both bytes pass their parity and differ, with no more than
 *    kNabtsPageDifferenceQuietFailures bytes failing parity within
 *    kNabtsPageDifferenceQuietReach of a run in either; or
 *  - kNabtsPageDifferenceVouchedBytes bytes or more, and
 *    kNabtsPageDifferenceVouchedPercent of the positions where both bytes were
 *    vouched for by the suffix check (CEA-516 §3.4), where the two vouched
 *    bytes differ — the check passes two misread bits down one column, and
 *    the MLSE detector's reading of a block again under its check can pass it
 *    with a byte still wrong, so a few vouched bytes differ between copies of
 *    one page, but a few in a hundred, not one in four;
 *
 * and only where no displacement explains a byte: the copy's bytes around it,
 * kNabtsDisplacementContext either side, are found nowhere in the other within
 * kNabtsDisplacementReach of it. Each way round, because a copy cut short or
 * moved carries nothing its page does not, however much of the page it lacks.
 *
 * Measured against the intact copy of their own record, a damaged copy is
 * almost never taken for another page; of pairs of different pages sent at
 * one address — mostly a page and its next version — that both arrived
 * intact and that the agreement tests alone take for one page, nine in ten
 * are told apart, and the rest differ by a few dozen bytes at most, or by a
 * block moved: the same page revised.
 */
constexpr std::size_t kNabtsPageDifferenceBytes = 32;
constexpr std::size_t kNabtsPageDifferenceRun = 4;
constexpr std::size_t kNabtsPageDifferenceQuietReach = 14;
constexpr std::size_t kNabtsPageDifferenceQuietFailures = 2;
constexpr std::size_t kNabtsPageDifferenceVouchedBytes = 6;
constexpr unsigned kNabtsPageDifferenceVouchedPercent = 8;
constexpr std::ptrdiff_t kNabtsDisplacementContext = 3;
constexpr std::ptrdiff_t kNabtsDisplacementReach = 96;

/**
 * @brief The window slip contents of one identity are compared at when they
 *        are joined
 *
 * A copy whose group was given holes for blocks it never lost — a continuity
 * index misread where lines went by, so the gap it showed was believed — has
 * every byte after them two or three blocks late, further than
 * kNabtsContentWindowSlip looks, and so opens a content of its own. Once the
 * last copy is in, the contents of an identity are therefore compared with
 * their windows allowed this slip: three blocks. Single sightings of a page
 * shifted so agree with their page nine in ten or more that way, while two
 * different pages of one carousel agree on about a third. At this slip two
 * pages built on one template agree as well, nine in ten, which is what
 * kNabtsPageDifferenceBytes keeps apart, so every pair of intact contents
 * known to differ stays apart.
 */
constexpr std::ptrdiff_t kNabtsJoinWindowSlip = 81;

/**
 * @brief Positions two records must both hold a parity-passing byte at before
 *        their agreement says whether they are one page
 *
 * NabtsRecordCatalogue::reconcile_identities() compares the votes of records
 * the recording never named as transmitted with those it did, over the bytes
 * both received, and a record kept under a name of its own needs this many
 * such bytes of its own. A few dozen positions agree by chance between pages
 * built from one template; a page whose copies lost most of their bytes has
 * too little left to say whose it is.
 */
constexpr std::size_t kNabtsIdentityMinJudged = 48;

/**
 * @brief How much of the record a copy must agree with before the suffix check
 *        of its blocks is believed over the other copies
 *
 * A percentage of the positions both it and the vote parity and weight settle
 * have a byte for. The check of CEA-516 §3.4 vouches for a block as it was
 * received — not for where the record layer put its bytes, nor for which
 * record it is a copy of. A copy slipped by a misread header, one assembled
 * here by a mis-corrected packet address, and one of an earlier version of the
 * record are all vouched for as readily as a copy of the record itself, and
 * all disagree with much of it; a copy of the record disagrees only where it
 * was damaged, a few bytes in a hundred of a recovered recording. Below this
 * share a copy's vouched bytes vote as bytes that merely arrived, which is the
 * vote parity alone would hold.
 */
constexpr unsigned kNabtsVouchAgreementPercent = 90;

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

/// One record's data as the vote settled it, with which of its bytes any copy
/// delivered — see @ref NabtsCataloguedRecord::data_present for what the mask
/// means and why it is worth keeping. Each entry is kNabtsByteLost where no
/// copy delivered the byte, kNabtsByteVouched where the value the vote settled
/// on is one the suffix check vouched for in a copy that agrees with nearly
/// all of the record (kNabtsVouchAgreementPercent), and kNabtsByteArrived
/// otherwise; a vote of one copy is that copy's own mask.
struct NabtsVoteResult {
  std::vector<uint8_t> data;
  std::vector<uint8_t> present;
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
 * Above both, a byte whose block the suffix check vouched for — the copy's
 * mask holds kNabtsByteVouched there — wins outright over every byte of a
 * block it did not. Parity sees an odd number of wrong bits in a byte and
 * nothing else, and the commonest damage a recording does is two neighbouring
 * bits of one byte, which parity passes; nor is that damage random, the same
 * bits being misread the same way copy after copy, so agreement among copies
 * the check failed is no evidence against one it passed. The check vouches
 * for a block as received, though, not for the record it was filed under, so
 * only a copy that agrees with nearly all of what the copies mostly say
 * (kNabtsVouchAgreementPercent) is believed this way. Where the check passed
 * more than one value, the weight of the vouched copies alone decides.
 *
 * Within a class, each copy contributes how sure the detector was of its byte
 * rather than one vote apiece, so a value read cleanly outweighs the same
 * number of copies of one the detector nearly decided the other way. A copy
 * nothing measured contributes full weight, which is the reading the row
 * squasher gives an unmeasured copy and the only honest one for a detector with
 * no way of saying it is unsure. Ties go to the most recent copy, which is what
 * keeping a single copy would have shown.
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
 * A copy whose group closed up over a block it lost without a hole agrees as
 * far as that block and holds the next one after it, and where every other
 * copy lost the block it would be the only voter there: where few copies hold
 * a byte, a copy that holds the vote's bytes a block late beside it abstains
 * (kNabtsAlignmentWitnesses).
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
 * bytes it has. Its mask keeps what the suffix check vouched for (see
 * NabtsVoteResult), so that what reads the record afterwards — the test for
 * two pages on one layout (kNabtsPageDifferenceBytes) among them — can tell a
 * byte the check vouched for from one that merely arrived.
 *
 * Linear in the record length and the number of copies.
 */
NabtsVoteResult nabts_vote_record(const std::vector<NabtsRecordCopy>& copies);

/// Set |record|'s identity — channel, address, version and reserved purpose —
/// as |message| names it, first seen at |frame_id|.
void nabts_take_identity(NabtsCataloguedRecord& record,
                         const NabtsMessage& message, uint64_t frame_id);

/// Whether |message| is a better copy to show than the one kept, which arrived
/// complete and undamaged where |kept_intact| and is |kept_bytes| long. An
/// undamaged copy always beats a damaged one, and never the other way round;
/// between copies of one quality the longer carries more of the record, and
/// equal length keeps what is there rather than churning between copies.
bool nabts_copy_is_better(const NabtsMessage& message, bool kept_intact,
                          std::size_t kept_bytes);

/// Offer |message| to |copies|, the copies of a record held for the vote
/// (nabts_vote_record()), oldest first and at most |max_copies| of them.
/// |kept_intact| is whether the copy kept to show arrived undamaged, which
/// leaves a vote nothing to improve on: an undamaged copy clears the copies
/// held, and none is taken once one has arrived. A copy that is not aligned
/// (NabtsMessage::aligned) is not taken either.
void nabts_offer_copy(std::vector<NabtsRecordCopy>& copies,
                      const NabtsMessage& message, bool kept_intact,
                      std::size_t max_copies);

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
 * whichever copy happened to be longest. See nabts_vote_record() for the
 * vote, and NabtsMessage::aligned for which copies are eligible to take part.
 * The copies are discarded the moment an undamaged one arrives: there is then
 * nothing left for a vote to improve on.
 *
 * The caption service is the exception. §7.3.10.1 sends each new caption as a
 * new version of one address, so over a recording of any length the sixteen
 * versions Y16 allows are used over and over for captions that share nothing
 * but the number, and {channel, address, version} names a caption only while
 * it is on air. A message of the caption service
 * (NabtsCaptionStream::accepts()) therefore never enters the catalogue: it goes
 * to a caption stream, which keeps each caption as the run of arrivals it came
 * in and votes only within the run (see captions()).
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
   * least recently seen record no appearance named as transmitted is dropped —
   * one that was named only once no other is left — and the dataset flagged
   * truncated. A damaged tape fills the catalogue with misread identities long
   * before it holds a thousand pages: hundreds of entries for under a hundred
   * records it lists.
   */
  static constexpr std::size_t kMaxCataloguedRecords = 1024;

  /**
   * Copies of a record's data retained for the vote.
   *
   * A recording of any length would otherwise hold every copy of every record
   * it carried. The most recent copies are the ones kept, and on the damaged
   * tapes more of them still help: judged by other recordings of the same
   * pages, going from 32 to 64 puts many times more bytes right than wrong
   * and leaves fewer lost, as going from 16 to 32 did. Record data is capped at
   * 1904 bytes by §8.4.2.5, so the two bounds together put the worst case at
   * 125 MB of data, and about three times that with each byte's mask and
   * confidence — reached only by a capture that fills the catalogue with
   * full-length records none of which ever arrives undamaged. The worst
   * recording measured holds its record chain at 37 MB at its peak against
   * 27 MB at 32 copies. A record that
   * does arrive undamaged holds no copies at all.
   */
  static constexpr std::size_t kMaxCopiesPerRecord = 64;

  explicit NabtsRecordCatalogue(std::size_t max_records = kMaxCataloguedRecords,
                                std::size_t max_copies = kMaxCopiesPerRecord);
  ~NabtsRecordCatalogue();
  NabtsRecordCatalogue(NabtsRecordCatalogue&& other) noexcept;
  NabtsRecordCatalogue& operator=(NabtsRecordCatalogue&& other) noexcept;
  NabtsRecordCatalogue(const NabtsRecordCatalogue&) = delete;
  NabtsRecordCatalogue& operator=(const NabtsRecordCatalogue&) = delete;

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
   *
   * A message of the caption service goes to the caption stream instead (see
   * the class comment and captions()).
   *
   * A copy whose record type is one §5.2.2 reserves (4 to 15) names its
   * identity as transmitted no more than a corrected one does, whatever its
   * Hamming bytes say: no service sent a reserved type, and both decoders of
   * the day refuse a header carrying one, so the type byte was misread — a
   * damaged tape can read one digit as another in every Hamming byte, and
   * deliver hundreds of copies of a reserved type. Such a copy
   * still votes as a copy of its record.
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
   *  - where its content matches an attested (or kept) identity's, it is a
   *    misreading of that one. Its copies join that record's vote — which is
   *    the point of folding rather than discarding: the bytes were received
   *    and they are that record's — and its appearances and frame extent are
   *    added to it. Contents are compared by their votes, over the positions
   *    both received with a byte passing its parity (at least
   *    kNabtsIdentityMinJudged of them), at the slip that agrees best: a copy
   *    of a damaged tape is mostly holes, and compared byte for byte over the
   *    longer of two copies, as it was, two pages' holes agreed with each
   *    other and one page's damaged copies matched nothing: a page's copies
   *    under a misread channel were kept as a page of their own that way, and
   *    identities misread onto one channel were folded into one another and
   *    voted into one record.
   *  - where it recurs with content no other identity carries, it is a page
   *    whose name is damaged the same way every time, and it is kept — if
   *    two of its copies are copies of each other (at least half of the
   *    kNabtsIdentityMinJudged or more positions both received), or one
   *    arrived intact. Copies of several pages misread onto one name are not
   *    kept that way: each of them agrees with the vote they make up, but not
   *    with one another. A record whose voted type is one §5.2.2 reserves is
   *    never kept (see merge()).
   *  - a single sighting folds into its one attested neighbour a hexadecimal
   *    digit away, and is otherwise removed: several neighbours make the
   *    misreading ambiguous, and none at all is a false lock on noise.
   *
   * Two pages built on one template agree on it as a page's misread copies
   * do, so neither fold is made where the two are different pages
   * (kNabtsPageDifferenceBytes): a fold by content needs the content to be
   * the page's, and a single sighting that is another page is removed rather
   * than voted into its neighbour — unless its record header said otherwise.
   * An identity whose record header arrived as transmitted on some
   * appearance — address and version every one a codeword, only the packet
   * address corrected — on the data channel most of the recording's attested
   * identities are on is a page the service sent under that version, and is
   * kept as one where it would be removed, rather than folded into the page
   * whose template it shares and voted into it byte by byte.
   * An identity one of whose contents was named as transmitted is the
   * service's already, and another content of it no header vouches for.
   *
   * A record on a data channel other than that one, never received intact,
   * whose address and version a page of that channel carries, seen far more
   * often (kNabtsMisaddressedSeenRatio), is that page's synchronizing packet
   * misaddressed and the other packets misaddressed after it — no page,
   * however its identity arrived — and is removed.
   *
   * Before any of that, the contents held under each identity are settled:
   * one that the copies held for it show to be the page of an earlier content
   * of the identity (kNabtsSameContentPercent, judged on the two votes) is
   * joined to it. A content opened by a copy damaged where the first copies
   * were not can be told for what it is only once more copies are in. Then a
   * version seen only inside the span of another version of its address, far
   * less often, and carrying its bytes, is folded into it however its header
   * arrived (kNabtsMisreadVersionRatio): §5.2.7.2 has a revision replace its
   * predecessor, not come and go inside it.
   *
   * vbi_reconcile_identities() holds the rule, which the teletext sink shares.
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
   */
  std::vector<NabtsCataloguedRecord> records() const;

  /**
   * @brief The caption service (CEA-516 §7.3.10), one entry per caption
   *
   * In transmission order, each caption voted from its own copies only and
   * dated by the frame its first copy arrived in. Kept apart from records():
   * see NabtsCaptionStream.
   */
  std::vector<NabtsCataloguedRecord> captions() const;

  /// Captions that arrived after NabtsCaptionStream::kMaxCaptions were held.
  uint64_t captions_dropped() const;

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
    kAutoAcquire,
    kClassificationFlags
  };

  /// A classification sequence's pointer-byte count and the reserved flag
  /// bytes it set, as NabtsCataloguedRecord lists them.
  using ClassificationForm = std::pair<uint8_t, std::vector<std::string>>;

  /// |flags|'s form, as tallied into Entry::forms.
  static ClassificationForm form_of(const NabtsClassification& flags);

  struct Entry {
    /// The record as it will be listed, all but its data: the kept copy's
    /// bytes are |representative|'s, and records() puts them in.
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
    /// The kept copy's bytes and which of them arrived: what a new copy under
    /// the same identity is judged against, with the held copies, to say
    /// whether it carries this content (content_of()), and the data listed
    /// where no copies are held for a vote.
    NabtsRecordCopy representative;
    /// How many appearances arrived with each classification flag set, and how
    /// many of those named the record as transmitted — the evidence the flags
    /// are voted from (see voted_flags()), indexed as ClassificationFlag.
    std::array<uint32_t, kClassificationFlags> flags_set{};
    std::array<uint32_t, kClassificationFlags> flags_set_attested{};
    /// The same for the rest of the classification sequence's form — how many
    /// pointer bytes it had and which reserved flag bytes it set (see
    /// voted_flags()).
    std::map<ClassificationForm, uint32_t> forms;
    std::map<ClassificationForm, uint32_t> forms_attested;
    /// The same for the record type (§5.2.2), indexed by type.
    std::array<uint32_t, 16> types{};
    std::array<uint32_t, 16> types_attested{};
    /// Appearances whose record header named the address and version as
    /// transmitted though their packet address was corrected
    /// (NabtsMessage::header_attested): what keeps a page sent once under a
    /// version of its own when it is no other page (reconcile_identities()).
    uint32_t times_header_attested = 0;
  };

  /// Identity of a record (§5.2.1) and which of the contents carried under it
  /// (NabtsCataloguedRecord::content). Ordered so iteration is channel order,
  /// then address, then version, then content in the order the contents first
  /// arrived — which is how a reader would list a service.
  using Key = std::tuple<uint16_t, uint64_t, uint8_t, uint32_t>;

  /// Which content |message| is a copy of, of those held under its identity:
  /// the one it agrees with best, or the next unused number where it agrees
  /// with none (kNabtsSameContentPercent).
  uint32_t content_of(const NabtsMessage& message) const;

  /// What |entry|'s copies say the record is: the vote over the copies held,
  /// or the kept copy where none is.
  static NabtsRecordCopy view_of(const Entry& entry);

  /// Join each content of an identity to the earlier content of it that the
  /// two votes show it to be (kNabtsSameContentPercent), and number what is
  /// left from zero in the order it first arrived.
  void join_contents();

  /// Move |source|, a content found to be |target|'s, into |target|: its
  /// appearances, its tallies and frame extent, and its copies or, where only
  /// it arrived intact, its kept copy.
  void absorb(Entry& target, Entry& source) const;

  /// Whether |entry|, never named as transmitted, can be kept as a page of its
  /// own: its vote |view| holds kNabtsIdentityMinJudged parity-passing bytes,
  /// two of its copies are copies of each other (or one arrived intact), and
  /// its voted type is not a reserved one.
  static bool keepable(const Entry& entry, const NabtsRecordCopy& view);

  /// Whether |entry|, never named as transmitted and no other page, is a page
  /// sent under a version of its own: one of its appearances' record header
  /// named the address and version as transmitted, its voted type is not a
  /// reserved one and its vote |view| holds kNabtsIdentityMinJudged
  /// parity-passing bytes (see reconcile_identities()).
  static bool kept_by_its_header(const Entry& entry,
                                 const NabtsRecordCopy& view);

  /// The record type most of |entry|'s appearances carried — of those that
  /// named the record as transmitted, where any did — the kept copy's where
  /// another is carried as often.
  static uint8_t voted_type(const Entry& entry);

  /// Overwrite |entry|'s kept copy from |message|.
  static void take_copy(Entry& entry, const NabtsMessage& message);

  /// Count |message|'s classification flags and record type into |entry|'s
  /// tallies, as an appearance that named the record as transmitted where
  /// |attested|.
  static void tally_flags(Entry& entry, const NabtsMessage& message,
                          bool attested);

  /// Set |record|'s classification flags from |entry|'s tallies.
  ///
  /// Every flag is a bit of a Hamming 8/4 byte (§5.2.7), so a three-bit burst
  /// sets or clears one as silently as it moves an address — and a single
  /// flag is enough to move a page into the caption track or make it its
  /// channel's Support Record. Taking the flags from whichever copy was kept
  /// lets one misreading decide that for every copy; a flag is instead held
  /// set when most appearances that named the record as transmitted had it
  /// set, or, where none did, most appearances of any kind. The pointer-byte
  /// count and the reserved flag bytes are read the same way, as the form
  /// the most of those appearances carried, and so is the record type
  /// (voted_type()): taken from the kept copy, 32 records across the
  /// recordings were listed with a type §5.2.2 reserves, most of them pages
  /// of type 0 whose longest copy had its type byte misread.
  static void voted_flags(const Entry& entry, NabtsCataloguedRecord& record);

  /// Move what |misread| knows into |target|, the record it was a misreading
  /// of: its appearances, its frame extent and its copies. |misread| is left
  /// gutted, and its caller erases it.
  void fold_into(Entry& target, Entry& misread) const;

  /// Add |source|'s appearances, its tallies of flags, forms and types, and
  /// its frame extent to |target|'s. The tallies of attested appearances are
  /// left to the caller.
  static void add_tallies(Entry& target, const Entry& source);

  /// Fold each version of an address that was seen only inside the span of
  /// another version of it, at most 1 / kNabtsMisreadVersionRatio as often,
  /// and whose bytes are that version's (kNabtsMisreadVersionAgreement), into
  /// that version (see reconcile_identities()).
  void fold_misread_versions();

  void enforce_bounds();

  std::size_t max_records_;
  std::size_t max_copies_;
  std::map<Key, Entry> records_;
  /// The caption service, which never enters |records_|.
  std::unique_ptr<NabtsCaptionStream> captions_;
  uint64_t touch_counter_ = 0;
  bool truncated_ = false;
};

}  // namespace orc

#endif  // ORC_NABTS_RECORD_CATALOGUE_H
