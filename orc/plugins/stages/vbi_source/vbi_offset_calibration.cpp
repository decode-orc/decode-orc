/*
 * File:        vbi_offset_calibration.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Fits the capture offset of a card capture from its clock run-in
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_offset_calibration.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>

#include "vbi_line_mapping.h"

namespace orc {

namespace {

// Scaling that turns a median absolute deviation into the standard deviation
// it implies for a normal distribution, so that the spread can be read against
// design §5.3.4's table of standard deviations.
constexpr double kMADToStandardDeviation = 1.4826;

// Accepted positions below which a median is not a fit but an anecdote.
constexpr uint64_t kMinimumAcceptedRecords = 8;

// A framing code read with at most this many bits wrong is the service's; a
// code of eight random bits reads so one time in 28.
constexpr uint32_t kMaximumFramingErrors = 1;

// The share of a run-in window's variance its alternation must carry to count
// as a run-in.  Noise puts about two over the window's length at any one
// frequency — 0,03 over a 525-line run-in at 27 MHz — and a clean run-in
// carries most of it; a tape's collapsed run-in keeps a tenth or more on most
// of its lines.
constexpr double kMinimumRunInPurity = 0.1;

// The share of a fit's accepted records that must show a mark of the service.
// On thirteen card dumps that carry teletext, 78 to 100 % of the records
// their fits accept show one; of the 44 the NABTS fit accepted on a line-11
// code that is no teletext, 13.
constexpr double kMinimumMarkedFraction = 0.5;

constexpr double kPi = 3.14159265358979323846;

// How many standard errors a fitted slope must exceed before it is called a
// drift rather than the estimator's own noise.  Without this, a long capture's
// enormous line span turns any residual scatter into a confident-looking
// slope.
constexpr double kDriftSignificance = 3.0;

std::string format_number(double value, int precision) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision) << value;
  return stream.str();
}

double median_of(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2u;
  if ((values.size() % 2u) == 1u) {
    return values[middle];
  }
  return 0.5 * (values[middle - 1u] + values[middle]);
}

std::string join_diagnostics(const std::vector<std::string>& diagnostics) {
  std::string joined;
  for (const std::string& diagnostic : diagnostics) {
    if (!joined.empty()) {
      joined += " ";
    }
    joined += diagnostic;
  }
  return joined;
}

std::string spread_class_name(VBIOffsetSpreadClass spread_class) {
  switch (spread_class) {
    case VBIOffsetSpreadClass::kTight:
      return "time-base corrected";
    case VBIOffsetSpreadClass::kMild:
      return "mild jitter";
    case VBIOffsetSpreadClass::kUnusable:
      return "not usable";
  }
  return "unknown";
}

// The one-line summary of a fit; |fields| names how each field was fitted when
// the fit was made field by field, and is empty otherwise.
std::string fit_summary(const VBIOffsetCalibration& calibration,
                        const std::string& fields) {
  return std::string(calibration.converged ? "Capture offset "
                                           : "Capture offset (rejected) ") +
         format_number(calibration.capture_offset_samples, 2) +
         " samples, run-in at " +
         format_number(calibration.anchor_position_samples, 2) + " samples" +
         (fields.empty() ? std::string()
                         : " fitted field by field (" + fields + ")") +
         ", spread " + format_number(calibration.spread_samples, 2) +
         " samples (" + spread_class_name(calibration.spread_class) +
         "), locked on " + std::to_string(calibration.records_accepted) +
         " of " + std::to_string(calibration.records_examined) + " records (" +
         format_number(calibration.acceptance_fraction * 100.0, 1) + "%).";
}

// A record's sample at a fractional position, read between its neighbours.
double sample_at(const std::vector<double>& samples, double position) {
  const double floor_position = std::floor(position);
  const size_t index = static_cast<size_t>(floor_position);
  const double fraction = position - floor_position;
  const double next = samples[std::min(index + 1u, samples.size() - 1u)];
  return samples[index] + fraction * (next - samples[index]);
}

// The share of the variance of the run-in window — whole samples inside its
// alternating bits — the tone at half the bit rate carries, by the same
// quadrature measure the caption reader uses.
double run_in_purity(const std::vector<double>& samples,
                     const VBITeletextService& service, double sample_rate_hz,
                     double anchor) {
  const double samples_per_bit = service.samples_per_bit(sample_rate_hz);
  const double first = std::max(0.0, std::ceil(anchor));
  const double last =
      std::min(static_cast<double>(samples.size()),
               std::floor(anchor + static_cast<double>(service.cri_bits) *
                                       samples_per_bit));
  if (!(samples_per_bit > 0.0) || last - first < 2.0) {
    return 0.0;
  }
  const size_t begin = static_cast<size_t>(first);
  const size_t end = static_cast<size_t>(last);

  double total = 0.0;
  for (size_t index = begin; index < end; ++index) {
    total += samples[index];
  }
  const double count = static_cast<double>(end - begin);
  const double mean = total / count;

  // Two bits to a cycle of the alternation.
  const double step = kPi / samples_per_bit;
  double in_phase = 0.0;
  double quadrature = 0.0;
  double energy = 0.0;
  for (size_t index = begin; index < end; ++index) {
    const double value = samples[index] - mean;
    const double angle = step * static_cast<double>(index);
    in_phase += value * std::cos(angle);
    quadrature += value * std::sin(angle);
    energy += value * value;
  }
  if (!(energy > 0.0)) {
    return 0.0;
  }
  // The tone's power over the window's: (A^2 / 2) / (energy / n), A being
  // 2 |in_phase + i quadrature| / n.
  return 2.0 * (in_phase * in_phase + quadrature * quadrature) /
         (count * energy);
}

// How many bits of the service's framing code read wrong, sliced at bit centres
// at the midpoint of the record's data levels.  A bit centre outside the record
// counts as wrong.
uint32_t framing_errors(const std::vector<double>& samples,
                        const VBITeletextService& service,
                        double sample_rate_hz, double anchor,
                        const VBILineLevels& levels) {
  const double samples_per_bit = service.samples_per_bit(sample_rate_hz);
  if (!(samples_per_bit > 0.0) || samples.empty() || !levels.usable) {
    return service.frc_bits;
  }
  const double midpoint = levels.logic0 + 0.5 * levels.amplitude();
  uint32_t errors = 0;
  for (uint32_t bit = 0; bit < service.frc_bits; ++bit) {
    const bool expected =
        ((service.cri_frc_pattern >> (service.frc_bits - 1u - bit)) & 1u) != 0u;
    const double centre =
        anchor +
        (static_cast<double>(service.cri_bits + bit) + 0.5) * samples_per_bit;
    if (centre < 0.0 || centre > static_cast<double>(samples.size() - 1u)) {
      ++errors;
      continue;
    }
    if ((sample_at(samples, centre) > midpoint) != expected) {
      ++errors;
    }
  }
  return errors;
}

}  // namespace

bool vbi_shows_teletext_marks(const VBICRIObservation& observation) {
  return observation.framing_errors <= kMaximumFramingErrors ||
         observation.run_in_purity >= kMinimumRunInPurity;
}

uint64_t vbi_line_sequence(const VBISourceFormat& format,
                           const VBILineRecord& record) {
  const uint64_t records_per_frame =
      static_cast<uint64_t>(format.field_lines) * 2u;
  return record.frame_index * records_per_frame +
         static_cast<uint64_t>(record.field_index) * format.field_lines +
         record.record_index;
}

std::vector<uint64_t> vbi_calibration_frame_indices(uint64_t frame_count,
                                                    uint32_t sample_frames) {
  std::vector<uint64_t> indices;
  if (frame_count == 0 || sample_frames == 0) {
    return indices;
  }

  const uint64_t wanted = std::min<uint64_t>(sample_frames, frame_count);
  indices.reserve(static_cast<size_t>(wanted));

  // Frames at the centres of equal divisions of the capture: the sample covers
  // the whole file, and no division's frame is the file's first or last, which
  // are the two most likely to be atypical.
  for (uint64_t index = 0; index < wanted; ++index) {
    const uint64_t frame = ((2u * index + 1u) * frame_count) / (2u * wanted);
    indices.push_back(std::min(frame, frame_count - 1u));
  }

  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  return indices;
}

VBIOffsetCalibration fit_vbi_capture_offset(
    const VBISourceFormat& format, const VBITeletextService& service,
    const std::vector<VBICRIObservation>& observations,
    uint64_t records_examined) {
  VBIOffsetCalibration calibration;
  calibration.records_examined = records_examined;
  calibration.records_accepted = observations.size();
  calibration.acceptance_fraction =
      (records_examined > 0) ? static_cast<double>(observations.size()) /
                                   static_cast<double>(records_examined)
                             : 0.0;
  calibration.suggested_sample_rate_hz = format.sample_rate_hz;

  const VBICalibrationThresholds& thresholds = format.calibration;

  if (observations.size() < kMinimumAcceptedRecords) {
    calibration.diagnostics.push_back(
        "The clock run-in was found on only " +
        std::to_string(observations.size()) + " of " +
        std::to_string(records_examined) +
        " sampled records, which is too few to fit a capture offset from. "
        "Either the capture carries no teletext on the configured lines, or "
        "the container format, sampling rate or teletext system is wrong.");
    calibration.summary =
        "Capture offset calibration failed: no usable clock run-in.";
    return calibration;
  }

  std::vector<double> positions;
  positions.reserve(observations.size());
  for (const VBICRIObservation& observation : observations) {
    positions.push_back(observation.anchor_position_samples);
  }

  calibration.anchor_position_samples = median_of(positions);

  std::vector<double> deviations;
  deviations.reserve(positions.size());
  for (const double position : positions) {
    deviations.push_back(
        std::abs(position - calibration.anchor_position_samples));
  }
  calibration.spread_samples =
      kMADToStandardDeviation * median_of(std::move(deviations));

  // The data levels in the same robust way: the median logic 0 and the median
  // amplitude of the records whose payload gave a measurement.
  std::vector<double> logic0_values;
  std::vector<double> amplitude_values;
  for (const VBICRIObservation& observation : observations) {
    if (observation.data_levels.usable) {
      logic0_values.push_back(observation.data_levels.logic0);
      amplitude_values.push_back(observation.data_levels.amplitude());
    }
  }
  if (!logic0_values.empty()) {
    calibration.data_levels.logic0 = median_of(std::move(logic0_values));
    calibration.data_levels.logic1 =
        calibration.data_levels.logic0 + median_of(std::move(amplitude_values));
    calibration.data_levels.usable = true;
  }

  // capture_offset = t_offset - cri_position / sample_rate, in samples.
  calibration.capture_offset_samples =
      service.t_offset_ns * 1e-9 * format.sample_rate_hz -
      calibration.anchor_position_samples;

  if (calibration.spread_samples <= thresholds.tight_spread_samples) {
    calibration.spread_class = VBIOffsetSpreadClass::kTight;
  } else if (calibration.spread_samples <= thresholds.maximum_spread_samples) {
    calibration.spread_class = VBIOffsetSpreadClass::kMild;
  } else {
    calibration.spread_class = VBIOffsetSpreadClass::kUnusable;
  }

  // Least squares slope of position against line ordinal.  The ordinals are
  // taken relative to the first observation because a capture holds tens of
  // millions of lines and their absolute values would swamp the fit's
  // precision.
  const uint64_t first_sequence = observations.front().line_sequence;
  double sum_x = 0.0;
  double sum_y = 0.0;
  for (const VBICRIObservation& observation : observations) {
    sum_x += static_cast<double>(observation.line_sequence - first_sequence);
    sum_y += observation.anchor_position_samples;
  }
  const double count = static_cast<double>(observations.size());
  const double mean_x = sum_x / count;
  const double mean_y = sum_y / count;

  double covariance = 0.0;
  double variance_x = 0.0;
  double lowest_x = 0.0;
  double highest_x = 0.0;
  bool first = true;
  for (const VBICRIObservation& observation : observations) {
    const double x =
        static_cast<double>(observation.line_sequence - first_sequence);
    const double y = observation.anchor_position_samples;
    covariance += (x - mean_x) * (y - mean_y);
    variance_x += (x - mean_x) * (x - mean_x);
    if (first || x < lowest_x) {
      lowest_x = x;
    }
    if (first || x > highest_x) {
      highest_x = x;
    }
    first = false;
  }

  bool drift_significant = false;
  if (variance_x > 0.0 && observations.size() > 2u) {
    calibration.drift_samples_per_line = covariance / variance_x;
    const double intercept =
        mean_y - calibration.drift_samples_per_line * mean_x;

    double residual_energy = 0.0;
    for (const VBICRIObservation& observation : observations) {
      const double x =
          static_cast<double>(observation.line_sequence - first_sequence);
      const double predicted =
          intercept + calibration.drift_samples_per_line * x;
      const double residual = observation.anchor_position_samples - predicted;
      residual_energy += residual * residual;
    }

    const double residual_deviation =
        std::sqrt(residual_energy / (count - 2.0));
    const double slope_standard_error =
        residual_deviation / std::sqrt(variance_x);
    drift_significant = (slope_standard_error <= 0.0) ||
                        (std::abs(calibration.drift_samples_per_line) >
                         kDriftSignificance * slope_standard_error);

    calibration.drift_total_samples =
        calibration.drift_samples_per_line * (highest_x - lowest_x);
  }

  if (drift_significant && format.line_length > 0) {
    // A drift of d samples over N lines means the capture actually holds
    // d more samples than N x line_length of them, so the true rate is the
    // configured one scaled by that fraction (design §5.3.4).
    calibration.suggested_sample_rate_hz =
        format.sample_rate_hz *
        (1.0 + calibration.drift_samples_per_line /
                   static_cast<double>(format.line_length));
  }

  calibration.drift_detected =
      drift_significant && (std::abs(calibration.drift_total_samples) >
                            thresholds.maximum_drift_samples);

  if (calibration.acceptance_fraction <
      thresholds.minimum_acceptance_fraction) {
    calibration.diagnostics.push_back(
        "The clock run-in was found on only " +
        format_number(calibration.acceptance_fraction * 100.0, 1) +
        "% of the sampled records, below the " +
        format_number(thresholds.minimum_acceptance_fraction * 100.0, 1) +
        "% this source format expects. The capture may carry teletext on "
        "different lines than configured, or the container format may be "
        "wrong.");
  }

  if (calibration.spread_class == VBIOffsetSpreadClass::kUnusable) {
    calibration.diagnostics.push_back(
        "The clock run-in position varies by " +
        format_number(calibration.spread_samples, 2) +
        " samples across the capture, above this format's limit of " +
        format_number(thresholds.maximum_spread_samples, 2) +
        " samples. Either the configured sampling rate of " +
        format_number(format.sample_rate_hz, 0) +
        " Hz is wrong, or the source is not time-base corrected. A single "
        "global capture offset cannot describe it.");
  } else if (calibration.spread_class == VBIOffsetSpreadClass::kMild) {
    calibration.warnings.push_back(
        "The clock run-in position varies by " +
        format_number(calibration.spread_samples, 2) + " samples, above the " +
        format_number(thresholds.tight_spread_samples, 2) +
        " samples a cleanly time-base corrected source shows. This is "
        "residual jitter or a slight sampling-rate error; the fit is usable "
        "but worth checking.");
  }

  if (calibration.drift_detected) {
    calibration.diagnostics.push_back(
        "The clock run-in position drifts monotonically by " +
        format_number(calibration.drift_total_samples, 2) +
        " samples across the sampled span, which means the configured "
        "sampling rate of " +
        format_number(format.sample_rate_hz, 0) +
        " Hz is wrong. The drift implies a true rate of " +
        format_number(calibration.suggested_sample_rate_hz, 0) +
        " Hz. A global capture offset cannot correct a rate error.");
  }

  calibration.converged = calibration.diagnostics.empty();

  calibration.summary = fit_summary(calibration, std::string());

  return calibration;
}

VBIOffsetCalibration fit_vbi_capture_offset_by_field(
    const VBISourceFormat& format, const VBITeletextService& service,
    const std::vector<VBICRIObservation>& observations,
    const std::array<uint64_t, 2>& records_examined) {
  VBIOffsetCalibration calibration;
  calibration.records_examined = records_examined[0] + records_examined[1];
  calibration.records_accepted = observations.size();
  calibration.acceptance_fraction =
      (calibration.records_examined > 0)
          ? static_cast<double>(observations.size()) /
                static_cast<double>(calibration.records_examined)
          : 0.0;
  calibration.suggested_sample_rate_hz = format.sample_rate_hz;

  std::array<std::vector<VBICRIObservation>, 2> by_field;
  for (const VBICRIObservation& observation : observations) {
    by_field[observation.field_index % 2u].push_back(observation);
  }

  // Each field held to every check of a whole fit, on its own records.
  std::vector<double> anchors;
  std::vector<double> logic0_values;
  std::vector<double> amplitude_values;
  std::string fields_summary;
  std::vector<std::string> reasons;
  for (uint32_t field = 0; field < 2u; ++field) {
    const VBIOffsetCalibration fit = fit_vbi_capture_offset(
        format, service, by_field[field], records_examined[field]);
    const std::string name = "stored field " + std::to_string(field + 1u);

    VBIFieldFit field_fit;
    field_fit.records_examined = records_examined[field];
    field_fit.records_accepted = fit.records_accepted;
    field_fit.carries_service = fit.converged;
    field_fit.anchor_position_samples = fit.anchor_position_samples;
    field_fit.spread_samples = fit.spread_samples;
    calibration.fields.push_back(field_fit);

    if (!fields_summary.empty()) {
      fields_summary += "; ";
    }
    if (!fit.converged) {
      fields_summary += name + " not used (" +
                        std::to_string(fit.records_accepted) + " of " +
                        std::to_string(records_examined[field]) + " locked)";
      const std::string reason =
          (fit.records_accepted == 0)
              ? "The run-in of " + name +
                    " was found on none of its records, so it carries no "
                    "captions."
              : "The run-in of " + name +
                    " gives no fit of its own, so it is left out: " +
                    join_diagnostics(fit.diagnostics);
      calibration.warnings.push_back(reason);
      reasons.push_back(reason);
      continue;
    }

    fields_summary += name + " at " +
                      format_number(fit.anchor_position_samples, 2) +
                      ", spread " + format_number(fit.spread_samples, 2) +
                      ", " + std::to_string(fit.records_accepted) + " of " +
                      std::to_string(records_examined[field]);
    anchors.push_back(fit.anchor_position_samples);
    calibration.spread_samples =
        std::max(calibration.spread_samples, fit.spread_samples);
    if (std::abs(fit.drift_total_samples) >=
        std::abs(calibration.drift_total_samples)) {
      calibration.drift_samples_per_line = fit.drift_samples_per_line;
      calibration.drift_total_samples = fit.drift_total_samples;
      calibration.suggested_sample_rate_hz = fit.suggested_sample_rate_hz;
    }
    for (const VBICRIObservation& observation : by_field[field]) {
      if (observation.data_levels.usable) {
        logic0_values.push_back(observation.data_levels.logic0);
        amplitude_values.push_back(observation.data_levels.amplitude());
      }
    }
  }

  const VBICalibrationThresholds& thresholds = format.calibration;
  if (anchors.empty()) {
    calibration.diagnostics.push_back(
        "Neither stored field's run-in gives a fit of its own. " +
        join_diagnostics(reasons));
  } else if (anchors.size() == 2u && std::abs(anchors[0] - anchors[1]) >
                                         thresholds.search_tolerance_samples) {
    calibration.diagnostics.push_back(
        "The two fields' run-ins sit " +
        format_number(std::abs(anchors[0] - anchors[1]), 2) +
        " samples apart, more than this format's search tolerance of " +
        format_number(thresholds.search_tolerance_samples, 2) +
        " samples, so one global capture offset cannot place both.");
  }

  if (!anchors.empty()) {
    double total = 0.0;
    for (const double anchor : anchors) {
      total += anchor;
    }
    calibration.anchor_position_samples =
        total / static_cast<double>(anchors.size());
    calibration.capture_offset_samples =
        service.t_offset_ns * 1e-9 * format.sample_rate_hz -
        calibration.anchor_position_samples;
  }
  if (!logic0_values.empty()) {
    calibration.data_levels.logic0 = median_of(std::move(logic0_values));
    calibration.data_levels.logic1 =
        calibration.data_levels.logic0 + median_of(std::move(amplitude_values));
    calibration.data_levels.usable = true;
  }

  // Each field that carries passed its own spread limit, so the class is the
  // worse of their own spreads; the fields' disagreement is two encoders'.
  if (anchors.empty() ||
      calibration.spread_samples > thresholds.maximum_spread_samples) {
    calibration.spread_class = VBIOffsetSpreadClass::kUnusable;
  } else if (calibration.spread_samples <= thresholds.tight_spread_samples) {
    calibration.spread_class = VBIOffsetSpreadClass::kTight;
  } else {
    calibration.spread_class = VBIOffsetSpreadClass::kMild;
  }

  calibration.converged = calibration.diagnostics.empty();
  calibration.summary = fit_summary(calibration, fields_summary);
  return calibration;
}

namespace {

// CTA-608-E §4: captions ride line 21 of each field, broadcast frame lines 21
// and 284, which are stored frame lines 20 and 283.
constexpr uint32_t kCaptionFrameLineField1 = 20;
constexpr uint32_t kCaptionFrameLineField2 = 283;

// How far ahead of a card's window a caption run-in may have begun and still
// be fitted, in bit periods: one and a half of its seven cycles.  A cx23885
// dump can start 0,82 of a cycle into every run-in; the
// caption reader downstream measures the run-in from 1,5 us after its start
// (closed_caption_observer.cpp), so a run-in clipped by this much leaves it a
// tone window still nine-tenths full.  Searched further back, the template's
// remaining tail starts to match lines carrying no captions at all: Land
// Before Time 2's second stored field locks three times at 1,5 cycles and
// five at 2.
constexpr double kCaptionClippedRunInBits = 1.5;

// The correlation a line-21 record's run-in must reach to count.  The caption
// template is the waveform CTA-608-E draws, and a caption line matches it at
// 0,8 to 0,97.  Of 4 467 line-21 records sampled from eighteen card dumps of
// captions, the 3 964 of fields that carry captions correlate at
// 0,8 or better (3 678), under 0,5 (176, the blank lines between captions) or
// in between (110, all but five under 0,7); the 503 of fields that carry none
// reach 0,5 to 0,6 eight times and never more.  Counted, such a line misleads
// the fit: on one long dump three of them, in the hours before
// its captions begin, made a drift of 45 samples out of a field whose captions
// sit within 3 samples of one place.  The teletext fit's 0,5 is set for run-ins
// a tape has all but erased, which a caption's 503 kHz run-in never is.
constexpr double kCaptionAcceptanceCorrelation = 0.7;

// Which records of a frame a survey correlates.
using VBISurveyedRecord = std::function<bool(const VBILineRecord&)>;

// What a survey found: the accepted run-in positions, and how many records of
// each stored field it looked at.
struct VBICRISurvey {
  std::vector<VBICRIObservation> observations;
  std::array<uint64_t, 2> records_examined{};

  // False when the transport could not report the capture's length, so the
  // frames were taken from its head.
  bool spread_across_capture = true;

  uint64_t total_examined() const {
    return records_examined[0] + records_examined[1];
  }
};

// The measurement every entry point below is built on: correlate a run-in
// template against the surveyed records of a sample of frames, in a window.
//
// The survey is one procedure because the question it answers is one question —
// where is the clock run-in? — and only what the answer is then used for
// differs between the two families (design §5.3.3), or which service's run-in
// it is.
bool survey_vbi_cri_positions(const VBILineReader& reader,
                              const VBITeletextService& service,
                              const VBICalibrationConfig& config,
                              const VBICRITemplateConfig& template_config,
                              const VBICRISearchWindow& window,
                              double acceptance_correlation,
                              const VBISurveyedRecord& surveyed,
                              VBICRISurvey& out_survey,
                              std::string& error_message) {
  const VBISourceFormat& format = reader.format();
  out_survey = VBICRISurvey();

  VBICRITemplate cri_template;
  if (!make_vbi_cri_frc_template(service, format.sample_rate_hz,
                                 template_config, cri_template,
                                 error_message)) {
    return false;
  }

  std::vector<uint64_t> frames;
  const std::optional<uint64_t> frame_count = reader.frame_count();
  if (frame_count.has_value()) {
    frames = vbi_calibration_frame_indices(*frame_count, config.sample_frames);
  } else {
    // A transport that cannot report its length can still be calibrated, but
    // only from its head; the sample is no longer spread across the capture
    // and the user is told so.
    for (uint32_t index = 0; index < config.sample_frames; ++index) {
      frames.push_back(index);
    }
    out_survey.spread_across_capture = false;
  }
  if (frames.empty()) {
    error_message = "The capture holds no whole frames to calibrate from.";
    return false;
  }

  for (const uint64_t frame : frames) {
    VBIFrameRecords records;
    if (!reader.read_frame(frame, records, error_message)) {
      return false;
    }

    for (const VBILineRecord& record : records.lines) {
      // A caption record beside the field range is placed for the caption
      // observer only (VBISourceFormat::caption_records).
      if (record.record_index > format.field_range.end || !surveyed(record)) {
        continue;
      }
      const uint32_t field = record.field_index % 2u;
      ++out_survey.records_examined[field];
      const VBICRIDetection detection = detect_vbi_cri_position(
          record.samples, cri_template, window, acceptance_correlation);
      if (!detection.accepted) {
        continue;
      }

      VBICRIObservation observation;
      observation.line_sequence = vbi_line_sequence(format, record);
      observation.field_index = field;
      observation.anchor_position_samples = detection.anchor_position_samples;
      observation.peak_correlation = detection.peak_correlation;

      // The record's own levels, read where its run-in was found rather than
      // where the fit will put it.
      observation.data_levels = estimate_vbi_data_levels(
          record.samples,
          vbi_data_windows(service, format.sample_rate_hz,
                           detection.anchor_position_samples,
                           format.valid_samples),
          config.levels);
      observation.run_in_purity =
          run_in_purity(record.samples, service, format.sample_rate_hz,
                        detection.anchor_position_samples);
      observation.framing_errors = framing_errors(
          record.samples, service, format.sample_rate_hz,
          detection.anchor_position_samples, observation.data_levels);
      out_survey.observations.push_back(observation);
    }
  }
  return true;
}

// A survey that took its frames from the head of the capture says so.
void warn_if_from_the_head(const VBICRISurvey& survey,
                           VBIOffsetCalibration& calibration) {
  if (!survey.spread_across_capture) {
    calibration.warnings.push_back(
        "The transport could not report the capture's length, so the "
        "calibration sample was taken from the head of the file rather than "
        "spread across it.");
  }
}

// Survey every record in the window the format's own offset and tolerance
// imply, and fit one offset to all of them.
bool survey_and_fit(const VBILineReader& reader,
                    const VBITeletextService& service,
                    const VBICalibrationConfig& config, VBICRISurvey& survey,
                    VBIOffsetCalibration& out_calibration,
                    std::string& error_message) {
  const VBISourceFormat& format = reader.format();
  if (!survey_vbi_cri_positions(
          reader, service, config, config.template_config,
          vbi_cri_search_window(format, service),
          format.calibration.acceptance_correlation,
          [](const VBILineRecord& /*record*/) { return true; }, survey,
          error_message)) {
    return false;
  }
  out_calibration = fit_vbi_capture_offset(format, service, survey.observations,
                                           survey.total_examined());
  warn_if_from_the_head(survey, out_calibration);
  return true;
}

