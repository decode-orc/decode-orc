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
 */
class TeletextClockCalibrator {
 public:
  /// Trial bit clocks, as fractions of the nominal rate.
  static constexpr std::array<double, 14> kTrialClocks = {
      0.9970, 0.9976, 0.9982, 0.9988, 0.9991, 0.9994, 0.9997,
      1.0003, 1.0006, 1.0009, 1.0012, 1.0018, 1.0024, 1.0030};
  /// Whether a line read at each trial clock arrived with every byte verified.
  using Scores = std::array<bool, kTrialClocks.size()>;

  /// Packets scored before the clock is decided.
  static constexpr uint32_t kPackets = 400;

  /// Standard deviations by which a trial clock must win the packets it and
  /// the nominal clock disagree on.
  static constexpr double kMinSigmas = 3.0;

  /// Whether |line| read at each trial clock verifies every byte, with slicers
  /// configured as |options| and pinned to where |read| locked.
  static Scores score(const int16_t* line, size_t sample_count,
                      int16_t black_level, int16_t white_level,
                      double sample_rate, TeletextSystem system,
                      const TeletextSlicerOptions& options,
                      const TeletextLineResult& read);

  /// Take in one packet read at the nominal clock, and its trial scores.
  void observe(const TeletextLineResult& read, const Scores& scores);

  /// Whether lines are still being scored.
  bool calibrating() const { return packets_ < kPackets; }

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
  double bit_clock_ = 1.0;
};

}  // namespace orc

#endif  // ORC_TELETEXT_BIT_CLOCK_H
