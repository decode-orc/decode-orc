/*
 * File:        fm_beat_calibration.cpp
 * Module:      analysis
 * Purpose:     Measures a PAL LaserDisc master's FM carrier at black from
 *              the beat FM Beat Cancel removes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "fm_beat_calibration.h"

#include <orc/support/fm_beat.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <tuple>

namespace orc {

namespace {

struct Candidate {
  size_t flat_samples = 0;
  FrameID frame = 0;
  size_t field = 0;
};

// The fields' beats, predicted at |carrier_hz|.
std::vector<std::unique_ptr<fm_beat::FieldBeat>> predict(
    const VideoFrameRepresentation& source,
    const std::vector<Candidate>& candidates, double carrier_hz) {
  std::vector<std::unique_ptr<fm_beat::FieldBeat>> beats;
  const auto params = source.get_video_parameters();
  for (const auto& c : candidates) {
    const auto desc = source.get_frame_descriptor(c.frame);
    const auto frame = source.get_frame_copy(c.frame);
    if (!desc || !params || frame.size() < desc->samples_total) continue;
    const auto fields = fm_beat::frame_fields(*desc, *params);
    beats.push_back(std::make_unique<fm_beat::FieldBeat>(
        frame.data(), fields[c.field], carrier_hz));
  }
  return beats;
}

struct Totals {
  double removed_energy = 0.0;
  size_t lines_corrected = 0;
};

Totals fit_all(const std::vector<std::unique_ptr<fm_beat::FieldBeat>>& beats,
               double offset_hz) {
  Totals totals;
  for (const auto& beat : beats) {
    const auto fit = beat->fit(offset_hz);
    totals.removed_energy += fit.removed_energy;
    totals.lines_corrected += fit.lines_corrected;
  }
  return totals;
}

}  // namespace

FmBeatCalibration calibrate_fm_beat(const VideoFrameRepresentation& source,
                                    const FmBeatCalibrationOptions& options,
                                    const FmBeatCalibrationProgress& progress) {
  FmBeatCalibration result;
  const auto params = source.get_video_parameters();
  if (!params || params->system != VideoSystem::PAL ||
      source.has_separate_channels()) {
    return result;
  }
  auto report = [&](int percent, const std::string& status) {
    return !progress || progress(percent, status);
  };

  // Probe a spread of frames for the fields with the most flat colour.
  const uint64_t count = source.frame_count();
  const FrameID first = source.frame_range().first;
  const size_t probes =
      static_cast<size_t>(std::min<uint64_t>(options.probe_frames, count));
  std::vector<Candidate> candidates;
  size_t picture_lines_per_field = 0;
  int32_t width = 0;
  for (size_t k = 0; k < probes; ++k) {
    if (!report(static_cast<int>(30 * k / std::max<size_t>(probes, 1)),
                "Looking for flat colour")) {
      return {};
    }
    const FrameID id = first + ((k * count) / probes);
    const auto desc = source.get_frame_descriptor(id);
    const auto frame = source.get_frame_copy(id);
    if (!desc || frame.size() < desc->samples_total) continue;
    ++result.probed_frames;
    const auto fields = fm_beat::frame_fields(*desc, *params);
    for (size_t f = 0; f < fields.size(); ++f) {
      const size_t flat = fm_beat::flat_colour_samples(frame.data(), fields[f]);
      if (flat >= fm_beat::kMinFlatColourSamples) {
        candidates.push_back({flat, id, f});
      }
      picture_lines_per_field = fields[f].picture_end - fields[f].picture_first;
      width = fields[f].active_end - fields[f].active_start;
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              return std::tie(b.flat_samples, a.frame, a.field) <
                     std::tie(a.flat_samples, b.frame, b.field);
            });
  if (candidates.size() > options.max_fields) {
    candidates.resize(options.max_fields);
  }
  result.fields = candidates.size();
  result.picture_lines = candidates.size() * picture_lines_per_field;
  if (candidates.empty()) return result;

  // The coarse sweep, the beat predicted at the nominal carrier.
  const double nominal = fm_beat::kNominalCarrierBlackHz;
  auto beats = predict(source, candidates, nominal);
  const auto steps = static_cast<size_t>(std::floor(
                         (options.max_carrier_hz - options.min_carrier_hz) /
                         options.coarse_step_hz)) +
                     1;
  double best = nominal;
  double best_energy = -1.0;
  for (size_t s = 0; s < steps; ++s) {
    if (!report(30 + static_cast<int>(55 * s / steps),
                "Searching the carrier")) {
      return {};
    }
    const double carrier = options.min_carrier_hz +
                           (static_cast<double>(s) * options.coarse_step_hz);
    const auto totals = fit_all(beats, carrier - nominal);
    result.sweep.emplace_back(carrier, totals.removed_energy);
    if (totals.removed_energy > best_energy) {
      best_energy = totals.removed_energy;
      best = carrier;
    }
  }

  // The fine sweep, the beat predicted at the coarse best.
  if (!report(85, "Refining the carrier")) return {};
  beats = predict(source, candidates, best);
  const double coarse = best;
  best_energy = -1.0;
  Totals best_totals;
  for (double offset = -options.fine_span_hz;
       offset <= options.fine_span_hz + (options.fine_step_hz / 2.0);
       offset += options.fine_step_hz) {
    const auto totals = fit_all(beats, offset);
    if (totals.removed_energy > best_energy) {
      best_energy = totals.removed_energy;
      best = coarse + offset;
      best_totals = totals;
    }
  }
  report(100, "Done");

  result.carrier_black_hz = best;
  result.lines_corrected = best_totals.lines_corrected;
  result.found = best_totals.lines_corrected > 0;
  if (best_totals.lines_corrected > 0 && width > 0) {
    result.removed_rms_ire =
        std::sqrt(best_totals.removed_energy /
                  static_cast<double>(best_totals.lines_corrected *
                                      static_cast<size_t>(width)));
  }
  ORC_LOG_DEBUG(
      "FM beat calibration: {} fields of {} frames, carrier {:.4f} MHz, {} of "
      "{} lines corrected, {:.3f} IRE removed",
      result.fields, result.probed_frames, best / 1.0e6, result.lines_corrected,
      result.picture_lines, result.removed_rms_ire);
  return result;
}

}  // namespace orc
