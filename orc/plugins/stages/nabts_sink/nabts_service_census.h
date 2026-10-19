/*
 * File:        nabts_service_census.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Recognise the services on a NABTS recording that are not
 *              teletext: FEC datacasts, idle fill, test sequences and clocks
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_SERVICE_CENSUS_H
#define ORC_NABTS_SERVICE_CENSUS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "nabts_data_group.h"
#include "nabts_packet.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

// Packets an address needs before anything is said about it. Fewer is noise
// that corrected into an address, or too little of a service to name.
constexpr uint64_t kNabtsCensusMinPackets = 20;

// Addresses followed at once. A real recording carries a few dozen (up to
// about 70 on one datacast tape); an address that arrived as transmitted
// beyond this many is counted as unfollowed rather than given state.
constexpr size_t kNabtsCensusMaxChannels = 256;

// Distinct packet contents an idle fill can be made of. The Norpak BERT
// pattern is 16 contents and its group header, with
// 34-39 distinct in all once damaged copies are counted; teletext of any size
// runs past this quickly, because its contents repeat only once a carousel
// cycle is over.
constexpr size_t kNabtsIdleFillMaxContents = 256;

// Packets of a linear-feedback shift register sequence sampled for its linear
// complexity (Berlekamp-Massey over the 224 bits after the prefix), and the
// complexity at or under which a packet counts as a stretch of one.
constexpr uint32_t kNabtsLfsrSamplePackets = 64;
constexpr uint32_t kNabtsLfsrMaxComplexity = 32;

/**
 * @brief Whether a byte follows the one before it as one step of a byte-serial
 *        pseudo-random pattern
 *
 * The bit-error-rate test pattern Norpak encoders send as idle fill makes each
 * byte from the one before it: its seven information bits are the previous
 * byte's moved down one place with one new bit entering at the top, and the
 * eighth is odd parity. So bits 0-5 of each byte are bits 1-6 of the one
 * before. Where the pattern is sent, 99.6 % of the pairs of unequal bytes on
 * its address relate this way; on other addresses at most 6.9 % do — 1 in 64
 * is chance. Equal neighbours say nothing either way and
 * are not judged.
 */
constexpr bool nabts_serial_pattern_step(uint8_t previous, uint8_t next) {
  return (next & 0x3F) == ((previous >> 1) & 0x3F);
}

/**
 * @brief Recognises the services on a recording that are not teletext
 *
 * Fed every packet the sink recovers, in broadcast order, and asked at the end
 * what each packet address carried. Four kinds of service are recognised from
 * the packets alone, each by a test teletext does not pass:
 *
 *  - an FEC datacast: packets with the two-byte suffix (CEA-516 §3.4 PS b8,b6 =
 *    1,0) whose suffix is the Reed-Solomon row code of RFC 2728 Appendix A
 *    (Norpak's FEC: GF(2^8) modulo x^8+x^4+x^3+x^2+1, roots alpha and alpha^3
 *    for alpha = 00011101, so g(x) = x^2 + 146x + 157), with how its bundles
 *    lay out data and check rows (packets with the 28-byte suffix);
 *  - idle fill: at most kNabtsIdleFillMaxContents distinct packet contents,
 *    repeated, each a byte-serial pseudo-random pattern
 *    (nabts_serial_pattern_step()) — the bit-error-rate test (BERT) data an
 *    encoder sends when it has nothing else to send, which the Ross Video
 *    (Norpak) TES3/TES5 encoders always send on address F00;
 *  - a test sequence: packets that are stretches of one short linear-feedback
 *    shift register sequence, judged by Berlekamp-Massey;
 *  - a clock: packets of Hamming 8/4 bytes carrying a Unix time of day that
 *    advances from packet to packet.
 *
 * Only packets whose address arrived as Hamming 8/4 codewords are counted
 * (NabtsPacket::address_attested): a corrected address may be a neighbour's.
 *
 * State is bounded per address — a capped set of contents, a capped sample of
 * packets for the shift-register test — and at most kNabtsCensusMaxChannels
 * addresses are followed, so the census costs the same on a recording of an
 * hour as on one of a minute.
 *
 * Not thread safe: fed from the one thread that walks the emitted packets.
 */
