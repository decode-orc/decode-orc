/*
 * File:        amplitude_units.h
 * Module:      decode-orc Plugin SDK (view-support tier)
 * Purpose:     10-bit sample levels in the project's amplitude unit, for
 *              views that show signal levels
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * STABILITY: PUBLIC — This header is part of the stable plugin SDK.
 */

#pragma once

// SDK TIER: view-support — header only. orc-gui's own amplitude conversions
// forward to these, so a view and orc-gui show the same figures.

#include <orc/view/signal_units.h>

#include <cstdint>
#include <string>
#include <utility>

namespace orc::view {

/// Active video amplitude in mV for the given video system.
/// EBU Tech. 3280-E §1.1: PAL = 700 mV.
/// SMPTE 170M-2004 §11.4: NTSC/PAL_M = 7.143 mV/IRE × 100 = 714.3 mV.
inline double active_video_mv(VideoSystem system) {
  if (system == VideoSystem::NTSC || system == VideoSystem::PAL_M) {
    return 714.3;
  }
  return 700.0;
}

/// A 10-bit sample in millivolts: blanking is 0 mV, white is
/// active_video_mv(system).
inline double samples10_to_mv(int32_t sample, int32_t blanking, int32_t white,
                              VideoSystem system) {
  const double range = static_cast<double>(white - blanking);
  if (range <= 0.0) return 0.0;
  return (static_cast<double>(sample - blanking) / range) *
         active_video_mv(system);
}

/// A 10-bit sample in IRE: blanking is 0 IRE, white is 100 IRE.
inline double samples10_to_ire(int32_t sample, int32_t blanking,
                               int32_t white) {
  const double range = static_cast<double>(white - blanking);
  if (range <= 0.0) return 0.0;
  return (static_cast<double>(sample - blanking) / range) * 100.0;
}

/// A 10-bit sample in @p unit.
inline double samples10_to_display(int32_t sample, int32_t blanking,
                                   int32_t white, VideoSystem system,
                                   AmplitudeUnit unit) {
  switch (unit) {
    case AmplitudeUnit::Millivolts:
      return samples10_to_mv(sample, blanking, white, system);
    case AmplitudeUnit::Samples10Bit:
      return static_cast<double>(sample);
    default:
      return samples10_to_ire(sample, blanking, white);
  }
}

/// Default major-tick interval in display units (labelled grid lines).
/// IRE: 20 IRE; mV: 100 mV; 10-bit: 128 samples.
inline double amplitude_major_tick(AmplitudeUnit unit) {
  switch (unit) {
    case AmplitudeUnit::Millivolts:
      return 100.0;
    case AmplitudeUnit::Samples10Bit:
      return 128.0;
    default:
      return 20.0;
  }
}

/// Short suffix for a unit: "IRE", "mV", or "" (10-bit has none).
inline std::string amplitude_unit_suffix(AmplitudeUnit unit) {
  switch (unit) {
    case AmplitudeUnit::Millivolts:
      return "mV";
    case AmplitudeUnit::Samples10Bit:
      return "";
    default:
      return "IRE";
  }
}

/// Display range [min, max] in @p unit, from @p sync_tip to @p peak.
inline std::pair<double, double> amplitude_display_range(
    int32_t sync_tip, int32_t blanking, int32_t white, int32_t peak,
    VideoSystem system, AmplitudeUnit unit) {
  return {samples10_to_display(sync_tip, blanking, white, system, unit),
          samples10_to_display(peak, blanking, white, system, unit)};
}

}  // namespace orc::view
