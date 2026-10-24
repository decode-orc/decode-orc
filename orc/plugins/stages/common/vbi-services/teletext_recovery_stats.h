/*
 * File:        teletext_recovery_stats.h
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     Accumulates WST teletext recovery outcomes into a
 *              diagnostic profile of a decoding run
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_RECOVERY_STATS_H
#define ORC_TELETEXT_RECOVERY_STATS_H

// Shared plugin-side library, NOT part of the SDK contract: it compiles
// against the public SDK headers only and is linked privately into the stage
// plugins that need it. Nothing here crosses the plugin boundary, so changes
// never force an ABI bump.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "teletext_slicer.h"

namespace orc {

/**
 * @brief Recovery profile of a teletext decoding run
 *
 * Fed one TeletextLineResult per sliced candidate VBI line, this accumulates
 * the figures that say *how* a recording is failing rather than merely how
 * much: which gate discarded each line, how the odd-parity failures of the
 * recovered packets are distributed across the 40 data-byte positions, which
 * VBI lines carry the service, and where the MLSE detector's fit residuals
 * sit relative to their gates.
 *
 * The per-position parity profile is the primary diagnostic. ETSI EN 300 706
 * §9.3.1 gives every display byte odd parity, so an undamaged packet fails
 * nowhere. A failure rate that grows with byte position means the bit clock is
 * drifting across the 52 µs of the packet; a flat rate means noise and
 * intersymbol interference. The two call for opposite remedies.
 *
 * Pure accumulation: no I/O, no clock, no allocation beyond the per-line map.
 *
 * Thread safety: none; confine an instance to one thread (both the observer
 * and the sink accumulate per field, on the thread that sliced the lines).
 */
class TeletextRecoveryStats {
 public:
  // ETSI EN 300 706 §7.1: the T42 packet is 2 MRAG bytes then 40 data bytes.
  // The 525-line packet is 2 then 32 (ITU-R BT.653 Table 1b), so a run of that
  // service fills only the leading 32 positions of the parity profile.
  static constexpr size_t kDataBytes = kTeletextPacketBytes - 2;

  // MLSE residual histogram. Residuals are fractions of the fitted channel
  // gain and both gates sit below 1,0 (see teletext_slicer.cpp), so 0,05-wide
  // bins resolve the neighbourhood of a gate; the last bin collects
  // everything from 0,95 up.
  static constexpr size_t kResidualBins = 20;
  static constexpr double kResidualBinWidth = 0.05;

  // Payload bit positions the per-bit reconstruction-error profile covers
  // (ETSI EN 300 706 §7.1: the 42 T42 bytes, LSB first). A 525-line run fills
  // the leading 272 of them.
  static constexpr size_t kPayloadBits = kTeletextPayloadBits;

  /// Outcomes for one VBI line position, across every field examined
  struct LineStats {
    // Candidate lines examined at this position.
    uint64_t lines = 0;
    // ... of which carried a data burst (passed the amplitude gate).
    uint64_t bursts = 0;
    // ... of which yielded a packet.
    uint64_t packets = 0;
    // Bursts that yielded no packet: lines that held data the slicer could
    // not recover, which is the loss the later phases have to reduce.
    uint64_t failures = 0;
  };

  /**
   * @brief Record the outcome of slicing one candidate line
   *
   * @param vbi_line Caller's line identity — the 0-based field line for both
   *                 the observer and the teletext sink. Lines are kept apart
   *                 so a service carried on a subset of the VBI window shows
   *                 as such.
   * @param result   What the slicer returned for that line, valid or not.
   */
  void add_line(int vbi_line, const TeletextLineResult& result);

  /**
   * @brief Record one candidate line read back from a stored observation
   *
   * The consumer of a cached observation gets the recovered packet but not the
   * slicing that produced it: whether the line held a burst, which detector
   * won, which gate discarded it and what the fit residuals were are all
   * properties of a slice that has already happened elsewhere. What survives
   * storage is the packet and, where the detector could measure it, its
   * per-byte confidence — which is enough for the parity profile that is the
   * primary diagnostic, for the per-line yield, and for the confidence mean.
   *
   * Lines recorded this way are kept apart from sliced ones so summary() can
   * omit the figures it would otherwise have to invent.
   *
   * @param vbi_line     Caller's line identity, as add_line() above
   * @param packet       The recovered packet, or nullptr when the line yielded
   *                     none
   * @param confidence   Per-byte confidence carried by the observation, or
   *                     nullptr when it carries none
   * @param packet_bytes Bytes of @p packet the service transmitted
   *                     (TeletextObservedPacket::byte_count); the rest were
   *                     never sent and are left out of every profile
   * @param system       Service the packet was recovered under, which decides
   *                     whether the parity profile applies to it at all (see
   *                     teletext_has_parity_coded_rows)
   */
  void add_observed_line(
      int vbi_line, const std::array<uint8_t, kTeletextPacketBytes>* packet,
      const TeletextPacketConfidence* confidence,
      size_t packet_bytes = kTeletextPacketBytes,
      TeletextSystem system = TeletextSystem::kWst625);

