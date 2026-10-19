/*
 * File:        teletext_page_decoder.h
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     WST (System B) teletext magazine/page decoder producing Level 1
 *              page snapshots and subtitle cues
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_PAGE_DECODER_H
#define ORC_TELETEXT_PAGE_DECODER_H

// Shared plugin-side library, NOT part of the SDK contract: it compiles
// against the public SDK headers only and is linked privately into the stage
// plugins that need it. Nothing here crosses the plugin boundary, so changes
// never force an ABI bump.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "teletext_row_squasher.h"
#include "teletext_slicer.h"

namespace orc {

// Check a display byte for odd parity.
// ETSI EN 300 706 §8.1: bit 8 is the parity bit, bits 1-7 carry the data;
// the byte is accepted when it contains an odd number of '1' bits.
bool teletext_odd_parity_valid(uint8_t byte);

// Encode a 7-bit value as an odd-parity protected byte (ETSI EN 300 706
// §8.1). Only the low 7 bits of |value| are used.
uint8_t teletext_odd_parity_encode(uint8_t value);

// National option sub-sets a Level 1 page can select, in the order the C12,
// C13 and C14 header bits designate them.
//
// ETSI EN 300 706 §15.2 Table 32 indexes the sub-set by the default G0/G2
// designation *and* those three bits; a Level 1 page has no packet X/28 or
// M/29 to carry a designation, so §15.2 says the sub-set "is defined by the
// C12, C13 and C14 control bits in the page header alone" and the default
// designation row (triplet 1 bits 14-11 = 0000) is the one that applies.
// C12 is the most significant of the three bits, as the table prints them.
//
// Table 32 leaves 1 1 1 blank for this designation — no sub-set is defined —
// and there is nothing to fall back on: Table 35's own glyphs at these
// positions apply only when the set is reached through a packet X/26
// (Table 35 NOTE 2). It is rendered as English.
enum class TeletextNationalOption : uint8_t {
  English = 0,
  German = 1,
  SwedishFinnishHungarian = 2,
  Italian = 3,
  French = 4,
  PortugueseSpanish = 5,
  CzechSlovak = 6,
  Undefined = 7,
};

// G0 primary character sets a Level 1 page can be displayed in.
//
// The G0 set is a property of the transmission rather than of the page's
// header bits: ETSI EN 300 706 §15.2 designates it by the four *Default G0 and
// G2 Character Set Designation* bits of a packet X/28/0 Format 1 or M/29/0,
// and the three national option bits then choose a sub-set within it. A Level
// 1 service transmits neither packet, and §15.2 is explicit about what happens
// then: "in the absence of a packet X/28/0 Format 1, X/28/4, M/29/0 or M/29/4,
// the default sets are established by a local Code of Practice" — that is, by
// where the receiver was sold. That is why this is a decoder setting and not
// something recoverable from the stream (see
// TeletextPageDecoder::set_default_g0_set).
//
// Only the sets Table 32 reaches with a Cyrillic designation are enumerated
// here alongside Latin. Greek, Arabic and Hebrew are defined by the standard
// and not implemented; a page designating one of them is displayed as Latin,
// which is what this decoder did for every page before the Cyrillic sets were
// added.
enum class TeletextG0Set : uint8_t {
  // §15.6.1 Table 35, with the national option sub-sets of Table 36.
  Latin = 0,
  // §15.6.4 Table 38 — Serbian/Croatian (Table 32 designation 0100, national
  // option bits 000).
  Cyrillic1 = 1,
  // §15.6.5 Table 39 — Russian/Bulgarian (designation 0100, bits 100).
  Cyrillic2 = 2,
  // §15.6.6 Table 40 — Ukrainian (designation 0100, bits 101).
  Cyrillic3 = 3,
};

// A designated G0 set together with the national option sub-set the
// designation itself names for it.
//
// The two travel together because ETSI EN 300 706 §15.2 Table 32 and §15.3
// Table 33 are single 7-bit values selecting both at once. It matters for the
// *second* G0 set in particular: §15.3 says in as many words that "the national
// option sub-set selected by the C12, C13 and C14 bits is not relevant to the
// secondary set", so a second Latin set carries its own sub-set and cannot
// borrow the page header's.
struct TeletextG0Designation {
  TeletextG0Set g0_set = TeletextG0Set::Latin;
  // A TeletextNationalOption; only consulted when |g0_set| is Latin, the
  // Cyrillic sets reserving no positions for a sub-set.
  int national_option_subset = 0;
};

// Human-readable name of a G0 set, as the parameter surface spells it.
std::string to_string(TeletextG0Set g0_set);

// The G0 set a name from to_string() denotes, or std::nullopt when it names
// none.
std::optional<TeletextG0Set> teletext_g0_set_from_string(std::string_view name);

// Map a 7-bit G0 display code to its Unicode code point.
//
// For TeletextG0Set::Latin this is the primary set of ETSI EN 300 706 §15.6.1
// Table 35 with the |national_option_subset| substitutions of §15.6.2 Table 36
// at the thirteen positions Table 35 NOTE 2 reserves for them (2/3, 2/4, 4/0,
// 5/B-5/F, 6/0 and 7/B-7/E). The Cyrillic sets define all 96 positions
// themselves and reserve none, so the sub-set is not consulted for them.
//
// |national_option_subset| is a TeletextNationalOption, i.e. the value
// TeletextPageSnapshot::national_option_subset carries; anything outside its
// range is treated as English. Codes below 2/0 are spacing attributes rather
// than characters and return SPACE.
char32_t teletext_g0_to_unicode(uint8_t code, TeletextG0Set g0_set,
                                int national_option_subset);

// teletext_g0_to_unicode() encoded as UTF-8, for text output (subtitle cues
// and the like) rather than a glyph grid.
std::string teletext_g0_to_utf8(uint8_t code, TeletextG0Set g0_set,
                                int national_option_subset);

// Level 1 display colours in spacing-attribute code order.
// ETSI EN 300 706 §12.2 Table 26: alpha colour codes 0/0-0/7 and mosaic
// colour codes 1/0-1/7 select black through white in this order.
enum class TeletextColour : uint8_t {
  Black = 0,
  Red = 1,
  Green = 2,
  Yellow = 3,
  Blue = 4,
  Magenta = 5,
  Cyan = 6,
  White = 7,
};

// One rendered character cell of a Level 1 page.
struct TeletextPageCell {
  // 7-bit transmitted code (odd parity removed). For alphanumeric cells this
  // indexes the current G0 set; for mosaic cells the G1 set. Cells occupied
  // by a spacing attribute hold 0x20 (SPACE), or the held-mosaic character
  // when |held_mosaic| is set (EN 300 706 §12.2 code 1/E).
  uint8_t character = 0x20;
  TeletextColour foreground = TeletextColour::White;
  TeletextColour background = TeletextColour::Black;
  // G1 mosaic set selected (EN 300 706 §12.2 codes 1/1-1/7). Character codes
  // 0x40-0x5F remain alphanumeric capitals even in mosaic mode.
  bool mosaic = false;
  // Separated (bordered) rather than contiguous mosaic blocks (§12.2 1/A).
  bool separated_mosaic = false;
  // Cell is a spacing attribute displayed as the held mosaic character
  // (§12.2 1/E); |separated_mosaic| then reflects the held character's
  // original mode.
  bool held_mosaic = false;
  // Origin (upper) cell of a double-height pair (§12.2 0/D).
  bool double_height = false;
  // Lower cell of a double-height pair: no foreground data, background
  // copied from the origin row (§12.2 0/D).
  bool double_height_lower = false;
  // §12.2 0/8: foreground pixels alternate with the background colour, at a
  // rate the renderer chooses.
  bool flash = false;
  bool conceal = false;  // §12.2 1/8: display as SPACE until revealed
  bool boxed = false;    // inside a Start Box/End Box region (§12.2 0/A-0/B)
  // The transmitted byte failed odd parity (EN 300 706 §8.1); |character| is
  // replaced with SPACE and the cell flagged so renderers can mark it.
  bool parity_error = false;

  // G0 set |character| is to be read in, and the national option sub-set that
  // goes with it. Resolved per cell rather than per page because the ESC
  // spacing attribute (§12.2 Table 26 code 1/B) toggles the row between the
  // page's default G0 set and its second one, so a single row can hold two
  // alphabets — the very thing two-alphabet services use it for. A consumer
  // converting a cell to a glyph reads these and needs to know nothing about
  // ESC; on a page with no second set designated they are the page's own
  // g0_set and national_option_subset throughout.
  TeletextG0Set g0_set = TeletextG0Set::Latin;
  int national_option_subset = 0;

  // A byte earlier in this row failed odd parity while a second G0 set was in
  // force for the page, so |g0_set| above may be the wrong one of the two: a
  // damaged byte cannot be told from the ESC it might have been, and one lost
  // ESC inverts every cell after it to the end of the row.
  //
  // Cells are only ever marked from the damage onwards, and never at all on a
  // page with no second set — where ESC does nothing and there is nothing to
  // get wrong. §12.2 Table 26 re-selects the default set at the start of every
  // row, which is what stops this reaching past the row it began in.
  bool g0_set_uncertain = false;
};

// A completed Level 1 page: the 25-row grid (row 0 is the header row) plus
// the page address and header control bits of ETSI EN 300 706 §9.3.1.
struct TeletextPageSnapshot {
  static constexpr int kRows = 25;     // header row 0 + display rows 1-24
  static constexpr int kColumns = 40;  // EN 300 706 §9.3.2: 40 display bytes

  // Display columns to draw. kColumns on both services: the Level 1 display is
  // a 40-column grid whatever the packet length, and a row the service left
  // short simply shows spaces to the right of what it sent — which is what a
  // receiver puts on screen. Carried in the snapshot rather than read from
  // kColumns by consumers so a service that displays fewer can say so.
  int columns = kColumns;

  // Whether character codes 4/0-5/F keep their alphanumeric meaning while
  // mosaic graphics are selected — "blast-through", ETSI EN 300 706 §15.7.1
  // Table 47 NOTE 1 — so that capitals can be written into a graphic without
  // leaving mosaic mode. True on 625 lines.
  //
  // The 525-line recordings say otherwise: their page graphics run codes from
  // that range in among the mosaic ones (the service logo alternates 6/0 and
  // 7/E with 5/7 and 5/F, identically in every copy of the row), and read as
  // mosaics those are the block patterns the drawing needs — 5/F being a solid
  // block. Rendered as capitals they put stray letters through the artwork.
  // Nothing in ITU-R BT.653 settles it: §5.2.2 describes mosaic coding without
  // giving a code table, so this is what the material shows rather than what a
  // standard states.
  //
  // A renderer reads this to decide whether a cell in mosaic mode holding such
  // a code is a character or a block; nothing else about the page depends on
  // it.
  bool mosaic_blast_through = true;

  // Displayed magazine number 1-8. Transmission magazine 0 is displayed as
  // magazine 8 (EN 300 706 §3.1 "page number" convention: page 100 = 1/00).
  int magazine = 8;
  // Two-digit hexadecimal page number 0x00-0xFF (EN 300 706 §9.3.1.1).
  int page_number = 0;
  // 13-bit page sub-code S1-S4 (EN 300 706 §9.3.1.2).
  int subcode = 0;

  // Page header control bits (EN 300 706 §9.3.1.3 Table 2).
  bool erase_page = false;            // C4
  bool newsflash = false;             // C5
  bool subtitle = false;              // C6
  bool suppress_header = false;       // C7
  bool update_indicator = false;      // C8
  bool interrupted_sequence = false;  // C9
  bool inhibit_display = false;       // C10
  bool magazine_serial = false;       // C11
  // C12-C14 as a TeletextNationalOption: which national option sub-set the
  // page's G0 set uses (EN 300 706 §15.2 Table 32). C12 is the most
  // significant bit, so the value indexes Table 32 as printed.
  int national_option_subset = 0;

  // G0 primary set the page's alphanumeric codes are read in: what a packet
  // X/28/0 Format 1 designated for this page, failing that what an M/29/0
  // designated for its magazine, failing both the decoder's configured default
  // (EN 300 706 §15.2). The Cyrillic sets reserve no national option
  // positions, so |national_option_subset| above is only consulted when this
  // is Latin.
  //
  // This is the set every row *starts* in (§12.2 Table 26 code 1/B). Where the
  // page also has a second set, the cells say which of the two each of them is
  // actually in; see TeletextPageCell::g0_set.
  TeletextG0Set g0_set = TeletextG0Set::Latin;

  // The page's second G0 set, if it has one: what a packet X/28/0 Format 1 or
  // X/28/4 designated for this page, failing that what an M/29/0 or M/29/4
  // designated for its magazine, failing all four the decoder's configured
  // default (EN 300 706 §15.3, set_default_second_g0_set()).
  //
  // Unset means the ESC spacing attribute does nothing on this page — either
  // because no second set was designated, or because the designation was the
  // 1111111 that §15.3 defines as "no second G0 set required" and says a
  // decoder may read as disabling ESC. Every cell is then in |g0_set| above.
  std::optional<TeletextG0Designation> second_g0_set;

  // Field indices (as passed to process_packet()) of the header packet that
  // *opened* this transmission and of the last packet that contributed to
  // the page. A header re-sent while the page's own rows are still being
  // transmitted does not restamp the first of these, so every snapshot of
  // one appearance of a page shares a header_field_index and a consumer can
  // use it to tell appearances apart.
  int64_t header_field_index = 0;
  int64_t last_field_index = 0;

  // Whether a packet was received for each row of this page (row 0 = the
  // X/0 header). A row with no packet displays as spaces on a black
  // background, which is indistinguishable from a transmitted blank row —
  // so recovery gaps can only be reported from this flag, never inferred
  // from the cells.
  std::array<bool, kRows> row_received{};

  // Copies of each display row that were combined to produce it: 0 where no
  // packet was received, 1 where the row rests on a single copy, more where
  // repeated transmissions corrected each other (see teletext_row_squasher.h).
  // Index 0 is always 0 — header rows carry a live clock and are not squashed.
  //
  // This is the page's confidence in its own rows. One copy is not a fault,
  // but it is unchecked: the row is shown exactly as it was received, and a
  // burst long enough to carry a row's address onto another codeword (Hamming
  // 8/4 corrects one bit and detects two, EN 300 706 §8.2 — a longer burst can
  // still land on a valid address) puts that row in the wrong place with
  // nothing to contradict it. A second copy is what turns that into a vote.
  std::array<int, kRows> row_copies{};

  // Times each display row arrived in this snapshot's content run, which
  // row_copies stops counting at the squasher's copy bound (see
  // TeletextRowSquasher::arrival_count()). Without a squasher, 1 for a row
  // received and 0 otherwise, as row_copies is.
  std::array<int, kRows> row_arrivals{};

  // Whether the page's transmission had finished when this snapshot was
  // taken. False means more rows of *this* transmission were still to come:
  // either the header was repeated part-way through the page (a rolling
  // header, EN 300 706 §9.3.1.4 — the service re-sends X/0 while the rows
  // continue to arrive), or a consumer peeked at the page in progress with
  // open_page_snapshots(). A partial snapshot is not damaged data; it is a
  // page that has not all arrived yet, and the two look identical on screen,
  // so only this flag distinguishes them.
  bool transmission_complete = true;

  // Which content run of its sub-page this snapshot belongs to, counting from
  // 0. Runs are what the row squasher keeps apart (see
  // TeletextPageDecoder::set_row_squasher()): a new one starts where the
  // service replaced the page's content rather than repeating it, so two
  // snapshots of one sub-page in different runs are different versions of the
  // page (ETSI EN 300 706 Annex A.2 item 5). Always 0 without a squasher.
  int content_run = 0;

  // Whether the header that opened this transmission named the page as
  // transmitted: the two MRAG bytes (§7.1.2) and the two page-number bytes
  // (§9.3.1.1) all arrived as Hamming 8/4 codewords rather than being corrected
  // into them (see teletext_hamming84_clean()).
  //
  // False does not mean the page number is wrong — most corrections are right.
  // It means the code was asked to guess, and §8.2's distance of 4 makes a
  // three-bit burst resolve silently to a neighbouring codeword: on a damaged
  // recording that does not corrupt the page, it *duplicates* it at a number
  // the service never sent. A catalogue cannot tell such a page from a real one
  // by looking at it, which is why this travels with the snapshot rather than
  // being recomputed later.
  //
  // Restamped only by a header that opens a transmission, alongside
  // |header_field_index|, so a rolling header does not overwrite what the
  // opening one said.
  bool identity_attested = false;

  std::array<std::array<TeletextPageCell, kColumns>, kRows> cells{};
};

// One subtitle cue recovered from a C6-flagged page. Times are expressed as
// the field indices passed to process_packet(); consumers convert to seconds
// via the field rate (50 fields/s for 625-line PAL).
struct TeletextSubtitleCue {
  int64_t start_field_index = 0;
  int64_t end_field_index = 0;
  // Plain text, rows separated by '\n', Level 1 attributes dropped.
  std::string text;
};

// How the transmissions of one page number are filed into pages (see
// TeletextPageDecoder::set_page_separation()).
enum class TeletextPageSeparation : uint8_t {
  // A page number and sub-code name one page (ETSI EN 300 706 §9.3.1), and a
  // transmission is a new version of it only where its rows show the content
  // was replaced.
  kPageNumber,
  // Every transmission of a page may be a screen of its own, told apart from
  // the others by its content alone: what the VirText service sent, a
  // rotation of screens under one page number for a Zenith terminal that
  // showed whatever arrived. Pages sent in Zenith's own packet layout are read
  // in it (see the class comment).
  kTransmission,
};

// Where one stretch of a packet's display bytes sits on its page: |count|
// bytes from |packet_byte| of the packet are columns |first_column| onwards of
// display row |row|.
struct TeletextPacketSpan {
  int row = 0;
  size_t packet_byte = 0;
  size_t first_column = 0;
  size_t count = 0;
};

/**
 * @brief WST teletext magazine/page decoder (Level 1).
 *
 * Consumes T42 packets (MRAG + data bytes, transmission coding) in strictly
 * temporal order, applies Hamming 8/4 and odd-parity decoding (ETSI EN 300 706
 * §8.1-8.2), and assembles pages in both serial and parallel magazine
 * transmission modes (§7.2, §7.3, control bit C11).
 *
 * Both packet lengths ITU-R BT.653 defines for System B are handled: 42 bytes
 * on 625 lines and 34 on 525 (Table 1b). Everything the decoder reads by
 * position — the MRAG, the page number, the sub-code and the control bits —
 * sits at the same byte offsets in both, so only the number of display bytes a
 * packet carries changes: 40 and 32 (see process_packet()).
 *
 * Pages are 40 columns wide on both, because a 525-line service sends the
 * remaining 8 columns of its rows in separate *row-extension* packets. This is
 * not in BT.653 — the standard describes the 32-byte data block and stops — but
 * it is what the surviving 525-line WST recordings carry, and it is the only
 * way a 34-byte packet can deliver the 40-column page the standard's own
 * addressing, header layout and display model assume.
 *
 * An extension packet is addressed to magazine M|4. Its 32 display bytes are
 * four groups of 8 carrying columns 32-39 of four consecutive rows, and its
 * packet number identifies that block of four rather than naming a row: it
 * rounds down to a multiple of four, so packets numbered 1, 4, 8, 12, 16 and 20
 * complete rows 0-3, 4-7, 8-11, 12-15, 16-19 and 20-23. The first block's
 * packets carry 1 rather than 0 because 0 is the page header's own packet
 * number. Row 0 is therefore extended like any other: on the reference
 * recordings its columns 32-39 carry the service name, which is where a
 * 40-column receiver photograph of the same service shows it.
 *
 * Magazine M|4 is only borrowed where the service is not using it for pages.
 * The reference recordings carry pages in magazines 8, 1, 2, 3 *and* 4 (test
 * pages 400-403), with extensions in 5 and 6 on one and in 5 and 7 on the
 * 1984 Keyfax capture — so which of 4 to 7 are extension carriers has to be
 * read from the stream rather than assumed. The signal is exact: a magazine
 * carrying pages opens every one of them with an X/0 header (§7.2.1), and an
 * extension carrier, having no pages, never sends one, and sends nothing but
 * the six block numbers. A magazine 4-7 is therefore taken to carry pages the
 * moment an X/0 arrives in it *as transmitted* (see header_claims_pages() for
 * why nothing less will do), and to be an extension carrier once it has sent a
 * page's worth of block-numbered packets and nothing else. Its packets are
 * discarded until then, because an extension applied to a page it was never
 * meant for cannot be taken back — a squasher keeps every copy — while ones
 * discarded here come round again with the next cycle.
 *
 * A row that gets no extension shows spaces there, as it would on a receiver.
 *
 * The VirText service sent some of its pages in a layout of its own, the one
 * Zenith's VirText terminals read, and with
 * TeletextPageSeparation::kTransmission those pages are read in it. Every
 * packet of such a page ends with the byte 0x55 — alternating bits, which fail
 * the odd parity every display byte carries (EN 300 706 §8.1), so no Level 1
 * row can hold one — and keeps the standard addressing ahead of it:
 *
 *  - the header's ten addressing and control bytes, then 0x55, then 22
 *    characters of header text for columns 8-29;
 *  - display rows 1 to 19 (a 20-line page): 0x55, then columns 0-29;
 *  - packets 20 and 21: a Hamming 8/4 digit N, then columns 30-39 of three
 *    rows from N + 16 × (packet number − 20), ten bytes to a row — packet 20
 *    with N = 0, 3, 6, 9, 12 and 15 and packet 21 with N = 2 complete rows 0
 *    to 19, and row 0's columns 30-39 hold the rest of the header text;
 *  - packet 22, sent once before each new screen: a two-digit Hamming 8/4
 *    counter, then a run-in, a framing code and a header image, which is not
 *    page content and is not displayed.
 *
 * Which pages use it is read from the stream rather than assumed, page number
 * by page number: on recordings of the service it can be every page of
 * magazine 1 and one more, beside test pages of magazine 2 in the layout
 * above. A header or
 * row framed by 0x55 at both ends votes for it, one holding characters that
 * pass parity there votes against it, and the page is read the way its votes
 * lean, or the way its own header looks while they are even.
 *
 * Completed pages are delivered as Level 1 snapshots through the page
 * callback when the page transmission is terminated by the next page header
 * (§7.2.1) or by finalize(). When a subtitle page filter is set, subtitle
 * cues are additionally emitted per the C5/C6 conventions (§9.3.1.3): page
 * arrival displays the text, a header for the page with C4 (erase) set or C6
 * clear removes it.
 *
 * Where the service designates a *second* G0 set (§15.3, packets X/28/0
 * Format 1, X/28/4, M/29/0 and M/29/4) — or one is configured for a Level 1
 * service that designates none — the ESC spacing attribute of §12.2 Table 26
 * code 1/B toggles each display row between the two sets, which is how a
 * two-alphabet service mixes Latin words into Cyrillic text. Every rendered
 * cell carries the set it resolved to, so the toggling is invisible downstream;
 * on a page with no second set ESC does nothing, as it did before.
 *
 * A sub-code is believed only as far as it arrived. The four bytes that carry
 * it (§9.3.1.2) are Hamming 8/4, whose correction resolves a three-bit error
 * silently to a neighbouring codeword (§8.2), and a sub-page read under a
 * sub-code the service never sent is a duplicate of the page that a catalogue
 * lists as one of its own. So a header whose sub-code needed correction takes
 * the corrected digits only where they name a sub-code the page has already
 * been sent with as codewords throughout; otherwise those digits are the page's
 * last sub-code's, and the transmission is the page it repeats. A page's first
 * header is taken as read. Tapes misread a sub-code byte in the same few ways
 * over and over — 0xD0 (C4 alone) as 0xE8, 0x15 as 0x0A, 0x49 as 0x55, each a
 * transition of the byte read a bit early or late — and each lands on a
 * sub-code no header of the page ever carried as codewords. Read as they
 * decode, 794 of 124,408 headers on one recording named a sub-page 0001 of a
 * page the service sent as 0000; and 1,341 of 60,867 on another named
 * sub-pages of a page it never sent, which filled the page's place in a
 * catalogue before its own rotation did: 27 of its 64 sub-pages were listed,
 * where settled as above all 64 are.
 *
 * Error handling degrades gracefully: packets whose MRAG is uncorrectable
 * are dropped, headers whose page number is uncorrectable are dropped,
 * uncorrectable control nibbles fall back to zero (a sub-code nibble as above),
 * and display bytes failing odd parity render as flagged SPACE cells.
 *
 * This component is deliberately stateful (page assembly spans many fields)
 * and therefore lives outside the stateless teletext observer.
 *
 * Thread safety: none; confine an instance to one thread.
 */
