/*
 * File:        vbi_output_frame.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Sample lattice and amplitude domain of the emitted CVBS frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_output_frame.h"

#include <orc/stage/cvbs_signal_constants.h>

namespace orc {

namespace {

// ETSI EN 300 706 Section 5: WST logic 0 is at black and logic 1 at 66 % of
// the black-to-white excursion, which is 256 + 0.66 x 588 = 644 counts.
constexpr uint16_t kPALWSTLogic0 = static_cast<uint16_t>(kPalBlack);
constexpr uint16_t kPALWSTLogic1 = 644;

// The same 66 % rule on the NTSC black-to-white excursion, which starts at the
// 7,5 IRE setup: 282 + 0.66 x (800 - 282) = 624 counts.
constexpr uint16_t kNTSCWSTLogic0 = static_cast<uint16_t>(kNtscBlack);
constexpr uint16_t kNTSCWSTLogic1 = 624;

// EIA-516: NABTS puts logic 0 at blanking and logic 1 at 70 IRE, which on the
// NTSC scale of 5,6 counts per IRE is 240 + 70 x 5.6 = 632 counts.
constexpr uint16_t kNTSCNABTSLogic0 = static_cast<uint16_t>(kNtscBlanking);
constexpr uint16_t kNTSCNABTSLogic1 = 632;

}  // namespace

bool make_vbi_output_frame(VBITVSystem tv_system, VBITeletextSystem tt_system,
                           VBIOutputFrame& out_frame,
                           std::string& error_message) {
  out_frame = VBIOutputFrame{};

  switch (tv_system) {
    case VBITVSystem::kPAL:
    case VBITVSystem::kSECAM:
      // SECAM's levels and raster are 625-line PAL's (ITU-R BT.1700-1
      // Annex 1 Part C), so only the frame's system differs.
      out_frame.system = (tv_system == VBITVSystem::kSECAM) ? VideoSystem::SECAM
                                                            : VideoSystem::PAL;
      out_frame.sample_rate_hz = kPalSampleRate;
      out_frame.lines_per_frame = static_cast<uint32_t>(kPalFrameLines);
      out_frame.samples_per_frame = static_cast<uint32_t>(kPalFrameSamples);
      out_frame.samples_per_line_nominal =
          static_cast<uint32_t>(kPalSamplesPerLineNominal);
      out_frame.levels.blanking = static_cast<uint16_t>(kPalBlanking);
      // PAL has no setup, so black is blanking and the two services' logic 0
      // references coincide.
      out_frame.levels.logic0 = kPALWSTLogic0;
      out_frame.levels.logic1 = kPALWSTLogic1;
      return true;

    case VBITVSystem::kNTSC:
      out_frame.system = VideoSystem::NTSC;
      out_frame.sample_rate_hz = kNtscSampleRate;
      out_frame.lines_per_frame = static_cast<uint32_t>(kNtscFrameLines);
      out_frame.samples_per_frame = static_cast<uint32_t>(kNtscFrameSamples);
      out_frame.samples_per_line_nominal =
          static_cast<uint32_t>(kNtscSamplesPerLine);
      out_frame.levels.blanking = static_cast<uint16_t>(kNtscBlanking);
      if (tt_system == VBITeletextSystem::kNABTS) {
        out_frame.levels.logic0 = kNTSCNABTSLogic0;
        out_frame.levels.logic1 = kNTSCNABTSLogic1;
      } else {
        out_frame.levels.logic0 = kNTSCWSTLogic0;
        out_frame.levels.logic1 = kNTSCWSTLogic1;
      }
      return true;

    case VBITVSystem::kPALM:
      // PAL-M shares the 525-line structure but not the lattice: 909 samples
      // per line against NTSC's 910, so its frames are a different size and
      // every placement figure derived from the sampling rate differs.  There
      // is no capture in circulation to check that against, so it is refused
      // rather than assumed to be NTSC.
      error_message =
          "Output frames for PAL-M are not implemented; PAL, SECAM and NTSC "
          "frames can be emitted.";
      return false;
  }

  error_message = "Unrecognised television system.";
  return false;
}

}  // namespace orc
