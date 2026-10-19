/*
 * File:        vbi_offset_calibration.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Fits the capture offset of a card capture from its clock run-in
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_OFFSET_CALIBRATION_H
#define ORC_VBI_OFFSET_CALIBRATION_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "vbi_cri_correlator.h"
#include "vbi_cri_template.h"
#include "vbi_level_mapper.h"
#include "vbi_line_reader.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"

namespace orc {

// How tightly the fitted run-in positions agree, judged against the source
// format's own thresholds (design §5.3.4).  This is the single best health
// check the stage has.
enum class VBIOffsetSpreadClass {
  // The source is genuinely time-base corrected and the whole model holds.
  kTight,

  // Mild residual jitter or a slight sampling-rate error.  Usable, but the
  // user is told.
  kMild,

  // Either the sampling rate is wrong or the source was never time-base
  // corrected.  Not usable.
  kUnusable,
};

// One accepted run-in position.
struct VBICRIObservation {
  // Position of the record in the capture, counting every stored record of
  // every frame.  It is the abscissa a drift is fitted against, so it has to
  // be the true line ordinal rather than an index into the sampled subset.
  uint64_t line_sequence = 0;

  // Which stored field of its frame the record is, 0 or 1.  The caption fit
  // fits each field on its own (fit_vbi_capture_offset_by_field()).
  uint32_t field_index = 0;

  double anchor_position_samples = 0.0;
  double peak_correlation = 0.0;

  // The record's logic levels, read from its payload where the run-in was
  // found.
  VBILineLevels data_levels;

  // The two marks of a teletext line, read where the run-in was found: the
  // share of the run-in window's variance its alternation at half the bit rate
  // carries (one for a clean run-in, a few hundredths for noise), and how many
  // bits of the framing code read wrong, sliced at the midpoint of
  // data_levels.  See vbi_shows_teletext_marks().
  double run_in_purity = 0.0;
  uint32_t framing_errors = 0;
};

// Whether an accepted record shows either mark a teletext line carries: its
// framing code, read with at most one bit wrong, or its clock run-in's
// alternation.  A tape can take either away — the run-in collapses towards the
// data midpoint long before the data does (design §5.3.6), and a service
// framed otherwise than the configured one reads its framing code wrong (a
// datacast framed otherwise can read NABTS's with two or three bits wrong on
// most lines) — but a line that carries the service keeps one of them.  A line
// the correlation merely fitted its shape to keeps neither: on one cx23885
// dump the NABTS fit locked on a 0,9 Mbit/s code on line 11, whose run-in
// window is blanking and whose framing code reads two or more bits wrong.
bool vbi_shows_teletext_marks(const VBICRIObservation& observation);

// One stored field's part in a fit made field by field.
struct VBIFieldFit {
  uint64_t records_examined = 0;
  uint64_t records_accepted = 0;

  // True when the field's own run-ins passed every check a whole fit is held
  // to — enough of them, at one place, without drift — so the field carries
  // the service and its anchor is part of the fitted offset.
  bool carries_service = false;

  // The field's own median run-in position and robust spread, in samples.
  double anchor_position_samples = 0.0;
  double spread_samples = 0.0;
};

// The fitted offset and everything the user needs to decide whether to trust
// it.
struct VBIOffsetCalibration {
  // False when the run must stop.  The stage does not proceed with a bad fit:
  // a wrong global offset silently mis-places every line in a multi-hour
  // decode (design §5.3.4).
  bool converged = false;

  // The fitted result: time from 0H to sample 0 of every record, in source
  // samples.  Applied globally and never per line — a per-line correction
  // would erase real timing information and shift lines that were already
  // right.
  double capture_offset_samples = 0.0;

  // Median position of the run-in within a record, from which the offset above
  // is derived.  A median rather than a mean, because a partial match on a
  // damaged line is an outlier a mean would follow.
  double anchor_position_samples = 0.0;

  // Robust spread of the accepted positions, in samples: the median absolute
  // deviation scaled to the standard deviation it would imply for a normal
  // distribution.
  double spread_samples = 0.0;

  // The logic levels the accepted records carry their data at: the median of
  // their payloads' levels, unusable when none was measured.  A card capture's
  // frames are mapped at these when none of their own lines carries the
  // service (VBILevelMapperConfig::capture_levels).
  VBILineLevels data_levels;

  VBIOffsetSpreadClass spread_class = VBIOffsetSpreadClass::kUnusable;

  uint64_t records_examined = 0;
  uint64_t records_accepted = 0;

  // Fraction of examined records that locked.  A diagnostic in its own right:
  // a capture with sparse teletext locks on few lines, and a configuration
  // that is wrong locks on almost none.
  double acceptance_fraction = 0.0;

  // Slope of position against line ordinal, in samples per line, and the total
  // it implies over the span examined.  A monotonic drift is diagnostic of a
  // sampling-rate error specifically.
  double drift_samples_per_line = 0.0;
  double drift_total_samples = 0.0;
  bool drift_detected = false;

  // Sampling rate the fitted drift implies, in Hz: the configured rate scaled
  // by the drift over the samples it accumulated across.  Equal to the
  // configured rate when no drift was fitted.
  double suggested_sample_rate_hz = 0.0;

  // Why the fit was rejected.  Empty when it converged.
  std::vector<std::string> diagnostics;

  // Concerns that do not stop the run.
  std::vector<std::string> warnings;

  // One-line human-readable summary of the fit.
  std::string summary;

  // Each stored field's own fit, when the offset was fitted field by field
  // (fit_vbi_capture_offset_by_field()); empty otherwise.
  std::vector<VBIFieldFit> fields;
};

// Ordinal of a record within the whole capture.
uint64_t vbi_line_sequence(const VBISourceFormat& format,
                           const VBILineRecord& record);

// Stored frames to sample for calibration, spread across the capture.
//
// Spread rather than taken from the head, so that a bad opening segment — a
// tape settling, a disc's lead-in, a mistracked first minute — cannot dominate
// a figure that is then applied to the whole file (design §5.3.4).
std::vector<uint64_t> vbi_calibration_frame_indices(uint64_t frame_count,
                                                    uint32_t sample_frames);

// Fit a global capture offset from accepted run-in positions.
//
// records_examined counts every record the search was run over, accepted or
// not, so that the acceptance fraction reflects the capture rather than the
// observations that survived.
VBIOffsetCalibration fit_vbi_capture_offset(
    const VBISourceFormat& format, const VBITeletextService& service,
    const std::vector<VBICRIObservation>& observations,
    uint64_t records_examined);

// Fit a global capture offset from run-in positions that two independently
// timed streams carried, one in each stored field.
//
// Line 21's two fields are two caption streams — field 1 carries CC1, CC2,
// Text 1 and Text 2, field 2 CC3, CC4, Text 3, Text 4 and XDS (CTA-608-E) — and
// the encoders that insert them need not time their run-ins alike.  On seven
// of eighteen card dumps of captions measured the two fields' run-ins sit
// 8 to 17 samples apart at 27 MHz, each field tight to within a sample, while
// the NABTS lines of the same captures show no such split — on one, the NABTS
// fields at 33,4 and 33,9 samples and the caption fields at 4,0 and 19,6.
// Pooled, six of the seven scatter by 6 to 12 samples, past the cx23885
// preset's limit, and no limit can let that through without letting a wrong
// sampling rate through too.
//
// So each field is fitted on its own observations, by fit_vbi_capture_offset()
// and its every check, against its own records_examined.  A field whose fit
// fails carries no captions, or none this fit can use — a single-field caption
// service, or a handful of noise lines that correlated — and is reported but
// left out.  The fit converges when at least one field carries the service,
// and when two do, their anchors sit within the format's search tolerance of
// each other: one global offset then places each field's run-in within half
// of that of the service's anchor.  The anchor reported is the midpoint of the
// fields' anchors, the one global offset that serves both best, and the spread
// the larger of their own spreads: the fields' disagreement is two encoders,
// not scatter.
//
// observations carry their field_index; records_examined counts each field's
// examined records.
VBIOffsetCalibration fit_vbi_capture_offset_by_field(
    const VBISourceFormat& format, const VBITeletextService& service,
    const std::vector<VBICRIObservation>& observations,
    const std::array<uint64_t, 2>& records_examined);

// What a calibration run should read.
struct VBICalibrationConfig {
  // Stored frames sampled across the capture.  Sixteen bt8x8 PAL frames is
  // 512 records, comfortably the few hundred lines design §5.3.4 asks for,
  // and a fraction of a second of decoding.
  uint32_t sample_frames = 16;

  VBICRITemplateConfig template_config{};

  // How the accepted records' data levels are read: the same policy the frames
  // are then mapped with.
  VBILevelMapperConfig levels{};
};

// Run the whole procedure against a capture: sample frames across the file,
// correlate every record of each against the run-in template, and fit the
// global offset.
//
// A fit whose accepted records mostly show neither mark of a teletext line
// (vbi_shows_teletext_marks()) is refused whatever its spread: it locked on
// something shaped enough like the run-in and framing code to correlate, but
// is not them.  On every card dump measured that carries teletext 78 % or
// more of the accepted records show one; on a line-11 code it fitted, 30 %.
//
// Returns false with an error message only when the capture could not be read
// or the configuration cannot produce a template at all.  A fit that fails its
// health checks is a converged == false result with diagnostics, not an error
// here: the difference matters because one is a broken input and the other is
// a configuration the user can correct.
bool calibrate_vbi_capture_offset(const VBILineReader& reader,
                                  const VBITeletextService& service,
                                  const VBICalibrationConfig& config,
                                  VBIOffsetCalibration& out_calibration,
                                  std::string& error_message);

// The same procedure fitted on the line-21 caption run-in (CTA-608-E §4)
// instead of the teletext one, for a 525-line card capture that carries no
// teletext.
//
// Such a capture is not a wrong configuration, but the teletext fit cannot tell
// it from one: it finds no run-in either way.  A caption run-in found at one
// place across the capture, within the format's own spread and drift limits,
// is the evidence the container, the sampling rate and the timing are right
// that the teletext one would have been — and nearly every US recording
// carries one, so a captions-only dump opens rather than being refused.
//
// Only the records holding line 21 of either field are correlated, and each
// field is fitted on its own (fit_vbi_capture_offset_by_field()), its
// acceptance fraction of its own line-21 records.  The run-in is looked for
// from where the format's offset predicts it, widened by its search tolerance
// on the late side and on the early side out to a run-in that began up to one
// and a half of its cycles before the card's window opened: a cx23885 dump
// can start 44 samples, eight-tenths of a cycle, into every run-in, 84 samples
// ahead of where the preset predicts it.  A record counts
// when its run-in correlates at 0,7 or better, rather than at the format's
// acceptance correlation: the template is the waveform CTA-608-E draws, which
// a caption line matches closely and a line without captions does not.
// Sample a capture whose captions come and go — a long dump can carry them in
// its last quarter only, on one field — across enough frames for both fields'
// fits; the stage samples 128.
//
// The capture offset is fitted to the caption service's own 10,5 us anchor;
// data_levels are the caption payload's two levels, which are blanking and 50
// IRE rather than a teletext service's logic levels (see
// vbi_levels_from_caption_levels()).
//
// Returns false with an error message only when the capture could not be read
// or holds no line-21 records; a fit that fails its health checks is a
// converged == false result, as for the teletext fit.
bool calibrate_vbi_capture_offset_on_captions(
    const VBILineReader& reader, const VBICalibrationConfig& config,
    VBIOffsetCalibration& out_calibration, std::string& error_message);

// The same survey, read the other way round, for time-base corrected sources.
//
// Such a capture's offset is exactly zero and is never fitted — sample 0 of
// every record is 0H by construction (design §5.3.3) — but that leaves the
// other half of the timing unmeasured. The service's 0H-to-run-in anchor is a
// property of the transmission, not of the file, and transmissions disagree
// about it: the tabulated 525-line figures were measured on particular
// captures, and a broadcaster that transmitted its run-in a microsecond
// earlier puts it outside the window derived from them, clipping the head of
// every data line the stage places.
//
// On a card capture there is nothing to separate here, because the fitted
// capture offset already absorbs the disagreement and lands the run-in on the
// nominal anchor. On a TBC-derived one it is the anchor that has to move.
//
// The returned calibration is the survey's: capture_offset_samples is not
// meaningful for such a source and must not be applied. Read
// anchor_position_samples, or vbi_measured_anchor_ns() below.
bool measure_vbi_service_anchor(const VBILineReader& reader,
                                const VBITeletextService& service,
                                const VBICalibrationConfig& config,
                                VBIOffsetCalibration& out_calibration,
                                std::string& error_message);

// The measured anchor as a service 0H offset in nanoseconds, ready to replace
// VBITeletextService::t_offset_ns. Only meaningful for a TBC-derived source,
// whose record sample 0 is 0H.
double vbi_measured_anchor_ns(const VBIOffsetCalibration& calibration,
                              double sample_rate_hz);

}  // namespace orc

#endif  // ORC_VBI_OFFSET_CALIBRATION_H