class TeletextPageDecoder {
 public:
  using PageCallback = std::function<void(const TeletextPageSnapshot&)>;

  TeletextPageDecoder();

  // Parse a page number string in the conventional magazine + two-hex-digit
  // form (e.g. "888", "100", "1F0"). Returns {displayed magazine 1-8, page
  // number 0x00-0xFF}, or std::nullopt when malformed.
  static std::optional<std::pair<int, int>> parse_page_number(
      std::string_view page);

  // Invoked for every completed page snapshot, in temporal order.
  void set_page_callback(PageCallback callback);

  /**
   * @brief Set the G0 set to display pages in when the service designates none
   *
   * ETSI EN 300 706 §15.2 designates the default G0 set in a packet X/28/0
   * Format 1 or M/29/0 and says that in their absence "the default sets are
   * established by a local Code of Practice" — the receiver's own region. A
   * Level 1 service transmits neither packet, so for such material this is the
   * only thing that can say the page is Cyrillic rather than Latin, and there
   * is nothing in the recording to check it against.
   *
   * A designation the service *does* transmit always wins, so setting this
   * cannot corrupt a page that says what it is. Latin, the default, is what
   * every page was displayed as before the Cyrillic sets existed.
   *
   * Takes effect on pages opened from here on; pages already assembled keep
   * the set they were opened with.
   */
  void set_default_g0_set(TeletextG0Set g0_set) { default_g0_set_ = g0_set; }
  TeletextG0Set default_g0_set() const { return default_g0_set_; }

