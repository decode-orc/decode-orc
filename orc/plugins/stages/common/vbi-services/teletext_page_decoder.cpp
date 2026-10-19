/*
 * File:        teletext_page_decoder.cpp
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     WST (System B) teletext magazine/page decoder producing
 *              Level 1 page snapshots and subtitle cues
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_page_decoder.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace orc {

namespace {

// ETSI EN 300 706 §7.1.2 / ITU-R BT.653 Table 1b §3.3: every packet opens with
// the two Hamming 8/4 magazine-and-row address bytes, so the display bytes of
// a row start after them on either service.
constexpr int kMragBytes = 2;

// ETSI EN 300 706 §9.3.1: a page header (X/0) spends its first ten bytes on
// addressing and control — the MRAG, the two page-number bytes, the four
// sub-code bytes and the two control-bit bytes. The header text follows, and
// is displayed from column 8: columns 0-7 carry the receiver's own page number
// and clock.
constexpr size_t kHeaderControlBytes = 10;
constexpr int kHeaderTextColumn = 8;

// §9.3.1.2: the four header bytes from this one carry the sub-code, S1 to S4,
// and the bits of the 13-bit sub-code each holds (S2 shares its byte with C4,
// S4 with C5 and C6).
constexpr size_t kSubcodeByte = 4;
constexpr std::array<int, 4> kSubcodeDigitBits = {0x000F, 0x0070, 0x0780,
                                                  0x1800};

// Magazine address bit that marks a 525-line row-extension packet (see the
// TeletextPageDecoder class comment): the service uses magazines 8, 1, 2 and 3
// for its pages and addresses each one's extension packets to that magazine
// with this bit set. Read only when the service's packets are short, so a
// 625-line stream's magazines 4-7 are untouched.
constexpr int kExtensionMagazineFlag = 0x4;

// Rows one extension packet serves: its display bytes are that many equal
// groups, one per row, in ascending row order.
constexpr int kExtensionPacketRows = 4;

// Packet numbers the row-extension scheme uses: one per block of four display
// rows, which is six for the 24 rows of a page (see extension_first_row()).
bool is_extension_block_number(int packet_number) {
  return packet_number == 1 ||
         (packet_number >= kExtensionPacketRows &&
          packet_number <= TeletextPageSnapshot::kRows - kExtensionPacketRows &&
          packet_number % kExtensionPacketRows == 0);
}

// Consecutive block-numbered packets a magazine 4-7 must send, without a header
// and without a packet numbered anything else, before it is taken to carry row
// extensions rather than pages of its own. One page's worth: a magazine sending
// only these six numbers, over and over, is not transmitting pages.
constexpr int kExtensionEvidencePackets = 6;

// First display row an extension packet numbered |packet_number| serves. The
// number identifies a block of kExtensionPacketRows rows rather than naming a
// row, so it rounds down: the observed packets are numbered 1, 4, 8, 12, 16 and
// 20 and serve rows 0-3, 4-7, 8-11, 12-15, 16-19 and 20-23. The first block
// cannot be numbered 0 — that is the page header's packet number — which is why
// its packets carry 1.
int extension_first_row(int packet_number) {
  return (packet_number / kExtensionPacketRows) * kExtensionPacketRows;
}

// The Zenith VirText packet layout (see the TeletextPageDecoder class comment).
// Every packet of a page in it ends with this byte, and a display row opens
// with it too: alternating bits, which fail the odd parity of ETSI EN 300 706
// §8.1, so it never stands for a character.
constexpr uint8_t kVirTextFrame = 0x55;
// The last byte of a 525-line packet, where the frame closes every packet.
constexpr size_t kVirTextClosingByte = kTeletext525PacketBytes - 1;
// Columns a display packet carries, after its opening frame byte.
constexpr int kVirTextColumns = 30;
// Rows of a page, the header among them: the 20 lines a VirText terminal
// showed ("40 characters on 20 lines", Electronic Technician/Dealer, August
// 1980, announcing Zenith's VirText decoders).
constexpr int kVirTextRows = 20;
// Packets 20 and 21 carry columns 30-39 of three rows each, ten bytes to a row
// after a Hamming 8/4 digit N; their first row is N on packet 20 and 16 + N on
// packet 21, and always a multiple of three.
constexpr int kVirTextExtensionPacket = 20;
constexpr int kVirTextExtensionRows = 3;
constexpr int kVirTextExtensionPacketRows = 16;
// Bound on a page's layout votes, so a page that changed layout part way
// through would be read in its new one after a few transmissions.
constexpr int kVirTextLayoutVoteBound = 16;

// The Zenith VirText layout's evidence in one 525-line packet: +1 where the
// byte at |opening| and the packet's last byte are both the frame byte, -1
// where both hold characters that pass parity, which a packet in that layout
// never has there, and 0 where damage leaves neither.
int virtext_layout_evidence(
    const std::array<uint8_t, kTeletextPacketBytes>& packet, size_t opening) {
  const uint8_t first = packet[opening];
  const uint8_t last = packet[kVirTextClosingByte];
  if (first == kVirTextFrame && last == kVirTextFrame) {
    return 1;
  }
  if (teletext_odd_parity_valid(first) && teletext_odd_parity_valid(last)) {
    return -1;
  }
  return 0;
}

// Whether a parity-clean display byte draws something: a character or mosaic,
// 2/1 to 7/F, rather than a space or a spacing attribute (ETSI EN 300 706
// §12.2 Table 26).
bool draws_something(uint8_t byte) { return (byte & 0x7F) > 0x20; }

// Whether |received| is |held| read one bit early or one bit late, as the
// detector reads every byte after a point part way along a packet where it
// slipped a bit. A byte is sent least significant bit first (EN 300 706 §6.2),
// so a bit early shifts it right and takes its top bit from the first bit of
// the byte after, and a bit late shifts it left and takes its bottom bit from
// the last bit of the byte before. |before| and |after| are those neighbours,
// or nullopt at the ends of the row, where the borrowed bit is not known.
bool slipped_by_a_bit(uint8_t received, uint8_t held,
                      std::optional<uint8_t> before,
                      std::optional<uint8_t> after) {
  const auto early = static_cast<uint8_t>(held >> 1);
  const auto late = static_cast<uint8_t>(held << 1);
  const bool read_early =
      after.has_value()
          ? received == static_cast<uint8_t>(early | ((*after & 0x01) << 7))
          : (received & 0x7F) == early;
  const bool read_late =
      before.has_value()
          ? received == static_cast<uint8_t>(late | (*before >> 7))
          : (received & 0xFE) == late;
  return read_early || read_late;
}

// ETSI EN 300 706 §9.3.1.1: page number 0xFF marks time-filling / page
// terminating headers ("null page" convention, §7.2.2 with sub-code 3F7F);
// such headers terminate a transmission but never open a page.
constexpr int kTimeFillingPageNumber = 0xFF;

// Spacing-attribute codes of ETSI EN 300 706 §12.2 Table 26 (7-bit values).
constexpr uint8_t kAlphaColourBase = 0x00;   // 0/0-0/7 alpha colours
constexpr uint8_t kFlash = 0x08;             // 0/8
constexpr uint8_t kSteady = 0x09;            // 0/9
constexpr uint8_t kEndBox = 0x0A;            // 0/A
constexpr uint8_t kStartBox = 0x0B;          // 0/B
constexpr uint8_t kNormalSize = 0x0C;        // 0/C
constexpr uint8_t kDoubleHeight = 0x0D;      // 0/D
constexpr uint8_t kMosaicColourBase = 0x10;  // 1/0-1/7 mosaic colours
constexpr uint8_t kConceal = 0x18;           // 1/8
constexpr uint8_t kContiguousMosaic = 0x19;  // 1/9
constexpr uint8_t kSeparatedMosaic = 0x1A;   // 1/A
constexpr uint8_t kEscape = 0x1B;            // 1/B ESC (or Switch)
constexpr uint8_t kBlackBackground = 0x1C;   // 1/C
constexpr uint8_t kNewBackground = 0x1D;     // 1/D
constexpr uint8_t kHoldMosaics = 0x1E;       // 1/E
constexpr uint8_t kReleaseMosaics = 0x1F;    // 1/F

int hex_digit_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Transmission magazine 0 is displayed as magazine 8 (ETSI EN 300 706 §3.1:
// page 800-8FF carries magazine address 0).
int displayed_magazine(int transmission_magazine) {
  return transmission_magazine == 0 ? 8 : transmission_magazine;
}

// The thirteen G0 positions a national option sub-set replaces
// (ETSI EN 300 706 §15.6.1 Table 35 NOTE 2 — the shaded positions).
constexpr std::array<uint8_t, 13> kNationalOptionPositions = {
    0x23, 0x24, 0x40, 0x5B, 0x5C, 0x5D, 0x5E,
    0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E};

// ETSI EN 300 706 §15.6.2 Table 36, the seven sub-sets a Level 1 page can
// reach through C12-C14, in TeletextNationalOption order. Columns follow
// kNationalOptionPositions.
//
// Two glyphs the standard draws rather than names take the nearest Unicode:
// English 6/0 is a horizontal line across the character rectangle (em dash
// here) and 7/C a double vertical line (U+2016). Note that the sub-sets which
// take 2/3 for a currency or accented character put the '#' the primary set
// holds there at 5/F instead, rather than dropping it.
constexpr std::array<std::array<char32_t, 13>, 7> kNationalOptionSubsets = {{
    // English
    {U'£', U'$', U'@', U'←', U'½', U'→', U'↑', U'#', U'—', U'¼', U'‖', U'¾',
     U'÷'},
    // German
    {U'#', U'$', U'§', U'Ä', U'Ö', U'Ü', U'^', U'_', U'°', U'ä', U'ö', U'ü',
     U'ß'},
    // Swedish/Finnish/Hungarian
    {U'#', U'¤', U'É', U'Ä', U'Ö', U'Å', U'Ü', U'_', U'é', U'ä', U'ö', U'å',
     U'ü'},
    // Italian
    {U'£', U'$', U'é', U'°', U'ç', U'→', U'↑', U'#', U'ù', U'à', U'ò', U'è',
     U'ì'},
    // French
    {U'é', U'ï', U'à', U'ë', U'ê', U'ù', U'î', U'#', U'è', U'â', U'ô', U'û',
     U'ç'},
    // Portuguese/Spanish
    {U'ç', U'$', U'¡', U'á', U'é', U'í', U'ó', U'ú', U'¿', U'ü', U'ñ', U'è',
     U'à'},
    // Czech/Slovak
    {U'#', U'ů', U'č', U'ť', U'ž', U'ý', U'í', U'ř', U'é', U'á', U'ě', U'ú',
     U'š'},
}};

// The three Cyrillic G0 primary sets, ETSI EN 300 706 §15.6.4 to §15.6.6
// Tables 38, 39 and 40, as the 96 code points of positions 2/0 to 7/F in
// ascending code order.
//
// Unlike the Latin set these reserve no positions for national option
// sub-sets: each is a complete alphabet in its own right, chosen by the
// character set *designation* rather than by the header's C12-C14 bits (§15.2
// Table 32, designation 0100). Every one of them therefore has to be written
// out in full — the letters are not an ASCII-compatible permutation of
// anything, and only positions 2/0-2/5, 2/7-2/9, 2/B-3/F agree with Latin.
//
// Common to all three (each table's NOTE 1 to NOTE 3):
//   - 2/0 is SPACE;
//   - 2/A is the asterisk, replaced by '@' only when the set is reached
//     through a packet X/26 column address triplet, which a Level 1 page has
//     no way to send;
//   - 7/F is a filled rectangle, as it is in the Latin set.

// Table 38 — Option 1, Serbian/Croatian. The Macedonian Ѓ/Ќ at 5/7 and 5/1
// come with it. The block at 7/F takes the position lowercase џ would have
// held, so this set has Џ and no џ; that is the table as printed, not an
// omission here.
constexpr std::array<char32_t, 96> kCyrillic1G0 = {
    /* 2/0 */ U' ', U'!', U'"', U'#', U'$', U'%', U'&', U'\'',
    /* 2/8 */ U'(', U')', U'*', U'+', U',', U'-', U'.', U'/',
    /* 3/0 */ U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7',
    /* 3/8 */ U'8', U'9', U':', U';', U'<', U'=', U'>', U'?',
    /* 4/0 */ U'Ч', U'А', U'Б', U'Ц', U'Д', U'Е', U'Ф', U'Г',
    /* 4/8 */ U'Х', U'И', U'Ј', U'К', U'Л', U'М', U'Н', U'О',
    /* 5/0 */ U'П', U'Ќ', U'Р', U'С', U'Т', U'У', U'В', U'Ѓ',
    /* 5/8 */ U'Љ', U'Њ', U'З', U'Ћ', U'Ж', U'Ђ', U'Ш', U'Џ',
    /* 6/0 */ U'ч', U'а', U'б', U'ц', U'д', U'е', U'ф', U'г',
    /* 6/8 */ U'х', U'и', U'ј', U'к', U'л', U'м', U'н', U'о',
    /* 7/0 */ U'п', U'ќ', U'р', U'с', U'т', U'у', U'в', U'ѓ',
    /* 7/8 */ U'љ', U'њ', U'з', U'ћ', U'ж', U'ђ', U'ш', U'■',
};

// Table 39 — Option 2, Russian/Bulgarian. The alphabet is laid out as KOI-7
// lays it out, except that Ъ and Ы are the other way round (5/9 and 5/F) and
// the block at 7/F displaces lowercase ы to 2/6, where the Latin set has '&'.
constexpr std::array<char32_t, 96> kCyrillic2G0 = {
    /* 2/0 */ U' ', U'!', U'"', U'#', U'$', U'%', U'ы', U'\'',
    /* 2/8 */ U'(', U')', U'*', U'+', U',', U'-', U'.', U'/',
    /* 3/0 */ U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7',
    /* 3/8 */ U'8', U'9', U':', U';', U'<', U'=', U'>', U'?',
    /* 4/0 */ U'Ю', U'А', U'Б', U'Ц', U'Д', U'Е', U'Ф', U'Г',
    /* 4/8 */ U'Х', U'И', U'Й', U'К', U'Л', U'М', U'Н', U'О',
    /* 5/0 */ U'П', U'Я', U'Р', U'С', U'Т', U'У', U'Ж', U'В',
    /* 5/8 */ U'Ь', U'Ъ', U'З', U'Ш', U'Э', U'Щ', U'Ч', U'Ы',
    /* 6/0 */ U'ю', U'а', U'б', U'ц', U'д', U'е', U'ф', U'г',
    /* 6/8 */ U'х', U'и', U'й', U'к', U'л', U'м', U'н', U'о',
    /* 7/0 */ U'п', U'я', U'р', U'с', U'т', U'у', U'ж', U'в',
    /* 7/8 */ U'ь', U'ъ', U'з', U'ш', U'э', U'щ', U'ч', U'■',
};

