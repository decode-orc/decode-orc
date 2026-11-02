/*
 * File:        teletext_data_channels.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     The data a teletext service sent outside its pages: broadcast
 *              service data and independent data lines
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_TELETEXT_DATA_CHANNELS_H
#define ORC_TELETEXT_DATA_CHANNELS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "vbi-services/teletext_slicer.h"

namespace orc {

// Data the data lines' payloads may hold between them, past which more is
// counted and not kept.
constexpr size_t kTeletextDataChannelMaxBytes = size_t{16} << 20;
// Streams held across every data channel.
constexpr size_t kTeletextDataChannelMaxStreams = 64;

// Evidence a stream needs before it is believed, where its packets carry no
// check strong enough to rule out one misread packet. Audio has none, so it
// needs a run of packets, as a real service sends; a Format B packet's own
// code puts right some byte of one random packet in seven, so two must hold
// as received; an implicit continuity indicator checks only eight bits, so
// it takes two packets. Packet 8/30, its fields mostly unprotected, needs a
// few: a service sends one every second or so.
constexpr uint64_t kTeletextAudioMinPackets = 32;
constexpr uint64_t kTeletextCheckedMinPackets = 2;
constexpr uint64_t kTeletextServiceDataMinPackets = 3;

// The generators of the data lines' check words less their x^16 terms:
// x^16 + x^9 + x^7 + x^4 + 1 for IDL Format A (EN 300 708 §6.5.8.1) and
// x^16 + x^12 + x^5 + 1 for Datavideo (§6.6.6.1).
constexpr uint16_t kTeletextIdlFormatAPolynomial = 0x0291;
constexpr uint16_t kTeletextDatavideoPolynomial = 0x1021;

/**
 * @brief What packet 8/30 — broadcast service data — said over a recording
 *
 * ETSI EN 300 706 §9.8. Format 1 names the network (its Network
 * Identification code, TR 101 231), the page a receiver should show first,
 * the time and date (Modified Julian Date and UTC, each digit sent one up, and
 * the local time's offset), and a status line of twenty characters; Format 2
 * carries the same page and status line with a programme label for video
 * recorders (EN 300 231 §8.2.1) in place of the network and the time.
 */
struct TeletextServiceData {
  /// Packets of each format read: their designation code arrived whole.
  uint64_t format1_packets = 0;
  uint64_t format2_packets = 0;
  /// Designation code bit 1: the service is sent within a video signal
  /// (§4.1) rather than on every line of one carrying data alone (§4.2), as
  /// the last packet read said.
  bool multiplexed = false;
  /// The initial page, as the decoder would show it ("100", or "100:0003"
  /// with a sub-code), from the commonest reading; empty where none was
  /// given (page FF, §9.8.1 NOTE 2).
  std::string initial_page;
  /// Network Identification codes and the Format 1 packets that carried each.
  std::map<uint16_t, uint64_t> network_ids;
  /// The status display (bytes 26-45), from the commonest reading of it.
  std::string status_display;
  /// The time and date the Format 1 packets gave, first and last, in seconds
  /// since 1970-01-01 UTC, and the local time's offset from UTC.
  std::optional<int64_t> first_utc;
  std::optional<int64_t> last_utc;
  int offset_minutes = 0;
  /// The programme labels of Format 2 (bytes 13-25), each distinct one as
  /// text — its label channel, network, programme identification label,
  /// sound and programme type — with how many packets gave it.
  std::map<std::string, uint64_t> programme_labels;

  /// Whether enough packets were read to believe them
  /// (kTeletextServiceDataMinPackets).
  bool any() const {
    return format1_packets + format2_packets >= kTeletextServiceDataMinPackets;
  }
  /// Lines for the stage report, empty where no packet was read.
  std::vector<std::string> summary() const;
};