  /**
   * @brief Set the second G0 set to toggle to when the service designates none
   *
   * The counterpart of set_default_g0_set() for the *second* G0 set of ETSI
   * EN 300 706 §15.3 — the one the ESC spacing attribute (§12.2 Table 26 code
   * 1/B) toggles a display row into and back out of, so that a service can mix
   * two alphabets on one page. §15.3 says the pair is "implied by a local Code
   * of Practice" on a transmission that sends no packet X/28 or M/29, which is
   * every Level 1 service, so it is a decoder setting for the same reason the
   * default set is.
   *
   * std::nullopt — the default — means the page has no second set and ESC is
   * ignored, which is what this decoder did before ESC was honoured at all. A
   * designation the service transmits always wins, including the 1111111 that
   * §15.3 defines as "no second G0 set required".
   *
   * Takes effect on pages opened from here on.
   */
  void set_default_second_g0_set(std::optional<TeletextG0Designation> second) {
    default_second_g0_set_ = second;
  }
  const std::optional<TeletextG0Designation>& default_second_g0_set() const {
    return default_second_g0_set_;
  }

  // Enable subtitle cue emission for one page ("888"-style string, see
  // parse_page_number()). Returns false and leaves the filter unset when the
  // string is malformed.
  bool set_subtitle_page(std::string_view page);

