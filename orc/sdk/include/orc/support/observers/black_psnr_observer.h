/*
 * File:        black_psnr_observer.h
 * Module:      decode-orc Plugin SDK (support)
 * Purpose:     Black PSNR (Peak Signal-to-Noise Ratio) observer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/support/observers/observer.h>

#include <vector>

namespace orc {

/**
 * @brief Observer for black level PSNR analysis
 *
 * Extracts PSNR (Peak Signal-to-Noise Ratio) from VITS black level test
 * signals. The black level reference is used to measure noise in the black
 * region of the video signal.
 *
 * Stores observations in the "black_psnr" namespace:
 * - "psnr_db" (double): Peak signal-to-noise ratio in decibels
 */
class BlackPSNRObserver : public Observer {
 public:
  BlackPSNRObserver() = default;
  ~BlackPSNRObserver() override = default;

  std::string observer_name() const override { return "BlackPSNRObserver"; }

  std::string observer_version() const override { return "1.0.0"; }

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override;

  std::vector<ObservationKey> get_provided_observations() const override {
    return {
        {"black_psnr", "psnr_db", ObservationType::DOUBLE,
         "Black level PSNR in dB"},
    };
  }

 private:
  // Extract samples from a specific region of a line and convert to IRE
  std::vector<double> get_line_slice_ire(
      const VideoFrameRepresentation& representation, FrameID frame_id,
      size_t line_offset, size_t field_line, double start_us, double length_us,
      size_t field_height, const SourceParameters& vp) const;

  // Calculate PSNR from IRE samples
  double calculate_psnr(const std::vector<double>& data) const;

  // Helper math functions
  double calc_mean(const std::vector<double>& data) const;
  double calc_std(const std::vector<double>& data) const;
};

}  // namespace orc
