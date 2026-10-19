/*
 * File:        teletext_bit_clock.cpp
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     Learns the bit clock a recording's data lines were sampled at
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "teletext_bit_clock.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>

#include "teletext_page_decoder.h"

namespace orc {

namespace {

// Half-width of the window a trial reading is pinned to, in samples: the burst
// starts where the nominal reading found it, whatever the clock.
constexpr double kTrialPinRadius = 3.0;

// Whether |byte|'s bits alternate as the clock run-in's do (ETSI EN 300 706
// §6.1): a byte no parity-coded row carries as a character, and the one a
// slightly wrong clock misreads first (see TeletextClockCalibrator).
bool alternating(uint8_t byte) { return byte == 0x55 || byte == 0xAA; }

// Whether |reading| arrived with every byte the calibrator judges checking
// itself, as teletext_verified_bytes() counts them: on a parity-coded system,
// a data byte that |reading| or the nominal reading |nominal| of the same line
// holds as an alternating byte is not judged.
bool whole_where_judged(const TeletextLineResult& reading,
                        const TeletextLineResult& nominal) {
  if (!reading.valid) {
    return false;
  }
  const size_t prefix = teletext_hamming_prefix_bytes(reading.system);
  const bool parity_coded = teletext_has_parity_coded_rows(reading.system);
  for (size_t i = 0; i < reading.packet_bytes; ++i) {
    const uint8_t byte = reading.bytes[i];
    if (i < prefix) {
      if (!teletext_hamming84_clean(byte)) {
        return false;
      }
    } else if (!(parity_coded &&
                 (alternating(byte) || alternating(nominal.bytes[i]))) &&
               !teletext_odd_parity_valid(byte)) {
      return false;
    }
  }
  return true;
}

}  // namespace

uint32_t teletext_verified_bytes(const TeletextLineResult& result) {
  if (!result.valid) {
    return 0;
  }
  const size_t prefix = teletext_hamming_prefix_bytes(result.system);
  uint32_t verified = 0;
  for (size_t i = 0; i < result.packet_bytes; ++i) {
    const uint8_t byte = result.bytes[i];
    verified += (i < prefix ? teletext_hamming84_clean(byte)
                            : teletext_odd_parity_valid(byte))
                    ? 1
                    : 0;
  }
  return verified;
}

TeletextClockCalibrator::Scores TeletextClockCalibrator::score(
    const int16_t* line, size_t sample_count, int16_t black_level,
    int16_t white_level, double sample_rate, TeletextSystem system,
    const TeletextSlicerOptions& options, const TeletextLineResult& read) {
  TeletextPhaseHint pin;
  pin.valid = true;
  pin.centre = read.lock_sample;
  pin.radius = kTrialPinRadius;
  Scores scores{};
  for (size_t i = 0; i < kTrialClocks.size(); ++i) {
    TeletextSlicerOptions trial = options;
    trial.bit_clock = kTrialClocks[i];
    const TeletextSlicer slicer(sample_rate, system, trial);
    const TeletextLineResult trial_read =
        slicer.slice(line, sample_count, black_level, white_level, pin);
    scores[i] = whole_where_judged(trial_read, read);
  }
  return scores;
}

void TeletextClockCalibrator::observe(const TeletextLineResult& read,
                                      const Scores& scores) {
  if (!calibrating() || !read.valid) {
    return;
  }
  const bool whole = whole_where_judged(read, read);
  for (size_t i = 0; i < scores.size(); ++i) {
    won_[i] += scores[i] && !whole ? 1 : 0;
    lost_[i] += whole && !scores[i] ? 1 : 0;
  }
  // Judged after kPackets packets, then after twice and four times as many.
  ++packets_;
  if (packets_ != kPackets && packets_ != 2 * kPackets &&
      packets_ != kMaxPackets) {
    return;
  }
  size_t best = 0;
  for (size_t i = 1; i < won_.size(); ++i) {
    if (static_cast<int64_t>(won_[i]) - lost_[i] >
        static_cast<int64_t>(won_[best]) - lost_[best]) {
      best = i;
    }
  }
  const double margin = static_cast<double>(won_[best]) - lost_[best];
  const double sigma = std::sqrt(static_cast<double>(won_[best]) +
                                 static_cast<double>(lost_[best]));
  if (margin > kMinSigmas * sigma) {
    bit_clock_ = kTrialClocks[best];
    decided_ = true;
  } else if (packets_ >= kMaxPackets ||
             margin * std::sqrt(static_cast<double>(kMaxPackets) / packets_) <=
                 kMinSigmas * sigma) {
    // A real lead grows with the square root of the packets scored; one that
    // would not pass by kMaxPackets at that rate is not worth scoring on.
    decided_ = true;
  }
}

std::string TeletextClockCalibrator::summary() const {
  if (calibrating()) {
    return fmt::format(
        "as specified ({} of the {} packets needed to calibrate)", packets_,
        packets_ < kPackets ? kPackets : kMaxPackets);
  }
  if (bit_clock_ == 1.0) {
    return fmt::format("as specified (no trial clock read {} packets better)",
                       packets_);
  }
  return fmt::format("{:.2f}% {} than specified, calibrated on {} packets",
                     100.0 * std::abs(1.0 - bit_clock_),
                     bit_clock_ < 1.0 ? "slower" : "faster", packets_);
}

}  // namespace orc