  /**
   * @brief Attach a squasher so repeated copies of a row correct each other
   *
   * With a squasher attached, every displayable row packet is recorded into
   * it under the page identity the packet was attributed to, and rendered
   * pages are built from the squashed rows rather than from the last copy
   * received. Because the squasher outlives any one decoder, this also lets
   * a page be assembled from several partial transmissions — a page whose
   * transmission was clipped still renders from rows recovered earlier.
   *
   * Copies are only combined within one *content run* of a sub-page, so a
   * page the service replaced is never blended with what replaced it. When a
   * transmission of the page ends, its display rows are compared with the
   * run's combined rows; a row has changed where eight or more characters
   * parity-clean in both differ.
   *
   * Neither header bit that bears on this (ETSI EN 300 706 §9.3.1.3 Table 2)
   * can be taken on trust: services set C4 (erase page) on every transmission
   * of pages they never change, and the 525-line recordings set C8 (update)
   * on a third of theirs. So a transmission sent with C4 starts a new run
   * unless kRepeatEvidenceRows or more rows could be compared and none had
   * changed. One sent without C4 starts a new run only where it arrived nearly
   * undamaged (kCleanTransmissionDivisor) and kContentChangeRows or more rows,
   * a third of those compared, had changed: a damaged transmission differs
   * from the run by its damage alone.
   *
   * A service rotating several screens under one sub-code brings each of them
   * round again, so before a new run is started the transmission is compared
   * with the page's earlier runs, most recent first, and rejoins the first it
   * repeats.
   *
   * The comparison needs the transmission's rows, so its copies are held back
   * from the squasher until the magazine's next header — a header re-sent
   * while the page continues settles what came before it too. A page rendered
   * before then shows the held rows from this decoder's own store. A consumer
   * replaying the stream hands this decoder the first pass's decisions (see
   * run_changes()) so that every packet lands in the same run both times.
   *
   * The squasher is not owned and must outlive the decoder. Pass nullptr to
   * detach. See teletext_row_squasher.h for the technique and its origin.
   */
  void set_row_squasher(TeletextRowSquasher* squasher) {
    row_squasher_ = squasher;
  }

