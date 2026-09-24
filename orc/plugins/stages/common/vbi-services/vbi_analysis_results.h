/*
 * File:        vbi_analysis_results.h
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     Plugin-private catalogue types for the teletext and NABTS sink
 *              stages
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_SERVICES_VBI_ANALYSIS_RESULTS_H
#define ORC_VBI_SERVICES_VBI_ANALYSIS_RESULTS_H

// Shared plugin-side library, NOT part of the SDK contract, and no longer
// crossing the plugin boundary at all: the two sink stages hold these types
// privately and publish what the host reads through orc::ICatalogueResults
// (<orc/stage/tooling/catalogue_results.h>), which is SDK. Nothing here has a
// cross-DSO dynamic_cast against it, so nothing here depends on the runtime's
// typeinfo-name fallback, and changing any of it costs a rebuild of the two
// plugins alone — never an ABI bump for unrelated plugins, which is what these
// types cost while they sat in <orc/stage/analysis_sink_results.h>.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nabts_page.h"
#include "teletext_page_decoder.h"
#include "vbi_identity_attestation.h"

namespace orc {

/**
 * @brief One sub-page of a catalogued teletext page
 *
 * A page number can carry a sequence of sub-pages that the service cycles
 * through — the multi-page set of ETSI EN 300 706 Annex A.1, what a receiver
 * presents as a rotating page. Each is a page in its own right, told from its
 * siblings by the page sub-code (§9.3.1.2), and each is catalogued separately
 * so a reader can step through the sequence instead of seeing only whichever
 * one the carousel happened to leave behind.
 */
struct TeletextCataloguedSubPage {
  /// 13-bit page sub-code S1-S4 as transmitted (ETSI EN 300 706 §9.3.1.2),
  /// packed S1 in bits 0-3, S2 in 4-6, S3 in 7-10, S4 in 11-12
  int subcode = 0;

  /// Frames carrying the first and the most recent header packet of this
  /// sub-page
  uint64_t first_seen_frame = 0;
  uint64_t last_seen_frame = 0;

  /// Appearances of this sub-page counted over the analysed range. A header
  /// re-sent part-way through the sub-page's own transmission is the same
  /// appearance, not another one.
  uint64_t times_seen = 0;

  /// Appearances that named the page as transmitted, of @ref times_seen: the
  /// two MRAG bytes and the two page-number bytes of the opening header all
  /// arrived as Hamming 8/4 codewords rather than being corrected into them
  /// (§7.1.2, §9.3.1.1; see TeletextPageSnapshot::identity_attested).
  ///
  /// Zero here on a page the carousel brought round many times is the signature
  /// of a mis-corrected page number rather than of a real page — see
  /// TeletextPageCatalogue::reconcile_identities(), which is what acts on it.
  uint64_t times_attested = 0;

  /// Packet slots this sub-page's own transmissions lost, estimated the same
  /// way as TeletextRecoverySummary::lost_packets_estimate but scoped to the
  /// fields this sub-page occupied: a service part-way through a page fills
  /// every VBI line it inserts on, in every field it uses, so a field of this
  /// sub-page's extent that came back short is short by packets the recording
  /// lost. Which row each would have carried is not knowable.
  ///
  /// Zero is the answer for a sub-page whose transmissions all came back full,
  /// and it is what makes a missing row in @ref page readable: without a loss
  /// to blame, a row the assembly never received is one the service chose not
  /// to send, which most pages do to space themselves out.
  uint64_t lost_packets = 0;

  /// Best assembly of the sub-page, built from every row copy recovered over
  /// the analysed range rather than from one transmission
  TeletextPageSnapshot page;
};

/**
 * @brief One teletext page the analysed range carried
 *
 * The page is catalogued rather than kept per transmission: a carousel brings
 * the same page round hundreds of times in a recording, and what a reader wants
 * is one best assembly of it plus how often and where it was seen.
 *
 * Where the page is a multi-page set, that assembly is per sub-page: see
 * @ref subpages, which always holds at least one entry.
 */
