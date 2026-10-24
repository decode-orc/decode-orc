/*
 * File:        nabts_datacast.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Decoding of the FEC datacasts NABTS carried (RFC 2728 §3.3,
 *              Appendix A)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_DATACAST_H
#define ORC_NABTS_DATACAST_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nabts_packet.h"

namespace orc {

// RFC 2728 §3.2: a data row is 26 bytes of data and the two bytes of its row
// code; a check row is all column check bytes.
constexpr size_t kNabtsFecDataBytes = 26;
constexpr size_t kNabtsFecRowBytes = 28;

// Data rows a channel must send before it is decoded, and the share of them
// that must pass the row code as received: the census's test for an FEC
// datacast (NabtsServiceCensus), which teletext's own two-byte suffix passes
// by chance on at most 0.02 % of an address.
constexpr uint64_t kNabtsDatacastMinRows = 64;
constexpr uint64_t kNabtsDatacastMinCleanPercent = 50;

// A row RFC 2728 §3.2 flags as not full of data (PS b4) ends in its filler,
// 0x15 or 0xEA. A channel whose flagged rows end in one of them less often
// than this, in percent — where rows full of data would, by chance, under 1 %
// of the time — sets the flag on rows full of data, and its rows are taken
// whole: a service of the recordings flags every row it sends.
constexpr uint64_t kNabtsDatacastFillerEndPercent = 50;

// Bytes of rows held over a pass, all channels together. The largest datacast
// measured sends a little over a million packets on fifty addresses, some
// 36 MB of rows held; past the bound a channel's later rows are counted and
// not held.
constexpr size_t kNabtsDatacastMaxHeldBytes = size_t{256} << 20;

/// How a channel's rows make up its bundles (RFC 2728 §3.3), as the stream
/// shows it.
struct NabtsDatacastLayout {
  /// Data rows to a bundle: 14 in RFC 2728's own layout, 16 in a layout that
  /// sends one check row of the two.
  uint32_t data_rows = 0;
  /// The two check rows in the order they are sent, codeword positions 0 and
  /// 1: 'C' a check row sent, 'M' one the service never sends, whose
  /// continuity index it skips. Empty where the stream shows no two check
  /// positions to a bundle, and the column code is not used.
  std::string checks;
  /// Whether the check rows belong to the data rows after them rather than
  /// those before (as RFC 2728 sends them): decided by which reading the
  /// column code verifies.
  bool checks_lead = false;
};

// A channel that sends its data in no data groups and with no row code — a
// raw stream — is written out as one when it carries at least this many
// packets whose address arrived as transmitted and at most this share of them
// synchronizing: a teletext channel sends a synchronizing packet every few
// dozen, an encoder's idle fill every sixty or so.
constexpr uint64_t kNabtsRawStreamMinPackets = 64;
constexpr uint64_t kNabtsRawStreamMaxSyncPerThousand = 5;

// A stream's continuity index counts its packets (§3.2.4), so the packets it
// says went missing are the recording's losses: about one for every one
// received on the poorest recording measured. A channel whose index says more
// than this many were lost for every one received is no stream: the index of
// another service's line read as NABTS falls anywhere, about seven and a half
// lost a packet, and that of a packet sent over and over never moves, fifteen.
constexpr uint64_t kNabtsRawStreamMaxLostPerPacket = 3;

/// One data row of a datacast's payload (NabtsDatacastChannel::rows).
struct NabtsDatacastRow {
  /// Where the row's bytes begin in the payload and how many there are: its
  /// data less any filler. Zero for a row that went missing.
  uint32_t offset = 0;
  uint16_t length = 0;
  /// The row went missing — lost on the way, or failing its row code with
  /// nothing to rebuild it — and the payload holds nothing of it.
  bool missing = false;
  /// The row failed its row code and nothing could rebuild it, so its bytes
  /// are the payload's as received (NabtsDatacastChannel::rows_damaged).
  bool damaged = false;
  /// The frame of the recording the row arrived in; for a row of a bundle
  /// that the column code rebuilt or that went missing, the frame the bundle's
  /// first row to arrive came in.
  uint32_t frame = 0;
};

/// What decoding one channel came to.
struct NabtsDatacastChannel {
  uint16_t channel = 0;
  /// A raw stream: no data groups and no row code, its data blocks joined in
  /// the order the continuity index gives them, gaps where it skipped.
  bool raw = false;
  NabtsDatacastLayout layout;
  /// Data rows received; check rows received.
  uint64_t data_rows = 0;
  uint64_t check_rows = 0;
  /// Rows passing the row code as received, and rows it put right in one
  /// byte (Appendix A 12.4.3).
  uint64_t rows_clean = 0;
  uint64_t rows_corrected = 0;
  /// Data rows the column code rebuilt: lost on the way, or received failing
  /// a row code it could not correct (Appendix A 12.4.4); and of those, the
  /// rows rebuilt by the column code and the rows' own codes together, in
  /// bundles holding more lost and failing rows than the column code takes
  /// alone (nabts_fec_repair_bundle()).
  uint64_t rows_rebuilt = 0;
  uint64_t rows_rebuilt_jointly = 0;
  /// Data rows lost, or failing their row code, that nothing could rebuild:
  /// gaps in the payload, and rows taken as received.
  uint64_t rows_missing = 0;
  uint64_t rows_damaged = 0;
  /// Columns whose code was checked — every column of a bundle with at most
  /// one row missing — and how many of them it held for.
  uint64_t columns_judged = 0;
  uint64_t columns_verified = 0;
  /// Rows held past kNabtsDatacastMaxHeldBytes: counted, not decoded.
  uint64_t rows_not_held = 0;

  /// The data rows' bytes in order, the filler removed (RFC 2728 §3.2: 0x15
  /// then as many 0xEA as fill the block).
  std::vector<uint8_t> payload;
  /// Places in @ref payload where data rows went missing.
  uint64_t payload_gaps = 0;
  /// The data rows in order, those that went missing held open where the
  /// continuity index or the bundle says they were: what a format framed by
  /// the row, or by the bundle, reads the payload through
  /// (nabts_datacast_files()), since a missing row would otherwise shift
  /// everything after it.
  std::vector<NabtsDatacastRow> rows;

  /// RFC 2728 §3.4-3.5: the payload's SLIP frames, and those whose MPEG-2
  /// CRC-32 holds — an IP datacast's every frame.
  uint64_t frames = 0;
  uint64_t frames_with_crc = 0;
  /// The share of payload bytes that are printable seven-bit text or line
  /// ends, in percent.
  uint32_t text_percent = 0;
  /// File formats whose signatures the payload holds, with how many of each.
  std::vector<std::pair<std::string, uint64_t>> signatures;

  /// One line for the stage report.
  std::string summary() const;
};

/**
 * @brief The FEC datacasts on a recording's packet addresses, decoded
 *
 * RFC 2728 describes the forward error correction a datacast service used on
 * NABTS before it carried IP with it (§3.3: "has been in use for a number of
 * years, and has proven popular with the broadcast industry"): each data row
 * of 26 bytes carries two bytes of a Reed-Solomon row code, and the rows of a
 * bundle are protected down every byte position by two check rows of the
 * same code (Appendix A). The census finds such a channel by its row code
 * (NabtsServiceCensus); this decodes it.
 *
 * Services laid their bundles out differently, and the stream shows how: the
 * commonest run of data rows between check rows is a bundle's data, and what
 * comes after the run — two check rows (RFC 2728's own layout), or a skipped
 * continuity index and one check row (a service sending only the second of the
 * two) — is its check part. Whether those check rows close the bundle before
 * them or open the one after is read off the column code itself: the reading
 * under which its columns verify. Rows are placed by their continuity index
 * and re-anchored at every check row, so a run of lost packets longer than the
 * index can count costs only the bundles it fell in.
 *
 * A bundle is decoded as Appendix A 12.5 recommends at least: each row
 * corrected in one byte where its code says so, then each column, with up to
 * two rows that are missing or still fail their row code replaced from the
 * column code (12.4.4) — which is what brings a lost packet back. A check row
 * a service never sends is one of those two.
 *
 * What the payload means is the service's own: the corpus holds weather file
 * transfers, programme guides and encrypted feeds, and no IP. The decoder says
 * what it can of it without knowing the service: whether its SLIP frames carry
 * RFC 2728's CRC, how much of it is text, and which file formats' signatures
 * it holds.
 *
 * Fed every packet in transmission order; not thread safe.
 */