  /**
   * @brief Say how the transmissions of a page number are filed into pages
   *
   * TeletextPageSeparation::kPageNumber, the default, is the standard's model
   * and everything above describes it.
   *
   * TeletextPageSeparation::kTransmission is for a service that sends a
   * rotation of screens under one page number with nothing in the header to
   * tell them apart — VirText, whose pages carried a rotation of screens to a
   * Zenith terminal at the cable head-end that showed whatever arrived. Each
   * transmission is then a screen in its own right, compared when it ends with
   * the screens the page has shown (see set_row_squasher() for how rows are
   * compared). One that changed no row of the screen on show stays with it,
   * however few rows it brought; otherwise it goes to the screen it differs
   * from least — the one on show, or an earlier one coming round again — where
   * that is by fewer than kContentChangeRows rows, and starts a new screen
   * where none is that close. Fewer changed rows than that is damage: on
   * VirText recordings a burst read as characters that pass parity rewrites a
   * row or two of a page that never changes, while the rotation's screens
   * differ from each other in five rows or more. And a difference counts
   * towards a row's eight only where one side of it draws something and the
   * received byte is not the held one read a bit early or late: read at a wrong
   * bit clock, as some recordings once were (see TeletextClockCalibrator), the
   * detector slips a bit part way along a packet, which turns spaces into the
   * spacing attribute 1/0 and solid mosaics into 3/F, both passing parity,
   * while what changes between screens is characters. Between screens that
   * differ from it equally, the one its rows have come in more often wins, so a
   * damaged copy that once started a screen of its own does not draw later
   * transmissions away from the screen it copies. The screens become the page's
   * versions, each combined only from its own copies.
   *
   * A screen's rows arrive in order, so a display row numbered below the last
   * one ends the transmission: it is the first row of a page whose header was
   * lost, and the rows that follow go to no page until the next header — they
   * would otherwise be taken for a new screen of the page before. And only
   * complete transmissions reach the page callback, because a screen's run is
   * known only once its rows have arrived.
   *
   * It also reads the pages a service sends in the Zenith VirText packet
   * layout (see the class comment), which on a standard service never occurs.
   * Needs a squasher for the screens to be told apart: without one there are
   * no copies to compare, and the page is assembled as always.
   */
  void set_page_separation(TeletextPageSeparation separation) {
    page_separation_ = separation;
  }
  TeletextPageSeparation page_separation() const { return page_separation_; }