/// The ways EN 300 708 §6 codes an independent data line.
enum class TeletextDataFormat {
  kIdlFormatA,  ///< §6.5: addressed packets with a check word
  kIdlFormatB,  ///< §6.8: application packets in bundles with FEC
  kDatavideo,   ///< §6.6: addressed packets, optionally masked
  kAudio,       ///< §6.7: low bit-rate audio
};

/**
 * @brief One stream of an independent data line service (EN 300 708 §6)
 *
 * Data a service sent in packets numbered 30 or 31 rather than on pages: a
 * data channel (the sixteen §6.4.2 numbers) and, within it, the packets of
 * one address — a Format A service packet address, a Format B application, a
 * Datavideo packet address or an audio service. The user data of the packets
 * believed, in the order they arrived, repeats dropped, is its payload.
 */
struct TeletextDataStream {
  /// Data channel 0-15 (EN 300 708 Table 16).
  int channel = 0;
  TeletextDataFormat format = TeletextDataFormat::kIdlFormatA;
  /// Format A: the service packet address, in |address_digits| hexadecimal
  /// digits (0 where none was sent). Format B: the application identifier,
  /// with the application number in bits 4 and 5. Datavideo: the 12-bit
  /// packet address. Audio: the service byte.
  uint32_t address = 0;
  int address_digits = 0;
  /// Packets believed, repeats dropped among them, and new packets the
  /// continuity indicator says went missing.
  uint64_t packets = 0;
  uint64_t repeats = 0;
  uint64_t gaps = 0;
  /// Packets whose own check held as received: a Format A packet with an
  /// explicit continuity indicator, a Format B packet whose code needed no
  /// correction.
  uint64_t packets_checked = 0;
  /// Format B: bytes the forward error correction put right, the data
  /// packets it restored whole, and those it could not — lost from the
  /// payload, or kept with errors it could not reach.
  uint64_t bytes_corrected = 0;
  uint64_t rows_restored = 0;
  uint64_t rows_lost = 0;
  uint64_t rows_damaged = 0;
  /// Datavideo: packets with the packet type bit set, which a higher layer
  /// reads as holding a length or as control data (§6.6.4); and packets that
  /// arrived masked (§6.6.3).
  uint64_t typed_packets = 0;
  uint64_t masked_packets = 0;
  /// Audio: the control byte's values and the packets that carried each.
  std::map<int, uint64_t> controls;
  std::vector<uint8_t> payload;

  /// Whether the packets are evidence enough of a service (see
  /// kTeletextAudioMinPackets).
  bool believed() const;
  /// The format's name, as the report gives it.
  std::string format_name() const;
  /// What distinguishes the stream's file beside the stream: "8", "8-2A",
  /// "9-b1-3", "5-123" or "4-s0".
  std::string tag() const;
  /// One line for the stage report.
  std::string summary() const;
};

/**
 * @brief Reads the packets of a teletext service that carry data rather than
 *        pages: packet 8/30 and the independent data lines
 *
 * Fed every packet in transmission order. Its first address byte numbers a
 * data channel (EN 300 708 §6.4.2) and Table 16 says how each is coded:
 * packet 8/30 (channel 0) as broadcast service data (TeletextServiceData);
 * channels 8 to 11 as IDL Format A or B, which the format type tells apart,
 * and 15 as Format B; 5, 6, 13 and 14 as Datavideo; 4 and 12 as low bit-rate
 * audio. Channels 1, 2, 3 and 7 have no format and are only counted.
 *
 * Only a packet whose address arrived as codewords, whose header bytes decode,
 * and whose check holds — Format A's and Datavideo's check word, Format B's
 * error correction, and for audio, which has no check, a service byte that
 * arrived as a codeword — is believed: a recording's damaged packets misread
 * as packets 30 or 31 do none of that. The rest are counted.
 *
 * Pure: no I/O. Bounded by kTeletextDataChannelMaxBytes and
 * kTeletextDataChannelMaxStreams.
 */
