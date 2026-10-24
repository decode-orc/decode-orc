/*
 * File:        vbi_source_format.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Generic raw VBI container descriptor and its named presets
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_SOURCE_FORMAT_H
#define ORC_VBI_SOURCE_FORMAT_H

#include <cstdint>
#include <string>
#include <vector>

namespace orc {

// Sample word of a stored line record.  Every raw VBI capture in scope is a
// flat sequence of these; nothing in the container declares which one is in
// use, so it is always configuration.
enum class VBISampleFormat {
  kU8,     // unsigned 8-bit (bt8x8 / cx88 / saa7131 card captures)
  kU16LE,  // unsigned 16-bit little-endian (ld-decode / vhs-decode .tbc)
  kS16LE,  // signed 16-bit little-endian
};

// Television system the capture was made from.  Fixes the frame geometry and
// the amplitude domain of the synthesised output.
enum class VBITVSystem {
  kPAL,
  kNTSC,
  kPALM,
};

// VBI data service carried on the captured lines.
enum class VBITeletextSystem {
  kWST,    // Teletext System B, 625 lines (ETSI EN 300 706)
  kNABTS,  // Teletext System C, 525 lines (EIA-516 / ITU-R BT.653)
};

// Which of the two source families a format belongs to.  The distinction is
// not cosmetic: a TBC-derived capture already has sample 0 of every record at
// 0H by construction, so its capture offset is known to be exactly zero and
// must never be "calibrated" (design §5.3.3).
enum class VBISourceFamily {
  kCardCapture,  // family A: bt8x8, cx88, saa7131; offset is hardware folklore
  kTBCDerived,   // family B: .tbc and cropped VBI-only .tbc; offset is 0
};

// Inclusive, 0-based range of stored records within a field that carry the
// data service.  Sources store their useful records contiguously.
struct VBIFieldRange {
  uint32_t start = 0;
  uint32_t end = 0;

  uint32_t count() const { return (end >= start) ? (end - start + 1) : 0; }
};

// Health thresholds of the clock run-in calibration (design §5.3.4).
//
// These belong to the source format rather than being global constants: a
// clean broadcast or LaserDisc capture locks to a fraction of a sample, while
// a VHS-sourced one legitimately scatters by several samples because head
// switching lands inside the teletext lines and the run-in has been very
// nearly filtered away (design §5.3.6).  A single set of numbers would either
// accept a broken clean capture or cry wolf on every tape.
struct VBICalibrationThresholds {
  // Half-width of the search for the run-in, in source samples, centred on the
  // position the descriptor's capture offset predicts.  Wide enough to cover a
  // wrong hardware folklore figure, narrow enough that the search cannot lock
  // onto data a bit period away.
  double search_tolerance_samples = 48.0;

  // Normalised correlation a record must reach for its position to be counted.
  // Records below it carry no data service, and there are many of them.
  double acceptance_correlation = 0.5;

  // Fraction of the examined records that must lock before a fit is trusted.
  double minimum_acceptance_fraction = 0.25;

  // Spread below which the source is behaving as a time-base corrected capture
  // should.
  double tight_spread_samples = 0.5;

  // Spread above which the fit is rejected outright: either the sampling rate
  // is wrong or the source was never time-base corrected.
  double maximum_spread_samples = 3.0;

  // Fitted drift across the sampled span, in samples, above which the run
  // stops.  A monotonic drift is diagnostic of a sampling-rate error and the
  // slope gives the correction directly, so it is reported rather than
  // silently absorbed into the global offset.
  double maximum_drift_samples = 3.0;
};

// The generic container descriptor (design §3.1).  Every named preset expands
// to exactly this structure, so presets are pure data and there is a single
// code path through the reader.
struct VBISourceFormat {
  // Exact sampling rate of the capture in Hz.  Never taken from a FLAC
  // wrapper's header — that value is a placeholder (design §3.3).
  double sample_rate_hz = 0.0;

  // Stored samples per line record: the record stride, including any
  // hardware padding.
  uint32_t line_length = 0;

  // Real samples at the start of each record.  Samples beyond this are
  // hardware padding and must not be resampled or contribute to statistics.
  uint32_t valid_samples = 0;

  VBISampleFormat sample_format = VBISampleFormat::kU8;

  // Stored line records per field (the field stride).
  uint32_t field_lines = 0;

  // Which of those records carry the data service.
  VBIFieldRange field_range{};

  // Records stored after the field range that carry the line next to the
  // caption line, read and placed on the line list's next entries for the
  // closed caption observer and for nothing else: the calibration survey and
  // the service anchor never look at them.  A recording whose time base slipped
  // a line carries its captions on line 22 (see ClosedCaptionObserver).  Only
  // a time-base corrected source has them, its levels being absolute, so a
  // record of picture beside the data lines cannot move theirs.
  uint32_t caption_records = 0;

  // Trailing bytes of each stored frame that are not sample data.  These
  // overlap the padding region of the final record rather than extending the
  // frame: a bt8x8 PAL frame is 65 536 bytes in total, the last four of which
  // are the driver's frame counter (design §3.3).
  uint32_t frame_trailer_bytes = 0;

  // True when those trailer bytes are the bt8x8 u32 frame sequence number,
  // which gives free frame-drop detection (design §6.3).
  bool frame_trailer_is_counter = false;

  // Time of sample 0 of each record relative to 0H, expressed in source
  // samples.  The single most important configuration value in the stage.
  double capture_offset_samples = 0.0;

  // True when the offset above is a starting hint to be replaced by CRI/FRC
  // calibration rather than a configured value.
  bool capture_offset_is_auto = false;

  // Which TV field the first stored field of each frame is (1 or 2).
  uint32_t first_field = 1;

  VBITVSystem tv_system = VBITVSystem::kPAL;
  VBITeletextSystem tt_system = VBITeletextSystem::kWST;
  VBISourceFamily family = VBISourceFamily::kCardCapture;

  // What the calibrator should expect of this format's timing.
  VBICalibrationThresholds calibration{};

  // Bytes occupied by one sample word.
  uint32_t bytes_per_sample() const {
    return (sample_format == VBISampleFormat::kU8) ? 1u : 2u;
  }

  // Bytes occupied by one stored line record, padding included.
  uint64_t bytes_per_record() const {
    return static_cast<uint64_t>(line_length) * bytes_per_sample();
  }

  // Bytes occupied by one stored field.
  uint64_t bytes_per_field() const { return bytes_per_record() * field_lines; }

  // Bytes occupied by one stored frame (two sequential fields).  The frame
  // trailer is inside this figure, not additional to it.
  uint64_t bytes_per_frame() const { return bytes_per_field() * 2u; }

  // Padding bytes at the tail of every stored record.
  uint64_t record_padding_bytes() const {
    return (line_length >= valid_samples)
               ? static_cast<uint64_t>(line_length - valid_samples) *
                     bytes_per_sample()
               : 0u;
  }
};

// Number of teletext line records the standard defines per field for a
// system pairing (design §5.1).  A source may carry fewer; carrying more
// means the configuration is wrong.
uint32_t standard_teletext_lines_per_field(VBITVSystem tv_system,
                                           VBITeletextSystem tt_system);

// Names of every container preset the stage understands, in presentation
// order.  A preset is a complete configuration, so the name is the whole of
// the user's choice; it is also what the project file stores.
std::vector<std::string> vbi_source_preset_names();

// The same list, restricted to the presets whose captures were made from one
// television system.  A capture's system fixes the output frame geometry, so a
// preset for the wrong system could never produce frames a project of that
// system can hold; offering it and then refusing it would be a worse parameter
// surface than not offering it at all.
std::vector<std::string> vbi_source_preset_names(VBITVSystem tv_system);

// Expand a named preset (design §3.2) into the descriptor above.  Returns
// false with an error message when the name is not a known preset.
bool expand_vbi_source_preset(const std::string& preset_name,
                              VBISourceFormat& out_format,
                              std::string& error_message);

// Human-readable spelling of a sample format, as used in project files and
// error messages ("u8", "u16le", "s16le").
std::string to_string(VBISampleFormat sample_format);

}  // namespace orc

#endif  // ORC_VBI_SOURCE_FORMAT_H