  // Feed one T42 packet. |field_index| is the packet's temporal position in
  // fields; it must be monotonically non-decreasing across calls.
  //
  // |source| identifies this copy for a attached squasher: re-feeding the
  // same recovered line (as a sliding-window previewer does on every window
  // rebuild) must reuse its source so the copy is replaced rather than
  // counted again. The default derives a unique id per call, which is what a
  // one-pass consumer wants.
  //
  // |confidence| is how sure the recovery chain was of each byte, passed on to
  // an attached squasher so its vote can be weighted by it (see
  // teletext_row_squasher.h). nullptr — the default — means the caller cannot
  // say, and the copy votes at full weight.
  //
  // |packet_bytes| is how many of |packet| the service transmitted:
  // kTeletextPacketBytes on 625 lines, kTeletext525PacketBytes on 525 (the
  // byte_count of TeletextObservedPacket, or the packet_bytes of
  // TeletextLineResult). It sets how many display bytes a packet carries for
  // every page assembled from here on — a recording carries one service
  // throughout, and rows are rendered long after the packet that brought them,
  // so this has to be decoder state rather than something each stored row
  // carries. A short packet also enables row-extension decoding (see above).
  void process_packet(const std::array<uint8_t, kTeletextPacketBytes>& packet,
                      int64_t field_index, int64_t source = kAutoSource,
                      const TeletextPacketConfidence* confidence = nullptr,
                      size_t packet_bytes = kTeletextPacketBytes);

  /// Sentinel for process_packet()'s |source|: allocate a fresh copy id.
  static constexpr int64_t kAutoSource = -1;

  /// Display rows that must have changed for a transmission sent without C4
  /// to start a new content run, and the share of the compared rows they must
  /// be (a third; see set_row_squasher()). Three, so that a row or two the
  /// service rewrites on every transmission — a clock, a ticker — never splits
  /// a page whose other rows hold still.
  static constexpr int kContentChangeRows = 3;

  /// Rows that must be compared, and found unchanged, for a transmission sent
  /// with C4 to be taken as a repeat of the page rather than its replacement.
  static constexpr int kRepeatEvidenceRows = 2;

  /// A transmission sent without C4 may only start a run when no more than one
  /// display byte in this many failed parity (see set_row_squasher()).
  static constexpr size_t kCleanTransmissionDivisor = 50;

  /**
   * @brief Headers whose transmission moved its page to another content run
   *
   * The source id each header was fed with, mapped to the run (erase_epoch)
   * its page went to: a new one, or an earlier one the transmission repeated.
   * Recorded while a squasher is attached and no decisions are being replayed
   * (see replay_run_changes()).
   */
  const std::map<int64_t, int>& run_changes() const { return run_changes_; }

  /**
   * @brief Replay the content-run decisions of an earlier pass
   *
   * A consumer that feeds the same packets, under the same source ids, through
   * a second decoder sharing the squasher — the stream rewrite of the teletext
   * sink — passes the first decoder's run_changes() here. This decoder then
   * moves each page to the run the first did, at the header, and adds each
   * copy to the squasher as it arrives: the squasher already holds every copy,
   * so re-deciding against it would see a different picture from the one the
   * first pass saw. Not owned; nullptr returns to deciding.
   */
  void replay_run_changes(const std::map<int64_t, int>* changes) {
    replay_run_changes_ = changes;
  }

  /**
   * @brief Page identity the last process_packet() call was attributed to
   *
   * Set for displayable row packets (X/1 to X/24) that belonged to an open
   * page, cleared otherwise. Lets a consumer rewriting a packet stream ask
   * the squasher for the corrected form of the row it just fed in.
   */
  const std::optional<TeletextPageKey>& last_row_attribution() const {
    return last_row_attribution_;
  }
  /// Display row the last packet carried, valid when the above is set
  int last_row_number() const { return last_row_number_; }

  /**
   * @brief Page identity the last process_packet() call's row extension was
   * attributed to
   *
   * Set for a 525-line row-extension packet that belonged to an open page,
   * cleared otherwise. The packet carries the columns past the service's
   * display width — 32 to 39 — of the four display rows from
   * last_extension_first_row(), eight bytes to a row in row order (see the
   * class comment), and the squasher combines them with the rest of each row.
   * Lets a consumer rewriting a packet stream write that combined form back.
   */
  const std::optional<TeletextPageKey>& last_extension_attribution() const {
    return last_extension_attribution_;
  }
  /// First display row the last extension packet served, valid when the above
  /// is set
  int last_extension_first_row() const { return last_extension_first_row_; }

  /**
   * @brief Page identity a packet in the Zenith VirText layout was attributed
   * to, and where its display bytes sit on that page
   *
   * Set, in place of the two above, for a packet 1 to 22 of a page read in
   * the VirText layout (see the class comment), whose display bytes sit at
   * different offsets from a standard packet's: a row's 30 columns from the
   * packet's fourth byte, a packet 20/21's three groups of columns 30-39 after
   * its Hamming digit. The spans say where, so a consumer rewriting the stream
   * can write each row's combined form back without knowing the layout. A
   * packet that carries none — packet 22, or a packet 20/21 whose digit was
   * corrected — has no spans.
   */
  const std::optional<TeletextPageKey>& last_layout_attribution() const {
    return last_layout_attribution_;
  }
  /// The spans of the last packet, valid when the above is set
  const std::vector<TeletextPacketSpan>& last_layout_spans() const {
    return last_layout_spans_;
  }

  // Flush open page assemblies and close any open subtitle cue at
  // |end_field_index|.
  void finalize(int64_t end_field_index);

