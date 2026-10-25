/*
 * File:        frame_quality_observer.h
 * Module:      orc-core
 * Purpose:     Frame quality observer for frame quality metrics
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <observer.h>

#include <array>
#include <cstdint>

namespace orc {

// Forward declarations
class ObservationContext;

/**
 * @brief Observer for frame quality analysis
 *
 * Calculates quality metrics for each frame based on:
 * - Dropout count/density
 * - Phase correctness
 * - Signal-to-noise estimates (if available)
 *
 * Dropout hints are frame-level, so the metrics describe the whole frame and
 * are stored once, keyed by the frame's first field (the same convention as
 * the other frame-level observers such as BlackPSNRObserver).
 */
class FrameQualityObserver : public Observer {
 public:
  FrameQualityObserver() = default;

  std::string observer_name() const override { return "FrameQualityObserver"; }

  // 2.0.0: one frame-level result keyed by the first field (1.x wrote the
  // same frame result twice, once per derived field).
  std::string observer_version() const override { return "2.0.0"; }

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override;

  std::vector<ObservationKey> get_provided_observations() const override {
    return {
        {"disc_quality", "quality_score", ObservationType::DOUBLE,
         "Frame quality score 0.0-1.0"},
        {"disc_quality", "dropout_count", ObservationType::INT32,
         "Number of dropouts detected in the frame"},
        {"disc_quality", "phase_valid", ObservationType::BOOL,
         "Phase correctness indicator"},
    };
  }

 private:
  /**
   * @brief Calculate quality score from frame data
   *
   * Combines multiple quality indicators:
   * - Dropout density
   * - Phase correctness
   * - Signal metrics
   *
   * @return Quality score 0.0-1.0
   */
  double calculate_quality_score(const VideoFrameRepresentation& representation,
                                 FrameID frame_id,
                                 const SourceParameters& vp) const;
};

}  // namespace orc
