/*
 * File:        vbi_teletext_service.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Timing constants of the VBI data services the stage places
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_teletext_service.h"

namespace orc {

namespace {

// ETSI EN 300 706 Section 6: Teletext System B on 625 lines.  The 0H anchor
// is libzvbi's, which measures to the leading edge of the first clock run-in
// one bit; EN 300 706 states the same timing from a different reference and
// derives 10.198 us, a difference of well under one bit period.  The nominal
// is configured at libzvbi's value for compatibility with the rest of the
// ecosystem and settled from the data by calibration.
constexpr double kWSTOffsetNs = 10300.0;

// 444 x fH, the line frequency being 15 625 Hz.
constexpr double kWSTBitRateHz = 6937500.0;

constexpr uint32_t kWSTCRIBits = 16;
constexpr uint32_t kWSTFRCBits = 8;

// libzvbi cri_frc for System B: 0xAAAAE4 over 24 bits.
constexpr uint32_t kWSTCRIFRCPattern = 0xAAAAE4u;

constexpr uint32_t kWSTPayloadBytes = 42;

// ITU-R BT.653 Table 3: Teletext System B on 525 lines — the service the US
// WST broadcasts (Electra and its contemporaries) carried.  The framing code
// and the clock run-in are the 625-line ones; the bit rate and the packet
// length are not, so a 625-line slicer pointed at a 525-line line reads noise.

// The 0H anchor is measured rather than tabulated.  libzvbi's service table
// gives 10 500 ns, and the circulating 525-line WST captures do not agree with
// it: the run-in sits at 9,29 to 9,43 us on two different Electra tapes and at
// two points within one of them, with an interquartile range of a single
// sample over eight hundred lines.  That is a 1,2 us disagreement — seven bit
// periods — and the tabulated figure would clip the head of every run-in.
//
// The 625-line path settles the same figure from the data by calibrating the
// capture offset (design §5.2).  A TBC-derived capture is never calibrated,
// because its records already start at 0H, so configuring the anchor from the
// data is where that same settling has to happen instead.
constexpr double kWST525OffsetNs = 9300.0;

// 364 x fH, the 525-line frequency being 15 734.264 Hz.  At 4 x fsc NTSC that
// is exactly 2,5 samples per bit.
constexpr double kWST525BitRateHz = 5727272.0;

constexpr uint32_t kWST525PayloadBytes = 34;

// EIA-516 / ITU-R BT.653 System C: NABTS, the service the US networks carried
// (ExtraVision, NBC Teletext).  It shares the 525-line teletext lines and the
// 364 x fH bit rate with System B and differs in the framing code and the
// packet length, so the framing code is the only thing that tells the two
// apart on a capture.
//
// The anchor is measured, as System B's is.  Here the tabulated figure very
// nearly holds: libzvbi gives 10 480 ns and the ExtraVision captures put the
// run-in at 10 27O to 10 34O ns over some three hundred lines, an
// interquartile range of three samples.  The measured figure is configured
// anyway, because the tabulated one leaves the head of the run-in sitting on
// the very edge of the written window.
constexpr double kNABTSOffsetNs = 10300.0;

// Framing code 0xE7, transmitted 11100111.
constexpr uint32_t kNABTSCRIFRCPattern = 0xAAAAE7u;

constexpr uint32_t kNABTSPayloadBytes = 33;

// Where a capture offset fitted on the captions puts the anchor below, their
// run-in's first rising midpoint: 10.5 us after 0H.  CTA-608-E §4 starts the
// run-in itself there, a quarter of a bit period earlier, so the placed run-in
// begins at 10.0 us — inside the 9,6 to 11,1 us the decode-orc corpus's own
// captions begin at (closed_caption_observer.cpp), and where the caption fit
// has always put it.  A capture offset fitted on the run-in absorbs whatever
// the recording's own timing was.
constexpr double kCaptionOffsetNs = 10500.0;

// CTA-608-E §4: the data rate is 32 x fH, fH being 4,5 MHz / 286 on a 525-line
// system.  The symbols counted here are half bits, at twice that rate.
constexpr double kNTSCLineFrequencyHz = 4500000.0 / 286.0;
constexpr double kCaptionSymbolRateHz = 2.0 * 32.0 * kNTSCLineFrequencyHz;

// CTA-608-E §4, Table 2: the run-in's seven cycles start at blanking, its
// negative-going midpoints fall on the data's bit boundaries, the last of them
// 6.75 bit periods in, and the two 0 start bits follow it with the 1 start bit
// 8.75 periods in.  Counted in half-bit symbols from the run-in's first rising
// midpoint — the anchor, a quarter period in — that is thirteen alternating
// symbols (the seven peaks and the six troughs between them), then the two 0
// start bits and the 1 start bit as 0000 11.  Measured on the cx23885 and
// bt8x8 caption dumps of the corpus, the 1 start bit rises 8,47 to 8,69 periods
// after the first rising midpoint, against the 8,5 of the standard.  A
// fourteenth alternating symbol, as the pattern once had, puts the start bit a
// half period late: real lines then correlate at 0,5 to 0,7 rather than 0,8 to
// 0,97, the fit lands two or three samples early at 27 MHz, and a search that
// lets the run-in begin before the record takes the run-in a cycle early
// instead on some card dumps.
constexpr uint32_t kCaptionRunInSymbols = 13;
constexpr uint32_t kCaptionStartSymbols = 6;
constexpr uint32_t kCaptionPattern = 0x55543u;  // 1010101010101 000011

// Sixteen data bits, two bytes of seven data bits and odd parity: 32 symbols.
constexpr uint32_t kCaptionPayloadBytes = 4;

}  // namespace

bool vbi_teletext_service(VBITVSystem tv_system, VBITeletextSystem tt_system,
                          VBITeletextService& out_service,
                          std::string& error_message) {
  out_service = VBITeletextService{};

  if (tv_system == VBITVSystem::kPAL && tt_system == VBITeletextSystem::kWST) {
    out_service.t_offset_ns = kWSTOffsetNs;
    out_service.bit_rate_hz = kWSTBitRateHz;
    out_service.cri_bits = kWSTCRIBits;
    out_service.frc_bits = kWSTFRCBits;
    out_service.cri_frc_pattern = kWSTCRIFRCPattern;
    out_service.payload_bytes = kWSTPayloadBytes;
    return true;
  }

  if ((tv_system == VBITVSystem::kNTSC || tv_system == VBITVSystem::kPALM) &&
      tt_system == VBITeletextSystem::kWST) {
    out_service.t_offset_ns = kWST525OffsetNs;
    out_service.bit_rate_hz = kWST525BitRateHz;
    out_service.cri_bits = kWSTCRIBits;
    out_service.frc_bits = kWSTFRCBits;
    out_service.cri_frc_pattern = kWSTCRIFRCPattern;
    out_service.payload_bytes = kWST525PayloadBytes;
    return true;
  }

  if ((tv_system == VBITVSystem::kNTSC || tv_system == VBITVSystem::kPALM) &&
      tt_system == VBITeletextSystem::kNABTS) {
    out_service.t_offset_ns = kNABTSOffsetNs;
    out_service.bit_rate_hz = kWST525BitRateHz;
    out_service.cri_bits = kWSTCRIBits;
    out_service.frc_bits = kWSTFRCBits;
    out_service.cri_frc_pattern = kNABTSCRIFRCPattern;
    out_service.payload_bytes = kNABTSPayloadBytes;
    return true;
  }

  error_message =
      "The configured teletext system is not defined on the configured "
      "television system; WST is defined on PAL and on 525-line systems, and "
      "NABTS on NTSC/PAL_M.";
  return false;
}

bool vbi_caption_service(VBITVSystem tv_system, VBITeletextService& out_service,
                         std::string& error_message) {
  out_service = VBITeletextService{};

  if (tv_system != VBITVSystem::kNTSC && tv_system != VBITVSystem::kPALM) {
    error_message =
        "Line-21 captions are a 525-line service; a 625-line capture carries "
        "none.";
    return false;
  }

  out_service.t_offset_ns = kCaptionOffsetNs;
  out_service.bit_rate_hz = kCaptionSymbolRateHz;
  out_service.cri_bits = kCaptionRunInSymbols;
  out_service.frc_bits = kCaptionStartSymbols;
  out_service.cri_frc_pattern = kCaptionPattern;
  out_service.payload_bytes = kCaptionPayloadBytes;
  return true;
}

}  // namespace orc
