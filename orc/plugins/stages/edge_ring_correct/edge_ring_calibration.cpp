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
  return FrameFields{
      edge_ring::field_image(frame.data(), spl, 0, f1, black, white),
      edge_ring::field_image(frame.data(), spl, f1, desc->height - f1, black,
                             white)};
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
    const auto feats = edge_ring::find_features(fields->first, true);
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

EdgeRingWindowReport calibrate_window(int64_t first, int64_t end,
                                      const std::vector<FieldImage>& fit_fields,
                                      std::vector<CertifyField> certify_fields,
                                      std::vector<CertifyField> report_fields) {
  EdgeRingWindowReport r;
  r.first_frame = first;
  r.end_frame = end;
  // fit fields come in frame pairs
  edge_ring::ProfileSums sums, line_art;
  int line_art_frames = 0;
  for (size_t k = 0; k + 1 < fit_fields.size(); k += 2) {
    edge_ring::ProfileSums frame;
    for (size_t p = 0; p < 2; ++p) {
      const auto& f = fit_fields[k + p];
      frame += edge_ring::measure(f, edge_ring::find_features(f, true));
    }
    sums += frame;
    if (frame.lines >= edge_ring::kLineArtLines) {
      line_art += frame;
      ++line_art_frames;
    }
  }
  auto fitted = edge_ring::fit(sums, line_art, line_art_frames);
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
  for (int64_t k = 0; k < nw; ++k) {
    const int64_t first = k * count / nw;
    const int64_t end = (k + 1) * count / nw;
    if (progress && !progress(static_cast<int>(100 * k / nw),
                              "Window " + std::to_string(k + 1) + " of " +
                                  std::to_string(nw))) {
      return EdgeRingCalibration{};
    }
    std::vector<FieldImage> fit_fields;
    std::vector<CertifyField> certify_fields, report_fields;
    for (int third = 0; third < 3; ++third) {
      for (int64_t f :
           pick_frames(source, base, first, end, third, o, black, white)) {
        auto fields = read_frame(source, base + f, black, white);
        if (!fields) continue;
        if (third == 0) {
          fit_fields.push_back(std::move(fields->first));
          fit_fields.push_back(std::move(fields->second));
        } else {
          auto& to = third == 1 ? certify_fields : report_fields;
          to.emplace_back(std::move(fields->first));
          to.emplace_back(std::move(fields->second));
        }
      }
    }
    auto r =
        calibrate_window(base + first, base + end, fit_fields,
                         std::move(certify_fields), std::move(report_fields));
    if (r.accepted) accepted.push_back(r.chosen);
    out.windows.push_back(std::move(r));
  }
  if (auto t = edge_ring::RingTrack::from_windows(accepted)) out.track = *t;
  if (progress) progress(100, "Done");
  return out;
}

}  // namespace orc