// Table 40 — Option 3, Ukrainian. Option 2's layout with the letters Ukrainian
// does not use replaced by the ones it does: І at 5/9 for Ъ, Є at 5/C for Э,
// Ї at 5/F for Ы, and ї at 2/6 for ы.
constexpr std::array<char32_t, 96> kCyrillic3G0 = {
    /* 2/0 */ U' ', U'!', U'"', U'#', U'$', U'%', U'ї', U'\'',
    /* 2/8 */ U'(', U')', U'*', U'+', U',', U'-', U'.', U'/',
    /* 3/0 */ U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7',
    /* 3/8 */ U'8', U'9', U':', U';', U'<', U'=', U'>', U'?',
    /* 4/0 */ U'Ю', U'А', U'Б', U'Ц', U'Д', U'Е', U'Ф', U'Г',
    /* 4/8 */ U'Х', U'И', U'Й', U'К', U'Л', U'М', U'Н', U'О',
    /* 5/0 */ U'П', U'Я', U'Р', U'С', U'Т', U'У', U'Ж', U'В',
    /* 5/8 */ U'Ь', U'І', U'З', U'Ш', U'Є', U'Щ', U'Ч', U'Ї',
    /* 6/0 */ U'ю', U'а', U'б', U'ц', U'д', U'е', U'ф', U'г',
    /* 6/8 */ U'х', U'и', U'й', U'к', U'л', U'м', U'н', U'о',
    /* 7/0 */ U'п', U'я', U'р', U'с', U'т', U'у', U'ж', U'в',
    /* 7/8 */ U'ь', U'і', U'з', U'ш', U'є', U'щ', U'ч', U'■',
};

// Lowest G0 code the tables above start at: everything below 2/0 is a spacing
// attribute rather than a character (§15.5).
constexpr uint8_t kFirstG0Code = 0x20;

const std::array<char32_t, 96>* cyrillic_table(TeletextG0Set g0_set) {
  switch (g0_set) {
    case TeletextG0Set::Cyrillic1:
      return &kCyrillic1G0;
    case TeletextG0Set::Cyrillic2:
      return &kCyrillic2G0;
    case TeletextG0Set::Cyrillic3:
      return &kCyrillic3G0;
    case TeletextG0Set::Latin:
      break;
  }
  return nullptr;
}

// The G0 set names the parameter surface and the project file use. They name
// the languages rather than the standard's option numbers, because "Cyrillic
// option 2" tells a user nothing about whether it is the one their recording
// needs.
struct G0SetName {
  TeletextG0Set set;
  const char* name;
};

constexpr std::array<G0SetName, 4> kG0SetNames = {{
    {TeletextG0Set::Latin, "Latin"},
    {TeletextG0Set::Cyrillic1, "Cyrillic (Serbian/Croatian)"},
    {TeletextG0Set::Cyrillic2, "Cyrillic (Russian/Bulgarian)"},
    {TeletextG0Set::Cyrillic3, "Cyrillic (Ukrainian)"},
}};

// ETSI EN 300 706 §15.2 Table 32: the G0 set a *Default G0 and G2 Character
// Set Designation and National Option Selection* value selects. |designation|
// is triplet 1 bits 14-11 and |national_option| bits 10-8.
//
// Only the Cyrillic rows are distinguished. Every other designation carries a
// Latin G0 set — including the four Latin rows of designation 0100 itself —
// and the national option sub-set of a Latin page keeps coming from the
// header's C12-C14 bits, as it does at Level 1 (§15.2). The Latin sub-sets
// that only some designations reach (Polish, Turkish, Estonian,
// Lettish/Lithuanian, Romanian) are not modelled; such a page displays in the
// designation-0000 sub-set its header bits name, which is what it did before.
TeletextG0Set g0_set_for_designation(int designation, int national_option) {
  if (designation == 0b0100) {
    switch (national_option) {
      case 0b000:
        return TeletextG0Set::Cyrillic1;
      case 0b100:
        return TeletextG0Set::Cyrillic2;
      case 0b101:
        return TeletextG0Set::Cyrillic3;
      default:
        break;
    }
  }
  return TeletextG0Set::Latin;
}

// ETSI EN 300 706 §15.3 Table 33: the G0 set and national option sub-set a
// *Second G0 Set Designation and National Option Selection* value selects.
// std::nullopt is the 1111111 the table gives as "no second G0 set required",
// which §15.3 says a decoder may read as disabling the ESC function for the
// page or magazine — and which is exactly what a service transmitting one
// alphabet sends, so honouring it costs nothing and stops a configured second
// set from being applied where the service has said there is none.
//
// Table 33 differs from Table 32 only in Latin rows this decoder does not
// distinguish anyway (Estonian, Lettish/Lithuanian, Rumanian and the like):
// the three Cyrillic rows sit at designation 0100 with national option bits
// 000, 100 and 101 in both, so g0_set_for_designation() reads either table.
std::optional<TeletextG0Designation> second_g0_for_designation(int value) {
  constexpr int kNoSecondG0Set = 0b1111111;
  if (value == kNoSecondG0Set) {
    return std::nullopt;
  }
  const int designation = (value >> 3) & 0xF;
  const int national_option = value & 0x7;
  return TeletextG0Designation{
      g0_set_for_designation(designation, national_option), national_option};
}

// Designation codes and packet numbers of the packets that can carry a
// character set designation (§9.1 Table 1, §9.4.2, §9.5).
constexpr int kPacketX26 = 26;
constexpr int kPacketX27 = 27;
constexpr int kPacketX28 = 28;
constexpr int kPacketM29 = 29;
constexpr int kDesignationCodeZero = 0;
// X/28/4 and M/29/4 code the same character set fields at the same bit
// positions as the /0 packets (§9.4.2.2: "The same coding also applies to
// packets X/28/4 except that they redefine CLUTs 0 and 1 instead of CLUTs 2
// and 3"), and the same clause gives the /0 packet precedence over the /4 one
// for everything but the colour map.
constexpr int kDesignationCodeFour = 4;

// X/28/0 carries Format 1 — the format Table 4 codes, and the only one with a
// character set designation in it — when triplet 1's Page Function bits say
// the page is a basic Level 1 teletext page (§9.4.2.1 Table 3, code 0000).
constexpr int kPageFunctionBasicLevel1 = 0;

// Whether a Hamming 8/4 byte arrived as a codeword rather than being corrected
// into one. §8.2 gives the code minimum distance 4: one error is corrected and
// two are detected, so a byte that needed correction is one channel error away
// from having been mis-corrected into a neighbouring codeword instead.
bool hamming84_uncorrected(uint8_t byte, int decoded) {
  return teletext_hamming84_encode(static_cast<uint8_t>(decoded)) == byte;
}

// Whether the header packet's addressing arrived as transmitted: the two MRAG
// bytes (§7.1.2) and the two page-number bytes (§9.3.1.1), which between them
// are the whole of a page's identity.
//
// A mis-corrected MRAG or page number does not damage a page — it invents one,
// at a number the service never transmitted, holding a copy of whatever page
// was really being sent. The catalogue cannot tell that from a real page by
// looking at it, so the evidence has to travel with the snapshot.
bool header_identity_attested(
    const std::array<uint8_t, kTeletextPacketBytes>& packet) {
  return teletext_hamming84_clean(packet[0]) &&
         teletext_hamming84_clean(packet[1]) &&
         teletext_hamming84_clean(packet[2]) &&
         teletext_hamming84_clean(packet[3]);
}

// Whether a packet may be believed at the address it arrived under.
//
// The MRAG is two Hamming 8/4 bytes and §8.2 gives that code minimum distance
// 4: a two-bit burst — the ordinary error on a band-limited recording rather
// than the exotic one — is not detected but silently resolved to a neighbouring
// codeword. A packet that reaches this decoder with a corrected MRAG is one
// channel error away from having been mis-corrected instead, and a
// mis-corrected MRAG does not damage the packet: it *moves* it, onto another
// magazine's open page or onto another row of this one, where it arrives
// looking exactly like content.
//
// Combining repeated rows is what makes that intolerable. A squashed row is a
// vote across the copies of one row, and the vote has no way to tell a copy of
// the row from a copy of something else: enough intruders and the page becomes
// a per-character blend of every page that was mis-addressed into it, which is
// worse the more copies are combined. So the address has to be evidence in its
// own right, and the only address that is evidence is one that arrived as
// transmitted.
//
// Two measurements set the rule. Over the reference SECAM capture's 228 839
// packets, rows X/1-23 arrive with both MRAG bytes uncorrected 78.4% of the
// time and X/24 on the pages that really carry FLOF labels 82.8% — but X/24 on
// every other page only 29.8%. Genuine traffic sits at the baseline; the
// deficit is packets that could only ever reach that row by being corrected
// into it. Over the 525-line reference capture's 102 685 packets, magazines 0
// and 2 — which that service never transmits a row on — receive 531 display
// packets between them, of which 530 have a corrected MRAG; they build
// catalogue pages holding nothing but other pages' rows, and a quarter of the
// identities that capture catalogues are pages of that kind.
//
// The cost is the genuine packets that arrived with a single-bit address
// error: about one row in twenty on that capture and about one in five on the
// noisier SECAM one. A page is re-sent for the length of a recording, so a
// refused row comes round again with it — and the packet is still written to
// the exported stream, which is a record of what was recovered rather than of
// what could be attributed.
bool address_attested(const std::array<uint8_t, kTeletextPacketBytes>& packet,
                      int mrag_low, int mrag_high) {
  return hamming84_uncorrected(packet[0], mrag_low) &&
         hamming84_uncorrected(packet[1], mrag_high);
}

// Whether an X/0 header is evidence that its magazine carries pages of its own
// rather than the row extensions of magazine M&3 (see the class comment).
//
// A header is otherwise exempt from address_attested(): a page opened at a
// mis-corrected number is recoverable afterwards, because its identity travels
// with the snapshot for the catalogue to reconcile. Nothing recovers this
// reading. It is made once, from one packet, and it holds for the rest of the
// recording — so it takes the strictest evidence a packet can offer, which is
// the whole of the identity arriving as codewords.
//
// The measurement is the 525-line reference capture. A corrupted packet 121
// packets into its 102 685 corrects into a header for a page of magazine 7,
// which is that service's extension carrier for magazine 3; the 1 991 extension
// packets that followed were read as another magazine's display rows, and every
// page of magazine 3 lost its columns 32-39 for the length of the recording.
// None of the six spurious headers on that capture arrives attested, and the
// genuine header that marks magazine 4 as page-carrying — the service really
// does transmit test pages there — does.
//
// The time-filling header is refused for the reason handle_header_packet()
// drops it: §7.3 makes it a terminator rather than a page, so a magazine that
// sends nothing else has still shown no page of its own.
bool header_claims_pages(
    const std::array<uint8_t, kTeletextPacketBytes>& packet) {
  if (!header_identity_attested(packet)) {
    return false;
  }
  const int units = teletext_hamming84_decode(packet[2]);
  const int tens = teletext_hamming84_decode(packet[3]);
  return ((tens << 4) | units) != kTimeFillingPageNumber;
}

}  // namespace

bool teletext_odd_parity_valid(uint8_t byte) {
  // ETSI EN 300 706 §8.1: accept when D1..D7 ⊕ P = 1 (odd number of set
  // bits over the whole byte).
  int ones = 0;
  for (int bit = 0; bit < 8; ++bit) {
    ones += (byte >> bit) & 1;
  }
  return (ones % 2) == 1;
}

uint8_t teletext_odd_parity_encode(uint8_t value) {
  uint8_t byte = value & 0x7F;
  int ones = 0;
  for (int bit = 0; bit < 7; ++bit) {
    ones += (byte >> bit) & 1;
  }
  // ETSI EN 300 706 §8.1: P = 1 ⊕ D1 ⊕ ... ⊕ D7.
  if ((ones % 2) == 0) {
    byte |= 0x80;
  }
  return byte;
}

std::string to_string(TeletextG0Set g0_set) {
  for (const G0SetName& entry : kG0SetNames) {
    if (entry.set == g0_set) {
      return entry.name;
    }
  }
  return kG0SetNames.front().name;
}

std::optional<TeletextG0Set> teletext_g0_set_from_string(
    std::string_view name) {
  for (const G0SetName& entry : kG0SetNames) {
    if (name == entry.name) {
      return entry.set;
    }
  }
  return std::nullopt;
}

char32_t teletext_g0_to_unicode(uint8_t code, TeletextG0Set g0_set,
                                int national_option_subset) {
  const uint8_t c = static_cast<uint8_t>(code & 0x7F);
  // Codes 0/0-1/F are spacing attributes, not characters (§15.5).
  if (c < kFirstG0Code) {
    return U' ';
  }

  // A Cyrillic set defines all 96 of its positions, so it is a lookup and the
  // national option sub-set has no part in it.
  if (const std::array<char32_t, 96>* table = cyrillic_table(g0_set);
      table != nullptr) {
    return (*table)[static_cast<size_t>(c - kFirstG0Code)];
  }

  // §15.6.1 Table 35 NOTE 4: 7/F is a rectangle filling the character area.
  if (c == 0x7F) {
    return U'■';
  }
  const size_t subset =
      national_option_subset >= 0 &&
              static_cast<size_t>(national_option_subset) <
                  kNationalOptionSubsets.size()
          ? static_cast<size_t>(national_option_subset)
          : 0;  // Table 32 designates no sub-set for 1 1 1; render as English
  for (size_t i = 0; i < kNationalOptionPositions.size(); ++i) {
    if (kNationalOptionPositions[i] == c) {
      return kNationalOptionSubsets[subset][i];
    }
  }
  // Every remaining Table 35 position coincides with ASCII.
  return static_cast<char32_t>(c);
}

std::string teletext_g0_to_utf8(uint8_t code, TeletextG0Set g0_set,
                                int national_option_subset) {
  return teletext_unicode_to_utf8(
      teletext_g0_to_unicode(code, g0_set, national_option_subset));
}

