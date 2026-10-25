/*
 * File:        frame_quality_observer.cpp
 * Module:      orc-core
 * Purpose:     Frame quality observer implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <frame_quality_observer.h>
#include <orc/stage/field_id.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>

namespace orc {

void FrameQualityObserver::process_frame(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    IObservationContext& context) {
  // Frame-level results are keyed by the frame's first field.
  const FieldID frame_fid(frame_id * 2);

  auto vp_opt = representation.get_video_parameters();
  if (!vp_opt.has_value()) {
    // No video parameters — write zeroed quality for the frame
    context.set(frame_fid, "disc_quality", "quality_score", 0.0);
    context.set(frame_fid, "disc_quality", "dropout_count", 0);
    context.set(frame_fid, "disc_quality", "phase_valid", false);
    return;
  }
  const auto& vp = vp_opt.value();

  double quality_score = calculate_quality_score(representation, frame_id, vp);

  // Get dropout hints (frame-level) for diagnostics
  auto dropout_hints = representation.get_dropout_hints(frame_id);

  context.set(frame_fid, "disc_quality", "quality_score", quality_score);
  context.set(frame_fid, "disc_quality", "dropout_count",
              static_cast<int32_t>(dropout_hints.size()));
  context.set(frame_fid, "disc_quality", "phase_valid", true);

  ORC_LOG_DEBUG("FrameQualityObserver: Frame {} quality={:.3f} dropouts={}",
                frame_id, quality_score, dropout_hints.size());
}

double FrameQualityObserver::calculate_quality_score(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    const SourceParameters& vp) const {
  double score = 1.0;

  // Factor 1: Dropout density across the whole frame
  auto dropout_hints = representation.get_dropout_hints(frame_id);
  if (!dropout_hints.empty()) {
    // Total frame samples (approximate using nominal line width)
    size_t total_samples = static_cast<size_t>(vp.frame_width_nominal) *
                           static_cast<size_t>(vp.frame_height);

    if (total_samples > 0) {
      size_t total_dropout_samples = 0;
      for (const auto& region : dropout_hints) {
        total_dropout_samples += region.sample_count;
      }

      double dropout_ratio = static_cast<double>(total_dropout_samples) /
                             static_cast<double>(total_samples);

      score *= std::exp(-10.0 * dropout_ratio);
    }
  }

  return std::max(0.0, std::min(1.0, score));
}

}  // namespace orc
