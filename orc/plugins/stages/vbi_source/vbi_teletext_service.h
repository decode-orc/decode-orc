/*
 * File:        vbi_teletext_service.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Timing constants of the VBI data services the stage places
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_TELETEXT_SERVICE_H
#define ORC_VBI_TELETEXT_SERVICE_H

#include <cstdint>
#include <string>

#include "vbi_source_format.h"

namespace orc {

// Timing of one VBI data service, as the stage needs it to place and measure
// a data line (design §5.2).
//
// The figures follow libzvbi's service table, which is the most complete
// single reference for the 0H-to-clock-run-in anchor.
struct VBITeletextService {
  // Time from 0H to the leading edge of the first clock run-in one bit, at
  // half amplitude, in nanoseconds.
  double t_offset_ns = 0.0;

  double bit_rate_hz = 0.0;

  // Clock run-in length, in bits.  Sixteen alternating bits whose energy sits
  // at half the bit rate.
  uint32_t cri_bits = 0;

  // Framing code length, in bits.
  uint32_t frc_bits = 0;

  // Combined clock run-in and framing code, in transmission order, most
  // significant bit first over cri_bits + frc_bits bits.
  uint32_t cri_frc_pattern = 0;

  // Payload length in bytes, framing code excluded.
  uint32_t payload_bytes = 0;

  // Samples occupied by one transmitted bit at a source's sampling rate.
  double samples_per_bit(double sample_rate_hz) const {
    return (bit_rate_hz > 0.0) ? (sample_rate_hz / bit_rate_hz) : 0.0;
  }

  // Sample offset, within a stored line record, of the leading edge of the
  // first clock run-in one bit.
  //
  // The record's sample 0 sits capture_offset samples after 0H, so the
  // service's 0H-referred anchor moves back by exactly that much.
  double cri_start_samples(double sample_rate_hz,
                           double capture_offset_samples) const {
    return (t_offset_ns * 1e-9 * sample_rate_hz) - capture_offset_samples;
  }
};

// Timing of the data service a source carries.  Returns false with an error
// message for services the stage does not yet place.
bool vbi_teletext_service(VBITVSystem tv_system, VBITeletextSystem tt_system,
                          VBITeletextService& out_service,
                          std::string& error_message);

// The line-21 caption waveform of a 525-line source (CTA-608-E §4), in the
// same terms, for a capture that carries no teletext to fit its capture offset
// on.  Its clock run-in is seven cycles of a sine at the 32 x fH bit rate,
// then two 0 start bits and a 1 start bit, then sixteen data bits.  Counted in
// half-bit symbols from the run-in's first rising midpoint, its half cycles are
// an alternating NRZ pattern, so the whole is described exactly as a teletext
// run-in and framing code are: thirteen alternating symbols, then 000011, then
// a 32-symbol payload — which lets the same correlator and the same payload
// level reading serve it.
// Returns false with an error message on a 625-line system, which carries no
// line-21 captions.
bool vbi_caption_service(VBITVSystem tv_system, VBITeletextService& out_service,
                         std::string& error_message);

// The level a caption line's data 1 bits sit at, in IRE units above blanking;
// its 0 bits sit at blanking (CTA-608-E §4).
constexpr double kCaptionDataOneIre = 50.0;

}  // namespace orc

#endif  // ORC_VBI_TELETEXT_SERVICE_H