std::string teletext_unicode_to_utf8(char32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    // The G3 set's smooth mosaics lie beyond the Basic Multilingual Plane.
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

namespace {

// ETSI EN 300 706 §15.6.3 Table 37, the Latin G2 supplementary set, positions
// 2/0 to 7/F: ISO 6937's supplementary set with 5/6 to 5/8 added (NOTE 3),
// 5/6 the euro sign (NOTE 4), and 5/9 to 5/B and 6/5 never transmitted (NOTE
// 5). Column 4's diacritical marks are given in their spacing forms, as a mark
// called up on its own is drawn alone; dot below has none, and is a full stop.
constexpr std::array<char32_t, 96> kLatinG2 = {
    /* 2/0 */ U' ', U'¡', U'¢', U'£', U'$', U'¥', U'#', U'§',
    /* 2/8 */ U'¤', U'‘', U'“', U'«', U'←', U'↑', U'→', U'↓',
    /* 3/0 */ U'°', U'±', U'²', U'³', U'×', U'µ', U'¶', U'·',
    /* 3/8 */ U'÷', U'’', U'”', U'»', U'¼', U'½', U'¾', U'¿',
    /* 4/0 */ U' ', U'`', U'´', U'ˆ', U'˜', U'¯', U'˘', U'˙',
    /* 4/8 */ U'¨', U'.', U'˚', U'¸', U'_', U'˝', U'˛', U'ˇ',
    /* 5/0 */ U'―', U'¹', U'®', U'©', U'™', U'♪', U'€', U'‰',
    /* 5/8 */ U'α', 0,    0,    0,    U'⅛', U'⅜', U'⅝', U'⅞',
    /* 6/0 */ U'Ω', U'Æ', U'Đ', U'ª', U'Ħ', 0,    U'Ĳ', U'Ŀ',
    /* 6/8 */ U'Ł', U'Ø', U'Œ', U'º', U'Þ', U'Ŧ', U'Ŋ', U'ŉ',
    /* 7/0 */ U'ĸ', U'æ', U'đ', U'ð', U'ħ', U'ı', U'ĳ', U'ŀ',
    /* 7/8 */ U'ł', U'ø', U'œ', U'ß', U'þ', U'ŧ', U'ŋ', U'■',
};

// §15.6.7 Table 41, the Cyrillic G2 supplementary set: the Latin one's
// symbols, less 2/6 and 2/8 (NOTE 2), with the Latin letters the Cyrillic G0
// sets lack in columns 6 and 7.
constexpr std::array<char32_t, 96> kCyrillicG2 = {
    /* 2/0 */ U' ', U'¡', U'¢', U'£', U'$', U'¥', 0,    U'§',
    /* 2/8 */ 0,    U'‘', U'“', U'«', U'←', U'↑', U'→', U'↓',
    /* 3/0 */ U'°', U'±', U'²', U'³', U'×', U'µ', U'¶', U'·',
    /* 3/8 */ U'÷', U'’', U'”', U'»', U'¼', U'½', U'¾', U'¿',
    /* 4/0 */ U' ', U'`', U'´', U'ˆ', U'˜', U'¯', U'˘', U'˙',
    /* 4/8 */ U'¨', U'.', U'˚', U'¸', U'_', U'˝', U'˛', U'ˇ',
    /* 5/0 */ U'—', U'¹', U'®', U'©', U'™', U'♪', U'€', U'‰',
    /* 5/8 */ U'α', U'Ł', U'ł', U'β', U'⅛', U'⅜', U'⅝', U'⅞',
    /* 6/0 */ U'D', U'E', U'F', U'G', U'I', U'J', U'K', U'L',
    /* 6/8 */ U'N', U'Q', U'R', U'S', U'U', U'V', U'W', U'Z',
    /* 7/0 */ U'd', U'e', U'f', U'g', U'i', U'j', U'k', U'l',
    /* 7/8 */ U'n', U'q', U'r', U's', U'u', U'v', U'w', U'z',
};

// §15.7.2 Table 48, the G3 smooth mosaics and line drawing set, as Unicode
// 13 encodes it: the diagonal mosaics of the Symbols for Legacy Computing
// block, the half-cell triangles of Geometric Shapes, box drawing, and at 5/F
// the background alone (NOTE 1). 6/E, 6/F, 7/E and 7/F are not assigned.
constexpr std::array<char32_t, 96> kG3 = {
    // 2/0 to 2/F
    0x1FB3C, 0x1FB3D, 0x1FB3E, 0x1FB3F, 0x1FB40, 0x25E3, 0x1FB41, 0x1FB42,
    0x1FB43, 0x1FB44, 0x1FB45, 0x1FB46, 0x1FB68, 0x1FB69, 0x1FB70, 0x2592,
    // 3/0 to 3/F
    0x1FB47, 0x1FB48, 0x1FB49, 0x1FB4A, 0x1FB4B, 0x25E2, 0x1FB4C, 0x1FB4D,
    0x1FB4E, 0x1FB4F, 0x1FB50, 0x1FB51, 0x1FB6A, 0x1FB6B, 0x1FB75, 0x2588,
    // 4/0 to 4/F
    0x2537, 0x252F, 0x251D, 0x2525, 0x1FBA4, 0x1FBA5, 0x1FBA6, 0x1FBA7, 0x1FBA0,
    0x1FBA1, 0x1FBA2, 0x1FBA3, 0x253F, 0x2022, 0x25CF, 0x25CB,
    // 5/0 to 5/F
    0x2502, 0x2500, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C,
    0x2534, 0x253C, 0x2B62, 0x2B60, 0x2B61, 0x2B63, 0x00A0,
    // 6/0 to 6/F
    0x1FB52, 0x1FB53, 0x1FB54, 0x1FB55, 0x1FB56, 0x25E5, 0x1FB57, 0x1FB58,
    0x1FB59, 0x1FB5A, 0x1FB5B, 0x1FB5C, 0x1FB6C, 0x1FB6D, 0, 0,
    // 7/0 to 7/F
    0x1FB5D, 0x1FB5E, 0x1FB5F, 0x1FB60, 0x1FB61, 0x25E4, 0x1FB62, 0x1FB63,
    0x1FB64, 0x1FB65, 0x1FB66, 0x1FB67, 0x1FB6E, 0x1FB6F, 0, 0};

// A G0 letter and one of the fifteen marks of G2 column 4 (in order: grave,
// acute, circumflex, tilde, macron, breve, dot above, diaeresis, dot below,
// ring, cedilla, low line, double acute, ogonek, caron), and the single
// Unicode character the two make: every pair Unicode's canonical composition
// gives one for, over the letters of the Latin and Cyrillic G0 sets. Sorted.
struct Composition {
  char32_t base;
  uint8_t mark;
  char32_t composed;
};
constexpr Composition kCompositions[] = {
    {0x0041, 1, 0x00C0},  {0x0041, 2, 0x00C1},  {0x0041, 3, 0x00C2},
    {0x0041, 4, 0x00C3},  {0x0041, 5, 0x0100},  {0x0041, 6, 0x0102},
    {0x0041, 7, 0x0226},  {0x0041, 8, 0x00C4},  {0x0041, 9, 0x1EA0},
    {0x0041, 10, 0x00C5}, {0x0041, 14, 0x0104}, {0x0041, 15, 0x01CD},
    {0x0042, 7, 0x1E02},  {0x0042, 9, 0x1E04},  {0x0043, 2, 0x0106},
    {0x0043, 3, 0x0108},  {0x0043, 7, 0x010A},  {0x0043, 11, 0x00C7},
    {0x0043, 15, 0x010C}, {0x0044, 7, 0x1E0A},  {0x0044, 9, 0x1E0C},
    {0x0044, 11, 0x1E10}, {0x0044, 15, 0x010E}, {0x0045, 1, 0x00C8},
    {0x0045, 2, 0x00C9},  {0x0045, 3, 0x00CA},  {0x0045, 4, 0x1EBC},
    {0x0045, 5, 0x0112},  {0x0045, 6, 0x0114},  {0x0045, 7, 0x0116},
    {0x0045, 8, 0x00CB},  {0x0045, 9, 0x1EB8},  {0x0045, 11, 0x0228},
    {0x0045, 14, 0x0118}, {0x0045, 15, 0x011A}, {0x0046, 7, 0x1E1E},
    {0x0047, 2, 0x01F4},  {0x0047, 3, 0x011C},  {0x0047, 5, 0x1E20},
    {0x0047, 6, 0x011E},  {0x0047, 7, 0x0120},  {0x0047, 11, 0x0122},
    {0x0047, 15, 0x01E6}, {0x0048, 3, 0x0124},  {0x0048, 7, 0x1E22},
    {0x0048, 8, 0x1E26},  {0x0048, 9, 0x1E24},  {0x0048, 11, 0x1E28},
    {0x0048, 15, 0x021E}, {0x0049, 1, 0x00CC},  {0x0049, 2, 0x00CD},
    {0x0049, 3, 0x00CE},  {0x0049, 4, 0x0128},  {0x0049, 5, 0x012A},
    {0x0049, 6, 0x012C},  {0x0049, 7, 0x0130},  {0x0049, 8, 0x00CF},
    {0x0049, 9, 0x1ECA},  {0x0049, 14, 0x012E}, {0x0049, 15, 0x01CF},
    {0x004A, 3, 0x0134},  {0x004B, 2, 0x1E30},  {0x004B, 9, 0x1E32},
    {0x004B, 11, 0x0136}, {0x004B, 15, 0x01E8}, {0x004C, 2, 0x0139},
    {0x004C, 9, 0x1E36},  {0x004C, 11, 0x013B}, {0x004C, 15, 0x013D},
    {0x004D, 2, 0x1E3E},  {0x004D, 7, 0x1E40},  {0x004D, 9, 0x1E42},
    {0x004E, 1, 0x01F8},  {0x004E, 2, 0x0143},  {0x004E, 4, 0x00D1},
    {0x004E, 7, 0x1E44},  {0x004E, 9, 0x1E46},  {0x004E, 11, 0x0145},
    {0x004E, 15, 0x0147}, {0x004F, 1, 0x00D2},  {0x004F, 2, 0x00D3},
    {0x004F, 3, 0x00D4},  {0x004F, 4, 0x00D5},  {0x004F, 5, 0x014C},
    {0x004F, 6, 0x014E},  {0x004F, 7, 0x022E},  {0x004F, 8, 0x00D6},
    {0x004F, 9, 0x1ECC},  {0x004F, 13, 0x0150}, {0x004F, 14, 0x01EA},
    {0x004F, 15, 0x01D1}, {0x0050, 2, 0x1E54},  {0x0050, 7, 0x1E56},
    {0x0052, 2, 0x0154},  {0x0052, 7, 0x1E58},  {0x0052, 9, 0x1E5A},
    {0x0052, 11, 0x0156}, {0x0052, 15, 0x0158}, {0x0053, 2, 0x015A},
    {0x0053, 3, 0x015C},  {0x0053, 7, 0x1E60},  {0x0053, 9, 0x1E62},
    {0x0053, 11, 0x015E}, {0x0053, 15, 0x0160}, {0x0054, 7, 0x1E6A},
    {0x0054, 9, 0x1E6C},  {0x0054, 11, 0x0162}, {0x0054, 15, 0x0164},
    {0x0055, 1, 0x00D9},  {0x0055, 2, 0x00DA},  {0x0055, 3, 0x00DB},
    {0x0055, 4, 0x0168},  {0x0055, 5, 0x016A},  {0x0055, 6, 0x016C},
    {0x0055, 8, 0x00DC},  {0x0055, 9, 0x1EE4},  {0x0055, 10, 0x016E},
    {0x0055, 13, 0x0170}, {0x0055, 14, 0x0172}, {0x0055, 15, 0x01D3},
    {0x0056, 4, 0x1E7C},  {0x0056, 9, 0x1E7E},  {0x0057, 1, 0x1E80},
    {0x0057, 2, 0x1E82},  {0x0057, 3, 0x0174},  {0x0057, 7, 0x1E86},
    {0x0057, 8, 0x1E84},  {0x0057, 9, 0x1E88},  {0x0058, 7, 0x1E8A},
    {0x0058, 8, 0x1E8C},  {0x0059, 1, 0x1EF2},  {0x0059, 2, 0x00DD},
    {0x0059, 3, 0x0176},  {0x0059, 4, 0x1EF8},  {0x0059, 5, 0x0232},
    {0x0059, 7, 0x1E8E},  {0x0059, 8, 0x0178},  {0x0059, 9, 0x1EF4},
    {0x005A, 2, 0x0179},  {0x005A, 3, 0x1E90},  {0x005A, 7, 0x017B},
    {0x005A, 9, 0x1E92},  {0x005A, 15, 0x017D}, {0x0061, 1, 0x00E0},
    {0x0061, 2, 0x00E1},  {0x0061, 3, 0x00E2},  {0x0061, 4, 0x00E3},
    {0x0061, 5, 0x0101},  {0x0061, 6, 0x0103},  {0x0061, 7, 0x0227},
    {0x0061, 8, 0x00E4},  {0x0061, 9, 0x1EA1},  {0x0061, 10, 0x00E5},
    {0x0061, 14, 0x0105}, {0x0061, 15, 0x01CE}, {0x0062, 7, 0x1E03},
    {0x0062, 9, 0x1E05},  {0x0063, 2, 0x0107},  {0x0063, 3, 0x0109},
    {0x0063, 7, 0x010B},  {0x0063, 11, 0x00E7}, {0x0063, 15, 0x010D},
    {0x0064, 7, 0x1E0B},  {0x0064, 9, 0x1E0D},  {0x0064, 11, 0x1E11},
    {0x0064, 15, 0x010F}, {0x0065, 1, 0x00E8},  {0x0065, 2, 0x00E9},
    {0x0065, 3, 0x00EA},  {0x0065, 4, 0x1EBD},  {0x0065, 5, 0x0113},
    {0x0065, 6, 0x0115},  {0x0065, 7, 0x0117},  {0x0065, 8, 0x00EB},
    {0x0065, 9, 0x1EB9},  {0x0065, 11, 0x0229}, {0x0065, 14, 0x0119},
    {0x0065, 15, 0x011B}, {0x0066, 7, 0x1E1F},  {0x0067, 2, 0x01F5},
    {0x0067, 3, 0x011D},  {0x0067, 5, 0x1E21},  {0x0067, 6, 0x011F},
    {0x0067, 7, 0x0121},  {0x0067, 11, 0x0123}, {0x0067, 15, 0x01E7},
    {0x0068, 3, 0x0125},  {0x0068, 7, 0x1E23},  {0x0068, 8, 0x1E27},
    {0x0068, 9, 0x1E25},  {0x0068, 11, 0x1E29}, {0x0068, 15, 0x021F},
    {0x0069, 1, 0x00EC},  {0x0069, 2, 0x00ED},  {0x0069, 3, 0x00EE},
    {0x0069, 4, 0x0129},  {0x0069, 5, 0x012B},  {0x0069, 6, 0x012D},
    {0x0069, 8, 0x00EF},  {0x0069, 9, 0x1ECB},  {0x0069, 14, 0x012F},
    {0x0069, 15, 0x01D0}, {0x006A, 3, 0x0135},  {0x006A, 15, 0x01F0},
    {0x006B, 2, 0x1E31},  {0x006B, 9, 0x1E33},  {0x006B, 11, 0x0137},
    {0x006B, 15, 0x01E9}, {0x006C, 2, 0x013A},  {0x006C, 9, 0x1E37},
    {0x006C, 11, 0x013C}, {0x006C, 15, 0x013E}, {0x006D, 2, 0x1E3F},
    {0x006D, 7, 0x1E41},  {0x006D, 9, 0x1E43},  {0x006E, 1, 0x01F9},
    {0x006E, 2, 0x0144},  {0x006E, 4, 0x00F1},  {0x006E, 7, 0x1E45},
    {0x006E, 9, 0x1E47},  {0x006E, 11, 0x0146}, {0x006E, 15, 0x0148},
    {0x006F, 1, 0x00F2},  {0x006F, 2, 0x00F3},  {0x006F, 3, 0x00F4},
    {0x006F, 4, 0x00F5},  {0x006F, 5, 0x014D},  {0x006F, 6, 0x014F},
    {0x006F, 7, 0x022F},  {0x006F, 8, 0x00F6},  {0x006F, 9, 0x1ECD},
    {0x006F, 13, 0x0151}, {0x006F, 14, 0x01EB}, {0x006F, 15, 0x01D2},
    {0x0070, 2, 0x1E55},  {0x0070, 7, 0x1E57},  {0x0072, 2, 0x0155},
    {0x0072, 7, 0x1E59},  {0x0072, 9, 0x1E5B},  {0x0072, 11, 0x0157},
    {0x0072, 15, 0x0159}, {0x0073, 2, 0x015B},  {0x0073, 3, 0x015D},
    {0x0073, 7, 0x1E61},  {0x0073, 9, 0x1E63},  {0x0073, 11, 0x015F},
    {0x0073, 15, 0x0161}, {0x0074, 7, 0x1E6B},  {0x0074, 8, 0x1E97},
    {0x0074, 9, 0x1E6D},  {0x0074, 11, 0x0163}, {0x0074, 15, 0x0165},
    {0x0075, 1, 0x00F9},  {0x0075, 2, 0x00FA},  {0x0075, 3, 0x00FB},
    {0x0075, 4, 0x0169},  {0x0075, 5, 0x016B},  {0x0075, 6, 0x016D},
    {0x0075, 8, 0x00FC},  {0x0075, 9, 0x1EE5},  {0x0075, 10, 0x016F},
    {0x0075, 13, 0x0171}, {0x0075, 14, 0x0173}, {0x0075, 15, 0x01D4},
    {0x0076, 4, 0x1E7D},  {0x0076, 9, 0x1E7F},  {0x0077, 1, 0x1E81},
    {0x0077, 2, 0x1E83},  {0x0077, 3, 0x0175},  {0x0077, 7, 0x1E87},
    {0x0077, 8, 0x1E85},  {0x0077, 9, 0x1E89},  {0x0077, 10, 0x1E98},
    {0x0078, 7, 0x1E8B},  {0x0078, 8, 0x1E8D},  {0x0079, 1, 0x1EF3},
    {0x0079, 2, 0x00FD},  {0x0079, 3, 0x0177},  {0x0079, 4, 0x1EF9},
    {0x0079, 5, 0x0233},  {0x0079, 7, 0x1E8F},  {0x0079, 8, 0x00FF},
    {0x0079, 9, 0x1EF5},  {0x0079, 10, 0x1E99}, {0x007A, 2, 0x017A},
    {0x007A, 3, 0x1E91},  {0x007A, 7, 0x017C},  {0x007A, 9, 0x1E93},
    {0x007A, 15, 0x017E}, {0x0406, 8, 0x0407},  {0x0410, 6, 0x04D0},
    {0x0410, 8, 0x04D2},  {0x0413, 2, 0x0403},  {0x0415, 1, 0x0400},
    {0x0415, 6, 0x04D6},  {0x0415, 8, 0x0401},  {0x0416, 6, 0x04C1},
    {0x0416, 8, 0x04DC},  {0x0417, 8, 0x04DE},  {0x0418, 1, 0x040D},
    {0x0418, 5, 0x04E2},  {0x0418, 6, 0x0419},  {0x0418, 8, 0x04E4},
    {0x041A, 2, 0x040C},  {0x041E, 8, 0x04E6},  {0x0423, 5, 0x04EE},
    {0x0423, 6, 0x040E},  {0x0423, 8, 0x04F0},  {0x0423, 13, 0x04F2},
    {0x0427, 8, 0x04F4},  {0x042B, 8, 0x04F8},  {0x042D, 8, 0x04EC},
    {0x0430, 6, 0x04D1},  {0x0430, 8, 0x04D3},  {0x0433, 2, 0x0453},
    {0x0435, 1, 0x0450},  {0x0435, 6, 0x04D7},  {0x0435, 8, 0x0451},
    {0x0436, 6, 0x04C2},  {0x0436, 8, 0x04DD},  {0x0437, 8, 0x04DF},
    {0x0438, 1, 0x045D},  {0x0438, 5, 0x04E3},  {0x0438, 6, 0x0439},
    {0x0438, 8, 0x04E5},  {0x043A, 2, 0x045C},  {0x043E, 8, 0x04E7},
    {0x0443, 5, 0x04EF},  {0x0443, 6, 0x045E},  {0x0443, 8, 0x04F1},
    {0x0443, 13, 0x04F3}, {0x0447, 8, 0x04F5},  {0x044B, 8, 0x04F9},
    {0x044D, 8, 0x04ED},  {0x0456, 8, 0x0457}};

}  // namespace

char32_t teletext_g2_to_unicode(uint8_t code, TeletextG0Set g0_set) {
  if (code < kFirstG0Code || code > 0x7F) {
    return 0;
  }
  const auto& table = g0_set == TeletextG0Set::Latin ? kLatinG2 : kCyrillicG2;
  return table[static_cast<size_t>(code - kFirstG0Code)];
}

char32_t teletext_g3_to_unicode(uint8_t code) {
  if (code < kFirstG0Code || code > 0x7F) {
    return 0;
  }
  return kG3[static_cast<size_t>(code - kFirstG0Code)];
}

char32_t teletext_x26_g0_to_unicode(uint8_t code, int mark,
                                    TeletextG0Set g0_set) {
  if (code < kFirstG0Code || code > 0x7F) {
    return 0;
  }
  // §12.3.4: with no mark, 2/A is the commercial at.
  if (mark == 0 && code == 0x2A) {
    return U'@';
  }
  char32_t base = 0;
  if (const auto* table = cyrillic_table(g0_set); table != nullptr) {
    base = (*table)[static_cast<size_t>(code - kFirstG0Code)];
  } else {
    // Table 35's own characters at the national option positions (NOTE 2):
    // ISO 646's, but for the currency sign at 2/4, the broken bar at 7/C and
    // the block at 7/F.
    switch (code) {
      case 0x24:
        base = U'¤';
        break;
      case 0x7C:
        base = U'¦';
        break;
      case 0x7F:
        base = U'■';
        break;
      default:
        base = static_cast<char32_t>(code);
        break;
    }
  }
  if (mark <= 0 || mark > 15) {
    return base;
  }
  const auto* end = std::end(kCompositions);
  const auto* found = std::lower_bound(
      std::begin(kCompositions), end,
      Composition{base, static_cast<uint8_t>(mark), 0},
      [](const Composition& lhs, const Composition& rhs) {
        return lhs.base != rhs.base ? lhs.base < rhs.base : lhs.mark < rhs.mark;
      });
  return found != end && found->base == base && found->mark == mark
             ? found->composed
             : base;
}

TeletextPageDecoder::TeletextPageDecoder() = default;

std::optional<std::pair<int, int>> TeletextPageDecoder::parse_page_number(
    std::string_view page) {
  if (page.size() != 3) {
    return std::nullopt;
  }
  const char magazine_char = page[0];
  if (magazine_char < '1' || magazine_char > '8') {
    return std::nullopt;
  }
  const int tens = hex_digit_value(page[1]);
  const int units = hex_digit_value(page[2]);
  if (tens < 0 || units < 0) {
    return std::nullopt;
  }
  return std::make_pair(magazine_char - '0', (tens << 4) | units);
}

void TeletextPageDecoder::set_page_callback(PageCallback callback) {
  page_callback_ = std::move(callback);
}

bool TeletextPageDecoder::set_subtitle_page(std::string_view page) {
  const auto parsed = parse_page_number(page);
  if (!parsed.has_value()) {
    return false;
  }
  subtitle_filter_ = parsed;
  return true;
}

void TeletextPageDecoder::process_packet(
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index, int64_t source,
    const TeletextPacketConfidence* confidence, size_t packet_bytes) {
  last_row_attribution_.reset();
  last_row_number_ = 0;
  last_extension_attribution_.reset();
  last_extension_first_row_ = 0;
  last_layout_attribution_.reset();
  last_layout_spans_.clear();

  // Display bytes one packet of this service carries: 42 bytes less the MRAG
  // gives the 40 of EN 300 706 §9.3.2, 34 gives the 32 of ITU-R BT.653
  // Table 1b §3.4. The grid stays 40 columns either way: what a short packet
  // does not fill comes from the row-extension packets, or shows as spaces.
  head_columns_ = std::clamp(static_cast<int>(packet_bytes) - kMragBytes, 0,
                             TeletextPageSnapshot::kColumns);

  // MRAG: two Hamming 8/4 bytes carrying the 3-bit magazine and 5-bit packet
  // number (ETSI EN 300 706 §7.1.2). An uncorrectable MRAG byte means the
  // packet cannot be attributed; drop it.
  const int mrag_low = teletext_hamming84_decode(packet[0]);
  const int mrag_high = teletext_hamming84_decode(packet[1]);
  if (mrag_low < 0 || mrag_high < 0) {
    return;
  }
  const int magazine = mrag_low & 0x7;
  const int packet_number = ((mrag_low >> 3) & 0x1) | (mrag_high << 1);

  // Everything but a header has to have arrived at the address it claims (see
  // address_attested()). A header is exempt because its own identity is
  // checked further on and travels with the snapshot for the catalogue to
  // reconcile — a page opened at a mis-corrected number is recoverable
  // afterwards, where a row filed against the wrong page is not.
  if (packet_number != 0 && !address_attested(packet, mrag_low, mrag_high)) {
    return;
  }

  if (head_columns_ < TeletextPageSnapshot::kColumns &&
      (magazine & kExtensionMagazineFlag) != 0) {
    // A magazine 4-7 either carries pages of its own or carries the row
    // extensions of magazine M&3, and an X/0 header is what tells the two apart
    // (see the class comment). An extension carrier never sends one, so a
    // header here is both a page and the evidence that this magazine has them.
    const auto index = static_cast<size_t>(magazine);
    const bool settled =
        magazine_extension_evidence_[index] >= kExtensionEvidencePackets;
    // An X/0 says this magazine has pages of its own — but only while that is
    // still an open question, and only from a header that arrived as
    // transmitted (see header_claims_pages()). A magazine already shown to
    // carry extensions does not acquire pages, and reading one mis-corrected
    // address as though it had would disable its extensions for the rest of the
    // recording.
    if (!settled && packet_number == 0 && header_claims_pages(packet)) {
      magazine_carries_pages_[index] = true;
    }
    if (!magazine_carries_pages_[index]) {
      if (!settled) {
        // Still deciding. A carrier sends nothing but block numbers; anything
        // else is a display row, so the evidence starts again — a page
        // magazine's rows include the block numbers, which is why one of those
        // alone proves nothing, and only an unbroken run of them settles it.
        //
        // Until it is settled the packets are discarded rather than guessed at:
        // a row extension written to the wrong page stays in the record, since
        // a squasher keeps every copy it is given, while one discarded here
        // comes round again with the page.
        if (is_extension_block_number(packet_number)) {
          ++magazine_extension_evidence_[index];
        } else {
          magazine_extension_evidence_[index] = 0;
        }
        return;
      }
      // Settled, and it stays settled. A magazine that has shown it carries no
      // pages does not acquire them, so a packet numbered anything but a block
      // is a mis-read address (the MRAG is Hamming 8/4, which mis-corrects on a
      // burst) — dropped, where before it restarted the evidence and cost the
      // six packets that followed it, which is a whole page's extensions.
      if (is_extension_block_number(packet_number)) {
        handle_extension_packet(magazine & ~kExtensionMagazineFlag,
                                packet_number, packet, field_index,
                                source == kAutoSource ? next_source_++ : source,
                                confidence);
      }
      return;
    }
  }

  if (packet_number == 0) {
    handle_header_packet(magazine, packet, field_index,
                         source == kAutoSource ? next_source_++ : source);
  } else if (packet_number >= 1 &&
             packet_number < TeletextPageSnapshot::kRows) {
    // X/1 to X/24: directly displayable rows (EN 300 706 §9.3.2). X/25
    // (key-word search labels) is outside the grid, and X/30-X/31 carry data
    // rather than pages (§9.7-§9.8), which a decoder of pages ignores.
    handle_display_packet(magazine, packet_number, packet, field_index,
                          source == kAutoSource ? next_source_++ : source,
                          confidence);
  } else if (packet_number == kPacketX26) {
    // The characters a Level 1.5 page adds to its grid (§9.4.1, §12.3).
    handle_enhancement_packet(magazine, packet);
  } else if (packet_number == kPacketX27) {
    // The pages this one links to (§9.6.1).
    handle_link_packet(magazine, packet);
  } else if (packet_number == kPacketX28 || packet_number == kPacketM29) {
    // Not display data, but the one thing outside the Level 1 grid that
    // changes how the grid is read: which G0 sets its codes are in, and so
    // whether ESC has anything to toggle to (§15.2, §15.3).
    const int designation_code = teletext_hamming84_decode(packet[2]);
    if (designation_code == kDesignationCodeZero ||
        designation_code == kDesignationCodeFour) {
      handle_character_set_designation(
          magazine, packet, packet_number == kPacketM29,
          designation_code == kDesignationCodeZero);
    }
  }
}

void TeletextPageDecoder::handle_enhancement_packet(
    int transmission_magazine,
    const std::array<uint8_t, kTeletextPacketBytes>& packet) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  if (!state.page_open) {
    return;  // a packet X/26 belongs to the page it follows (§9.4.1)
  }
  // Byte 6 the designation code, 0 to 15, then thirteen Hamming 24/18
  // triplets — fewer on 525 lines, whose packet is shorter. As with X/28 (see
  // handle_character_set_designation()), the packet is believed only when
  // every triplet decodes: one that was damaged comes round again with the
  // page.
  const int designation = teletext_hamming84_decode(packet[2]);
  if (designation < 0) {
    return;
  }
  const size_t packet_bytes = static_cast<size_t>(head_columns_) + 2;
  std::array<int32_t, 13> triplets{};
  triplets.fill(-1);
  for (size_t i = 0; i < triplets.size(); ++i) {
    const size_t byte = 3 + 3 * i;
    if (byte + 3 > packet_bytes) {
      break;
    }
    triplets[i] = teletext_hamming2418_decode(packet[byte], packet[byte + 1],
                                              packet[byte + 2]);
    if (triplets[i] < 0) {
      return;
    }
  }
  state.enhancements[static_cast<size_t>(designation)] = triplets;
}