  /**
   * @brief Snapshot every page whose transmission is currently in progress
   *
   * Renders the pages that are open right now without terminating them, so
   * decoding can carry on with the packets that follow. This is what lets a
   * consumer feeding the decoder incrementally show a page as it arrives:
   * finalize() would answer the same question, but it closes the pages, and
   * rows arriving afterwards would then be dropped as orphans.
   *
   * The returned snapshots all carry transmission_complete == false.
   */
  std::vector<TeletextPageSnapshot> open_page_snapshots() const;

  // Subtitle cues emitted so far (closed cues only; an open cue is closed by
  // finalize() or by the page's clear/replace events).
  const std::vector<TeletextSubtitleCue>& subtitle_cues() const {
    return subtitle_cues_;
  }

 private:
  // Raw stored bytes of one page row (7-bit codes, parity removed).
  struct RowData {
    // A packet carrying this row's own display bytes was received: columns 0
    // to head_columns_ are what it brought.
    bool present = false;
    // A row-extension packet covering this row was received: columns
    // head_columns_ to columns_ are what it brought. Tracked apart from
    // |present| because the two arrive in different packets and either can go
    // missing on a tape.
    bool extension_present = false;
    std::array<uint8_t, TeletextPageSnapshot::kColumns> characters{};
    std::array<bool, TeletextPageSnapshot::kColumns> parity_error{};
  };

  // One row copy of the transmission in progress, held until the transmission
  // ends and it is known which content run it belongs to (see
  // set_row_squasher()).
  struct PendingRowCopy {
    int row = 0;
    TeletextRowBytes bytes{};
    int64_t source = 0;
    bool has_weights = false;
    TeletextRowConfidence weights{};
    size_t first_column = 0;
    size_t column_count = 0;
  };

  // Assembly state for one magazine. Row data is retained after a page is
  // emitted so a retransmission of the same page without C4 (erase) updates
  // rows incrementally (EN 300 706 §9.3.1.3 Table 2, C4).
  struct MagazineState {
    bool page_open = false;
    bool have_page = false;  // rows/identity below are meaningful
    int page_number = 0xFF;
    int subcode = 0;
    bool erase_page = false;
    bool newsflash = false;
    bool subtitle = false;
    bool suppress_header = false;
    bool update_indicator = false;
    bool interrupted_sequence = false;
    bool inhibit_display = false;
    bool magazine_serial = false;
    int national_option_subset = 0;
    // G0 set of the open page: the magazine's default (below) when the page
    // was opened, replaced by this page's own X/28/0 designation if one
    // arrives while it is open.
    TeletextG0Set g0_set = TeletextG0Set::Latin;
    // Second G0 set of the open page, resolved the same way (see
    // TeletextPageSnapshot::second_g0_set). Unset means ESC is inert on it.
    std::optional<TeletextG0Designation> second_g0_set;
    // Whether the two above came from a designation-0 packet (X/28/0), which
    // §9.4.2.2 gives precedence over the designation-4 packet (X/28/4) that
    // codes the same fields — so a later X/28/4 must not overwrite them.
    bool g0_set_from_designation_zero = false;
    // What an M/29/0 designated for the whole magazine, unset until one
    // arrives — at which point it supersedes the decoder's configured default
    // for every page of the magazine opened afterwards (§15.2).
    std::optional<TeletextG0Set> magazine_g0_set;
    // The magazine-wide second set, and whether an M/29 has been seen at all.
    // Two fields rather than a nested optional: "no M/29 yet, use the
    // configured default" and "an M/29 said this magazine has no second set"
    // are different answers and only the second one silences ESC.
    bool magazine_g0_received = false;
    std::optional<TeletextG0Designation> magazine_second_g0_set;
    // M/29/0 over M/29/4, as X/28/0 over X/28/4 above.
    bool magazine_g0_from_designation_zero = false;
    int64_t header_field_index = 0;
    int64_t last_field_index = 0;
    // Whether the header that opened this transmission arrived as codewords
    // throughout its addressing (see TeletextPageSnapshot::identity_attested).
    bool identity_attested = false;
    std::array<RowData, TeletextPageSnapshot::kRows> rows{};
    // Source id of the latest header of the page in progress — the one that
    // opened it or one re-sent while it continues — and whether it was sent
    // with C4 (see set_row_squasher()).
    int64_t fragment_source = 0;
    bool fragment_erase = false;
    // Row copies of the transmission in progress not yet given to the
    // squasher, and which rows they carry own display bytes for.
    std::vector<PendingRowCopy> pending;
    std::array<bool, TeletextPageSnapshot::kRows> row_pending{};
    // Whether the open page is read in the Zenith VirText layout (see the class
    // comment), decided at its header.
    bool virtext_layout = false;
    // The highest display row the transmission in progress has brought, by
    // transmission (see set_page_separation()); reset by every header.
    int last_display_row = 0;
  };

  void handle_header_packet(
      int transmission_magazine,
      const std::array<uint8_t, kTeletextPacketBytes>& packet,
      int64_t field_index, int64_t source);
  void handle_display_packet(
      int transmission_magazine, int row,
      const std::array<uint8_t, kTeletextPacketBytes>& packet,
      int64_t field_index, int64_t source,
      const TeletextPacketConfidence* confidence);
  // A 525-line row-extension packet: |packet_number| identifies the block of
  // four rows whose columns head_columns_ to kColumns it carries (see the class
  // comment).
  void handle_extension_packet(
      int transmission_magazine, int packet_number,
      const std::array<uint8_t, kTeletextPacketBytes>& packet,
      int64_t field_index, int64_t source,
      const TeletextPacketConfidence* confidence);
  // X/28 (page-specific) and M/29 (magazine-wide) character set designation,
  // ETSI EN 300 706 §9.4.2.2 Table 4, §15.2 Table 32 and §15.3 Table 33.
  // |magazine_wide| distinguishes the two and |designation_zero| the /0 packet
  // from the /4 one; everything else about reading the designations out of the
  // packet is identical, Table 4 coding both.
  void handle_character_set_designation(
      int transmission_magazine,
      const std::array<uint8_t, kTeletextPacketBytes>& packet,
      bool magazine_wide, bool designation_zero);
  // A packet 1-22 of a page read in the Zenith VirText layout (see the class
  // comment): a display row, a packet 20/21 carrying columns 30-39 of three
  // rows, or the packet 22 that precedes a new screen.
  void handle_virtext_packet(
      int transmission_magazine, int packet_number,
      const std::array<uint8_t, kTeletextPacketBytes>& packet,
      int64_t field_index, int64_t source,
      const TeletextPacketConfidence* confidence);