// Refuse a fit most of whose records show neither mark of a teletext line.
void refuse_a_fit_without_teletext_marks(
    const std::vector<VBICRIObservation>& observations,
    VBIOffsetCalibration& calibration) {
  if (!calibration.converged || observations.empty()) {
    return;
  }
  const auto marked = static_cast<uint64_t>(std::count_if(
      observations.begin(), observations.end(), vbi_shows_teletext_marks));
  const double fraction =
      static_cast<double>(marked) / static_cast<double>(observations.size());
  if (fraction >= kMinimumMarkedFraction) {
    return;
  }
  calibration.converged = false;
  calibration.diagnostics.push_back(
      "Only " + std::to_string(marked) + " of the " +
      std::to_string(observations.size()) +
      " records the run-in template locked on show its framing code or the "
      "alternation of its clock run-in where it locked, so what it locked on "
      "is something else shaped like them rather than the configured "
      "service.");
  calibration.summary = fit_summary(calibration, std::string());
}

}  // namespace

bool calibrate_vbi_capture_offset(const VBILineReader& reader,
                                  const VBITeletextService& service,
                                  const VBICalibrationConfig& config,
                                  VBIOffsetCalibration& out_calibration,
                                  std::string& error_message) {
  out_calibration = VBIOffsetCalibration();

  if (reader.format().family == VBISourceFamily::kTBCDerived) {
    error_message =
        "A time-base corrected source has sample 0 of every record at 0H by "
        "construction, so its capture offset is exactly zero and must not be "
        "calibrated (design §5.3.3).";
    return false;
  }

  VBICRISurvey survey;
  if (!survey_and_fit(reader, service, config, survey, out_calibration,
                      error_message)) {
    return false;
  }
  refuse_a_fit_without_teletext_marks(survey.observations, out_calibration);
  return true;
}