void TeletextPageDecoder::handle_link_packet(
    int transmission_magazine,
    const std::array<uint8_t, kTeletextPacketBytes>& packet) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  // Only X/27/0 carries the links a reader follows and the row 24 flag
  // (§9.6.1, Annex H): /1 to /3 name more pages for a receiver to acquire,
  // and /4 up link Level 2.5 objects. It needs the whole 625-line packet.
  if (!state.page_open || head_columns_ < TeletextPageSnapshot::kColumns ||
      teletext_hamming84_decode(packet[2]) != 0) {
    return;
  }
  // Six links of six Hamming 8/4 bytes, then the link control byte; all of
  // them must decode for any to be believed.
  std::array<int, 37> nibble{};
  for (size_t i = 0; i < nibble.size(); ++i) {
    nibble[i] = teletext_hamming84_decode(packet[3 + i]);
    if (nibble[i] < 0) {
      return;
    }
  }
  for (size_t link = 0; link < state.links.size(); ++link) {
    // Coded as a header's page address (§9.3.1), the bits where a header has
    // C4, C5 and C6 complementing this page's magazine bits.
    const int* n = &nibble[6 * link];
    const int page_number = (n[1] << 4) | n[0];
    if (page_number == kTimeFillingPageNumber) {
      state.links[link].reset();  // FF: no page named
      continue;
    }
    const int flip =
        ((n[3] >> 3) & 1) | (((n[5] >> 2) & 1) << 1) | (((n[5] >> 3) & 1) << 2);
    const int subcode =
        n[2] | ((n[3] & 0x7) << 4) | (n[4] << 7) | ((n[5] & 0x3) << 11);
    state.links[link] = TeletextPageLink{
        displayed_magazine(transmission_magazine ^ flip), page_number, subcode};
  }
  state.display_row_24 = (nibble[36] & 0x8) != 0;
}

