/*
 * File:        edge_ring_calibration.cpp
 * Module:      orc-stage-plugin-edge-ring-correct
 * Purpose:     Edge Ring Calibration: per-window fit and held-out
 *              certification along a source
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "edge_ring_calibration.h"

#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <numeric>

namespace orc {

namespace {

using edge_ring::CertifyField;
using edge_ring::FieldImage;

struct FrameFields {
  FieldImage first;
  FieldImage second;
};

std::optional<FrameFields> read_frame(const VideoFrameRepresentation& source,
                                      FrameID id, double black, double white) {
  const auto desc = source.get_frame_descriptor(id);
  if (!desc || desc->system != VideoSystem::PAL) return std::nullopt;
  const auto frame = source.get_frame_copy(id);
  const size_t spl = desc->samples_per_line_nominal;
  if (frame.size() < desc->height * spl || desc->height < 2)
    return std::nullopt;
  const size_t f1 = std::min(field1_lines(VideoSystem::PAL), desc->height);
  FrameFields out{
      edge_ring::field_image(frame.data(), spl, 0, f1, black, white),
      edge_ring::field_image(frame.data(), spl, f1, desc->height - f1, black,
                             white)};
  std::vector<std::pair<uint64_t, uint32_t>> runs;
  for (const auto& d : source.get_dropout_hints(id)) {
    runs.emplace_back(d.sample_start, d.sample_count);
  }
  edge_ring::mark_dropouts(runs, spl, 0, out.first);
  edge_ring::mark_dropouts(runs, spl, f1, out.second);
  return out;
}

// The frames of one third of [first, end) to read.
std::vector<int64_t> pick_frames(const VideoFrameRepresentation& source,
                                 FrameID base, int64_t first, int64_t end,
                                 int third, const EdgeRingCalibrationOptions& o,
                                 double black, double white) {
  std::vector<int64_t> probes;
  for (int64_t f = first; f < end; ++f) {
    if (f % 3 == third) probes.push_back(f);
  }
  std::vector<int64_t> strided;
  for (size_t k = 0; k < probes.size();
       k += std::max<size_t>(o.probe_stride, 1)) {
    strided.push_back(probes[k]);
  }
  std::vector<double> score(strided.size(), 0.0);
  for (size_t k = 0; k < strided.size(); ++k) {
    const auto fields = read_frame(source, base + strided[k], black, white);
    if (!fields) continue;
    const auto feats = edge_ring::measured_features(
        fields->first, edge_ring::find_events(fields->first));
    score[k] = feats.steps.size() + 0.5 * feats.lines.size();
  }
  std::vector<size_t> order(strided.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return score[a] > score[b]; });
  std::vector<int64_t> picked;
  for (size_t k : order) {
    if (score[k] <= 0 || picked.size() >= o.frames_per_third) break;
    const bool spaced =
        std::all_of(picked.begin(), picked.end(), [&](int64_t p) {
          return std::llabs(p - strided[k]) >=
                 static_cast<long long>(o.min_spacing);
        });
    if (spaced) picked.push_back(strided[k]);
  }
  std::sort(picked.begin(), picked.end());
  return picked;
}

}  // namespace

EdgeRingFitSums fit_sums(const std::vector<FieldImage>& fit_fields) {
  EdgeRingFitSums out;
  for (size_t k = 0; k + 1 < fit_fields.size(); k += 2) {
    edge_ring::ProfileSums frame;
    for (size_t p = 0; p < 2; ++p) {
      const auto& f = fit_fields[k + p];
      const auto events = edge_ring::find_events(f);
      auto s = edge_ring::measure(f, edge_ring::measured_features(f, events));
      s.events = static_cast<int>(events.size());
      s.pairs = edge_ring::count_pairs(events);
      frame += s;
    }
    out.all += frame;
    if (edge_ring::is_line_art(frame)) {
      out.line_art += frame;
      ++out.line_art_frames;
    }
  }
  return out;
}

EdgeRingWindowReport calibrate_window(
    int64_t first, int64_t end, const EdgeRingFitSums& fit,
    std::shared_ptr<const edge_ring::LevelTables> levels,
    std::vector<CertifyField> certify_fields,
    std::vector<CertifyField> report_fields) {
  EdgeRingWindowReport r;
  r.first_frame = first;
  r.end_frame = end;
  auto fitted = edge_ring::fit(fit.all, fit.line_art, fit.line_art_frames,
                               std::move(levels));
  const edge_ring::WindowCorrection* chosen = nullptr;
  if (fitted && !certify_fields.empty()) {
    r.fitted = true;
    fitted->first_frame = first;
    fitted->end_frame = end;
    for (const auto& c : edge_ring::candidates(*fitted)) {
      // certified as the text form holds it
      const auto held = edge_ring::RingTrack::from_windows({c});
      if (!held) continue;
      const auto& candidate = held->windows().front();
      const auto cert = edge_ring::certify(certify_fields, candidate);
      if (cert.accepted) {
        r.accepted = true;
        r.chosen = candidate;
        chosen = &r.chosen;
        r.reasons.clear();
        break;
      }
      if (r.reasons.empty()) r.reasons = cert.reasons;
    }
  } else {
    r.reasons.push_back(fitted ? "no frames to certify on"
                               : "too few measurable edges to fit");
  }
  if (!report_fields.empty()) {
    r.held_out = edge_ring::compare(report_fields, chosen, 8);
  }
  return r;
}

EdgeRingWindowReport calibrate_window(int64_t first, int64_t end,
                                      const std::vector<FieldImage>& fit_fields,
                                      std::vector<CertifyField> certify_fields,
                                      std::vector<CertifyField> report_fields) {
  const auto fit = fit_sums(fit_fields);
  return calibrate_window(first, end, fit,
                          std::make_shared<const edge_ring::LevelTables>(
                              edge_ring::level_templates(fit.all)),
                          std::move(certify_fields), std::move(report_fields));
}

EdgeRingCalibration calibrate_edge_ring(
    const VideoFrameRepresentation& source, const EdgeRingCalibrationOptions& o,
    const EdgeRingCalibrationProgress& progress) {
  EdgeRingCalibration out;
  const auto params = source.get_video_parameters();
  if (!params || params->system != VideoSystem::PAL ||
      params->white_level <= params->black_level) {
    return out;
  }
  const double black = params->black_level, white = params->white_level;
  const int64_t count = static_cast<int64_t>(source.frame_count());
  const FrameID base = source.frame_range().first;
  const int64_t nw =
      std::max<int64_t>(1, count / std::max<int64_t>(1, o.window_frames));
  std::vector<edge_ring::WindowCorrection> accepted;
  auto window_of = [&](int64_t k) {
    return std::pair<int64_t, int64_t>{k * count / nw, (k + 1) * count / nw};
  };
  auto read_third = [&](int64_t k, int third) {
    std::vector<FieldImage> fields;
    const auto [first, end] = window_of(k);
    auto opts = o;
    if (third == 0) opts.frames_per_third = o.fit_frames;
    for (int64_t f :
         pick_frames(source, base, first, end, third, opts, black, white)) {
      auto pair = read_frame(source, base + f, black, white);
      if (!pair) continue;
      fields.push_back(std::move(pair->first));
      fields.push_back(std::move(pair->second));
    }
    return fields;
  };
  // pass 1: every window's fit third; the rising templates per level bin are
  // pooled over all of them
  std::vector<EdgeRingFitSums> fits;
  for (int64_t k = 0; k < nw; ++k) {
    if (progress && !progress(static_cast<int>(30 * k / nw),
                              "Fitting window " + std::to_string(k + 1) +
                                  " of " + std::to_string(nw))) {
      return EdgeRingCalibration{};
    }
    fits.push_back(fit_sums(read_third(k, 0)));
  }
  edge_ring::ProfileSums pooled;
  for (const auto& f : fits) pooled += f.all;
  const auto levels = std::make_shared<const edge_ring::LevelTables>(
      edge_ring::level_templates(pooled));
  for (int64_t k = 0; k < nw; ++k) {
    if (progress && !progress(static_cast<int>(30 + 70 * k / nw),
                              "Certifying window " + std::to_string(k + 1) +
                                  " of " + std::to_string(nw))) {
      return EdgeRingCalibration{};
    }
    std::vector<CertifyField> certify_fields, report_fields;
    for (auto& f : read_third(k, 1)) certify_fields.emplace_back(std::move(f));
    for (auto& f : read_third(k, 2)) report_fields.emplace_back(std::move(f));
    const auto [first, end] = window_of(k);
    auto r =
        calibrate_window(base + first, base + end, fits[k], levels,
                         std::move(certify_fields), std::move(report_fields));
    if (r.accepted) accepted.push_back(r.chosen);
    out.windows.push_back(std::move(r));
  }
  if (auto t = edge_ring::RingTrack::from_windows(accepted)) out.track = *t;
  if (progress) progress(100, "Done");
  return out;
}

}  // namespace orc
