/*
 * File:        vbi_cri_correlator.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Locates the clock run-in within a stored line record
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_cri_correlator.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace orc {

namespace {

// A parabola through three correlation values only means anything when the
// middle one is a maximum; a flat or inverted triple is refined by nothing.
constexpr double kMinimumCurvature = 1e-12;

// A parabolic fit cannot move a peak by more than half a sample without
// contradicting the whole-sample search that found it.
constexpr double kMaximumRefinementSamples = 0.5;

// The correlation at a negative lag: the template's tail, from sample -lag on,
// against the record's head.  Neither side is zero-mean over the overlap nor
// the template unit-norm there, so both are taken out explicitly.
double clipped_normalised_correlation(const std::vector<double>& record_samples,
                                      const std::vector<double>& tmpl,
                                      size_t clipped) {
  const size_t count = tmpl.size() - clipped;
  double record_total = 0.0;
  double record_energy = 0.0;
  double template_total = 0.0;
  double template_energy = 0.0;
  double product = 0.0;
  for (size_t index = 0; index < count; ++index) {
    const double value = record_samples[index];
    const double expected = tmpl[clipped + index];
    record_total += value;
    record_energy += value * value;
    template_total += expected;
    template_energy += expected * expected;
    product += value * expected;
  }

  const double n = static_cast<double>(count);
  const double covariance = product - (record_total * template_total) / n;
  const double record_variance =
      record_energy - (record_total * record_total) / n;
  const double template_variance =
      template_energy - (template_total * template_total) / n;
  if (!(record_variance > 0.0) || !(template_variance > 0.0)) {
    return 0.0;
  }
  return covariance / std::sqrt(record_variance * template_variance);
}

}  // namespace

VBICRISearchWindow vbi_cri_search_window(const VBISourceFormat& format,
                                         const VBITeletextService& service) {
  VBICRISearchWindow window;

  const double predicted = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);
  const double tolerance =
      std::max(0.0, format.calibration.search_tolerance_samples);

  window.begin_samples = std::max(0.0, predicted - tolerance);
  window.end_samples = predicted + tolerance;
  return window;
}

double vbi_normalised_correlation(const std::vector<double>& record_samples,
                                  const VBICRITemplate& tmpl, int64_t lag) {
  const size_t length = tmpl.samples.size();
  if (length == 0 || record_samples.size() < length) {
    return 0.0;
  }
  if (lag < 0) {
    const size_t clipped = static_cast<size_t>(-lag);
    if (2u * clipped > length) {
      return 0.0;
    }
    return clipped_normalised_correlation(record_samples, tmpl.samples,
                                          clipped);
  }
  const size_t first = static_cast<size_t>(lag);
  if (first + length > record_samples.size()) {
    return 0.0;
  }

  double total = 0.0;
  double energy = 0.0;
  double product = 0.0;
  for (size_t index = 0; index < length; ++index) {
    const double value = record_samples[first + index];
    total += value;
    energy += value * value;

    // The template is zero-mean, so the record's own mean contributes nothing
    // to this sum and does not have to be removed from it.
    product += value * tmpl.samples[index];
  }

  const double count = static_cast<double>(length);
  const double variance = energy - (total * total) / count;
  if (!(variance > 0.0)) {
    // A constant window carries no shape at all: blanking, or a saturated
    // line.  It correlates with nothing.
    return 0.0;
  }

  return product / std::sqrt(variance);
}

VBICRIDetection detect_vbi_cri_position(
    const std::vector<double>& record_samples, const VBICRITemplate& tmpl,
    const VBICRISearchWindow& window, double acceptance_correlation) {
  VBICRIDetection detection;

  const size_t length = tmpl.samples.size();
  if (length == 0 || record_samples.size() < length) {
    return detection;
  }

  // The window is expressed in run-in positions; the search runs over template
  // start positions, which sit one anchor offset earlier.
  const double first_lag =
      std::floor(window.begin_samples - tmpl.anchor_samples);
  const double last_lag = std::ceil(window.end_samples - tmpl.anchor_samples);
  const int64_t highest_lag =
      static_cast<int64_t>(record_samples.size() - length);
  // A clipped head may hang off the record by up to half the template, the
  // most vbi_normalised_correlation() measures.
  const int64_t lowest_lag =
      window.clipped_head ? -static_cast<int64_t>(length / 2u) : int64_t{0};
  const int64_t begin_lag =
      std::clamp(static_cast<int64_t>(first_lag), lowest_lag, highest_lag);
  const int64_t end_lag =
      std::clamp(static_cast<int64_t>(last_lag), begin_lag, highest_lag);

  std::vector<double> correlations;
  correlations.reserve(static_cast<size_t>(end_lag - begin_lag) + 1u);
  for (int64_t lag = begin_lag; lag <= end_lag; ++lag) {
    correlations.push_back(
        vbi_normalised_correlation(record_samples, tmpl, lag));
  }
  if (correlations.empty()) {
    return detection;
  }

  const size_t peak_index = static_cast<size_t>(std::distance(
      correlations.begin(),
      std::max_element(correlations.begin(), correlations.end())));
  detection.peak_correlation = correlations[peak_index];
  detection.peak_lag = begin_lag + static_cast<int64_t>(peak_index);

  // Sub-sample refinement by the parabola through the peak and its neighbours.
  if (peak_index > 0 && peak_index + 1u < correlations.size()) {
    const double before = correlations[peak_index - 1u];
    const double centre = correlations[peak_index];
    const double after = correlations[peak_index + 1u];
    const double curvature = before - 2.0 * centre + after;
    if (curvature < -kMinimumCurvature) {
      const double correction = 0.5 * (before - after) / curvature;
      if (std::isfinite(correction) &&
          std::abs(correction) <= kMaximumRefinementSamples) {
        detection.refinement_samples = correction;
        detection.refined = true;
      }
    }
  }

  detection.anchor_position_samples = static_cast<double>(detection.peak_lag) +
                                      detection.refinement_samples +
                                      tmpl.anchor_samples;
  detection.accepted = detection.peak_correlation >= acceptance_correlation;
  return detection;
}

}  // namespace orc