bool calibrate_vbi_capture_offset_on_captions(
    const VBILineReader& reader, const VBICalibrationConfig& config,
    VBIOffsetCalibration& out_calibration, std::string& error_message) {
  out_calibration = VBIOffsetCalibration();

  const VBISourceFormat& format = reader.format();
  if (format.family == VBISourceFamily::kTBCDerived) {
    error_message =
        "A time-base corrected source has sample 0 of every record at 0H by "
        "construction, so its capture offset is exactly zero and must not be "
        "calibrated (design §5.3.3).";
    return false;
  }

  VBITeletextService captions;
  if (!vbi_caption_service(format.tv_system, captions, error_message)) {
    return false;
  }

  // The records the format stores line 21 of either field in.
  VBITeletextLineMap line_map;
  if (!make_vbi_teletext_line_map(format.tv_system, format.tt_system, line_map,
                                  error_message)) {
    return false;
  }
  std::array<std::vector<uint8_t>, 2> caption_records;
  for (uint32_t field = 0; field < 2u; ++field) {
    caption_records[field].assign(format.field_lines, 0);
    for (uint32_t record = format.field_range.start;
         record <= format.field_range.end && record < format.field_lines;
         ++record) {
      uint32_t frame_line = 0;
      std::string unused;
      if (map_vbi_record_to_frame_line(format, line_map, field, record,
                                       frame_line, unused) &&
          (frame_line == kCaptionFrameLineField1 ||
           frame_line == kCaptionFrameLineField2)) {
        caption_records[field][record] = 1;
      }
    }
  }
  const VBISurveyedRecord line_21 =
      [&caption_records](const VBILineRecord& record) {
        const std::vector<uint8_t>& field =
            caption_records[record.field_index % 2u];
        return record.record_index < field.size() &&
               field[record.record_index] != 0;
      };

  // A card window can open within a few samples of a caption run-in (eight
  // samples in, on one cx23885 dump), so the template holds
  // no lead-in ahead of the run-in's first rise.
  VBICRITemplateConfig template_config = config.template_config;
  template_config.lead_in_bits = 0.0;

  // The window opens at a run-in clipped by up to kCaptionClippedRunInBits of
  // its cycles, however far that is from where the format's offset predicts
  // the run-in, and closes the format's search tolerance after that.
  VBICRISearchWindow window = vbi_cri_search_window(format, captions);
  window.begin_samples = -kCaptionClippedRunInBits * 2.0 *
                         captions.samples_per_bit(format.sample_rate_hz);
  window.clipped_head = true;

  VBICRISurvey survey;
  if (!survey_vbi_cri_positions(reader, captions, config, template_config,
                                window, kCaptionAcceptanceCorrelation, line_21,
                                survey, error_message)) {
    return false;
  }
  out_calibration = fit_vbi_capture_offset_by_field(
      format, captions, survey.observations, survey.records_examined);
  warn_if_from_the_head(survey, out_calibration);
  return true;
}

bool measure_vbi_service_anchor(const VBILineReader& reader,
                                const VBITeletextService& service,
                                const VBICalibrationConfig& config,
                                VBIOffsetCalibration& out_calibration,
                                std::string& error_message) {
  out_calibration = VBIOffsetCalibration();

  if (reader.format().family != VBISourceFamily::kTBCDerived) {
    error_message =
        "The service anchor is only measured on a time-base corrected source. "
        "On a card capture the anchor is not separable from the capture "
        "offset — the fitted offset already puts the run-in at the service's "
        "nominal time — so measuring it would be fitting one unknown twice.";
    return false;
  }

  VBICRISurvey survey;
  return survey_and_fit(reader, service, config, survey, out_calibration,
                        error_message);
}

double vbi_measured_anchor_ns(const VBIOffsetCalibration& calibration,
                              double sample_rate_hz) {
  if (!(sample_rate_hz > 0.0)) {
    return 0.0;
  }
  // Sample 0 of the record is 0H, so where the run-in sits within the record
  // is, without any further correction, how long after 0H it was transmitted.
  return calibration.anchor_position_samples / sample_rate_hz * 1e9;
}

}  // namespace orc
