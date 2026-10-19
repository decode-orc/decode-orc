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
 * @brief One earlier version of a catalogued teletext sub-page
 *
 * What the sub-page showed before the service replaced its content (see
 * TeletextCataloguedSubPage).
 */
struct TeletextPageVersion {
  /// Frames carrying the first and the most recent header packet of this
  /// version
  uint64_t first_seen_frame = 0;
  uint64_t last_seen_frame = 0;

  /// Appearances of this version (see TeletextCataloguedSubPage::times_seen)
  uint64_t times_seen = 0;

  /// Packets this version's own transmissions lost (see
  /// TeletextCataloguedSubPage::lost_packets)
  uint64_t lost_packets = 0;

  /// Best assembly of this version, from the copies of its own content run
  TeletextPageSnapshot page;
};

/**
 * @brief One sub-page of a catalogued teletext page
 *
 * A page number can carry a sequence of sub-pages that the service cycles
 * through — the multi-page set of ETSI EN 300 706 Annex A.1, what a receiver
 * presents as a rotating page. Each is a page in its own right, told from its
 * siblings by the page sub-code (§9.3.1.2), and each is catalogued separately
 * so a reader can step through the sequence instead of seeing only whichever
 * one the carousel happened to leave behind.
 *
 * A sub-page's content can also change during a recording: a news page is
 * rewritten, a score updates. ETSI EN 300 706 Annex A.2 item 5 calls what a
 * page showed before an erase "the previous version of the page", and each
 * version is catalogued here in its own right — combined only from its own
 * copies (see TeletextPageSnapshot::content_run) — so a reader can see every
 * version the recording held rather than whichever was on air last. The
 * appearance and frame figures below cover the sub-page over every version,
 * @ref page and the version_* figures the current one.
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
  /// the analysed range rather than from one transmission. With earlier
  /// versions, this is the version current at the end of the range, built from
  /// that version's copies only.
  TeletextPageSnapshot page;

  /// The current version's own first frame and appearances. Equal to the
  /// sub-page's figures above when it only ever had one version.
  uint64_t version_first_seen_frame = 0;
  uint64_t version_times_seen = 0;

  /// The versions the current one replaced, oldest first. Empty for a
  /// sub-page whose content never changed.
  std::vector<TeletextPageVersion> earlier_versions;

  /// Earlier versions not kept because the catalogue bounds how many a
  /// sub-page holds, oldest first (see TeletextPageCatalogue).
  uint64_t versions_dropped = 0;
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
  /// Sub-pages the caps dropped, and page numbers dropped with their last
  /// sub-page (see TeletextPageCatalogue).
  uint64_t subpages_dropped = 0;
  uint64_t pages_dropped = 0;
  /// Sub-pages whose content the service replaced during the range, the
  /// versions they went through between them, and the earlier versions not
  /// kept (see TeletextCataloguedSubPage).
  uint64_t subpages_with_versions = 0;
  uint64_t versions = 0;
  uint64_t versions_dropped = 0;
  /// Read by transmission, the versions only one transmission ever showed,
  /// removed where the sub-page had a version shown more often (see
  /// TeletextPageCatalogue::drop_unrepeated_versions()).
  uint64_t versions_unrepeated = 0;
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
  /// Which of the contents the recording carried under this identity this
  /// one is, in the order they first arrived: 0 for the first. A service can
  /// send different records under one channel, address and version over a
  /// recording — a carousel whose sixteen versions (§5.2.7.2 Y16) come round
  /// again carrying new pages, a page replaced without its version moving —
  /// and each is catalogued as a record of its own rather than voted into one.
  uint32_t content = 0;

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
  /// §5.2.7.8: the More Record "shall be acquired and presented after the
  /// completion of the presentation of the current Record, without user
  /// intervention".
  bool auto_acquire = false;

  /// Pointer bytes of the classification sequence (§5.2.7.1), 0 when the
  /// record carried none. More than one puts the record outside the Full
  /// Service Standard, whose receivers ignore it entirely (§8.5.2.7.1).
  uint8_t classification_pointers = 0;
  /// The flag bytes §5.2.7.2 reserves that the service set, as "Y11 = 4", in
  /// the order they were sent. Empty when it set none.
  std::vector<std::string> reserved_flags;

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

  /// §5.2.8.4: a header extension field redefines this record's More Record
  /// (§5.2.8.2 meaning 001), naming a record or a macro. §7.3.4 has the
  /// receiver consult it before the More Flag, so where this is set the
  /// flag's algorithmic address (§5.2.7.6) is not this record's More Record,
  /// whatever the extension names.
  bool more_redefined = false;
  /// The record the extension names as the More Record: in data channel
  /// @ref more_channel — this record's own unless the field named another (ES
  /// 6 or 12) — at long address @ref more_address. False where the extension
  /// designates a macro instead (@ref more_macro), or there is none.
  bool has_more_address = false;
  uint16_t more_channel = 0;
  uint64_t more_address = 0;
  /// The macro the extension makes the More function, column in the high
  /// nibble and row in the low (§5.2.8.4 ES 2), where @ref more_redefined is
  /// set without a record.
  uint8_t more_macro = 0;

  /// §5.2.8.2 meanings 010 and 011: a header extension field names the record
  /// a receiver's NEXT and INDEX keys fetch from this page, in this record's
  /// data channel, at long address @ref next_address and @ref index_address
  /// (§5.2.8.4). False where no field names a record there — a macro, a
  /// record in another data channel, or none at all; then the Panasonic
  /// TU-2000x's NEXT took the next page by number and its INDEX the last page
  /// it had shown with the Index Flag (§5.2.7.2 Y13), or else the Master
  /// Index.
  bool has_next_address = false;
  uint64_t next_address = 0;
  bool has_index_address = false;
  uint64_t index_address = 0;

  /// The pages this record is presented over, oldest first, each as its
  /// identity "CCC/AAA vN": a record that starts on the screen it was asked
  /// from, neither clearing the display nor filling it, is drawn over the page
  /// a viewer came to it from, as the TU-2000x drew it (see
  /// nabts_presentation_contexts()). Empty for a record presented on its own,
  /// or over its More Record predecessors alone.
  std::vector<std::string> presented_over;

  /// The long addresses of the pages this record's text offers its viewer —
  /// an index's entries, a quiz's answers, a story's "See page 500"
  /// (nabts_offered_numbers()) — in the order the text gives them, whether or
  /// not the recording carried them. A More Record's text is read with the
  /// chain's before it, which is on screen with it.
  std::vector<uint64_t> offered_pages;

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
   * Whether each byte of @ref data arrived, index for index with it: zero where
   * no copy of the record ever delivered the byte, so the value standing there
   * is a filler rather than a reading (see NabtsDataGroup::present). Empty for
   * a record kept from a copy that arrived whole and undamaged, which is the
   * honest reading: there is nothing to doubt and nothing was measured that
   * says otherwise.
   *
   * Kept past the vote because what reads the record afterwards — the
   * reconstruction of the bytes it lost — can otherwise only tell a filler from
   * a received byte by its parity, and the recovery knew more than that. One
   * byte per byte of record data: at the catalogue's own bounds
   * (NabtsRecordCatalogue::kMaxCataloguedRecords records of
   * kNabtsMaxGroupBytes each) that is under 2 MB, against the 31 MB the copies
   * it is derived from already cost.
   */
  std::vector<uint8_t> data_present;

  /// Function descriptors, for an application record. Empty otherwise.
  std::vector<NabtsRecordFunction> functions;

  /// The record's presentation code (§6.1 NAPLPS) run into a display list, for
  /// a presentation record — types 0, 1 and 3. Empty for an application record,
  /// whose data is function descriptors rather than drawing.
  NabtsPageSnapshot page;

  /// The digits the page links to macros it defines and never runs itself, bit
  /// n for digit n: a page a viewer played on the keypad, its macro 2/n run as
  /// key n was pressed (X3.110 §5.5, Table D1 item 7). Zero for a page that
  /// takes no keys, and for a Support Record, whose macros are the pages'.
  uint16_t linked_keys = 0;
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
 * @brief A service on the recording's packet addresses that is not teletext
 *
 * NABTS is a transport, and a recording can carry other services on it than
 * the teletext this sink catalogues: a datacast protected by the Reed-Solomon
 * row code RFC 2728 Appendix A describes, an encoder's idle fill, a test
 * sequence, a clock. Their packets carry no data group a record could be read
 * from, so without a name for them the recording reads as a recovery that
 * failed (see NabtsServiceCensus, which recognises them).
 */
