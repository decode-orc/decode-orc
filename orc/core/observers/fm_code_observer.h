/*
 * File:        fm_code_observer.h
 * Module:      orc-core
 * Purpose:     FM code observer (NTSC line 10)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/support/observers/observer.h>

namespace orc {

/**
 * @brief Observer for LD FM code (NTSC line 10).
 *
 * Observations (namespace "fm_code"):
 * - present (bool, optional): true when valid FM code decoded
 * - data_value (int32, optional): 20-bit FM code payload
 * - field_flag (bool, optional): field indicator bit
 */
class FmCodeObserver : public Observer {
 public:
  FmCodeObserver() = default;
  ~FmCodeObserver() override = default;

  std::string observer_name() const override { return "FmCodeObserver"; }
  std::string observer_version() const override { return "1.0.0"; }

  // LaserDisc FM code lives on NTSC line 10 only.
  bool applies_to(const SourceParameters& params) const override {
    return params.system == VideoSystem::NTSC;
  }

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override;

  std::vector<ObservationKey> get_provided_observations() const override {
    return {
        {"fm_code", "present", ObservationType::BOOL,
         "True when FM code decoded", true},
        {"fm_code", "data_value", ObservationType::INT32,
         "20-bit FM code payload", true},
        {"fm_code", "field_flag", ObservationType::BOOL, "Field indicator bit",
         true},
    };
  }

 private:
  struct DecodedFmCode {
    uint32_t data_value = 0;
    bool field_flag = false;
  };

  bool decode_line(const int16_t* line_data, size_t sample_count,
                   int16_t zero_crossing, size_t active_start,
                   double jump_samples, DecodedFmCode& out) const;
};

}  // namespace orc