class NabtsDatacastDecoder {
 public:
  /// Take in |packet|, whose 33 bytes as received are |raw|: the row code
  /// is checked on the bytes as they arrived, before the packet decoder read
  /// a longitudinal check into them that a datacast row does not carry.
  /// |frame| is the frame of the recording it arrived in, which its row keeps
  /// (NabtsDatacastRow::frame).
  void add_packet(const NabtsPacket& packet, const uint8_t* raw,
                  uint32_t frame = 0);

  /// Decode every channel that is an FEC datacast, ascending by channel.
  std::vector<NabtsDatacastChannel> finish();

 private:
  struct Row {
    /// Continuity index steps since the channel's previous row, 1 for the
    /// next (§3.2.4).
    uint8_t step = 1;
    bool check = false;
    /// PS b4: the data block holds filler (RFC 2728 §3.2).
    bool filler = false;
    std::array<uint8_t, kNabtsFecRowBytes> bytes{};
    uint32_t frame = 0;
  };
  struct Held {
    std::vector<Row> rows;
    uint8_t last_continuity = 0;
    bool started = false;
    uint64_t data_rows = 0;
    uint64_t clean_rows = 0;
    uint64_t not_held = 0;
  };

  /// A channel whose packets carry neither groups nor the row code.
  struct RawHeld {
    std::vector<std::vector<uint8_t>> blocks;
    std::vector<uint8_t> steps;
    std::vector<uint32_t> frames;
    uint8_t last_continuity = 0;
    bool started = false;
    uint64_t packets = 0;
    uint64_t synchronizing = 0;
    /// Given up on once synchronizing packets showed it to be a channel of
    /// data groups.
    bool grouped = false;
  };

