/*
 * File:        teletext_recovery_stats.cpp
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     Accumulates WST teletext recovery outcomes into a
 *              diagnostic profile of a decoding run
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_recovery_stats.h"

#include <fmt/format.h>

#include <algorithm>
#include <string>
#include <vector>

#include "teletext_page_decoder.h"

namespace orc {

namespace {

// Rows above this carry Hamming 24/18 triplets (ETSI EN 300 706 §9.6, packets
// X/26 to X/29) or independent data services (§9.8, packet X/31), neither of
// which is byte-wise odd parity — the same boundary the MLSE parity gate uses.
constexpr int kLastParityCodedRow = 25;

// ETSI EN 300 706 §7.1.2 / ITU-R BT.653 Table 1b §3.3: the packet opens with
// two addressing bytes, whatever the system, so the data bytes the parity
// profile covers start after them.
constexpr size_t kMragBytes = 2;

// Data bytes printed per line of the parity profile.
constexpr size_t kParityProfileColumns = 10;

// Packet bytes printed per line of the reconstruction-error profile. The
// profile is accumulated per bit and printed per byte: 336 numbers say nothing
// a reader can take in, and the byte grouping lines the drift profile up with
// the parity profile above it.
constexpr size_t kBitErrorProfileColumns = 10;
constexpr size_t kBitsPerByte = 8;

size_t detector_index(TeletextDetector detector) {
  switch (detector) {
    case TeletextDetector::kThreshold:
      return 0;
    case TeletextDetector::kMlse:
      return 1;
    case TeletextDetector::kAuto:
      return 2;
  }
  return 0;
}

// Bin a residual expressed as a fraction of the fitted channel gain; anything
// from the top bin's lower edge up lands in the top bin.
size_t residual_bin(double residual) {
  if (residual <= 0.0) {
    return 0;
  }
  const auto bin =
      static_cast<size_t>(residual / TeletextRecoveryStats::kResidualBinWidth);
  return std::min(bin, TeletextRecoveryStats::kResidualBins - 1);
}

// Render the non-zero bins of a residual histogram as one comma-separated
// line; empty when nothing was recorded.
std::string format_residual_histogram(
    const std::array<uint64_t, TeletextRecoveryStats::kResidualBins>& bins) {
  constexpr double kWidth = TeletextRecoveryStats::kResidualBinWidth;
  constexpr size_t kTop = TeletextRecoveryStats::kResidualBins - 1;
  std::string text;
  for (size_t bin = 0; bin < bins.size(); ++bin) {
    if (bins[bin] == 0) {
      continue;
    }
    if (!text.empty()) {
      text += ", ";
    }
    if (bin == kTop) {
      text += fmt::format(">={:.2f}: {}", static_cast<double>(bin) * kWidth,
                          bins[bin]);
    } else {
      text +=
          fmt::format("{:.2f}-{:.2f}: {}", static_cast<double>(bin) * kWidth,
                      static_cast<double>(bin + 1) * kWidth, bins[bin]);
    }
  }
  return text;
}

// Render the non-zero rejection counters as one comma-separated line of named
// reasons; empty when nothing was rejected.
std::string format_rejections(
    const std::array<uint64_t, kTeletextRejectReasonCount>& counts,
    TeletextSystem system) {
  std::string text;
  for (size_t index = 0; index < counts.size(); ++index) {
    if (counts[index] == 0) {
      continue;
    }
    if (!text.empty()) {
      text += ", ";
    }
    text += fmt::format("{} {}",
                        teletext_reject_reason_name(
                            static_cast<TeletextRejectReason>(index), system),
                        counts[index]);
  }
  return text;
}

}  // namespace

void TeletextRecoveryStats::add_line(int vbi_line,
                                     const TeletextLineResult& result) {
  if (!system_known_) {
    system_ = result.system;
    system_known_ = true;
  }
  ++lines_seen_;
  LineStats& line = per_line_[vbi_line];
  ++line.lines;

  // A line that got past the amplitude gate held something worth recovering,
  // so from there on an empty result is a recovery failure rather than an
  // empty VBI line. Most candidate lines of a real recording are the latter.
  const bool burst =
      result.valid ||
      (result.reject_reason != TeletextRejectReason::kInsufficientSamples &&
       result.reject_reason != TeletextRejectReason::kAmplitudeGate);
  if (burst) {
    ++lines_with_burst_;
    ++line.bursts;
  }

  // Residuals are binned whether or not the line survived its gates: the
  // rejected tail is what says whether a gate sits in the right place.
  if (result.detector == TeletextDetector::kMlse) {
    if (result.preamble_residual > 0.0) {
      ++preamble_residuals_[residual_bin(result.preamble_residual)];
    }
    if (result.payload_residual > 0.0) {
      ++payload_residuals_[residual_bin(result.payload_residual)];
    }
  }

  if (!result.valid) {
    const auto reason = static_cast<size_t>(result.reject_reason);
    if (reason < rejections_.size()) {
      ++rejections_[reason];
    }
    if (burst) {
      ++line.failures;
    }
    return;
  }

  ++packets_;
  ++line.packets;
  ++packets_by_detector_[detector_index(result.detector)];

  // How sure the detector was of what it emitted, and what it repaired on the
  // strength of that (ETSI EN 300 706 §8.1 parity plus the decision margins).
  if (result.has_byte_confidence) {
    double sum = 0.0;
    for (size_t i = 0; i < result.packet_bytes; ++i) {
      sum += static_cast<double>(result.byte_confidence[i]);
    }
    confidence_sum_ += sum / static_cast<double>(result.packet_bytes);
    ++confidence_packets_;
  }
  if (result.repaired_bytes > 0) {
    bytes_repaired_ += static_cast<uint64_t>(result.repaired_bytes);
    ++packets_repaired_;
  }

  // Per-bit reconstruction error, which only the MLSE detector reconstructs
  // anything to measure.
  if (result.detector == TeletextDetector::kMlse) {
    ++bit_error_packets_;
    // Only the bits the system transmits: a 525-line packet leaves the tail of
    // the buffer at zero, and averaging that in would read as a channel that
    // reconstructs its last bytes perfectly.
    for (size_t bit = 0; bit < result.packet_bytes * 8; ++bit) {
      bit_error_sums_[bit] +=
          static_cast<double>(result.payload_bit_errors[bit]);
    }
  }

  // Per-position parity profile. Only rows 0-25 carry byte-wise odd parity
  // (ETSI EN 300 706 §9.3.1), and the row is only known when the addressing
  // decodes (§7.1.2, §8.2), so packets failing either test contribute no
  // parity evidence and are left out of the denominator.
  //
  // A service whose data bytes are not row-parity coded at all contributes
  // none either: CEA-516 §3.3 gives a NABTS data block odd parity only when
  // its data group is type 0, which the packet does not say, and the first two
  // bytes of one are a packet address rather than a row number — so decoding a
  // row from them and profiling on it would be inventing a diagnostic.
  if (!teletext_has_parity_coded_rows(result.system)) {
    return;
  }
  const int mrag_low = teletext_hamming84_decode(result.bytes[0]);
  const int mrag_high = teletext_hamming84_decode(result.bytes[1]);
  if (mrag_low < 0 || mrag_high < 0) {
    return;
  }
  const int row = ((mrag_low >> 3) & 0x01) | ((mrag_high << 1) & 0x1E);
  if (row > kLastParityCodedRow) {
    return;
  }
  ++parity_checked_packets_;
  for (size_t i = 0; i + kMragBytes < result.packet_bytes; ++i) {
    if (!teletext_odd_parity_valid(result.bytes[i + kMragBytes])) {
      ++parity_failures_[i];
    }
  }
}

void TeletextRecoveryStats::add_observed_line(
    int vbi_line, const std::array<uint8_t, kTeletextPacketBytes>* packet,
    const TeletextPacketConfidence* confidence, size_t packet_bytes,
    TeletextSystem system) {
  if (!system_known_) {
    system_ = system;
    system_known_ = true;
  }
  const size_t byte_count = std::min(packet_bytes, kTeletextPacketBytes);
  ++lines_seen_;
  ++observed_lines_;
  LineStats& line = per_line_[vbi_line];
  ++line.lines;

  // A stored observation records only what was recovered, so a line with no
  // packet is indistinguishable from a blank VBI line: neither a burst nor a
  // rejection can be claimed for it.
  if (packet == nullptr) {
    return;
  }

  ++packets_;
  ++line.packets;

  if (confidence != nullptr) {
    double sum = 0.0;
    for (size_t i = 0; i < byte_count; ++i) {
      sum += static_cast<double>((*confidence)[i]);
    }
    confidence_sum_ += sum / static_cast<double>(byte_count);
    ++confidence_packets_;
  }

  // Per-position parity profile, on the same terms as add_line() above: only
  // rows whose addressing decodes and which carry byte-wise odd parity, and
  // only for a service that codes its rows that way.
  if (!teletext_has_parity_coded_rows(system)) {
    return;
  }
  const int mrag_low = teletext_hamming84_decode((*packet)[0]);
  const int mrag_high = teletext_hamming84_decode((*packet)[1]);
  if (mrag_low < 0 || mrag_high < 0) {
    return;
  }
  const int row = ((mrag_low >> 3) & 0x01) | ((mrag_high << 1) & 0x1E);
  if (row > kLastParityCodedRow) {
    return;
  }
  ++parity_checked_packets_;
  for (size_t i = 0; i + kMragBytes < byte_count; ++i) {
    if (!teletext_odd_parity_valid((*packet)[i + kMragBytes])) {
      ++parity_failures_[i];
    }
  }
}

uint64_t TeletextRecoveryStats::packets_from(TeletextDetector detector) const {
  return packets_by_detector_[detector_index(detector)];
}

uint64_t TeletextRecoveryStats::rejections(TeletextRejectReason reason) const {
  const auto index = static_cast<size_t>(reason);
  return index < rejections_.size() ? rejections_[index] : 0;
}

uint64_t TeletextRecoveryStats::parity_failures(size_t position) const {
  return position < kDataBytes ? parity_failures_[position] : 0;
}

double TeletextRecoveryStats::parity_failure_rate(size_t position) const {
  if (position >= kDataBytes || parity_checked_packets_ == 0) {
    return 0.0;
  }
  return static_cast<double>(parity_failures_[position]) /
         static_cast<double>(parity_checked_packets_);
}

double TeletextRecoveryStats::mean_byte_confidence() const {
  if (confidence_packets_ == 0) {
    return 0.0;
  }
  return confidence_sum_ / static_cast<double>(confidence_packets_);
}

double TeletextRecoveryStats::bit_error_mean(size_t bit) const {
  if (bit >= kPayloadBits || bit_error_packets_ == 0) {
    return 0.0;
  }
  return bit_error_sums_[bit] / static_cast<double>(bit_error_packets_);
}

void TeletextRecoveryStats::reset() {
  lines_seen_ = 0;
  observed_lines_ = 0;
  lines_with_burst_ = 0;
  packets_ = 0;
  parity_checked_packets_ = 0;
  packets_by_detector_.fill(0);
  rejections_.fill(0);
  parity_failures_.fill(0);
  preamble_residuals_.fill(0);
  payload_residuals_.fill(0);
  confidence_packets_ = 0;
  confidence_sum_ = 0.0;
  bytes_repaired_ = 0;
  packets_repaired_ = 0;
  bit_error_packets_ = 0;
  bit_error_sums_.fill(0.0);
  per_line_.clear();
}

std::string TeletextRecoveryStats::brief() const {
  // Same division of labour as summary() below: figures a stored observation
  // cannot support are omitted rather than printed as zeroes.
  const uint64_t sliced_lines = lines_seen_ - observed_lines_;
  std::string text = fmt::format("{} packets", packets_);
  if (sliced_lines > 0) {
    text += fmt::format(" from {} bursts on {} candidate lines",
                        lines_with_burst_, lines_seen_);

    // Which detector is carrying the run is the reading that separates a
    // broadcast-quality signal from a tape, and it takes only a few characters
    // once the usual all-of-one case is named as such.
    const uint64_t threshold = packets_from(TeletextDetector::kThreshold);
    const uint64_t mlse = packets_from(TeletextDetector::kMlse);
    if (packets_ > 0) {
      if (mlse == packets_) {
        text += " (all MLSE)";
      } else if (threshold == packets_) {
        text += " (all threshold)";
      } else {
        text += fmt::format(" (threshold {}, MLSE {})", threshold, mlse);
      }
    }
  } else if (observed_lines_ > 0) {
    text += fmt::format(" from {} stored observations", lines_seen_);
  }

  const std::string rejections = format_rejections(rejections_, system_);
  if (!rejections.empty()) {
    text += fmt::format("; rejected: {}", rejections);
  }

  // The parity profile in one number: how many display bytes of the recovered
  // packets are damaged, which is what a reader of the pages will see. Where
  // the damage sits across the packet is summary()'s table.
  if (parity_checked_packets_ > 0) {
    uint64_t failures = 0;
    for (const uint64_t count : parity_failures_) {
      failures += count;
    }
    const double rate = static_cast<double>(failures) /
                        static_cast<double>(parity_checked_packets_ *
                                            static_cast<uint64_t>(kDataBytes));
    text += fmt::format(
        "; odd parity failed on {:.1f}% of data bytes in {} "
        "packets",
        100.0 * rate, parity_checked_packets_);
  }

  if (confidence_packets_ > 0) {
    text += fmt::format("; confidence {:.2f}", mean_byte_confidence());
    if (packets_repaired_ > 0) {
      text += fmt::format(", {} bytes repaired in {} packets", bytes_repaired_,
                          packets_repaired_);
    }
  }

  return text;
}

std::string TeletextRecoveryStats::summary() const {
  // Lines read back from stored observations carry no burst, detector or
  // rejection evidence (see add_observed_line()), so those clauses are printed
  // only for the lines that were actually sliced here. A run that mixed the
  // two — which nothing does today — would still report each figure over the
  // lines that can support it.
  const uint64_t sliced_lines = lines_seen_ - observed_lines_;
  std::string text =
      fmt::format("Teletext recovery: {} candidate lines", lines_seen_);
  if (sliced_lines > 0) {
    text += fmt::format(", {} with a data burst", lines_with_burst_);
  }
  text += fmt::format(", {} packets recovered", packets_);
  if (sliced_lines > 0) {
    text += fmt::format(" (threshold {}, MLSE {})",
                        packets_from(TeletextDetector::kThreshold),
                        packets_from(TeletextDetector::kMlse));
  } else if (observed_lines_ > 0) {
    text += " from stored observations";
  }

  const std::string rejections = format_rejections(rejections_, system_);
  if (!rejections.empty()) {
    text += fmt::format("\n  Rejected: {}", rejections);
  }

  if (parity_checked_packets_ > 0) {
    text += fmt::format(
        "\n  Odd-parity failures per data byte, % of {} parity-coded packets:",
        parity_checked_packets_);
    for (size_t first = 0; first < kDataBytes; first += kParityProfileColumns) {
      text += fmt::format("\n    byte {:2}-{:2}:", first,
                          first + kParityProfileColumns - 1);
      for (size_t i = first;
           i < std::min(first + kParityProfileColumns, kDataBytes); ++i) {
        text += fmt::format(" {:5.1f}", 100.0 * parity_failure_rate(i));
      }
    }
  }

  const std::string preamble = format_residual_histogram(preamble_residuals_);
  if (!preamble.empty()) {
    text += fmt::format("\n  MLSE preamble-fit residual (fraction of gain): {}",
                        preamble);
  }
  const std::string payload = format_residual_histogram(payload_residuals_);
  if (!payload.empty()) {
    text += fmt::format("\n  MLSE payload residual (fraction of gain): {}",
                        payload);
  }
  if (confidence_packets_ > 0) {
    text += fmt::format("\n  Decision confidence: mean {:.2f} over {} packets",
                        mean_byte_confidence(), confidence_packets_);
    if (packets_repaired_ > 0) {
      text += fmt::format("; parity repaired {} bytes in {} packets",
                          bytes_repaired_, packets_repaired_);
    }
  }

  if (bit_error_packets_ > 0) {
    // Quarters of the packet, which is the shape the drift question turns on:
    // a bit clock at the wrong rate reads each bit further from its centre than
    // the last, so the last quarter costs more than the first.
    const auto quarter_mean = [this](size_t quarter) {
      const size_t width = kPayloadBits / 4;
      double sum = 0.0;
      for (size_t bit = quarter * width; bit < (quarter + 1) * width; ++bit) {
        sum += bit_error_mean(bit);
      }
      return sum / static_cast<double>(width);
    };
    text += fmt::format(
        "\n  MLSE reconstruction error per packet byte, % of gain, over {} "
        "packets (first quarter {:.1f}, last quarter {:.1f}):",
        bit_error_packets_, 100.0 * quarter_mean(0), 100.0 * quarter_mean(3));
    for (size_t first = 0; first < kTeletextPacketBytes;
         first += kBitErrorProfileColumns) {
      const size_t end =
          std::min(first + kBitErrorProfileColumns, kTeletextPacketBytes);
      text += fmt::format("\n    byte {:2}-{:2}:", first, end - 1);
      for (size_t byte = first; byte < end; ++byte) {
        double sum = 0.0;
        for (size_t bit = 0; bit < kBitsPerByte; ++bit) {
          sum += bit_error_mean(byte * kBitsPerByte + bit);
        }
        text += fmt::format(" {:5.1f}",
                            100.0 * sum / static_cast<double>(kBitsPerByte));
      }
    }
  }

  if (!per_line_.empty()) {
    // Which lines actually carry the service: a run whose packets all come
    // from two or three of the seventeen candidates is normal, and a run
    // spread evenly across all of them is a false-lock symptom.
    if (sliced_lines > 0) {
      text += "\n  Per VBI line (lines/bursts/packets/failures):";
      for (const auto& [vbi_line, stats] : per_line_) {
        text +=
            fmt::format("\n    line {:2}: {}/{}/{}/{}", vbi_line, stats.lines,
                        stats.bursts, stats.packets, stats.failures);
      }
    } else {
      text += "\n  Per VBI line (lines/packets):";
      for (const auto& [vbi_line, stats] : per_line_) {
        text += fmt::format("\n    line {:2}: {}/{}", vbi_line, stats.lines,
                            stats.packets);
      }
    }
  }

  return text;
}

}  // namespace orc
