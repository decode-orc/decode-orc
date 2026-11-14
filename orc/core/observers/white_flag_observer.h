/*
 * File:        white_flag_observer.h
 * Module:      orc-core
 * Purpose:     White flag observer (NTSC line 11)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/support/observers/observer.h>

namespace orc {

/**
 * @brief Observer for LaserDisc white flag (NTSC line 11).
 *
 * Observations (namespace "white_flag"):
 * - present (bool, optional): true when white flag detected on the field
 */
class WhiteFlagObserver : public Observer {
 public:
  WhiteFlagObserver() = default;
  ~WhiteFlagObserver() override = default;

  std::string observer_name() const override { return "WhiteFlagObserver"; }
  std::string observer_version() const override { return "1.0.0"; }

  // LaserDisc white flag lives on NTSC line 11 only.
  bool applies_to(const SourceParameters& params) const override {
    return params.system == VideoSystem::NTSC;
  }

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override;

  std::vector<ObservationKey> get_provided_observations() const override {
    return {
        {"white_flag", "present", ObservationType::BOOL, "White flag detected",
         true},
    };
  }
};

}  // namespace orc