struct TeletextCataloguedPage {
  int magazine = 8;     ///< Displayed magazine number 1-8
  int page_number = 0;  ///< Two-digit hexadecimal page number 0x00-0xFF

  /// Frames carrying the first and the most recent header packet of the page,
  /// over all of its sub-pages
  uint64_t first_seen_frame = 0;
  uint64_t last_seen_frame = 0;

  /// Appearances counted over the analysed range — how often the carousel
  /// brought the page round, and so a rough measure of how reliably it can be
  /// recovered. A header re-sent part-way through the page's own transmission
  /// is the same appearance, not another one. Summed over the sub-pages, so on
  /// a multi-page set this counts the transmissions rather than the cycles.
  uint64_t times_seen = 0;

  /// Appearances that named the page as transmitted, summed over the
  /// sub-pages (see TeletextCataloguedSubPage::times_attested).
  uint64_t times_attested = 0;

  /// The page has been transmitted with C6 (subtitle, ETSI EN 300 706 §9.3.1.3
  /// Table 2) set at least once. Sticky: a service may drop C6 between
  /// captions, and the page is still the subtitle page in between.
  bool subtitle = false;

  /// The page's sub-pages, ascending by sub-page number, never empty. A page
  /// that is not a multi-page set has exactly one entry — sub-code 0000 in the
  /// coding of Annex A.1 — so a consumer need not special-case either.
  std::vector<TeletextCataloguedSubPage> subpages;
};

/**
 * @brief How the recovery went over the analysed range
 *
 * Aggregate counts only; the per-line and per-page detail lives in the stage's
 * own report.
 */
struct TeletextRecoverySummary {
  uint64_t frames_analysed = 0;
  uint64_t fields_with_data = 0;
  uint64_t packets_recovered = 0;
  /// Row packets whose bytes were changed by combining repeated copies
  uint64_t packets_corrected = 0;
  /// Display bytes whose odd parity was restored by the detector's repair
  uint64_t bytes_repaired = 0;
  /// Display characters written, and how many of those are known damaged
  /// because they fail the odd parity of ETSI EN 300 706 §8.1. A floor rather
  /// than an exact count — a byte damaged in two bits passes parity.
  uint64_t characters_written = 0;
  uint64_t characters_damaged = 0;
  /// Packet slots that came back empty during a page transmission: a service
  /// part-way through sending a page fills every line it is using in every
  /// field, so an empty slot is a packet the recording lost. An estimate, and
  /// silent about pages that never started arriving at all.
  uint64_t lost_packets_estimate = 0;
  /// True when the sub-page cap was reached and the least recently seen ones
  /// were dropped, so the catalogue is not the whole set the range carried.
  bool pages_truncated = false;
  /// What separating the page numbers the service transmitted from the ones
  /// Hamming 8/4 mis-correction invented came to (see
  /// TeletextPageCatalogue::reconcile_identities()). All zero on an undamaged
  /// recording, where every page number arrives as transmitted.
  VbiIdentityReconciliation identity_reconciliation;

  /// G0 character set the pages were read in — the alphabet, not the data.
  ///
  /// Reported because it is the one thing about a rendered page that cannot be
  /// checked by looking at it: a Cyrillic service read as Latin produces
  /// pronounceable nonsense rather than anything obviously wrong, and half the
  /// Cyrillic capitals are drawn identically to their Latin counterparts, so
  /// even a correctly read page can look untouched. Saying which set was used
  /// turns "is this right?" into something the reader can answer.
  TeletextG0Set character_set = TeletextG0Set::Latin;

  /// The second G0 set the ESC control character switched into, where the run
  /// had one (ETSI EN 300 706 §15.3). Reported for the same reason as the set
  /// above and rather more urgently: a page holding two alphabets looks like a
  /// page holding one that has gone wrong in places, so a reader who is not
  /// told the run was reading two has no way to arrive at that reading.
  std::optional<TeletextG0Designation> second_character_set;
};

/// Everything the teletext sink caches from one trigger run.
struct TeletextAnalysisDataset {
  /// Ascending by {magazine, page number}
  std::vector<TeletextCataloguedPage> pages;
  TeletextRecoverySummary summary;
};