void TeletextPageDecoder::apply_enhancements(const MagazineState& state,
                                             TeletextPageSnapshot& snapshot) {
  // The local enhancement data: the page's packets X/26 in designation order,
  // each triplet in turn, from an Active Position of row 0, column 0
  // (§12.3.1-§12.3.2). A Level 1.5 decoder places the G2, G3 and diacritic
  // characters and follows the triplets that move the Active Position; what
  // the rest ask for is Level 2.5 presentation and is counted.
  int row = 0;
  int column = 0;
  for (const auto& packet : state.enhancements) {
    if (!packet.has_value()) {
      continue;
    }
    for (const int32_t triplet : *packet) {
      if (triplet < 0) {
        break;  // the end of a short packet
      }
      const int address = triplet & 0x3F;
      const int mode = (triplet >> 6) & 0x1F;
      const int data = (triplet >> 11) & 0x7F;
      if (address >= 40) {
        // Row Address triplets (§12.3.3 Table 28). Address 40 is row 24 and
        // 41 to 63 rows 1 to 23.
        const int addressed_row = address == 40 ? 24 : address - 40;
        switch (mode) {
          case 0x01:  // Full Row Colour: the row, column 0
            row = addressed_row;
            column = 0;
            ++snapshot.enhancements_not_shown;
            break;
          case 0x04:  // Set Active Position
            row = addressed_row;
            if (data < 40) {
              column = data;
            }
            break;
          case 0x07:  // Address Display Row 0
            if (address == 63) {
              row = 0;
              column = 0;
            }
            ++snapshot.enhancements_not_shown;
            break;
          case 0x15:  // An object definition: what follows draws an object
          case 0x16:  // where it is invoked, not at the triplets' addresses,
          case 0x17:  // and a Level 1.5 decoder invokes none.
          case 0x1F:  // Termination Marker
            return;
          default:
            ++snapshot.enhancements_not_shown;
            break;
        }
        continue;
      }
      // Column Address triplets (§12.3.4 Table 29). All but the PDC triplet
      // and the reserved ones move the Active Position to their column.
      if (mode == 0x04 || mode == 0x05 || mode == 0x06 || mode == 0x0A) {
        ++snapshot.enhancements_not_shown;
        continue;
      }
      column = address;
      char32_t character = 0;
      bool level_1_5 = true;
      const auto code = static_cast<uint8_t>(data);
      if (mode == 0x02) {
        character = teletext_g3_to_unicode(code);
      } else if (mode == 0x0F) {
        character = teletext_g2_to_unicode(code, snapshot.g0_set);
      } else if (mode >= 0x10) {
        character =
            teletext_x26_g0_to_unicode(code, mode & 0xF, snapshot.g0_set);
      } else {
        level_1_5 = false;
      }
      if (!level_1_5) {
        ++snapshot.enhancements_not_shown;
        continue;
      }
      if (character == 0 || row >= TeletextPageSnapshot::kRows ||
          column >= snapshot.columns) {
        continue;  // a reserved code, or a position off the page
      }
      TeletextPageCell& cell =
          snapshot.cells[static_cast<size_t>(row)][static_cast<size_t>(column)];
      if (cell.double_height_lower) {
        continue;  // the row above's double height covers it
      }
      if (cell.enhanced == 0) {
        ++snapshot.enhanced_characters;
      }
      cell.enhanced = character;
    }
  }
}

void TeletextPageDecoder::handle_character_set_designation(
    int transmission_magazine,
    const std::array<uint8_t, kTeletextPacketBytes>& packet, bool magazine_wide,
    bool designation_zero) {
  // The caller has already read the designation code out of byte 6 of the
  // packet — index 2 here, the MRAG being bytes 4 and 5 — and established that
  // it is 0 or 4, the two Table 4 codes. X/28/1 to X/28/3 and M/29/1 to
  // M/29/3 code other things at other positions and never reach here.

  // Before believing anything the packet says, require the whole of it to be
  // what it claims: §9.4.1 codes all 39 bytes after the designation code as
  // thirteen Hamming 24/18 triplets, so on a genuine packet every one of them
  // decodes and on a damaged one the drop costs nothing — it is re-sent with
  // every cycle of the page.
  //
  // This is not pedantry but the noise gate. On a tape recovery the MRAG's
  // Hamming 8/4 mis-corrects bursts into valid addresses, and a long capture
  // yields hundreds of garbage packets numbered 28 or 29 (the reference SECAM
  // capture: 246 over 80 677 frames). A random triplet passes 24/18 about 39%
  // of the time, so gating on triplet 1 alone let roughly one in forty of
  // those through — and one fake M/29/0 re-designates a whole magazine for
  // the rest of the recording, which on the reference capture silently undid
  // the configured Cyrillic set. All thirteen triplets at once is a pass rate
  // of about 5 in a million, and none of the observed fakes decodes more than
  // seven.
  for (size_t byte = 3; byte + 2 < kTeletextPacketBytes; byte += 3) {
    if (teletext_hamming2418_decode(packet[byte], packet[byte + 1],
                                    packet[byte + 2]) < 0) {
      return;
    }
  }

  // Triplet 1 is bytes 7 to 9 (indices 3 to 5), Hamming 24/18. An
  // uncorrectable triplet is dropped: a mis-read designation would change the
  // alphabet of every page in the magazine, and the packet is re-sent with the
  // page.
  const int32_t triplet =
      teletext_hamming2418_decode(packet[3], packet[4], packet[5]);
  if (triplet < 0) {
    return;
  }

  // An X/28 packet is Format 1 — the format that carries a designation — only
  // when its Page Function says a basic Level 1 page (§9.4.2.1 Table 3). An
  // M/29 carries no page function at all (§9.1 Table 1: "the same functions
  // apart from page function and coding"), so its triplet 1 bits 1-7 are not
  // read.
  if (!magazine_wide && (triplet & 0xF) != kPageFunctionBasicLevel1) {
    return;
  }

  // Triplet 2 is bytes 10 to 12 (indices 6 to 8); the second G0 set straddles
  // the two triplets, so it is needed here as well. Its own decode was already
  // proved by the all-thirteen-triplets gate above.
  const int32_t triplet2 =
      teletext_hamming2418_decode(packet[6], packet[7], packet[8]);
  if (triplet2 < 0) {
    return;
  }

  // Triplet 1 bits 8-14: the 7-bit Table 32 value, four designation bits
  // (14-11) over three national option bits (10-8).
  const int value = (triplet >> 7) & 0x7F;
  const TeletextG0Set g0_set =
      g0_set_for_designation((value >> 3) & 0xF, value & 0x7);

  // Triplet 1 bits 15-18 and triplet 2 bits 1-3: the 7-bit Table 33 value.
  // Table 33 prints its columns with triplet 2's three bits above triplet 1's
  // four, most significant first within each, so the value assembles as the
  // table reads.
  const int second_value =
      ((static_cast<int>(triplet2) & 0x7) << 4) | ((triplet >> 14) & 0xF);
  const std::optional<TeletextG0Designation> second_g0_set =
      second_g0_for_designation(second_value);

  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  if (magazine_wide) {
    // M/29 sets the magazine's defaults. They do not reach back into a page
    // already open: that page's rows were addressed under the sets it was
    // opened with, and §15.2 gives the page's own X/28 the higher priority
    // anyway.
    //
    // M/29/0 over M/29/4 (§9.4.2.2), so an M/29/4 arriving after an M/29/0 is
    // read for the colour map this decoder does not model and for nothing else.
    if (state.magazine_g0_received && state.magazine_g0_from_designation_zero &&
        !designation_zero) {
      return;
    }
    state.magazine_g0_set = g0_set;
    state.magazine_second_g0_set = second_g0_set;
    state.magazine_g0_received = true;
    state.magazine_g0_from_designation_zero = designation_zero;
    return;
  }
  // X/28 belongs to the page it was transmitted with, so it applies to the open
  // page immediately — it may well arrive after some of the page's rows, and
  // the page is only rendered when it terminates. X/28/0 over X/28/4 as above.
  if (state.g0_set_from_designation_zero && !designation_zero) {
    return;
  }
  state.g0_set = g0_set;
  state.second_g0_set = second_g0_set;
  state.g0_set_from_designation_zero = designation_zero;
}

int TeletextPageDecoder::erase_epoch(const PageIdentity& identity) const {
  const auto it = erase_epochs_.find(identity);
  return it == erase_epochs_.end() ? 0 : it->second;
}

TeletextPageKey TeletextPageDecoder::page_key(int transmission_magazine) const {
  const MagazineState& state =
      magazines_[static_cast<size_t>(transmission_magazine)];
  const int magazine = displayed_magazine(transmission_magazine);
  return TeletextPageKey{
      magazine, state.page_number, state.subcode,
      erase_epoch({magazine, state.page_number, state.subcode})};
}

void TeletextPageDecoder::handle_header_packet(
    int transmission_magazine,
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index, int64_t source) {
  // Page number: bytes 6-7 of the transmission packet = packet bytes 2-3
  // (EN 300 706 §9.3.1.1). Without a correctable page number the header
  // cannot be attributed to a page; drop it entirely.
  const int units = teletext_hamming84_decode(packet[2]);
  const int tens = teletext_hamming84_decode(packet[3]);
  if (units < 0 || tens < 0) {
    return;
  }
  const int page_number = (tens << 4) | units;

  // Sub-code and control bits (EN 300 706 §9.3.1.2, §9.3.1.3 Table 2).
  // Uncorrectable nibbles degrade to zero rather than dropping the header.
  const int s1_raw = teletext_hamming84_decode(packet[4]);
  const int s2_c4_raw = teletext_hamming84_decode(packet[5]);
  const int s3_raw = teletext_hamming84_decode(packet[6]);
  const int s4_c5_c6_raw = teletext_hamming84_decode(packet[7]);
  const int c7_c10_raw = teletext_hamming84_decode(packet[8]);
  const int c11_c14_raw = teletext_hamming84_decode(packet[9]);
  const int s1 = s1_raw < 0 ? 0 : s1_raw;
  const int s2_c4 = s2_c4_raw < 0 ? 0 : s2_c4_raw;
  const int s3 = s3_raw < 0 ? 0 : s3_raw;
  const int s4_c5_c6 = s4_c5_c6_raw < 0 ? 0 : s4_c5_c6_raw;
  const int c7_c10 = c7_c10_raw < 0 ? 0 : c7_c10_raw;
  const int c11_c14 = c11_c14_raw < 0 ? 0 : c11_c14_raw;

  const int read_subcode =
      s1 | ((s2_c4 & 0x7) << 4) | (s3 << 7) | ((s4_c5_c6 & 0x3) << 11);
  // A time-filling header's sub-code means nothing (§7.3), and it opens no
  // page whose sub-codes are to be followed.
  const int subcode = page_number == kTimeFillingPageNumber
                          ? read_subcode
                          : settled_subcode(transmission_magazine, page_number,
                                            packet, read_subcode);
  const bool erase_page = (s2_c4 & 0x8) != 0;         // C4
  const bool newsflash = (s4_c5_c6 & 0x4) != 0;       // C5
  const bool subtitle = (s4_c5_c6 & 0x8) != 0;        // C6
  const bool magazine_serial = (c11_c14 & 0x1) != 0;  // C11

  // A service may re-send a page's header while the page's rows are still
  // being transmitted (a rolling header keeps the on-screen clock live,
  // EN 300 706 §9.3.1.4). That header closes the assembly like any other,
  // but the transmission it closes has not finished — the same page simply
  // reopens and its remaining rows follow. Recognising this is what stops a
  // consumer counting one appearance of a page as several, and what lets it
  // tell a fragment from a finished page.
  const MagazineState& open_state =
      magazines_[static_cast<size_t>(transmission_magazine)];
  const bool same_page_continues =
      open_state.page_open && open_state.have_page && !erase_page &&
      open_state.page_number == page_number && open_state.subcode == subcode;

  // A page header terminates the page currently being transmitted: in serial
  // mode (C11 set) any magazine's page, in parallel mode only the page of
  // the same magazine (EN 300 706 §7.2.1).
  if (magazine_serial) {
    for (int m = 0; m < static_cast<int>(magazines_.size()); ++m) {
      terminate_page(m, m != transmission_magazine || !same_page_continues);
    }
  } else {
    terminate_page(transmission_magazine, !same_page_continues);
  }

  // Subtitle clear events act on header arrival: a header for the watched
  // page with C4 (erase) set, or with C6 no longer set, removes the display
  // (design §6; EN 300 706 §9.3.1.3 Table 2).
  if (subtitle_filter_.has_value() &&
      displayed_magazine(transmission_magazine) == subtitle_filter_->first &&
      page_number == subtitle_filter_->second && (erase_page || !subtitle)) {
    subtitle_clear_event(field_index);
  }

  // Time-filling / terminating headers never open a page (EN 300 706 §7.3).
  if (page_number == kTimeFillingPageNumber) {
    return;
  }

  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];

  // The packet layout the page is read in (see the class comment): the
  // standard one, unless this is a VirText decode and the page's packets have
  // been framed the Zenith way.
  bool virtext_layout = false;
  if (page_separation_ == TeletextPageSeparation::kTransmission &&
      head_columns_ < TeletextPageSnapshot::kColumns) {
    const int magazine = displayed_magazine(transmission_magazine);
    const int vote = virtext_layout_evidence(packet, kHeaderControlBytes);
    vote_virtext_layout(magazine, page_number, vote);
    const int votes = virtext_layout_votes_[{magazine, page_number}];
    virtext_layout = votes > 0 || (votes == 0 && vote > 0);
  }

  // Retain stored rows only for a retransmission of the same page and
  // sub-code without C4 (erase); a different page, a different sub-code
  // (sub-page replacement), or an erase starts from a clean grid
  // (EN 300 706 §9.3.1.3 Table 2, C4).
  const bool retain_rows =
      state.have_page && !erase_page && state.page_number == page_number &&
      state.subcode == subcode && state.virtext_layout == virtext_layout;
  if (!retain_rows) {
    state.rows = {};
    state.enhancements = {};
    state.links = {};
    state.display_row_24 = true;
  }

  state.page_open = true;
  state.have_page = true;
  state.page_number = page_number;
  state.subcode = subcode;
  state.erase_page = erase_page;
  state.virtext_layout = virtext_layout;
  state.last_display_row = 0;

  // A new run separates copies of the page's content from copies of what it
  // replaced, without discarding either (see set_row_squasher()). With a
  // squasher the rows decide, when the transmission ends, and a replaying
  // decoder starts the run here, where the first pass's decision says it
  // began. Without one only C4 is left to go on, and it replaces the page's
  // content by definition (EN 300 706 §9.3.1.3 Table 2).
  //
  // Each header begins a stretch of rows that is settled on its own, including
  // a header re-sent while the page continues: a magazine carrying one page
  // re-sends it back to back, and every repeat has to be compared with the
  // run for an update to be seen.
  const PageIdentity identity{displayed_magazine(transmission_magazine),
                              page_number, subcode};
  state.fragment_source = source;
  state.fragment_erase = erase_page;
  if (row_squasher_ == nullptr) {
    if (erase_page) {
      ++erase_epochs_[identity];
    }
  } else if (replay_run_changes_ != nullptr) {
    const auto change = replay_run_changes_->find(source);
    if (change != replay_run_changes_->end()) {
      erase_epochs_[identity] = change->second;
    }
  }
  state.newsflash = newsflash;
  state.subtitle = subtitle;
  state.suppress_header = (c7_c10 & 0x1) != 0;       // C7
  state.update_indicator = (c7_c10 & 0x2) != 0;      // C8
  state.interrupted_sequence = (c7_c10 & 0x4) != 0;  // C9
  state.inhibit_display = (c7_c10 & 0x8) != 0;       // C10
  state.magazine_serial = magazine_serial;           // C11
  // C12-C14. Byte 13 carries C11 in bit 2 and C12, C13, C14 in bits 4, 6 and 8
  // (§9.3.1.3 Table 2), which Hamming 8/4 delivers as D1-D4 in bits 0-3 — so
  // the three come out least-significant-first and have to be reversed to
  // index Table 32, which prints C12 as the most significant of them.
  state.national_option_subset = (((c11_c14 >> 1) & 0x1) << 2) |  // C12
                                 (((c11_c14 >> 2) & 0x1) << 1) |  // C13
                                 ((c11_c14 >> 3) & 0x1);          // C14

  // The G0 sets the page opens in: the magazine's designations if an M/29 has
  // established them, otherwise the local Code of Practice (§15.2, §15.3). A
  // packet X/28 for this page overrides them if one arrives before the page is
  // rendered — but only for this page, which is why they are taken afresh here
  // rather than left at whatever the previous page settled on, and why the
  // X/28/0-over-X/28/4 precedence resets with them. A rolling header re-sent
  // mid-page is the exception: it is the same transmission, so an X/28 already
  // received for it must survive.
  if (!same_page_continues) {
    state.g0_set = state.magazine_g0_set.value_or(default_g0_set_);
    // An M/29 that said the magazine has no second set is an answer, not an
    // absence: only a magazine no M/29 has spoken for falls back to the
    // configured default.
    state.second_g0_set = state.magazine_g0_received
                              ? state.magazine_second_g0_set
                              : default_second_g0_set_;
    state.g0_set_from_designation_zero = false;
  }
  // The header that *opened* this transmission stamps it, so a rolling header
  // re-sent while the rows are still going out does not make the same
  // appearance of the page look like a series of new ones.
  if (!same_page_continues) {
    state.header_field_index = field_index;
    state.identity_attested = header_identity_attested(packet);
  }
  state.last_field_index = field_index;

  // Header display bytes: the packet bytes after the ten addressing and
  // control ones carry odd-parity characters shown in row 0 from column 8
  // (EN 300 706 §9.3.1.4) — 32 of them on 625 lines, 24 on 525, the addressing
  // ahead of them being identical (ITU-R BT.653 Table 1b §3.3). Columns 0-7
  // are decoder-generated (page number, clock) and left as spaces here. The
  // VirText layout puts its frame byte ahead of them and carries 22.
  const int head = head_columns(state);
  const size_t text_byte = kHeaderControlBytes + (virtext_layout ? 1 : 0);
  RowData& header_row = state.rows[0];
  const bool header_extension_present = header_row.extension_present;
  const auto header_extension_columns = header_row.characters;
  const auto header_extension_parity = header_row.parity_error;
  header_row.present = true;
  header_row.characters.fill(0x20);
  header_row.parity_error.fill(false);
  // The header row's own extension columns survive the rewrite: they were sent
  // in a different packet and this one says nothing about them.
  if (header_extension_present) {
    header_row.extension_present = true;
    for (int column = head; column < TeletextPageSnapshot::kColumns; ++column) {
      const auto i = static_cast<size_t>(column);
      header_row.characters[i] = header_extension_columns[i];
      header_row.parity_error[i] = header_extension_parity[i];
    }
  }
  for (int column = kHeaderTextColumn; column < head; ++column) {
    const auto i = static_cast<size_t>(column - kHeaderTextColumn);
    const uint8_t byte = packet[text_byte + i];
    if (teletext_odd_parity_valid(byte)) {
      header_row.characters[static_cast<size_t>(column)] = byte & 0x7F;
    } else {
      header_row.characters[static_cast<size_t>(column)] = 0x20;
      header_row.parity_error[static_cast<size_t>(column)] = true;
    }
  }
}