class NabtsServiceCensus {
 public:
  /**
   * @brief Take in one recovered packet
   *
   * @param packet The packet as nabts_decode_packet() decoded it
   * @param raw    Its kNabtsPacketBytes bytes as sliced, prefix first: the row
   *               code is checked over the bytes as received, before the
   *               longitudinal check has had a chance to change one
   */
  void add_packet(const NabtsPacket& packet, const uint8_t* raw);

  /// What the addresses that are not teletext carried, ascending by address,
  /// consecutive addresses carrying one service as one entry.
  std::vector<NabtsOtherService> services() const;

 private:
  struct ChannelCensus {
    uint64_t packets = 0;

    // FEC datacast: two-byte-suffix packets, and those passing the row code.
    uint64_t row_coded = 0;
    uint64_t row_clean = 0;
    // Check rows (28-byte suffix), and how the bundles around them run: the
    // length of each run of data rows between check rows and of each run of
    // check rows, and how many pairs of check rows sat at CI 14 and 15.
    uint64_t check_rows = 0;
    uint64_t check_pairs = 0;
    uint64_t check_pairs_at_14 = 0;
    uint32_t data_run = 0;
    uint32_t check_run = 0;
    uint8_t last_ci = 0;
    bool last_was_check = false;
    std::array<uint64_t, 33> data_runs{};
    std::array<uint64_t, 5> check_runs{};

    // Idle fill: the distinct contents of the packets the suffix check
    // vouches for, until there are too many to be fill, and how the bytes of
    // those packets relate to their neighbours.
    uint64_t vouched = 0;
    std::unordered_set<uint64_t> contents;
    bool too_many_contents = false;
    uint64_t serial_pairs = 0;
    uint64_t serial_steps = 0;

    // Test sequence: the first kNabtsLfsrSamplePackets packets' complexity.
    uint32_t lfsr_sampled = 0;
    std::map<uint64_t, uint32_t> lfsr_polynomials;

    // Clock: Hamming 8/4 bytes, and the time packets decoded from them.
    uint64_t hamming_bytes = 0;
    uint64_t hamming_clean = 0;
    uint64_t time_packets = 0;
    uint64_t time_steps = 0;
    int64_t last_time = -1;
    int64_t first_confirmed = -1;
    int64_t last_confirmed = -1;
    std::map<std::string, uint32_t> names;
  };

  void count_row_code(ChannelCensus& census, const NabtsPacket& packet,
                      const uint8_t* raw) const;
  void count_contents(ChannelCensus& census, const NabtsPacket& packet) const;
  void count_lfsr(ChannelCensus& census, const NabtsPacket& packet) const;
  void count_clock(ChannelCensus& census, const NabtsPacket& packet) const;

  /// What one address carried, or nothing it can be named as.
  struct Verdict {
    NabtsOtherService::Kind kind = NabtsOtherService::Kind::kFecDatacast;
    /// What makes two addresses carry the same service — consecutive ones
    /// with equal keys are reported as one.
    std::string key;
    /// The description, less the packet count.
    std::string text;
    /// A clock's span and name, which decide whether two addresses carry
    /// the same clock.
    int64_t clock_first = 0;
    int64_t clock_last = 0;
    std::string clock_name;
  };
  bool classify(uint16_t channel, const ChannelCensus& census,
                Verdict& verdict) const;
  /// A clock's description from its span and name.
  static std::string clock_text(const Verdict& clock);

  std::map<uint16_t, ChannelCensus> channels_;
};

// Private-use text: a group is text when at least this many of its bytes that
// arrived are seven-bit characters with odd parity — printable, or a line
// break or tab — and at least this share of them are. Teletype groups read
// 82-99 % (a binary header of their own opens each), Hamming-coded control
// groups about 10 %.
constexpr size_t kNabtsPrivateTextMinBytes = 16;
constexpr size_t kNabtsPrivateTextMinPercent = 75;