/**
 * @brief One application function descriptor, ready to show (CEA-516 §7.2.2)
 *
 * A NABTS application record is a sequence of function descriptors rather than
 * anything displayable, and what a reader wants of one is a listing. Both
 * fields are text because both are read rather than acted on: the host has no
 * business executing a broadcaster's control functions, and rendering them is
 * all this is for.
 */
struct NabtsRecordFunction {
  /// Function code in the "2/0" column/row notation the standard uses.
  std::string code;
  /// Whether the code is control data (column 2) rather than information
  /// (column 3).
  bool control = false;
  /// Arguments as printable text, with anything unprintable shown as its
  /// hexadecimal value. Empty for a descriptor with no arguments, which
  /// §7.2.3.1 makes a request to restore that function's initial state.
  std::string arguments;
};

/**
 * @brief One teletext record the analysed range carried
 *
 * Catalogued rather than kept per transmission, for the reason the teletext
 * page catalogue is: a cyclic service (CEA-516 §7.1.2) brings the same record
 * round throughout a recording, and what a reader wants is one best copy of it
 * plus how often and where it was seen.
 *
 * A record here is a *message* in the standard's terms (§5.2.6) — one unlinked
 * record, or a linked series joined — because that is the unit a receiver
 * presents.
 */
struct NabtsCataloguedRecord {
  /// Data channel, i.e. the packet address of §3.2.3.
  uint16_t channel = 0;
  /// Record address in the nine-digit long form §5.2.5 makes equivalent to the
  /// short one, so records that name the same address either way compare equal.
  uint64_t address = 0;
  /// The address as transmitted: three hexadecimal digits, or nine when the
  /// record carried an address extension.
  std::string address_text;
  /// Channel and address together, as a reader would cite them.
  std::string channel_text;

  /// RT (§5.2.2): 0 cyclic presentation, 1 non-cyclic presentation,
  /// 2 application, 3 priority presentation; 4-15 reserved.
  uint8_t record_type = 0;
  /// Version number from classification flag byte Y16 (§5.2.7.2), 0 when the
  /// record carried no classification sequence.
  uint8_t version = 0;

  // Classification flags worth listing (§5.2.7.2). All false when the record
  // carried no classification sequence, which §5.2.7.2 makes the correct
  // reading of an absent flag byte.
  bool caption = false;
  bool cyclic_marker = false;
  bool priority = false;
  bool alarm = false;
  bool update = false;
  bool support_record = false;
  /// §5.2.7.9: the Data Channel's Support Record must be executed before this
  /// record is — it defines macros, DRCS or colour maps this record invokes.
  bool support_needed = false;
  bool index = false;
  bool more = false;

  /// What §7.1.5 reserves this channel and address for, or empty.
  std::string reserved_purpose;

  /// Frames carrying the first and the most recent copy of this record.
  uint64_t first_seen_frame = 0;
  uint64_t last_seen_frame = 0;
  /// Copies counted over the analysed range — how often the service brought the
  /// record round, and so a rough measure of how reliably it can be recovered.
  uint64_t times_seen = 0;
  /// Copies that arrived whole and undamaged, of @ref times_seen.
  uint64_t times_intact = 0;
  /// Copies that named this record as transmitted, of @ref times_seen: every
  /// Hamming 8/4 byte of the packet address and of the record header through
  /// its classification sequence arrived as a codeword rather than being
  /// corrected into one (§3.2.2, §5.2.1; see NabtsMessage::identity_attested).
  ///
  /// Zero here on a record the recording brought round many times is the
  /// signature of a mis-corrected identity rather than of a real record — see
  /// NabtsRecordCatalogue::reconcile_identities(), which is what acts on it.
  /// Equal to @ref times_seen on any undamaged source, where every byte
  /// arrives as transmitted.
  uint64_t times_attested = 0;

  /// §5.2.8.4: a header extension field naming this record's More Record
  /// explicitly, which §7.3.4 has the receiver consult before the More Flag.
  /// The address is in long form, same data channel. False when the record
  /// carried no such extension.
  bool has_more_address = false;
  uint64_t more_address = 0;