struct NabtsOtherService {
  enum class Kind : uint8_t {
    /// Packets whose two-byte suffix is RFC 2728's row code, and that pass it.
    kFecDatacast,
    /// A few packet contents repeated: an encoder's bit-error-rate test
    /// pattern, sent when it has nothing else to send.
    kIdleFill,
    /// Every packet a stretch of one short linear-feedback shift register
    /// sequence.
    kTestSequence,
    /// Hamming 8/4 coded packets carrying a time of day that advances.
    kClock,
    /// Data groups of a type CEA-516 §4.2.2 reserves whose data reads as
    /// seven-bit text, such as a service's private-use teletype feeds.
    kPrivateText,
  };
  Kind kind = Kind::kFecDatacast;
  /// The packet addresses, as the report names them: "300", "0F0-0FF" for a
  /// run of consecutive addresses, "EF0, FC0" for addresses carrying the same.
  std::string channels;
  /// Packets those addresses carried.
  uint64_t packets = 0;
  /// What the service was recognised as, and the figures it was recognised by.
  std::string description;
};

/**
 * @brief How the NABTS recovery went over the analysed range
 *
 * Aggregate counts only; the per-line and per-group detail lives in the stage's
 * own report.
 */
struct NabtsRecoverySummary {
  uint64_t frames_analysed = 0;
  uint64_t packets_recovered = 0;
  /// Packets refused because every byte of their prefix needed correction:
  /// test-signal lines read as data (nabts_prefix_every_byte_corrected()).
  uint64_t packets_refused_unprotected = 0;
  /// Of the packets recovered, the ones whose five prefix bytes all arrived
  /// whole (nabts_prefix_every_byte_whole()).
  uint64_t packets_prefix_whole = 0;
  /// True when so few prefixes arrived whole that the lines read are another
  /// signal behind NABTS's run-in and framing code
  /// (nabts_lines_carry_no_nabts()): nothing is catalogued from them, and the
  /// records and captions are empty.
  bool not_nabts = false;
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
  /// Captions that arrived after the caption stream's bound was met and were
  /// not kept (NabtsCaptionStream::kMaxCaptions), so the caption service is
  /// the range's first captions rather than all of them. Zero on any real
  /// recording.
  uint64_t captions_dropped = 0;
  /// What separating the record identities the service transmitted from the
  /// ones Hamming 8/4 mis-correction invented came to (see
  /// NabtsRecordCatalogue::reconcile_identities()). All zero on an undamaged
  /// recording, where every identity arrives as transmitted.
  VbiIdentityReconciliation identity_reconciliation;
  /// Other applications' data groups, ascending by channel then type. Empty on
  /// a recording that carries only teletext.
  std::vector<NabtsForeignGroups> foreign_groups;
  /// The services on packet addresses that carried no teletext, ascending by
  /// their first address. Empty on a recording that carries only teletext.
  std::vector<NabtsOtherService> other_services;
};

