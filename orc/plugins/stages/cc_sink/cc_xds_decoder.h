/*
 * File:        cc_xds_decoder.h
 * Module:      orc-core
 * Purpose:     Assemble and read the Extended Data Service packets of line 21
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_CORE_CC_XDS_DECODER_H
#define ORC_CORE_CC_XDS_DECODER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace orc {

/// One Extended Data Service packet as it arrived, its checksum good
/// [CTA-608-E §8].
struct CCXdsPacket {
  /// The class, as its start code: 01h current programme, 03h future
  /// programme, 05h channel, 07h miscellaneous, 09h public service, 0Bh
  /// reserved, 0Dh private data.
  uint8_t xds_class = 0;
  uint8_t type = 0;
  /// The informational characters, seven bits each, padding nulls removed.
  std::vector<uint8_t> data;
  /// The field the packet ended on, counted from the start of the run.
  uint64_t field_index = 0;
};

/**
 * @brief Separates the Extended Data Service from the second field's line 21
 *        stream and assembles its packets
 *
 * XDS shares line 21 of the second field with CC3, CC4, TEXT3 and TEXT4
 * [CTA-608-E §8]: a start code (01h-0Dh, odd, then the packet's type) opens a
 * packet, its informational characters follow two to a field, and an end code
 * (0Fh, then the checksum) closes it — the seven-bit sum of everything from the
 * start code to the checksum being zero. A caption or text control code
 * suspends the packet for that service's pairs, and a continue code (the start
 * code plus one, then the type) resumes it.
 *
 * Every second-field pair is offered here first; a pair that is XDS's is not
 * a caption's, and before this the character pairs of a packet were read as
 * CC3 or TEXT3 text.
 *
 * Not thread safe; one instance follows one field's stream in order.
 */
class CCXdsDecoder {
 public:
  using PacketCallback = std::function<void(const CCXdsPacket&)>;

  void set_packet_callback(PacketCallback callback) {
    callback_ = std::move(callback);
  }

  /**
   * @brief Offer one second-field byte pair, parity bits removed
   *
   * @return True when the pair is the Extended Data Service's — a start,
   *         continue or end code, or characters of an open packet — and so
   *         belongs to no caption or text service.
   */
  bool accept(uint8_t byte1, uint8_t byte2, uint64_t field_index);

  /// Packets whose checksum held, and packets that ended with one that did
  /// not, or ran past the longest packet the standard allows.
  uint64_t packets_good() const { return packets_good_; }
  uint64_t packets_bad() const { return packets_bad_; }

 private:
  struct Open {
    CCXdsPacket packet;
    unsigned sum = 0;
  };

  PacketCallback callback_;
  // Packets opened and not yet ended, by {class, type}: a packet suspended for
  // a caption is resumed by its continue code. A handful at most.
  std::map<std::pair<uint8_t, uint8_t>, Open> open_;
  // The packet characters are going to, while one is.
  bool in_packet_ = false;
  std::pair<uint8_t, uint8_t> current_{0, 0};
  uint64_t packets_good_ = 0;
  uint64_t packets_bad_ = 0;
};

/// What an XDS class and type are, as CTA-608-E names them: "Channel: network
/// name". Empty for a type it does not define.
std::string cc_xds_type_name(uint8_t xds_class, uint8_t type);

/**
 * @brief An XDS packet's value as a reader would want it
 *
 * Text for the types that carry text (programme name, network name, call
 * letters, programme description, weather message); the date and time for the
 * time of day and a programme's start; hours and daylight saving for the time
 * zone; and decoded as CTA-608-E §9.5 codes them, a programme's length and
 * time in show, its type's keywords, its content advisory (MPA, U.S. TV
 * Parental Guidelines, the Canadian English and French systems), its audio
 * and caption services and copy control, the channel's tape delay and
 * transmission signal identifier. The informational characters in hexadecimal
 * otherwise: private data, and the types and rating systems the standard
 * reserves.
 */
std::string cc_xds_value_text(const CCXdsPacket& packet);

/**
 * @brief What a recording's XDS said, for the report
 *
 * Keeps, per class and type, how many packets arrived and the values they
 * carried, the commonest few, and the span the time of day covered — enough
 * to say what station a recording is of, what was on and when, at a bounded
 * cost however long the recording.
 */
class CCXdsSummary {
 public:
  void add(const CCXdsPacket& packet);

  /// The report's lines, or nothing when no packet arrived.
  std::vector<std::string> lines() const;

 private:
  struct Values {
    uint64_t packets = 0;
    // Value -> packets, and the order values first arrived in.
    std::map<std::string, uint64_t> counts;
    std::vector<std::string> order;
  };
  std::map<std::pair<uint8_t, uint8_t>, Values> types_;
  std::string first_time_;
  std::string last_time_;
};

}  // namespace orc

#endif  // ORC_CORE_CC_XDS_DECODER_H