int TeletextPageDecoder::settled_subcode(
    int transmission_magazine, int page_number,
    const std::array<uint8_t, kTeletextPacketBytes>& packet, int read) {
  int corrected_digits = 0;
  for (size_t i = 0; i < kSubcodeDigitBits.size(); ++i) {
    if (!teletext_hamming84_clean(packet[kSubcodeByte + i])) {
      corrected_digits |= kSubcodeDigitBits[i];
    }
  }
  const auto [it, first_header] =
      page_subcodes_.try_emplace({transmission_magazine, page_number});
  PageSubcodes& page = it->second;
  int subcode = read;
  if (corrected_digits == 0) {
    page.sent.insert(read);
  } else if (!first_header && page.sent.count(read) == 0) {
    // A digit the correction made is one channel error from a neighbouring
    // codeword's (§8.2), and this one names a sub-code the page was never
    // sent with whole: the page's own digits stand.
    subcode = (read & ~corrected_digits) | (page.last & corrected_digits);
  }
  page.last = subcode;
  return subcode;
}

void TeletextPageDecoder::handle_display_packet(
    int transmission_magazine, int row,
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index, int64_t source,
    const TeletextPacketConfidence* confidence) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  // Rows belong to the page whose transmission is in progress in this
  // magazine (EN 300 706 §7.2.1); orphan rows with no open page are dropped.
  if (!state.page_open) {
    return;
  }
  // By transmission, a screen's rows arrive in order, so a row numbered below
  // the last one is the first of a page whose header was lost: on VirText
  // recordings the next page of the rotation, whose rows would otherwise be
  // taken for a new screen of this one. The transmission ends there, and its
  // rows go to no page until a header says which they belong to. Packets 20 to
  // 22 of the VirText layout are not display rows and are not counted.
  if (page_separation_ == TeletextPageSeparation::kTransmission &&
      (!state.virtext_layout || row < kVirTextRows)) {
    if (row < state.last_display_row) {
      terminate_page(transmission_magazine);
      return;
    }
    state.last_display_row = row;
  }
  if (state.virtext_layout) {
    handle_virtext_packet(transmission_magazine, row, packet, field_index,
                          source, confidence);
    return;
  }
  // A standard row of a VirText decode is evidence against the Zenith layout
  // for its page, where it holds characters in place of the frame bytes.
  if (page_separation_ == TeletextPageSeparation::kTransmission &&
      head_columns_ < TeletextPageSnapshot::kColumns && row < kVirTextRows) {
    vote_virtext_layout(displayed_magazine(transmission_magazine),
                        state.page_number,
                        virtext_layout_evidence(packet, kMragBytes));
  }

  const TeletextPageKey key = page_key(transmission_magazine);
  last_row_attribution_ = key;
  last_row_number_ = row;

  RowData& row_data = state.rows[static_cast<size_t>(row)];
  row_data.present = true;
  for (size_t column = 0; column < static_cast<size_t>(head_columns_);
       ++column) {
    // The service's display bytes, 7 data bits + odd parity (EN 300 706
    // §9.3.2, §8.1).
    const uint8_t byte = packet[kMragBytes + column];
    if (teletext_odd_parity_valid(byte)) {
      row_data.characters[column] = byte & 0x7F;
      row_data.parity_error[column] = false;
    } else {
      row_data.characters[column] = 0x20;
      row_data.parity_error[column] = true;
    }
  }

  if (row_squasher_ != nullptr) {
    // The squasher's rows are the widest a service transmits; a service whose
    // packets carry fewer leaves the rest at zero, and the render below takes
    // those columns from the row-extension store instead, so the squasher needs
    // no notion of the width itself.
    TeletextRowBytes display{};
    std::copy(packet.begin() + kMragBytes,
              packet.begin() + kMragBytes + head_columns_, display.begin());
    // The row's own bytes are the packet bytes after the MRAG (§9.3.2), so its
    // confidences are the matching slice of the packet's.
    PendingRowCopy copy;
    copy.row = row;
    copy.bytes = display;
    copy.source = source;
    copy.has_weights = confidence != nullptr;
    if (confidence != nullptr) {
      std::copy(confidence->begin() + kMragBytes,
                confidence->begin() + kMragBytes + head_columns_,
                copy.weights.begin());
    }
    copy.first_column = 0;
    copy.column_count = static_cast<size_t>(head_columns_);
    record_row_copy(transmission_magazine, std::move(copy));
  }

  state.last_field_index = field_index;
}

void TeletextPageDecoder::handle_extension_packet(
    int transmission_magazine, int packet_number,
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index, int64_t source,
    const TeletextPacketConfidence* confidence) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  // Like a display row, an extension belongs to the page whose transmission is
  // in progress in the magazine it is addressed to (EN 300 706 §7.2.1). A page
  // in the Zenith VirText layout brings its own columns 30-39 instead.
  if (!state.page_open || state.virtext_layout) {
    return;
  }

  const int extension_columns = TeletextPageSnapshot::kColumns - head_columns_;
  if (extension_columns <= 0) {
    return;
  }

  const int first_row = extension_first_row(packet_number);
  last_extension_attribution_ = page_key(transmission_magazine);
  last_extension_first_row_ = first_row;

  for (int group = 0; group < kExtensionPacketRows; ++group) {
    const int row = first_row + group;
    if (row >= TeletextPageSnapshot::kRows) {
      break;
    }
    const size_t group_base = static_cast<size_t>(kMragBytes) +
                              static_cast<size_t>(group * extension_columns);

    RowData& row_data = state.rows[static_cast<size_t>(row)];
    for (int offset = 0; offset < extension_columns; ++offset) {
      const size_t column =
          static_cast<size_t>(head_columns_) + static_cast<size_t>(offset);
      const uint8_t byte = packet[group_base + static_cast<size_t>(offset)];
      // A byte that fails odd parity (EN 300 706 §8.1) is known to be corrupt,
      // so it never replaces a clean one already recovered for this column.
      // This is the decoder's own store, which is all a caller without a row
      // squasher has; with one attached the render takes these columns from its
      // vote across every copy instead.
      const bool clean = teletext_odd_parity_valid(byte);
      if (!clean && row_data.extension_present &&
          !row_data.parity_error[column]) {
        continue;
      }
      row_data.characters[column] = clean ? static_cast<uint8_t>(byte & 0x7F)
                                          : static_cast<uint8_t>(0x20);
      row_data.parity_error[column] = !clean;
    }
    row_data.extension_present = true;

    if (row_squasher_ == nullptr) {
      continue;
    }
    // The extension columns go into the squasher as a copy speaking for those
    // columns only, so repeated transmissions of them correct each other just
    // as the display packets' columns do. One packet contributes to four rows;
    // they are separate buckets, so the one source id serves all four.
    PendingRowCopy copy;
    copy.row = row;
    std::copy(packet.begin() + static_cast<std::ptrdiff_t>(group_base),
              packet.begin() + static_cast<std::ptrdiff_t>(group_base) +
                  extension_columns,
              copy.bytes.begin() + head_columns_);
    copy.source = source;
    copy.has_weights = confidence != nullptr;
    if (confidence != nullptr) {
      std::copy(confidence->begin() + static_cast<std::ptrdiff_t>(group_base),
                confidence->begin() + static_cast<std::ptrdiff_t>(group_base) +
                    extension_columns,
                copy.weights.begin() + head_columns_);
    }
    copy.first_column = static_cast<size_t>(head_columns_);
    copy.column_count = static_cast<size_t>(extension_columns);
    record_row_copy(transmission_magazine, std::move(copy));
  }

  state.last_field_index = field_index;
}

void TeletextPageDecoder::handle_virtext_packet(
    int transmission_magazine, int packet_number,
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index, int64_t source,
    const TeletextPacketConfidence* confidence) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  const TeletextPageKey key = page_key(transmission_magazine);

  // One stretch of the packet, |count| bytes from |packet_byte|, as columns
  // |first_column| onwards of |row|: into this decoder's store, where a byte
  // failing parity (EN 300 706 §8.1) never replaces a clean one an extension
  // already brought, and on to the squasher as a copy speaking for those
  // columns alone.
  const auto take = [&](int row, size_t packet_byte, size_t first_column,
                        size_t count, bool extension) {
    RowData& row_data = state.rows[static_cast<size_t>(row)];
    for (size_t offset = 0; offset < count; ++offset) {
      const size_t column = first_column + offset;
      const uint8_t byte = packet[packet_byte + offset];
      const bool clean = teletext_odd_parity_valid(byte);
      if (extension && !clean && row_data.extension_present &&
          !row_data.parity_error[column]) {
        continue;
      }
      row_data.characters[column] = clean ? static_cast<uint8_t>(byte & 0x7F)
                                          : static_cast<uint8_t>(0x20);
      row_data.parity_error[column] = !clean;
    }
    if (extension) {
      row_data.extension_present = true;
    } else {
      row_data.present = true;
    }
    last_layout_spans_.push_back(
        TeletextPacketSpan{row, packet_byte, first_column, count});
    if (row_squasher_ == nullptr) {
      return;
    }
    PendingRowCopy copy;
    copy.row = row;
    std::copy(packet.begin() + static_cast<std::ptrdiff_t>(packet_byte),
              packet.begin() + static_cast<std::ptrdiff_t>(packet_byte + count),
              copy.bytes.begin() + static_cast<std::ptrdiff_t>(first_column));
    copy.source = source;
    copy.has_weights = confidence != nullptr;
    if (confidence != nullptr) {
      std::copy(
          confidence->begin() + static_cast<std::ptrdiff_t>(packet_byte),
          confidence->begin() +
              static_cast<std::ptrdiff_t>(packet_byte + count),
          copy.weights.begin() + static_cast<std::ptrdiff_t>(first_column));
    }
    copy.first_column = first_column;
    copy.column_count = count;
    record_row_copy(transmission_magazine, std::move(copy));
  };

  // The frame byte opens a display row and a packet 20/21's digit, so a
  // packet's own bytes start one past the MRAG.
  const size_t data_byte = kMragBytes + 1;
  if (packet_number < kVirTextRows) {
    vote_virtext_layout(displayed_magazine(transmission_magazine),
                        state.page_number,
                        virtext_layout_evidence(packet, kMragBytes));
    take(packet_number, data_byte, 0, kVirTextColumns, false);
  } else if (packet_number == kVirTextExtensionPacket ||
             packet_number == kVirTextExtensionPacket + 1) {
    // The digit places three rows' columns at once, so like the address it has
    // to have arrived as transmitted: corrected, it is one channel error away
    // from having put them on the wrong rows (see address_attested()).
    const uint8_t digit = packet[kMragBytes];
    const int first_row =
        teletext_hamming84_decode(digit) +
        kVirTextExtensionPacketRows * (packet_number - kVirTextExtensionPacket);
    if (teletext_hamming84_clean(digit) &&
        first_row % kVirTextExtensionRows == 0) {
      const auto tail =
          static_cast<size_t>(TeletextPageSnapshot::kColumns - kVirTextColumns);
      for (int group = 0; group < kVirTextExtensionRows; ++group) {
        const int row = first_row + group;
        if (row >= kVirTextRows) {
          break;
        }
        take(row, data_byte + static_cast<size_t>(group) * tail,
             static_cast<size_t>(kVirTextColumns), tail, true);
      }
    }
  }
  // Packet 22 precedes each new screen with a counter and a copy of a header's
  // framing, and is no part of what the page shows. It, and a packet 20/21
  // whose digit is not evidence, are attributed to the page with no spans, so
  // a consumer counting characters knows they hold none.
  last_layout_attribution_ = key;
  state.last_field_index = field_index;
}

