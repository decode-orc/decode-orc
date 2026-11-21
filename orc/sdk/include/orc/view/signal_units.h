/*
 * File:        signal_units.h
 * Module:      decode-orc Plugin SDK (view tier)
 * Purpose:     The signal level units and video systems a view shows levels
 *              in, without reaching into the stage tier
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * STABILITY: PUBLIC — This header is part of the stable plugin SDK.
 */

#pragma once

// SDK TIER: view — a layout change here bumps kViewPluginAbiVersion.

#include <cstdint>

namespace orc::view {

/// The unit a project shows signal amplitudes in (Project Properties).
enum class AmplitudeUnit : uint32_t {
  IRE = 0,          ///< 0 at blanking, 100 at white
  Millivolts = 1,   ///< mV from blanking: 700 mV PAL, 714.3 mV NTSC/PAL-M white
  Samples10Bit = 2  ///< Raw 10-bit CVBS_U10_4FSC sample value (0-1023)
};

/// The video system whose levels a value is measured against.
enum class VideoSystem : uint32_t { PAL = 0, NTSC = 1, PAL_M = 2, Unknown = 3 };

}  // namespace orc::view
