/*
 * File:        colour_frame_phase_observer.h
 * Module:      decode-orc Plugin SDK (support)
 * Purpose:     Colour-frame sequence index observer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/support/observers/observer.h>

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
// (e.g. blank tape or pre-programme leader), or when a PAL field's burst sits
// too near the midway point between two sequence positions to call.  Both ids
// are -1 on every frame of a source that is not subcarrier phase locked
// (SourceParameters::is_subcarrier_locked false): CVBS file format spec v1.8.1
// lets phase be computed from sample position only on a locked source.
//
// Ids are numbered as ld-decode numbers fieldPhaseID.  For PAL and PAL_M at
// the standard 4FSC sampling phase that is the CVBS File Format
// Specification's sequence: its frame 1 is fields 1/2.  NTSC frames are on the
// specification's line origin, where ids 1/2 are colour frame A (SMPTE 170M
// §13.2) and 3/4 colour frame B; ld-decode NTSC TBC frames, moved onto that
// origin by tbc_source, read ld-decode's own fieldPhaseID (see
// colour_frame_phase_observer.cpp).
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

  std::string observer_version() const override { return "1.4.0"; }

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
