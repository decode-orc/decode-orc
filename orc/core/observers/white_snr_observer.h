/*
 * File:        white_snr_observer.h
 * Module:      orc-core
 * Purpose:     White SNR (Signal-to-Noise Ratio) observer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <observer.h>

#include <vector>

namespace orc {

/**
 * @brief Observer for white flag SNR analysis
 *
 * Extracts SNR (Signal-to-Noise Ratio) from VITS white flag test signals.
 * The white flag is a reference signal used to measure noise in the white
 * level region of the video signal.
 *
 * Stores observations in the "white_snr" namespace:
 * - "snr_db" (double): Signal-to-noise ratio in decibels
 */
class WhiteSNRObserver : public Observer {
 public:
  WhiteSNRObserver() = default;
  ~WhiteSNRObserver() override = default;

  std::string observer_name() const override { return "WhiteSNRObserver"; }

  // 1.1.0: windows moved clear of the white bar's rising edge, so stored
  // 1.0.0 values are measured again.
  std::string observer_version() const override { return "1.1.0"; }

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override;

  std::vector<ObservationKey> get_provided_observations() const override {
    return {
        {"white_snr", "snr_db", ObservationType::DOUBLE,
         "White flag SNR in dB"},
    };
  }

 private:
  // Extract samples from a specific region of a line and convert to IRE
  std::vector<double> get_line_slice_ire(
      const VideoFrameRepresentation& representation, FrameID frame_id,
      size_t line_offset, size_t field_line, double start_us, double length_us,
      size_t field_height, const SourceParameters& vp) const;

  // Calculate SNR from IRE samples (uses mean of data as signal)
  double calculate_psnr(const std::vector<double>& data) const;

  // Helper math functions
  double calc_mean(const std::vector<double>& data) const;
  double calc_std(const std::vector<double>& data) const;
};

}  // namespace orc