  /// Candidate lines recorded
  uint64_t lines_seen() const { return lines_seen_; }
  /// Candidate lines read back from stored observations rather than sliced
  uint64_t observed_lines() const { return observed_lines_; }
  /// Candidate lines that carried a data burst (passed the amplitude gate)
  uint64_t lines_with_burst() const { return lines_with_burst_; }
  /// Valid packets recovered
  uint64_t packets() const { return packets_; }
  /// Valid packets recovered by @p detector
  uint64_t packets_from(TeletextDetector detector) const;
  /// Lines rejected for @p reason
  uint64_t rejections(TeletextRejectReason reason) const;

  /// Packets whose data bytes were parity checked (parity-coded rows only)
  uint64_t parity_checked_packets() const { return parity_checked_packets_; }
  /// Odd-parity failures at data-byte @p position (0-39)
  uint64_t parity_failures(size_t position) const;
  /// Odd-parity failure rate at data-byte @p position (0-39); 0 when no
  /// parity-coded packet has been recorded
  double parity_failure_rate(size_t position) const;

  /// Per-VBI-line outcomes, keyed by the line identity passed to add_line()
  const std::map<int, LineStats>& per_line() const { return per_line_; }

  /// MLSE preamble-fit residuals, binned by fraction of the channel gain
  const std::array<uint64_t, kResidualBins>& preamble_residual_histogram()
      const {
    return preamble_residuals_;
  }
  /// MLSE payload reconstruction residuals, binned the same way
  const std::array<uint64_t, kResidualBins>& payload_residual_histogram()
      const {
    return payload_residuals_;
  }

  /**
   * @brief Mean per-byte decision confidence over the recovered packets
   *
   * How sure the detector was of the bytes it emitted — the eye margin of
   * the threshold detector, the path-metric margin of the MLSE detector —
   * averaged over the 42 bytes of every packet that carried a measurement
   * (see TeletextLineResult::byte_confidence). A run whose confidence is high
   * while its parity failures are not is one whose losses are in the gates
   * rather than in the detector. Returns 0 before any such packet is recorded.
   */
  double mean_byte_confidence() const;

  /// Packets contributing to the mean confidence above
  uint64_t confidence_packets() const { return confidence_packets_; }

  /// Display bytes whose parity was restored by flipping the detector's
  /// least-confident bit (TeletextSlicerOptions::parity_repair)
  uint64_t bytes_repaired() const { return bytes_repaired_; }

  /// Packets in which at least one byte was repaired
  uint64_t packets_repaired() const { return packets_repaired_; }

  /**
   * @brief Mean MLSE reconstruction error at payload bit @p bit
   *
   * The timing diagnostic. Averaged over the recovered packets — a rejected
   * line contributes nothing, because the question this answers is how the
   * error behaves along a packet that *was* recovered, and the shape of a
   * failed lock says only that it failed. A profile that grows with bit
   * position means the bit clock is running at the wrong rate across the 52 µs
   * of the packet; a flat profile means noise and intersymbol interference.
   *
   * Errors are fractions of the fitted channel gain, as
   * TeletextLineResult::payload_bit_errors reports them. Returns 0 for an
   * out-of-range position or before any MLSE packet has been recorded.
   */
  double bit_error_mean(size_t bit) const;

  /// Packets contributing to the per-bit reconstruction-error profile
  uint64_t bit_error_packets() const { return bit_error_packets_; }

  /// Drop everything recorded so far
  void reset();

  /**
   * @brief Human-readable single-line headline
   *
   * The figures a reader wants per field — what came in, what came out, why
   * the rest was discarded, and how damaged what came out is — with the
   * profiles summary() prints as tables reduced to one number each. Intended
   * for per-field logging, where summary() would bury the run in tables.
   *
   * Never contains a newline. Stable for a given set of inputs.
   */
  std::string brief() const;

  /**
   * @brief Human-readable multi-line summary
   *
   * Sections with nothing to report are omitted, so a run that recovered
   * nothing prints one line. The text is stable for a given set of inputs:
   * no timestamps, no ordering by hash.
   */
  std::string summary() const;

 private:
  static constexpr size_t kDetectorCount = 3;  // threshold, MLSE, auto

  uint64_t lines_seen_ = 0;
  uint64_t observed_lines_ = 0;
  uint64_t lines_with_burst_ = 0;
  uint64_t packets_ = 0;
  uint64_t parity_checked_packets_ = 0;
  std::array<uint64_t, kDetectorCount> packets_by_detector_{};
  std::array<uint64_t, kTeletextRejectReasonCount> rejections_{};
  std::array<uint64_t, kDataBytes> parity_failures_{};
  std::array<uint64_t, kResidualBins> preamble_residuals_{};
  std::array<uint64_t, kResidualBins> payload_residuals_{};
  uint64_t confidence_packets_ = 0;
  double confidence_sum_ = 0.0;
  uint64_t bytes_repaired_ = 0;
  uint64_t packets_repaired_ = 0;
  uint64_t bit_error_packets_ = 0;
  std::array<double, kPayloadBits> bit_error_sums_{};
  std::map<int, LineStats> per_line_;
  // Service the run recovered under, taken from the lines it was given, so the
  // summary can name each rejection in that service's own terms. A run is one
  // service by construction — the stage builds one slicer — so the first line
  // to state a system settles it.
  TeletextSystem system_ = TeletextSystem::kWst625;
  bool system_known_ = false;
};

}  // namespace orc

#endif  // ORC_TELETEXT_RECOVERY_STATS_H
