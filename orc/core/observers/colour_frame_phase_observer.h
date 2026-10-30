/*
 * File:        colour_frame_phase_observer.h
 * Module:      orc-core
 * Purpose:     Colour-frame sequence index observer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <observer.h>

#include <vector>

namespace orc {

// Measures the per-field colour-sequence phase ID and colour-frame index for
// each frame and records them in the observation context.
//
// PAL/PAL_M: 8-field sequence — field_phase_id is 1–8 per field.
// NTSC:      4-field sequence — field_phase_id is 1–4 per field.
//
// Results are stored under namespace "colour_frame_phase" for BOTH fields of
// each frame (FieldID(frame_id*2) and FieldID(frame_id*2+1)):
//   "field_phase_id"     (int32_t) — per-field phase within the colour sequence
//   "colour_frame_index" (int32_t) — frame-level index derived from phase pair
//
// A value of -1 is stored when the burst is absent or too weak to classify
// (e.g. blank tape or pre-programme leader).
//
// EBU Tech. 3280-E §1.1.1 (PAL); SMPTE 244M-2003 §3.2 (NTSC);
// ITU-R BT.1700-1 Annex 1 Part B (PAL_M).
class ColourFramePhaseObserver : public Observer {
 public:
  ColourFramePhaseObserver() = default;
  ~ColourFramePhaseObserver() override = default;

  std::string observer_name() const override {
    return "ColourFramePhaseObserver";
  }

  std::string observer_version() const override { return "1.1.0"; }

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override;

  std::vector<ObservationKey> get_provided_observations() const override {
    return {
        {"colour_frame_phase", "field_phase_id", ObservationType::INT32,
         "Per-field colour-sequence phase ID (PAL/PAL_M: 1-8, NTSC: 1-4; "
         "-1 when absent)",
         /*optional=*/true},
        {"colour_frame_phase", "colour_frame_index", ObservationType::INT32,
         "Colour-frame sequence index derived from field_phase_id pair "
         "(-1 when absent)",
         /*optional=*/true},
    };
  }
};

}  // namespace orc
