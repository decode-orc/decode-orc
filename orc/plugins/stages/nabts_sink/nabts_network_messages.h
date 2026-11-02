/*
 * File:        nabts_network_messages.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     A network's data services for its stations, read out of the
 *              private-use data groups that carried them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_NETWORK_MESSAGES_H
#define ORC_NABTS_NETWORK_MESSAGES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nabts_data_group.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

// The most leading Hamming 8/4 bytes read as a provider's header: room for
// the longest seen, a station bitmap of 256 bits after the fields before it.
constexpr size_t kNabtsProviderHeaderMaxNibbles = 96;
// The fewest a provider's header has: what the item is, three nibbles of
// service, and a sequence number.
constexpr size_t kNabtsProviderHeaderMinNibbles = 5;

// What a provider header's first nibble says the group holds.
constexpr uint8_t kNabtsNetworkPage = 0x0;
constexpr uint8_t kNabtsNetworkMessage = 0x1;
constexpr uint8_t kNabtsNetworkPageWithText = 0x4;
constexpr uint8_t kNabtsNetworkUrgent = 0x5;
constexpr uint8_t kNabtsNetworkFill = 0xF;

// Group data the log keeps, in bytes; the groups after that are counted. The
// busiest recording of the corpus sends some 1.5 MB.
constexpr size_t kNabtsNetworkMaxBytes = 32UL * 1024UL * 1024UL;

/**
 * @brief Three bytes from four characters of the 4-for-3 coding
 *
 * A link that carries seven-bit characters can carry any byte so: each three
 * bytes as four characters 0x30 + six bits, the low six bits of each byte in
 * turn and then their high two bits, byte k's in bits 2k and 2k+1 of the
 * fourth. Nothing where a character is outside 0x30-0x6F.
 */
std::optional<std::array<uint8_t, 3>> nabts_decode_4for3(const char* four);

// A seven-bit character that failed its odd parity in every copy, as the log
// holds it: its seven bits with this bit set.
constexpr int kNabtsDamagedCharacter = 0x100;

/**
 * @brief A run of the 4-for-3 coding as bytes
 *
 * @param characters Seven-bit characters, a lost one as -1 and a damaged one
 *                   with kNabtsDamagedCharacter set: tokens of four, each
 *                   followed by a space
 * @return The bytes, -1 for each of the three a token held that did not
 *         decode or lost a character; kNabtsDamagedCharacter set on a byte a
 *         damaged character carried part of — its low six bits, or with the
 *         fourth character the top two bits of all three
 */
std::vector<int> nabts_decode_4for3_run(const std::vector<int>& characters);

/**
 * @brief The fields ANPA 1312 gives a news wire message's heading
 *
 * @param heading What lies between SOH and STX, seven-bit, controls kept
 * @return Name and value, in order: the service level and number, the
 *         selector, priority, category, format, keyword, then version,
 *         reference, filing date and word count as far as the heading has
 *         them. Empty where it does not read so.
 */
std::vector<std::pair<std::string, std::string>> nabts_anpa_heading_fields(
    const std::string& heading);

/**
 * @brief The fields of a 4-for-3 coded message's binary heading block
 *
 * A block of 0x100 bytes or more, magic 0xABCD at byte 10, holds the heading
 * of the wire message it precedes in fields of fixed width. Empty where the
 * block is not of that form.
 */
std::vector<std::pair<std::string, std::string>> nabts_heading_block_fields(
    const std::vector<uint8_t>& block);

/**
 * @brief A network message as a document to read
 *
 * What the header says of it, its heading's fields, its text and trailer, and
 * a dump of a binary heading block it carried: what the viewer shows of it and
 * what export_records writes for it.
 */
std::string nabts_network_message_document(const NabtsNetworkMessage& message);

/**
 * @brief A file name for a network message: its service, sequence number and
 *        title, made safe for a file system
 */
std::string nabts_network_message_file_stem(const NabtsNetworkMessage& message,
                                            size_t index);

/**
 * @brief The items of a network's data services, read out of the private-use
 *        groups that carried them
 *
 * A group of the private-use type (§4.2.2) whose data opens with a header of
 * Hamming 8/4 nibbles: what the group holds (a page, a message, an urgent
 * message, a page with text, or fill), three nibbles naming the service, a
 * sequence number, and whom it is for (all stations, a bitmap of them, or a
 * coded list). The header's length is the one the recording proves: a header
 * whose next byte is not a Hamming codeword shows where it ends, and a group
 * whose data happens to open with a codeword — a letter of its text — ends its
 * header where a proven header of its service does.
 *
 * A service's groups are put back into one stream in the order of their
 * sequence numbers: a copy of the group before is merged into it, the bytes
 * one lacked taken from the other, and a sequence number skipped marks the
 * groups lost there. The stream is cut into items at an EOT, at an SOH that no
 * SYN SYN leads, and at a run of NUL padding, and each item read by its form:
 * a news wire message as ANPA 1312 frames it, its heading and text coded 4 for
 * 3 or not; a station message (SOH, text, ETX, a trailer line, EOT); dated
 * text; or NAPLPS code. A service whose groups carry nothing after a header of
 * five nibbles is a status signal, its fifth nibble the value: its runs over
 * the recording are one item.
 *
 * Fed every data group the record layer did not read; keeps those of the
 * private-use type whose channel and header arrived as transmitted. Not thread
 * safe.
 */
class NabtsNetworkMessageLog {
 public:
  /// Take in one data group; |frame| is the frame it ended on.
  void add_group(const NabtsDataGroup& group, uint64_t frame);

  /// Every item read, by channel and service, in order of arrival.
  std::vector<NabtsNetworkMessage> messages() const;

  /// Groups kept, and those past kNabtsNetworkMaxBytes counted instead.
  size_t groups() const { return entries_.size(); }
  uint64_t dropped() const { return dropped_; }

  /// One group as the log keeps it.
  struct Entry {
    uint16_t channel = 0;
    uint64_t frame = 0;
    /// The leading run of Hamming 8/4 codewords, as nibbles.
    std::vector<uint8_t> nibbles;
    std::vector<uint8_t> data;
    std::vector<uint8_t> present;
  };

 private:
  std::vector<Entry> entries_;
  size_t bytes_ = 0;
  uint64_t dropped_ = 0;
};

}  // namespace orc

#endif  // ORC_NABTS_NETWORK_MESSAGES_H