class TeletextDataChannels {
 public:
  /// Read |packet|, of which |packet_bytes| are the service's (42 on 625
  /// lines, 34 on 525).
  void add_packet(const std::array<uint8_t, kTeletextPacketBytes>& packet,
                  size_t packet_bytes);

  const TeletextServiceData& service_data() const { return service_; }
  /// The data lines read and believed (TeletextDataStream::believed()), by
  /// channel and address. A Format B bundle still open is decoded as far as
  /// it arrived.
  std::vector<TeletextDataStream> streams() const;
  /// Packets numbered 30 or 31 that were not believed: a header byte did not
  /// decode, their check failed, or their stream was too little to believe.
  uint64_t packets_refused() const;
  /// Packets on the data channels Table 16 gives no format.
  uint64_t packets_unassigned() const { return unassigned_; }

 private:
  using Packet = std::array<uint8_t, kTeletextPacketBytes>;

  // A Format B bundle (§6.8.2.2): sixteen packets by continuity index, each
  // 35 bytes of user data and two suffix bytes.
  static constexpr size_t kBundleRows = 16;
  static constexpr size_t kBundleDataRows = 14;
  static constexpr size_t kBundleColumns = 37;
  struct BundleRow {
    std::array<uint8_t, kBundleColumns> bytes{};
    bool present = false;
    bool clean = false;
  };
  using Bundle = std::array<BundleRow, kBundleRows>;

  struct Held {
    TeletextDataStream stream;
    std::optional<int> last_continuity;
    // Datavideo: the continuity indices of the recent new packets, so a
    // repeat sent after newer packets is known for one (§6.6.2.1).
    std::vector<int> recent;
    Bundle bundle{};
  };

  void add_service_data(const Packet& packet, size_t packet_bytes,
                        int designation);
  void add_format_a(const Packet& packet, size_t packet_bytes, int channel);
  void add_format_b(const Packet& packet, size_t packet_bytes, int channel,
                    int format);
  void add_datavideo(const Packet& packet, size_t packet_bytes, int channel);
  void add_audio(const Packet& packet, size_t packet_bytes, int channel);

  /// The stream of |format| at |address| on |channel|, made if there is
  /// room; nullptr if there is not.
  Held* stream_for(int channel, TeletextDataFormat format, uint32_t address,
                   int address_digits);
  /// Append |count| bytes to |stream|'s payload, as far as the bound allows.
  static void keep(TeletextDataStream& stream, const uint8_t* bytes,
                   size_t count, size_t& held_bytes);
  /// Decode |bundle| into |stream|'s payload.
  static void decode_bundle(TeletextDataStream& stream, Bundle bundle,
                            size_t& held_bytes);

  TeletextServiceData service_;
  uint64_t multiplexed_packets_ = 0;
  std::optional<int64_t> last_time_read_;
  std::map<std::string, uint64_t> initial_pages_;
  std::map<std::string, uint64_t> status_displays_;
  std::map<std::tuple<int, int, uint32_t>, Held> streams_;
  size_t held_bytes_ = 0;
  uint64_t refused_ = 0;
  uint64_t unassigned_ = 0;
};

/**
 * @brief The register a data line's check word leaves (EN 300 708 Figures 19
 *        and 21)
 *
 * |bytes| fed bit by bit in transmission order — each byte least significant
 * bit first — through the generator |polynomial| (less its x^16 term) from
 * all zeros, register bit 0 the stage the feedback enters. Fed the checked
 * bytes and their check word, it is zero for a packet whose check holds; for
 * a Format A packet with an implicit continuity indicator it holds the
 * indicator in both bytes instead, least significant bit at the register's
 * output end (bit 7 of each byte).
 */
uint16_t teletext_data_line_crc_register(const uint8_t* bytes, size_t count,
                                         uint16_t polynomial);

}  // namespace orc

#endif  // ORC_TELETEXT_DATA_CHANNELS_H