  // Weigh one packet's evidence for the Zenith VirText layout into the votes
  // of page |page_number| of displayed magazine |magazine|: |vote| is +1 for a
  // packet framed by 0x55, -1 for one holding characters that pass parity in
  // the frame's place, 0 for one that says neither (see the class comment).
  void vote_virtext_layout(int magazine, int page_number, int vote);

  // A sub-page's identity without the erase epoch, which is what the epoch
  // counter below is keyed on: {displayed magazine, page number, sub-code}.
  using PageIdentity = std::array<int, 3>;

  // The sub-code a header of page |page_number| of |transmission_magazine|
  // names, |read| being what its four sub-code bytes decoded to (see the class
  // comment): |read| where those bytes arrived as codewords, or name a
  // sub-code the page has been sent with so, or the header is the page's
  // first; otherwise |read| with the digits of the bytes that needed
  // correction taken from the page's last sub-code.
  int settled_subcode(int transmission_magazine, int page_number,
                      const std::array<uint8_t, kTeletextPacketBytes>& packet,
                      int read);

  // What each page number has been read with so far: the sub-code its last
  // header was given, and every sub-code a header of it carried as codewords.
  struct PageSubcodes {
    int last = 0;
    std::set<int> sent;
  };
  std::map<std::pair<int, int>, PageSubcodes> page_subcodes_;

  // Identity of the page currently open in |magazine|, for squasher keying.
  TeletextPageKey page_key(int transmission_magazine) const;

  // Display columns of the page grid (see TeletextPageSnapshot::columns).
  int columns_ = TeletextPageSnapshot::kColumns;

  // Display bytes one packet of the service carries: the packet length passed
  // to process_packet() less the MRAG. Equal to columns_ on 625 lines; 32 of
  // the 40 on 525, the rest arriving in row-extension packets.
  int head_columns_ = TeletextPageSnapshot::kColumns;

  // Columns the display packets of |state|'s page carry: head_columns_, or the
  // 30 of the Zenith VirText layout, whose packets 20 and 21 bring the rest.
  int head_columns(const MagazineState& state) const;

  // See set_page_separation().
  TeletextPageSeparation page_separation_ = TeletextPageSeparation::kPageNumber;

  // The Zenith VirText layout's votes, per {displayed magazine, page number},
  // held within +-kVirTextLayoutVoteBound (see vote_virtext_layout()).
  std::map<std::pair<int, int>, int> virtext_layout_votes_;

  // Magazines seen to carry pages of their own, which for 4 to 7 is what says
  // they are not row-extension carriers (see the class comment). Indices 0 to 3
  // are unused: a service's own magazines are never read as carriers.
  std::array<bool, 8> magazine_carries_pages_{};

  // Consecutive block-numbered packets seen from magazines 4 to 7 while it is
  // still unsettled whether they carry pages, counted to the threshold that
  // decides it and reset by any packet numbered otherwise.
  std::array<int, 8> magazine_extension_evidence_{};

  // Content run of |identity| (see set_row_squasher()), which is the
  // erase_epoch of its TeletextPageKey.
  int erase_epoch(const PageIdentity& identity) const;

  // Give the squasher a row copy of the open page of |transmission_magazine|:
  // at once when decisions are being replayed, otherwise held until the
  // magazine's next header.
  void record_row_copy(int transmission_magazine, PendingRowCopy copy);

  // Settle the rows of |transmission_magazine|'s open page held since its
  // latest header: decide whether they started a new content run and hand
  // them to the squasher under the run they belong to.
  void settle_content_run(int transmission_magazine);

  // Emit the open page of |magazine| (if any) through the callback and the
  // subtitle machinery, then mark it closed (row data retained).
  //
  // |transmission_complete| is false when the page is being closed only
  // because its own header was re-sent mid-transmission: the rows keep
  // coming, so what is emitted is a fragment and is flagged as one.
  void terminate_page(int transmission_magazine,
                      bool transmission_complete = true);

  TeletextPageSnapshot render_snapshot(int transmission_magazine,
                                       const MagazineState& state) const;

  // Subtitle cue lifecycle (design §6: arrival = display, erase/C6-clear =
  // clear, changed text = replace).
  void subtitle_page_completed(const TeletextPageSnapshot& snapshot);
  void subtitle_clear_event(int64_t field_index);
  static std::string extract_subtitle_text(
      const TeletextPageSnapshot& snapshot);

  std::array<MagazineState, 8> magazines_{};

  // The local Code of Practice; see set_default_g0_set() and
  // set_default_second_g0_set().
  TeletextG0Set default_g0_set_ = TeletextG0Set::Latin;
  std::optional<TeletextG0Designation> default_second_g0_set_;

  // The content run each sub-page is in, which is the erase_epoch of its
  // TeletextPageKey: without a squasher, how many C4 headers it has been
  // given; with one, how many runs it has started (see set_row_squasher()).
  // Kept per sub-page rather than per magazine so a new run of one page does
  // not orphan the copies of another page carried in the same magazine.
  std::map<PageIdentity, int> erase_epochs_;

  // Runs each sub-page has started so far (the next new one's erase_epoch),
  // kept beside erase_epochs_ because a page can return to an earlier run.
  std::map<PageIdentity, int> run_counts_;

  // See run_changes() and replay_run_changes().
  std::map<int64_t, int> run_changes_;
  const std::map<int64_t, int>* replay_run_changes_ = nullptr;

  // Not owned; see set_row_squasher().
  TeletextRowSquasher* row_squasher_ = nullptr;
  int64_t next_source_ = 0;
  std::optional<TeletextPageKey> last_row_attribution_;
  int last_row_number_ = 0;
  std::optional<TeletextPageKey> last_extension_attribution_;
  int last_extension_first_row_ = 0;
  std::optional<TeletextPageKey> last_layout_attribution_;
  std::vector<TeletextPacketSpan> last_layout_spans_;

  PageCallback page_callback_;

  // Subtitle filter (displayed magazine 1-8 + page number) and cue state.
  std::optional<std::pair<int, int>> subtitle_filter_;
  std::vector<TeletextSubtitleCue> subtitle_cues_;
  std::optional<TeletextSubtitleCue> open_cue_;
};

}  // namespace orc

#endif  // ORC_TELETEXT_PAGE_DECODER_H