void TeletextPageDecoder::vote_virtext_layout(int magazine, int page_number,
                                              int vote) {
  if (vote == 0) {
    return;
  }
  int& votes = virtext_layout_votes_[{magazine, page_number}];
  votes = std::clamp(votes + vote, -kVirTextLayoutVoteBound,
                     kVirTextLayoutVoteBound);
}

int TeletextPageDecoder::head_columns(const MagazineState& state) const {
  return state.virtext_layout ? kVirTextColumns : head_columns_;
}

void TeletextPageDecoder::record_row_copy(int transmission_magazine,
                                          PendingRowCopy copy) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  if (replay_run_changes_ != nullptr) {
    // The run was decided at the header (see replay_run_changes()).
    row_squasher_->add_row(page_key(transmission_magazine), copy.row,
                           copy.bytes, copy.source,
                           copy.has_weights ? &copy.weights : nullptr,
                           copy.first_column, copy.column_count);
    return;
  }
  state.row_pending[static_cast<size_t>(copy.row)] = true;
  state.pending.push_back(std::move(copy));
}

void TeletextPageDecoder::settle_content_run(int transmission_magazine) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  state.row_pending = {};
  if (row_squasher_ == nullptr || state.pending.empty()) {
    state.pending.clear();
    return;
  }

  const PageIdentity identity{displayed_magazine(transmission_magazine),
                              state.page_number, state.subcode};
  const TeletextPageKey run{identity[0], identity[1], identity[2],
                            erase_epoch(identity)};

  // How a transmission's display rows compare with what one run has combined
  // for them, over the columns where both are parity-clean: a byte known to be
  // corrupt says nothing about whether the content changed. A row has changed
  // where a stretch of it now says something else; the handful of characters
  // that a burst passing parity rewrites on a band-limited tape is not that.
  struct Comparison {
    int rows_compared = 0;
    int rows_changed = 0;
  };
  // By transmission (see set_page_separation()) a difference counts only where
  // one side of it draws something, and where it is not the held byte read a
  // bit early or late. On a recording read at a wrong bit clock (see
  // TeletextClockCalibrator) the detector slips a bit part way along a
  // packet, and every byte after the slip is then its
  // neighbours' bits: a run of spaces becomes the spacing attribute 1/0 (0x20
  // a bit early is 0x10, which passes parity) and a run of solid mosaics a run
  // of 3/F. That slip is most of what tells a damaged copy from its screen,
  // while what changes between screens is characters.
  const bool slip_aware =
      page_separation_ == TeletextPageSeparation::kTransmission;
  const auto compare_with = [&](const TeletextPageKey& against) {
    constexpr int kComparableBytes = 16;
    constexpr int kChangedBytes = 8;
    Comparison out;
    for (const PendingRowCopy& copy : state.pending) {
      if (copy.first_column != 0 || copy.row < 1) {
        continue;  // header row carries a clock; extensions are compared with
                   // their own display packet
      }
      const auto combined = row_squasher_->squashed_row(against, copy.row);
      if (!combined.has_value()) {
        continue;
      }
      // The held row's byte at |at|, where the copy speaks for it.
      const auto neighbour = [&](size_t at) -> std::optional<uint8_t> {
        if (at >= copy.column_count) {
          return std::nullopt;
        }
        return (*combined)[at];
      };
      int both_clean = 0;
      int differing = 0;
      for (size_t column = 0; column < copy.column_count; ++column) {
        const uint8_t received = copy.bytes[column];
        const uint8_t held = (*combined)[column];
        if (!teletext_odd_parity_valid(received) ||
            !teletext_odd_parity_valid(held)) {
          continue;
        }
        ++both_clean;
        if (received == held) {
          continue;
        }
        if (slip_aware) {
          if (!draws_something(received) && !draws_something(held)) {
            continue;
          }
          if (slipped_by_a_bit(received, held, neighbour(column - 1),
                               neighbour(column + 1))) {
            continue;
          }
        }
        ++differing;
      }
      if (both_clean < kComparableBytes) {
        continue;
      }
      ++out.rows_compared;
      if (differing >= kChangedBytes) {
        ++out.rows_changed;
      }
    }
    return out;
  };
  const auto repeats = [](const Comparison& comparison) {
    return comparison.rows_changed == 0 &&
           comparison.rows_compared >= kRepeatEvidenceRows;
  };

  size_t transmitted_bytes = 0;
  size_t transmitted_failures = 0;
  for (const PendingRowCopy& copy : state.pending) {
    if (copy.first_column != 0 || copy.row < 1) {
      continue;
    }
    for (size_t column = 0; column < copy.column_count; ++column) {
      ++transmitted_bytes;
      transmitted_failures +=
          teletext_odd_parity_valid(copy.bytes[column]) ? 0 : 1;
    }
  }
  const Comparison current = compare_with(run);

  // C4 says the content was replaced, and it is believed unless the rows show
  // the page repeated: two or more of them compared, none changed. Without C4
  // the rows have to make the whole case, and only a transmission that
  // arrived nearly undamaged can: a damaged one differs from the run by its
  // damage, and starting a run from it leaves the next transmissions to be
  // compared against that damage in turn.
  const bool clean_transmission =
      transmitted_bytes > 0 &&
      transmitted_failures * kCleanTransmissionDivisor <= transmitted_bytes;
  bool new_run = false;
  if (page_separation_ == TeletextPageSeparation::kTransmission) {
    // Every transmission may be a screen of its own (see
    // set_page_separation()). It goes to the screen it differs from least,
    // where that is by fewer than kContentChangeRows rows — the screen on
    // show, or an earlier one coming round again — and otherwise starts a new
    // one. Fewer is damage: on VirText recordings a burst read as
    // characters that pass parity rewrites a row or two of a page that never
    // changes, while the rotation's screens differ from each other in five
    // rows or more. Between screens that differ from it equally, the one its
    // rows have come in more often wins, so a damaged copy that once started
    // a screen of its own does not draw the page's later transmissions away
    // from the screen it is a copy of.
    if (current.rows_changed > 0) {
      const auto arrivals = [&](const TeletextPageKey& against) {
        size_t total = 0;
        for (const PendingRowCopy& copy : state.pending) {
          if (copy.first_column == 0 && copy.row >= 1) {
            total += row_squasher_->arrival_count(against, copy.row);
          }
        }
        return total;
      };
      int& runs = run_counts_.try_emplace(identity, 1).first->second;
      int target = -1;
      int fewest_changed = kContentChangeRows;
      size_t most_arrivals = 0;
      if (current.rows_changed < kContentChangeRows) {
        target = run.erase_epoch;
        fewest_changed = current.rows_changed;
        most_arrivals = arrivals(run);
      }
      for (int earlier = runs - 1; earlier >= 0; --earlier) {
        if (earlier == run.erase_epoch) {
          continue;
        }
        const TeletextPageKey candidate{identity[0], identity[1], identity[2],
                                        earlier};
        const Comparison comparison = compare_with(candidate);
        if (comparison.rows_compared < kRepeatEvidenceRows ||
            comparison.rows_changed >= kContentChangeRows) {
          continue;
        }
        const size_t candidate_arrivals = arrivals(candidate);
        if (target < 0 || comparison.rows_changed < fewest_changed ||
            (comparison.rows_changed == fewest_changed &&
             candidate_arrivals > most_arrivals)) {
          target = earlier;
          fewest_changed = comparison.rows_changed;
          most_arrivals = candidate_arrivals;
        }
      }
      if (target < 0) {
        target = runs++;
      }
      if (target != run.erase_epoch) {
        erase_epochs_[identity] = target;
        run_changes_[state.fragment_source] = target;
      }
    }
  } else if (state.fragment_erase) {
    new_run = !repeats(current);
    if (current.rows_compared == 0) {
      // Nothing to compare: C4 only means something if the run holds content
      // for it to replace.
      new_run = false;
      for (int row = 1; row < TeletextPageSnapshot::kRows && !new_run; ++row) {
        new_run = row_squasher_->copy_count(run, row) > 0;
      }
    }
  } else {
    new_run = clean_transmission &&
              current.rows_changed >= kContentChangeRows &&
              current.rows_changed * 3 >= current.rows_compared;
  }
  if (new_run) {
    // A screen of a rotating page coming round again rejoins its own run,
    // most recent first; only content the page has not shown before starts a
    // new one.
    int& runs = run_counts_.try_emplace(identity, 1).first->second;
    int target = runs;
    for (int earlier = runs - 1; earlier >= 0; --earlier) {
      if (earlier == run.erase_epoch) {
        continue;
      }
      if (repeats(compare_with(TeletextPageKey{identity[0], identity[1],
                                               identity[2], earlier}))) {
        target = earlier;
        break;
      }
    }
    if (target == runs) {
      ++runs;
    }
    erase_epochs_[identity] = target;
    run_changes_[state.fragment_source] = target;
  }

  const TeletextPageKey key{identity[0], identity[1], identity[2],
                            erase_epoch(identity)};
  for (const PendingRowCopy& copy : state.pending) {
    row_squasher_->add_row(key, copy.row, copy.bytes, copy.source,
                           copy.has_weights ? &copy.weights : nullptr,
                           copy.first_column, copy.column_count);
  }
  state.pending.clear();
}

void TeletextPageDecoder::terminate_page(int transmission_magazine,
                                         bool transmission_complete) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  if (!state.page_open) {
    return;
  }
  state.page_open = false;
  settle_content_run(transmission_magazine);

  // A screen's run is known only once its rows are in (see
  // set_page_separation()), so a transmission is shown whole or not at all.
  if (!transmission_complete &&
      page_separation_ == TeletextPageSeparation::kTransmission) {
    return;
  }

  // Rendering the snapshot is the expensive part of closing a page — with a
  // squasher attached it votes over every stored copy of all 24 rows — and a
  // header packet closes a page every few dozen packets for the whole of a
  // recording. Skip it when nothing will consume it: no page callback, and
  // the closed page is not the watched subtitle page (the only page
  // subtitle_page_completed() acts on).
  const bool watched_subtitle_page =
      subtitle_filter_.has_value() &&
      displayed_magazine(transmission_magazine) == subtitle_filter_->first &&
      state.page_number == subtitle_filter_->second;
  if (page_callback_ == nullptr && !watched_subtitle_page) {
    return;
  }

  TeletextPageSnapshot snapshot = render_snapshot(transmission_magazine, state);
  snapshot.transmission_complete = transmission_complete;
  if (page_callback_) {
    page_callback_(snapshot);
  }
  subtitle_page_completed(snapshot);
}

std::vector<TeletextPageSnapshot> TeletextPageDecoder::open_page_snapshots()
    const {
  std::vector<TeletextPageSnapshot> snapshots;
  for (int magazine = 0; magazine < static_cast<int>(magazines_.size());
       ++magazine) {
    const MagazineState& state = magazines_[static_cast<size_t>(magazine)];
    if (!state.page_open) {
      continue;
    }
    TeletextPageSnapshot snapshot = render_snapshot(magazine, state);
    snapshot.transmission_complete = false;
    snapshots.push_back(std::move(snapshot));
  }
  return snapshots;
}