  /// Hold |packet|, which carries no row code and arrived in |frame|, for a
  /// raw stream.
  void add_raw(const NabtsPacket& packet, uint32_t frame);

  /// |held|'s payload as a raw stream, or nothing where it is a test sequence.
  static std::optional<NabtsDatacastChannel> raw_stream(uint16_t channel,
                                                        const RawHeld& held);

  std::map<uint16_t, Held> channels_;
  std::map<uint16_t, RawHeld> raw_;
  size_t held_bytes_ = 0;
};

// Printable characters a run of the payload must hold to be written out as
// text: a row of teletext's forty, more than chance strings together in
// compressed or encrypted bytes. The text written from one channel is bounded.
constexpr size_t kNabtsDatacastTextRun = 40;
constexpr size_t kNabtsDatacastTextMaxBytes = size_t{4} << 20;

/**
 * @brief The text a datacast payload holds
 *
 * Every run of at least kNabtsDatacastTextRun printable seven-bit characters,
 * a run to a line, line ends within a run (CR, LF, CR LF) kept as line ends.
 * At most kNabtsDatacastTextMaxBytes; empty where the payload holds no run that
 * long.
 */
std::string nabts_datacast_text(const std::vector<uint8_t>& payload);

/// The two RFC 2728 Appendix A sums of |codeword|, |length| bytes long: S0
/// over alpha to each byte's position, S1 over alpha to three times it.
std::array<uint8_t, 2> nabts_fec_sums(const uint8_t* codeword, size_t length);

/// Correct one byte of |codeword| where its sums point at one (Appendix A
/// 12.4.3): true where it now passes, whether it needed to or not.
bool nabts_fec_correct_byte(uint8_t* codeword, size_t length);

/// Replace the bytes of |codeword| at |first| and, where it is not |first|,
/// |second| from its sums (Appendix A 12.4.4). With one position the other
/// sum verifies the replacement, and the result says whether it held; with
/// two nothing is left to verify, and it is true where the equations solve.
bool nabts_fec_replace(uint8_t* codeword, size_t length, size_t first,
                       size_t second);

// The most readings of where a bundle's failing rows are wrong that
// nabts_fec_repair_bundle() tries before giving the bundle up.
constexpr size_t kNabtsFecJointReadings = 4096;

/**
 * @brief A bundle put right by its column code and its rows' own codes
 *        together, where the column code alone cannot; nothing where no one
 *        reading does
 *
 * |rows| holds a bundle's rows by codeword position — its two check rows at 0
 * and 1, its data rows after — each with whether it |arrived| (a row that did
 * not is erased whole, the check row a service never sends among them) and
 * whether it is |good|: its own code holds, or put it right in a byte. A row
 * that arrived failing its code is wrong in two bytes or more. With two rows
 * failing or more and at most one missing, the columns say where: those whose
 * two sums do not agree on the missing row's byte, or with none missing do
 * not hold. Each such column given to one failing row, every failing row two
 * of them at least, is a reading — so many as there are ways to do so, up to
 * kNabtsFecJointReadings — and a reading's erasures, the missing row whole and
 * the failing rows' bytes it names, are filled from the columns' code and the
 * rows' own codes in turn. A reading holds where every column's code and
 * every row's code then holds: the row the bundle never sent is a row of the
 * row code like the rest, so rebuilt from the columns it checks the reading.
 * Exactly one reading must hold; where none does, or two give different
 * rows, nothing is put right. With two rows missing every column's sums go
 * to filling them, and what the rows' codes add follows from the columns' —
 * a product code's checks are not all independent — so no reading could be
 * told from another, and nothing is tried.
 *
 * Pure.
 */
std::optional<std::vector<std::array<uint8_t, kNabtsFecRowBytes>>>
nabts_fec_repair_bundle(
    const std::vector<std::array<uint8_t, kNabtsFecRowBytes>>& rows,
    const std::vector<bool>& arrived, const std::vector<bool>& good);

}  // namespace orc

#endif  // ORC_NABTS_DATACAST_H
