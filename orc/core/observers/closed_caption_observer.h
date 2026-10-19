/*
 * File:        closed_caption_observer.h
 * Module:      orc-core
 * Purpose:     Closed caption observer (EIA-608 line 21 of both fields)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <observer.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace orc {

/**
 * @brief Observer for EIA-608 line 21 data (525-line systems)
 *
 * Reads line 21 of both fields. The first field's carries CC1, CC2, TEXT1 and
 * TEXT2; the second field's CC3, CC4, TEXT3, TEXT4 and the Extended Data
 * Service [CTA-608-E §8].
 *
 * Observations (namespace "closed_caption"), per field:
 * - present (bool): true when the line carries a line 21 waveform — the clock
 *   run-in and the start bits — and its byte pair was read
 * - data0 / data1 (int32, with present): the two bytes' seven data bits
 * - parity0_valid / parity1_valid (bool, with present): whether each byte's
 *   odd parity over all eight bits held
 * - other_line (int32): where line 21 carried no pair, the line next to it —
 *   22, then 20 — that did, with other_data0 / other_data1 and
 *   other_parity0_valid / other_parity1_valid read as above. A recording whose
 *   time base slipped a line carries its captions there; whether a line does
 *   is a question of the run, not of one field, and is left to the reader of
 *   the run (EIA608Line21Run).
 *
 * Thread-compatible: no state is kept between frames.
 */
class ClosedCaptionObserver : public Observer {
 public:
  ClosedCaptionObserver() = default;
  ~ClosedCaptionObserver() override = default;

  std::string observer_name() const override { return "ClosedCaptionObserver"; }
  // 2.0.0: reads both fields, finds the clock run-in before reading a line,
  // and checks parity over all eight bits; observations stored by 1.x would
  // carry the noise 1.x read on lines without a caption waveform.
  // 2.1.0: looks for the run-in from 7 us after 0H rather than 8.5, so 2.0
  // stored nothing for lines whose run-in starts between the two.
  // 2.2.0: reads lines 22 and 20 where line 21 carried no pair (other_*).
  std::string observer_version() const override { return "2.2.0"; }

  // EIA-608 captions are carried on line 21 of 525-line systems only
  // [CTA-608-E §4.1]. 625-line PAL has no equivalent line-21 service, so the
  // decode can only ever waste work and record present=false there.
  bool applies_to(const SourceParameters& params) const override {
    return params.system == VideoSystem::NTSC ||
           params.system == VideoSystem::PAL_M;
  }

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override;

  std::vector<ObservationKey> get_provided_observations() const override;

  /// The byte pair of one line 21: each byte's seven data bits and whether
  /// its odd parity held.
  struct Line21Pair {
    uint8_t data0 = 0;
    uint8_t data1 = 0;
    bool parity0_valid = false;
    bool parity1_valid = false;
  };

  /**
   * @brief Read the byte pair one line 21 carries
   *
   * @p count samples of one line, sample 0 at 0H, a line period long (the bit
   * clock is taken as count / 32 samples [CTA-608-E §4, Table 2]).
   *
   * @return The pair, or nothing where the line holds no line 21 waveform: no
   *         clock run-in, or no start bits where the run-in puts them.
   *
   * O(count); allocates nothing.
   */
  static std::optional<Line21Pair> read_line21(const int16_t* samples,
                                               size_t count);
};

}  // namespace orc