  /// The base of the More chain this record belongs to — the chain each
  /// member names its successor of, by an explicit More address (§5.2.8.4) or
  /// by the More Flag's algorithmic long-address-plus-one (§5.2.7.6, the last
  /// two digits counted in decimal per §7.3.4). Equal to @ref address for a
  /// record that starts a chain or stands alone. A continuation is rendered
  /// over the accumulated display of the chain members before it, so its
  /// @ref page is the screen a viewer stepping through the page would see.
  uint64_t chain_base_address = 0;
  /// Steps from the chain base: 0 for the base itself or a standalone record.
  uint32_t chain_position = 0;

  /// Records in the linked series (§5.2.6); 1 for an unlinked record.
  uint32_t records_in_message = 0;
  /// The best copy has every link of its series and every packet of every
  /// group. A false here is why a presentation record may render short.
  bool complete = false;

  /// Record data (§5.3): NAPLPS presentation code for record types 0, 1 and 3,
  /// application data for type 2. Where an undamaged copy arrived it is that
  /// copy; where none ever did, it is a vote across the damaged copies (see
  /// @ref copies_voted).
  std::vector<uint8_t> data;

  /// Damaged copies combined into @ref data, or zero when it is a single copy
  /// that arrived undamaged. A vote of one is the copy itself, so one here
  /// means the record was only ever seen damaged and only once.
  uint32_t copies_voted = 0;

  /**
   * What the recovery knows about each byte of @ref data, index for index with
   * it. Both are empty for a record kept from a copy that arrived whole and
   * undamaged, which is the honest reading: there is nothing to doubt and
   * nothing was measured that says otherwise.
   *
   * @ref data_present is zero where no copy of the record ever delivered the
   * byte, so the value standing there is a filler rather than a reading (see
   * NabtsDataGroup::present). @ref data_confidence is how sure the recovery
   * ended up being of each byte on the 0-255 scale of NabtsPacket::confidence
   * — the detector's own figure where one copy decided the byte, and the
   * agreement of the copies that voted for it where several did.
   *
   * Kept past the vote because everything downstream of it — the linter, the
   * repair pass — can otherwise only tell a damaged byte from a clean one by
   * its parity, and the recovery knew more than that. Two bytes per byte of
   * record data: at the catalogue's own bounds
   * (NabtsRecordCatalogue::kMaxCataloguedRecords records of
   * kNabtsMaxGroupBytes each) that is under 4 MB, against the 31 MB the copies
   * they are derived from already cost.
   */
  std::vector<uint8_t> data_present;
  std::vector<uint8_t> data_confidence;

  /// Positions where the vote's weights could not separate the leading
  /// candidates, and how many of those the grammar then settled (see
  /// nabts_vote_record). Both zero where no vote was held, and the second zero
  /// where the grammar tie-break was not asked for or could not decide.
  uint32_t vote_positions_contested = 0;
  uint32_t vote_positions_adjudicated = 0;

  /// Function descriptors, for an application record. Empty otherwise.
  std::vector<NabtsRecordFunction> functions;

  /// The record's presentation code (§6.1 NAPLPS) run into a display list, for
  /// a presentation record — types 0, 1 and 3. Empty for an application record,
  /// whose data is function descriptors rather than drawing.
  NabtsPageSnapshot page;
};

/**
 * @brief Data groups some other application sent, on one channel
 *
 * CEA-516 §4.2.2 gives group type zero to broadcast teletext, reserves fifteen
 * for the service provider's private use and every other value for future
 * standardization. A group of another type carries no teletext record, and a
 * channel that carries nothing else is a service this stage cannot read —
 * which a reader is owed in words, rather than an empty list.
 *
 * Only groups whose channel and header both arrived as Hamming 8/4 codewords
 * are counted. Noise corrects into a non-zero type often enough to be counted
 * in the hundreds on a poor transfer; it does not arrive clean.
 */
struct NabtsForeignGroups {
  uint16_t channel = 0;
  /// GT (§4.2.2), never zero.
  uint8_t type = 0;
  uint64_t groups = 0;
};

