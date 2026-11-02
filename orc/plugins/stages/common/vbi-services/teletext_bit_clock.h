/*
 * File:        teletext_bit_clock.h
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     Learns the bit clock a recording's data lines were sampled at
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_TELETEXT_BIT_CLOCK_H
#define ORC_TELETEXT_BIT_CLOCK_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "teletext_slicer.h"

namespace orc {

/// Bytes of |result| that check themselves: Hamming 8/4 addressing bytes that
/// arrived as codewords, and data bytes with odd parity. Zero for a line that
/// yielded no packet.
uint32_t teletext_verified_bytes(const TeletextLineResult& result);

/**
 * @brief Learns the bit clock a recording's data lines were sampled at
 *
 * ETSI EN 300 706 §5.3 and ITU-R BT.653 Table 1b hold the System B bit rate to
 * 25 ppm and CEA-516 §1.3 the System C rate to 16 bit/s, and a
 * time-base-corrected capture keeps it, so the slicer's nominal bit period is
 * right. A card sampling against its own crystal instead reads the tape at
 * whatever speed the transport ran: the NBC 1983 cx23885 transfers run 0,06
 * to 0,09 % slow, up to a third of a bit by the end of a packet.
 *
 * The clock run-in cannot measure that. Eight cycles through a tape channel,
 * starting on the burst's own transient and running into the framing code,
 * fit a sine to within a couple of per cent, and hundredths of one matter
 * here. So the clock is read off what decodes: while it is being learned,
 * every line that yields a packet is read again at each trial clock, and once
 * enough have been scored the clock that reads the most packets with every
 * byte checking out is adopted — a slipping clock costs the last bytes of a
 * packet, so whole packets are what it shows in. Both clocks read the same
 * packets, so only the packets they disagree on say anything: a trial clock
 * must win more of those than it loses by three standard deviations of their
 * number (McNemar's test), since on a corrected capture every trial scores
 * within noise of the nominal clock.
 *
 * The test is taken after kPackets packets and, while it is open, again after
 * twice and four times as many. A detector that reads most packets whole at
 * the nominal clock leaves few for the clocks to disagree on: on three card
 * dumps read best at 0,06 to 0,09 % slow (whole packets 81 to 89 %, 64 to 69 %
 * and 93 to 96 % over the capture), the slow clocks lead by 2,5 to 2,8 standard
 * deviations after 400 packets and pass after 800 or 1,600. A real lead grows
 * with the square root of the packets scored, so scoring goes on only while the
 * best trial's lead, grown at that rate, would pass by kMaxPackets; on the
 * time-base-corrected captures tried, and the card dumps read best at the
 * nominal clock, it stops at the first test or the second.
 *
 * On System B one kind of byte is left out of the test: a data byte that
 * either reading holds as an alternating pattern, 0x55 or 0xAA — the clock
 * run-in's own (ETSI EN 300 706 §6.1). Neither has odd parity (§8.1), so a
 * parity-coded row never carries one as a character, and a service that sends
 * one sends it as something else: VirText pages, in Zenith's own packet
 * layout, open every row and close every packet with 0x55. It is also the
 * pattern a slightly wrong clock misreads first, into a neighbour that passes
 * (0x54, 0x45, 0x15), so judged it made those packets whole at wrong clocks
 * only. On quest_for_fire, of the 2,285 packets of a sixth of the recording
 * that 0,18 % slow read whole and the nominal clock did not, 2,233 had failed
 * at the nominal clock on 0x55 alone; without them every slow trial clock lost
 * to the nominal one, by 8 to 94 standard deviations. With them the recording
 * was read 0,24 % slow, and against its combined rows that reading had 86
 * times the wrong bytes the nominal clock's has, nearly all of them the right
 * bits a bit early or late in the last bytes of a packet. System C is judged
 * on every byte: CEA-516 §3.3 codes its data bytes for parity in some data
 * groups only, and no service is known to frame them.
 */
class TeletextClockCalibrator {
 public:
  /// Trial bit clocks, as fractions of the nominal rate.
  static constexpr std::array<double, 14> kTrialClocks = {
      0.9970, 0.9976, 0.9982, 0.9988, 0.9991, 0.9994, 0.9997,
      1.0003, 1.0006, 1.0009, 1.0012, 1.0018, 1.0024, 1.0030};
  /// Whether a line read at each trial clock arrived with every byte verified.
  using Scores = std::array<bool, kTrialClocks.size()>;

  /// Packets scored before the clock is first judged, and the most it is
  /// scored on: the judgement is taken again after twice and four times
  /// kPackets while it is still open (see the class comment).
  static constexpr uint32_t kPackets = 400;
  static constexpr uint32_t kMaxPackets = 4 * kPackets;

  /// Standard deviations by which a trial clock must win the packets it and
  /// the nominal clock disagree on.
  static constexpr double kMinSigmas = 3.0;

  /// Whether |line| read at each trial clock verifies every byte the test
  /// judges (see the class comment), with slicers configured as |options| and
  /// pinned to where |read| locked.
  static Scores score(const int16_t* line, size_t sample_count,
                      int16_t black_level, int16_t white_level,
                      double sample_rate, TeletextSystem system,
                      const TeletextSlicerOptions& options,
                      const TeletextLineResult& read);

  /// Take in one packet read at the nominal clock, and its trial scores.
  void observe(const TeletextLineResult& read, const Scores& scores);

  /// Whether lines are still being scored.
  bool calibrating() const { return !decided_; }

  /// The clock to read at: nominal until one beats it.
  double bit_clock() const { return bit_clock_; }

  /// Packets scored so far.
  uint32_t packets() const { return packets_; }

  /// The clock read at and why, for a stage report.
  std::string summary() const;

 private:
  // Per trial clock, the packets it read whole where the nominal clock did
  // not, and the reverse.
  std::array<uint32_t, kTrialClocks.size()> won_{};
  std::array<uint32_t, kTrialClocks.size()> lost_{};
  uint32_t packets_ = 0;
  bool decided_ = false;
  double bit_clock_ = 1.0;
};

}  // namespace orc

#endif  // ORC_TELETEXT_BIT_CLOCK_H