TeletextPageSnapshot TeletextPageDecoder::render_snapshot(
    int transmission_magazine, const MagazineState& state) const {
  TeletextPageSnapshot snapshot;
  snapshot.magazine = displayed_magazine(transmission_magazine);
  snapshot.page_number = state.page_number;
  snapshot.subcode = state.subcode;
  snapshot.erase_page = state.erase_page;
  snapshot.newsflash = state.newsflash;
  snapshot.subtitle = state.subtitle;
  snapshot.suppress_header = state.suppress_header;
  snapshot.update_indicator = state.update_indicator;
  snapshot.interrupted_sequence = state.interrupted_sequence;
  snapshot.inhibit_display = state.inhibit_display;
  snapshot.magazine_serial = state.magazine_serial;
  snapshot.national_option_subset = state.national_option_subset;
  snapshot.g0_set = state.g0_set;
  snapshot.second_g0_set = state.second_g0_set;
  snapshot.header_field_index = state.header_field_index;
  snapshot.last_field_index = state.last_field_index;
  snapshot.identity_attested = state.identity_attested;
  snapshot.columns = columns_;
  // A short packet is the 525-line service, whose graphics use the codes a
  // 625-line one would blast alphanumerics through (see the field's comment).
  snapshot.mosaic_blast_through =
      head_columns_ == TeletextPageSnapshot::kColumns;

  // Columns the page's display packets carry; the rest come from extensions.
  const int head = head_columns(state);
  // A page in the Zenith VirText layout has 20 lines (see the class comment).
  // Below them is only what a transmission read in the standard layout, before
  // the page's packets had shown which it is, made of packets 20 to 22.
  const int rows =
      state.virtext_layout ? kVirTextRows : TeletextPageSnapshot::kRows;

  // With a squasher attached, display rows come from the combined copies
  // rather than from the last one received: repeated transmissions correct
  // each other, and a row recovered during an earlier transmission is still
  // available when the current one was clipped (teletext_row_squasher.h).
  const TeletextPageKey key{
      snapshot.magazine, state.page_number, state.subcode,
      erase_epoch({snapshot.magazine, state.page_number, state.subcode})};
  if (row_squasher_ != nullptr) {
    snapshot.content_run = key.erase_epoch;
  }

  for (int row = 0; row < rows; ++row) {
    // The decoder's own store is the starting point, and the columns it never
    // wrote show as spaces: the zeros of a row never written would otherwise
    // read as spacing attributes rather than as the blank they stand for.
    RowData row_data = state.rows[static_cast<size_t>(row)];
    if (!row_data.present) {
      std::fill(row_data.characters.begin(), row_data.characters.begin() + head,
                0x20);
      std::fill(row_data.parity_error.begin(),
                row_data.parity_error.begin() + head, false);
    }
    if (!row_data.extension_present) {
      std::fill(row_data.characters.begin() + head, row_data.characters.end(),
                0x20);
      std::fill(row_data.parity_error.begin() + head,
                row_data.parity_error.end(), false);
    }

    // Where a squasher has copies, its vote replaces what the store holds:
    // repeated transmissions correct each other, and a row recovered during an
    // earlier transmission is still available when the current one was clipped.
    // It votes per column, so for a service that splits its rows the display
    // packet's columns and the extension packets' are each combined across
    // their own repeats — including on row 0, whose extension columns hold
    // still even though the header text beside them carries a live clock.
    int row_copies = 0;
    int row_arrivals = 0;
    // A row the transmission in progress has brought but not yet given to the
    // squasher (see settle_content_run()) is shown as it arrived.
    if (row_squasher_ != nullptr &&
        !state.row_pending[static_cast<size_t>(row)]) {
      TeletextRowCoverage covered{};
      if (const auto squashed =
              row_squasher_->squashed_row(key, row, &covered)) {
        row_copies = static_cast<int>(row_squasher_->copy_count(key, row));
        row_arrivals = static_cast<int>(row_squasher_->arrival_count(key, row));
        for (int column = 0; column < columns_; ++column) {
          const auto i = static_cast<size_t>(column);
          if (!covered[i]) {
            continue;
          }
          if (column < head) {
            row_data.present = true;
          } else {
            row_data.extension_present = true;
          }
          const uint8_t byte = (*squashed)[i];
          if (teletext_odd_parity_valid(byte)) {
            row_data.characters[i] = byte & 0x7F;
            row_data.parity_error[i] = false;
          } else {
            row_data.characters[i] = 0x20;
            row_data.parity_error[i] = true;
          }
        }
      }
    }
    row_data.present = row_data.present || row_data.extension_present;
    auto& cells = snapshot.cells[static_cast<size_t>(row)];
    snapshot.row_received[static_cast<size_t>(row)] = row_data.present;
    // Without a squasher — or for a row it has no copies of, which is this
    // magazine's own store answering — the row rests on the one copy received.
    if (row_copies == 0 && row_data.present && row >= 1) {
      row_copies = 1;
    }
    snapshot.row_copies[static_cast<size_t>(row)] = row_copies;
    snapshot.row_arrivals[static_cast<size_t>(row)] =
        std::max(row_arrivals, row_copies);

    // Start-of-row default conditions (EN 300 706 §12.2 Table 26): white
    // alphanumeric foreground, black background, steady, unboxed, normal
    // size, contiguous mosaics, hold off, and — code 1/B — the page's default
    // G0 set rather than whatever the previous row's last ESC left selected.
    // That last one is what keeps a lost ESC from spilling past its own row.
    bool second_set_active = false;
    bool g0_set_uncertain = false;
    TeletextColour foreground = TeletextColour::White;
    TeletextColour background = TeletextColour::Black;
    bool mosaic = false;
    bool separated = false;
    bool hold = false;
    bool flash = false;
    bool conceal = false;
    bool boxed = false;
    bool double_height = false;
    uint8_t held_character = 0x20;
    bool held_separated = false;

    // Stamp a cell with the G0 set its code is to be read in: the page's
    // default, or its second set where an ESC on this row has toggled into it
    // (§12.2 Table 26 code 1/B, §15.3). Doing it per cell is what lets one row
    // hold two alphabets and still convert to glyphs without any consumer
    // knowing that ESC exists.
    const auto stamp_g0_set = [&](TeletextPageCell& cell) {
      if (second_set_active && snapshot.second_g0_set.has_value()) {
        cell.g0_set = snapshot.second_g0_set->g0_set;
        cell.national_option_subset =
            snapshot.second_g0_set->national_option_subset;
      } else {
        cell.g0_set = snapshot.g0_set;
        cell.national_option_subset = snapshot.national_option_subset;
      }
      cell.g0_set_uncertain = g0_set_uncertain;
    };

    for (int column = 0; column < columns_; ++column) {
      const uint8_t code =
          row_data.present ? row_data.characters[static_cast<size_t>(column)]
                           : static_cast<uint8_t>(0x20);
      const bool parity_error =
          row_data.present &&
          row_data.parity_error[static_cast<size_t>(column)];
      TeletextPageCell& cell = cells[static_cast<size_t>(column)];

      if (code < 0x20 && !parity_error) {
        // Spacing attribute: "Set-At" codes act on this cell, "Set-After"
        // codes from the next cell (EN 300 706 §12.2 Table 26).
        switch (code) {
          case kSteady:
            flash = false;
            break;
          case kNormalSize:
            if (double_height) {
              held_character = 0x20;  // size change resets the held mosaic
              held_separated = false;
            }
            double_height = false;
            break;
          case kConceal:
            conceal = true;
            break;
          case kContiguousMosaic:
            separated = false;
            break;
          case kSeparatedMosaic:
            separated = true;
            break;
          case kBlackBackground:
            background = TeletextColour::Black;
            break;
          case kNewBackground:
            background = foreground;
            break;
          case kHoldMosaics:
            hold = true;
            break;
          default:
            break;
        }

        // The attribute cell displays as SPACE, or as the held mosaic
        // character in mosaics + Hold Mosaics mode (§12.2 1/E); the held
        // character keeps its original contiguous/separated form.
        const bool substitute = hold && mosaic && held_character != 0x20;
        cell.character = substitute ? held_character : 0x20;
        cell.held_mosaic = substitute;
        cell.mosaic = substitute;
        cell.separated_mosaic = substitute ? held_separated : separated;
        cell.foreground = foreground;
        cell.background = background;
        cell.flash = flash;
        cell.conceal = conceal;
        cell.boxed = boxed;
        cell.double_height = double_height;
        cell.parity_error = false;
        stamp_g0_set(cell);

        // "Set-After" actions. The mosaic colours are tested first only so
        // that the alpha range, whose base is 0/0, needs no lower bound.
        if (code >= kMosaicColourBase && code <= kMosaicColourBase + 7) {
          // 1/0-1/7 mosaic colours, black included: Mosaics Black (1/0) is a
          // colour code like the other seven, and a page that sends it means
          // its graphics to be black. §12.2 Table 26 marks it a Level 2.5
          // facility only because Level 1 receivers of the day disagreed over
          // it, and its NOTE 10 is addressed to the broadcaster deciding
          // whether to transmit it — a recording that carries the code carries
          // an author's intent to honour.
          foreground = static_cast<TeletextColour>(code - kMosaicColourBase);
          conceal = false;
          if (!mosaic) {
            mosaic = true;
            held_character = 0x20;  // mode change resets the held mosaic
            held_separated = false;
          }
        } else if (code <= kAlphaColourBase + 7) {
          // 0/0-0/7 alpha colours, Alpha Black (0/0) included on the same
          // reading as Mosaics Black above. Colour codes cancel conceal
          // (§12.2 1/8) and select the G0 set.
          foreground = static_cast<TeletextColour>(code - kAlphaColourBase);
          conceal = false;
          if (mosaic) {
            mosaic = false;
            held_character = 0x20;
            held_separated = false;
          }
        } else if (code == kFlash) {
          flash = true;
        } else if (code == kEndBox) {
          boxed = false;
        } else if (code == kStartBox) {
          boxed = true;
        } else if (code == kDoubleHeight) {
          if (!double_height) {
            double_height = true;
            held_character = 0x20;
            held_separated = false;
          }
        } else if (code == kReleaseMosaics) {
          hold = false;
        } else if (code == kEscape) {
          // 1/B ESC (or Switch): toggle between the page's first and second G0
          // sets for the rest of the row. Inert on a page that has only one —
          // the §15.3 default, and what every page did before two sets could be
          // designated — so the code stays a blank spacing attribute there
          // rather than silently re-alphabeting the row.
          if (snapshot.second_g0_set.has_value()) {
            second_set_active = !second_set_active;
          }
        }
        // 0/E double width and 0/F double size: no Level 1 response.
      } else {
        // Displayable character (or a parity-damaged byte rendered as a
        // flagged SPACE).
        cell.character = parity_error ? 0x20 : code;
        cell.parity_error = parity_error;
        // G1 codes 0x40-0x5F are alphanumeric capitals even in mosaics mode
        // (§12.2: mosaic blocks live in G1 columns 2, 3, 6 and 7).
        const bool mosaic_glyph = mosaic && (code & 0x20) != 0;
        cell.mosaic = !parity_error && mosaic_glyph;
        cell.separated_mosaic = cell.mosaic && separated;
        cell.held_mosaic = false;
        cell.foreground = foreground;
        cell.background = background;
        cell.flash = flash;
        cell.conceal = conceal;
        cell.boxed = boxed;
        cell.double_height = double_height;
        stamp_g0_set(cell);
        if (cell.mosaic) {
          // §12.2 1/E: the held mosaic is the most recent G1 mosaic
          // character with bit 6 set on this row.
          held_character = code;
          held_separated = separated;
        }
      }

      // A byte lost to parity damage could have been the ESC that toggles the
      // rest of the row into the other alphabet, and there is no way to tell.
      // Every cell after it is therefore in one of two sets rather than
      // definitely the one stamped above — but only where the page has a second
      // set at all, since with one set there is nothing an unseen ESC could
      // have changed. Set after the cell is stamped because ESC is "Set-After":
      // a lost one would have acted from the *next* cell, not this one.
      if (parity_error && snapshot.second_g0_set.has_value()) {
        g0_set_uncertain = true;
      }
    }
  }

  // Double-height post-pass (EN 300 706 §12.2 0/D): a row containing double
  // height characters consumes the row below — its transmitted data is
  // ignored and it displays only the origin row's background.
  std::array<bool, TeletextPageSnapshot::kRows> is_lower_row{};
  for (int row = 0; row + 1 < TeletextPageSnapshot::kRows; ++row) {
    if (is_lower_row[static_cast<size_t>(row)]) {
      continue;
    }
    const auto& origin_cells = snapshot.cells[static_cast<size_t>(row)];
    bool row_has_double_height = false;
    for (const auto& cell : origin_cells) {
      if (cell.double_height) {
        row_has_double_height = true;
        break;
      }
    }
    if (!row_has_double_height) {
      continue;
    }
    const size_t lower_row = static_cast<size_t>(row) + 1;
    is_lower_row[lower_row] = true;
    auto& lower_cells = snapshot.cells[lower_row];
    for (int column = 0; column < snapshot.columns; ++column) {
      TeletextPageCell lower;
      lower.background = origin_cells[static_cast<size_t>(column)].background;
      lower.double_height_lower = true;
      // The cell shows no character, but it still belongs to a page with an
      // alphabet: leaving the struct default here would put a stray Latin cell
      // in a Cyrillic page for anything that reads the field without checking
      // double_height_lower first.
      lower.g0_set = snapshot.g0_set;
      lower.national_option_subset = snapshot.national_option_subset;
      lower_cells[static_cast<size_t>(column)] = lower;
    }
  }

  // Level 1.5: the characters the page's packets X/26 put on it, and the
  // links of its X/27/0.
  apply_enhancements(state, snapshot);
  snapshot.links = state.links;
  snapshot.display_row_24 = state.display_row_24;

  return snapshot;
}

void TeletextPageDecoder::subtitle_page_completed(
    const TeletextPageSnapshot& snapshot) {
  if (!subtitle_filter_.has_value() ||
      snapshot.magazine != subtitle_filter_->first ||
      snapshot.page_number != subtitle_filter_->second) {
    return;
  }

  // Only C6-flagged transmissions display subtitle text (EN 300 706
  // §9.3.1.3 Table 2); a completion without C6 clears at most (the header
  // arrival already fired the clear event).
  if (!snapshot.subtitle) {
    subtitle_clear_event(snapshot.last_field_index);
    return;
  }

  const std::string text = extract_subtitle_text(snapshot);
  const int64_t field_index = snapshot.last_field_index;

  if (open_cue_.has_value()) {
    if (open_cue_->text == text) {
      return;  // unchanged retransmission: the cue stays on screen
    }
    subtitle_clear_event(field_index);
  }
  if (!text.empty()) {
    TeletextSubtitleCue cue;
    cue.start_field_index = field_index;
    cue.text = text;
    open_cue_ = std::move(cue);
  }
}

void TeletextPageDecoder::subtitle_clear_event(int64_t field_index) {
  if (!open_cue_.has_value()) {
    return;
  }
  open_cue_->end_field_index = field_index;
  // Degenerate zero-length cues (cleared in the same field they appeared)
  // are dropped.
  if (open_cue_->end_field_index > open_cue_->start_field_index) {
    subtitle_cues_.push_back(std::move(*open_cue_));
  }
  open_cue_.reset();
}

std::string TeletextPageDecoder::extract_subtitle_text(
    const TeletextPageSnapshot& snapshot) {
  // On C5/C6 pages only boxed regions are displayed (EN 300 706 §12.2
  // 0/A-0/B); respect that so stray unboxed bytes never leak into cues.
  const bool boxed_only = snapshot.newsflash || snapshot.subtitle;

  std::string text;
  for (int row = 1; row < TeletextPageSnapshot::kRows; ++row) {
    // Row 24 is not shown where the page's X/27/0 says so (Annex H.2.4).
    if (row == TeletextPageSnapshot::kRows - 1 && !snapshot.display_row_24) {
      continue;
    }
    const auto& cells = snapshot.cells[static_cast<size_t>(row)];
    std::string row_text;
    bool last_was_space = true;  // collapses runs and trims the left edge
    for (int column = 0; column < snapshot.columns; ++column) {
      const TeletextPageCell& cell = cells[static_cast<size_t>(column)];
      // Displayable cells go out as UTF-8 through the cell's own G0 set, so
      // the text reads as the page does: a UK service's 2/3 is "£", not the
      // "#" the transmitted code would be in ASCII (§15.6.2 Table 36), and a
      // Latin word an ESC switched into on a Cyrillic page stays Latin
      // (§15.3).
      std::string glyph;
      const bool shown = !cell.conceal && !cell.double_height_lower &&
                         (!boxed_only || cell.boxed);
      if (shown && cell.enhanced > 0x20) {
        // A Level 1.5 character overwrites whatever the row held there.
        glyph = teletext_unicode_to_utf8(cell.enhanced);
      } else if (shown && cell.enhanced == 0 && !cell.mosaic &&
                 !cell.held_mosaic && !cell.parity_error &&
                 cell.character > 0x20 && cell.character < 0x7F) {
        glyph = teletext_g0_to_utf8(cell.character, cell.g0_set,
                                    cell.national_option_subset);
      }
      if (glyph.empty()) {
        if (!last_was_space) {
          row_text.push_back(' ');
        }
        last_was_space = true;
      } else {
        row_text += glyph;
        last_was_space = false;
      }
    }
    while (!row_text.empty() && row_text.back() == ' ') {
      row_text.pop_back();
    }
    if (!row_text.empty()) {
      if (!text.empty()) {
        text.push_back('\n');
      }
      text += row_text;
    }
  }
  return text;
}

void TeletextPageDecoder::finalize(int64_t end_field_index) {
  for (int magazine = 0; magazine < static_cast<int>(magazines_.size());
       ++magazine) {
    terminate_page(magazine);
  }
  subtitle_clear_event(end_field_index);
}

}  // namespace orc