/**
 * @brief How the NABTS recovery went over the analysed range
 *
 * Aggregate counts only; the per-line and per-group detail lives in the stage's
 * own report.
 */
struct NabtsRecoverySummary {
  uint64_t frames_analysed = 0;
  uint64_t fields_with_data = 0;
  uint64_t packets_recovered = 0;
  /// Packets whose Hamming 8/4 prefix did not decode (§3.2.2), and so could not
  /// even be filed under a channel.
  uint64_t packets_prefix_rejected = 0;
  /// Packet slots that came back empty on a line the recording has been seen to
  /// carry data on — an estimate of what was lost.
  uint64_t lost_packets_estimate = 0;
  /// Data blocks the suffix product code repaired, and data blocks whose suffix
  /// check failed and could not be repaired (§3.4).
  uint64_t blocks_corrected = 0;
  uint64_t blocks_damaged = 0;
  /// Data groups that arrived whole, and groups that ended without every packet
  /// S1,S2 promised (§4.2.5).
  uint64_t groups_completed = 0;
  uint64_t groups_incomplete = 0;
  /// Messages assembled whole, and messages missing at least one link.
  uint64_t messages_complete = 0;
  uint64_t messages_partial = 0;
  /// True when the record cap was reached and the least recently seen ones were
  /// dropped, so the catalogue is not everything the range carried.
  bool records_truncated = false;
  /// Byte positions the vote across a record's copies could not settle on the
  /// weights alone, totalled over the catalogue, and how many of them the
  /// grammar tie-break then decided.
  uint64_t vote_positions_contested = 0;
  uint64_t vote_positions_adjudicated = 0;
  /// What separating the record identities the service transmitted from the
  /// ones Hamming 8/4 mis-correction invented came to (see
  /// NabtsRecordCatalogue::reconcile_identities()). All zero on an undamaged
  /// recording, where every identity arrives as transmitted.
  VbiIdentityReconciliation identity_reconciliation;
  /// Other applications' data groups, ascending by channel then type. Empty on
  /// a recording that carries only teletext.
  std::vector<NabtsForeignGroups> foreign_groups;
};

/// Everything the NABTS sink caches from one trigger run.
struct NabtsAnalysisDataset {
  /// Ascending by {channel, address, version}
  std::vector<NabtsCataloguedRecord> records;
  NabtsRecoverySummary summary;
};

/**
 * @brief One caption, with the frames it was on screen for
 *
 * CEA-516 §7.3.10 carries captioning as non-cyclic presentation records marked
 * with the Caption Flag of §5.2.7.3, each new caption a new version of the same
 * record address (§7.3.10.1: "Each time the caption content is changed, the
 * Version Number shall be changed").
 */
struct NabtsCaptionCue {
  /// Frames the cue covers (0-based, as the catalogue counts them). The extent
  /// runs to the next caption: §7.3.10.1 has a receiver replace the caption on
  /// screen rather than being told when to take it down.
  uint64_t start_frame = 0;
  uint64_t end_frame = 0;
  uint16_t channel = 0;
  std::string address_text;
  uint8_t version = 0;
  /// The record's text, as nabts_page_text() reads it.
  std::string text;
};

/**
 * @brief The caption service a catalogue carries (CEA-516 §7.3.10)
 *
 * Ascending by the frame each caption was first seen at. A caption record that
 * drew nothing is an erase — §7.3.10.1: "Captions may be erased by the use of
 * PLPS code that erases either the entire display, or the area covered by the
 * caption" — so it ends the caption before it and yields no cue of its own.
 *
 * Shared between the sink stage's SubRip export and the caption track it
 * publishes in its catalogue, so the file and the screen cannot disagree about
 * what the service said. Both callers are inside the plugin: the host reads the
 * cues as catalogue data and never derives them.
 */
std::vector<NabtsCaptionCue> nabts_caption_cues(
    const std::vector<NabtsCataloguedRecord>& records);

}  // namespace orc

#endif  // ORC_VBI_SERVICES_VBI_ANALYSIS_RESULTS_H
