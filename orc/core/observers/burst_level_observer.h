/*
 * File:        burst_level_observer.h
 * Module:      orc-core
 * Purpose:     Color burst median IRE level observer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <observer.h>

#include <vector>

namespace orc {

/**
 * @brief Observer for color burst amplitude analysis
 *
 * Analyzes the color burst signal amplitude and reports it in 10-bit sample
 * units (CVBS_U10_4FSC domain). This is an AC amplitude — the peak burst
 * excursion, not an absolute level. The burst level is useful for quality
 * assessment and can indicate signal degradation or processing artifacts.
 *
 * Stores observations in the "burst_level" namespace:
 * - "median_burst_10bit" (double): Median burst peak amplitude in 10-bit
 *   sample units
 */
class BurstLevelObserver : public Observer {
 public:
  BurstLevelObserver() = default;
  ~BurstLevelObserver() override = default;

  std::string observer_name() const override { return "BurstLevelObserver"; }

  std::string observer_version() const override { return "1.1.0"; }

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override;

  std::vector<ObservationKey> get_provided_observations() const override {
    return {
        {"burst_level", "median_burst_10bit", ObservationType::DOUBLE,
         "Median color burst peak amplitude in 10-bit sample units"},
    };
  }

 private:
  double calculate_median(const double* values, size_t count) const;
};

}  // namespace orc
