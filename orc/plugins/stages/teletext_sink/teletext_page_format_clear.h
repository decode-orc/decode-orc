/*
 * File:        teletext_page_format_clear.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Data a teletext service sent on pages: EN 300 708 Page
 *              Format - Clear streams, and the scrambled pages of Page
 *              Format - CA
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_TELETEXT_PAGE_FORMAT_CLEAR_H
#define ORC_TELETEXT_PAGE_FORMAT_CLEAR_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "vbi-services/teletext_slicer.h"

namespace orc {

// Stream bytes held across every data page stream, past which more pages are
// counted and not kept.
constexpr size_t kTeletextPageDataMaxBytes = size_t{16} << 20;
// Data page streams held at once.
constexpr size_t kTeletextPageDataMaxStreams = 64;
// Received packets a page needs to be taken for a data page on its packets'
// structure alone, with no packet 28 to say so.
constexpr int kTeletextPageDataMinPackets = 4;

/**
 * @brief One application's data from a Page Format - Clear stream
 *
 * EN 300 708 §4: a page number and the Data Stream Identifier of its sub-code
 * name a stream; within it, each block of application data carries the
 * Application ID it belongs to (§4.2.2).
 */
struct TeletextPageDataStream {
  /// Displayed magazine 1-8 and page number of the stream's page.
  int magazine = 8;
  int page_number = 0;
  /// The Data Stream Identifier, sub-code S3 (§4.3.1.2 Table 2): stream
  /// |stream_id| + 1.
  int stream_id = 0;
  /// The Application ID, 1-31, and its type from the stream's latest Bundle
  /// Information (§4.2.3); unset where none arrived.
  int application = 0;
  std::optional<uint16_t> application_type;
  /// Pages of the stream read (a page's fragments and repeats once), pages
  /// the continuity index says went missing, and packets lost from pages
  /// that came.
  uint64_t pages = 0;
  uint64_t pages_lost = 0;
  uint64_t packets_lost = 0;
  /// Blocks of the application read whole, and those that lost bytes or
  /// whose length the stream contradicted, left out of the payload.
  uint64_t blocks = 0;
  uint64_t blocks_damaged = 0;
  /// The whole blocks, in order.
  std::vector<uint8_t> payload;

  /// "1F0-s1-a3": the page, the stream, the application.
  std::string tag() const;
  /// One line for the stage report.
  std::string summary() const;
};

/**
 * @brief Reads the data a service carried on teletext pages
 *
 * Fed every packet in transmission order. A page is a Page Format - Clear
 * data page (EN 300 708 §4) where its packet X/28/0 says so — page function
 * 0001, page coding 001 (Table 4) — or, with no packet 28, where the page's
 * own packets show it: every one opens with a Block Pointer (§4.3.2.4) and
 * each pointer that points finds the Block Separator there. Its packets 1 to
 * the Last Transmitted Packet (sub-code S2 and S4) are its bytes, in order;
 * the pages of a stream follow by their continuity index (S1), fragments and
 * repeats of one page combined. The stream is then cut into blocks at the
 * separators: a Structure Header (Application ID and length, Hamming 8/4),
 * the block, filler bytes. A block that lost a byte to a missing packet, or
 * whose length runs past the next pointed separator, is counted and left
 * out, and the stream resumes at the next separator a pointer points to.
 * Blocks of Application ID 0 are the Bundle Information, which names each
 * application's type; their checksum must hold.
 *
 * A page with a packet X/28/2 is a scrambled page of Page Format - CA (§5):
 * its page key is enciphered under the subscriber's keys, so it is only
 * counted.
 *
 * Only packets whose address arrived as Hamming 8/4 codewords are read. Pure:
 * no I/O. Bounded by kTeletextPageDataMaxBytes and kTeletextPageDataMaxStreams.
 */
class TeletextPageFormatClear {
 public:
  void add_packet(const std::array<uint8_t, kTeletextPacketBytes>& packet,
                  size_t packet_bytes);

  /// Each application of each stream, by page, stream and application.
  std::vector<TeletextPageDataStream> streams() const;
  /// Transmissions of scrambled pages (Page Format - CA), and the pages.
  uint64_t scrambled_transmissions() const { return scrambled_; }
  const std::set<std::pair<int, int>>& scrambled_pages() const {
    return scrambled_pages_;
  }

 private:
  using Packet = std::array<uint8_t, kTeletextPacketBytes>;
  static constexpr size_t kRows = 26;  // packets 0 to 25

  // One transmission of a page in progress in a magazine.
  struct OpenPage {
    bool open = false;
    int magazine = 0;  // transmission magazine 0-7
    int page_number = 0;
    int continuity = 0;
    int last_packet = 0;
    int stream_id = 0;
    bool marked_clear = false;
    bool marked_scrambled = false;
    std::array<std::optional<Packet>, kRows> rows{};
  };

  // A page of a stream awaiting the next continuity index, its fragments
  // combined.
  struct PendingPage {
    int continuity = 0;
    int last_packet = 0;
    std::array<std::optional<Packet>, kRows> rows{};
  };

  // A stream's bytes, which arrived, and where its pointers point.
  struct Stream {
    int magazine = 8;
    int page_number = 0;
    int stream_id = 0;
    uint64_t pages = 0;
    uint64_t pages_lost = 0;
    uint64_t packets_lost = 0;
    std::optional<PendingPage> pending;
    std::vector<uint8_t> bytes;
    std::vector<bool> present;
    std::vector<size_t> pointed;
  };

  void close_page(int magazine);
  /// Append |page|'s packets to |stream|, as far as |held_bytes| allows.
  static void append_page(Stream& stream, const PendingPage& page,
                          size_t packet_bytes, size_t& held_bytes);
  static bool looks_like_data_page(const OpenPage& page, size_t packet_bytes);
  /// Cut |stream| into its applications' blocks.
  static std::vector<TeletextPageDataStream> parse(const Stream& stream);

  std::array<OpenPage, 8> open_{};
  std::map<std::tuple<int, int, int>, Stream> streams_;
  size_t packet_bytes_ = kTeletextPacketBytes;
  size_t held_bytes_ = 0;
  uint64_t scrambled_ = 0;
  std::set<std::pair<int, int>> scrambled_pages_;
};

}  // namespace orc

#endif  // ORC_TELETEXT_PAGE_FORMAT_CLEAR_H