// Text kept for the companion file, in bytes; the groups after that are
// counted. The densest recording of the corpus carries some 200 kB.
constexpr size_t kNabtsPrivateTextMaxBytes = 4UL * 1024UL * 1024UL;
// What each channel and type keeps to show in the viewer, in bytes of text or
// dump; its groups after that are counted.
constexpr size_t kNabtsPrivateShownBytes = 1024UL * 1024UL;

/**
 * @brief The data a service sent in data groups of a type CEA-516 reserves
 *
 * CEA-516 §4.2.2 reserves group type 15 for the service provider's private use
 * and every other non-zero type for a standardization that never came, and
 * the record layer reads no record out of such a group (it counts them,
 * NabtsRecordStats). A service can use them for teletype traffic riding beside
 * its pages: news rundowns, wire stories and satellite feed notices sent to
 * affiliates, or broadcast-control messages. Such data is seven-bit ASCII with
 * odd parity and CR LF line ends, after a short binary header of the
 * provider's own; other groups carry data of the provider's own coding.
 *
 * Fed every data group the record layer did not read
 * (NabtsRecordAssembler::add_group()); keeps, in the order received, each
 * group of a non-zero type whose channel and header arrived as transmitted:
 * as text where its data reads as text — what was received of it, bytes that
 * are not text shown as '.', bytes lost with their packet as "[...]" — and
 * otherwise as a dump of its bytes, sixteen to a line with the characters
 * they read as, a byte lost shown as "??".
 *
 * Not thread safe.
 */
class NabtsPrivateTextLog {
 public:
  /// Take in one data group; |frame| is the frame it ended on.
  void add_group(const NabtsDataGroup& group, uint64_t frame);

  /// One entry per channel and type whose groups read as text, ascending.
  std::vector<NabtsOtherService> services() const;

  /// The companion file: every group under a line naming its channel, frame,
  /// type and group continuity, its text or the dump of its bytes. Empty when
  /// no group was kept.
  std::string document() const;

  /// Every channel and type of groups kept, ascending, with what each showed.
  std::vector<NabtsPrivateData> channels() const;

 private:
  struct Channel {
    uint64_t groups = 0;
    uint64_t packets = 0;
    uint64_t text_groups = 0;
    uint64_t characters = 0;
    std::string shown;
    uint64_t not_shown = 0;
  };
  std::map<std::pair<uint16_t, uint8_t>, Channel> channels_;
  std::string document_;
  /// Groups past kNabtsPrivateTextMaxBytes, counted and not kept.
  uint64_t dropped_ = 0;
};

/**
 * @brief The report's block naming the services that are not teletext
 *
 * One line per service: its addresses, then what it is. Empty when there are
 * none.
 */
std::string nabts_other_services_summary(
    const std::vector<NabtsOtherService>& services);

/**
 * @brief The syndromes of RFC 2728 Appendix A's row code over one data block
 *
 * @param block The 28 bytes after the prefix: 26 data bytes, then the two
 *              check bytes
 * @return {S0, S1}; both zero on a block that passes
 *
 * Codeword position 0 is the second-last byte and position 1 the last, the
 * data bytes following from position 2; S0 is the sum of c_i alpha^i and S1 of
 * c_i alpha^3i, over GF(2^8) modulo x^8+x^4+x^3+x^2+1 with alpha = 00011101.
 */
std::array<uint8_t, 2> nabts_rfc2728_row_syndromes(const uint8_t* block);

/**
 * @brief Linear complexity of a bit string, and its shortest generator
 *
 * Berlekamp-Massey over GF(2). @p polynomial receives the connection
 * polynomial as a bit mask, bit k the coefficient of x^k, when the complexity
 * is at most 63; it is left zero otherwise.
 */
uint32_t nabts_linear_complexity(const std::vector<uint8_t>& bits,
                                 uint64_t& polynomial);

/// A Unix time as "YYYY-MM-DD HH:MM:SS" UTC.
std::string nabts_utc_text(int64_t unix_seconds);

}  // namespace orc

#endif  // ORC_NABTS_SERVICE_CENSUS_H