/**
 * @brief A presentation record another recording carried, to rebuild this
 *        one's lost bytes from
 *
 * Loaded from what the NABTS sink's reference_records parameter names: a
 * packet stream is replayed through the same reassembly and vote as this
 * recording's own records, so a reference knows which of its bytes arrived just
 * as a record of this recording does; a record file export_records wrote is
 * the record as its own run voted it, with no mask, its lost bytes standing as
 * 0x00.
 */
struct NabtsReferenceRecord {
  /// The stream it came from, as a reader would name it (its file name, or
  /// for a run's out.t33 the folder it is in).
  std::string source;
  /// Channel, address and version.
  std::string key;
  std::vector<uint8_t> data;
  /// Non-zero where the byte arrived; empty when every byte did, and for a
  /// record read from a record file, whose lost bytes are its 0x00 bytes
  /// (which no byte passing its parity is).
  std::vector<uint8_t> present;
};

/// Everything the NABTS sink caches from one trigger run.
struct NabtsAnalysisDataset {
  /// Ascending by {channel, address, version}. The caption service is not
  /// here: see @ref captions.
  std::vector<NabtsCataloguedRecord> records;
  /**
   * The caption service (CEA-516 §7.3.10): one entry per caption, in
   * transmission order. §7.3.10.1 gives every new caption the next version of
   * one address, so the sixteen versions Y16 allows recur throughout a
   * recording and a caption is a run of arrivals rather than an identity; each
   * entry is dated by the frame its first copy arrived in (first_seen_frame)
   * and its data is voted from that caption's own copies only.
   */
  std::vector<NabtsCataloguedRecord> captions;
  NabtsRecoverySummary summary;
  /// Presentation records of the reference recordings, for the records
  /// viewer's reconstruction of lost bytes. Empty when none were named.
  std::vector<NabtsReferenceRecord> references;
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
 * @brief The caption service as cues (CEA-516 §7.3.10)
 *
 * @p records are the caption service's captions with their pages presented —
 * NabtsAnalysisDataset::captions, one per caption as it arrived. Records not
 * carrying the Caption Flag of §5.2.7.3 are passed over.
 *
 * Ascending by the frame each caption was first seen at. A caption record that
 * drew nothing is an erase — §7.3.10.1: "Captions may be erased by the use of
 * PLPS code that erases either the entire display, or the area covered by the
 * caption" — so it ends the caption before it and yields no cue of its own.
 * A caption reading exactly as the one before it at the same address, with no
 * erase between, is the same caption still on screen — sent again under a new
 * version, or one of its copies filed under a version misread in the Hamming
 * 8/4 byte that carries it — so it extends that cue rather than starting one.
 *
 * Shared between the sink stage's SubRip export and the caption track it
 * publishes in its catalogue, so the file and the screen cannot disagree about
 * what the service said. Both callers are inside the plugin: the host reads the
 * cues as catalogue data and never derives them.
 */
std::vector<NabtsCaptionCue> nabts_caption_cues(
    const std::vector<NabtsCataloguedRecord>& records);

/// Frame |frame| (0-based) as the time a caption track gives it, HH:MM:SS
/// with the milliseconds after |separator|: ',' for SubRip, '.' for the
/// catalogue's caption track.
std::string nabts_caption_time(uint64_t frame, char separator);

}  // namespace orc

#endif  // ORC_VBI_SERVICES_VBI_ANALYSIS_RESULTS_H
