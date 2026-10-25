/*
 * File:        video_parameter_observation_presenter.cpp
 * Module:      orc-presenters
 * Purpose:     Presenter for video parameter observer dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../include/video_parameter_observation_presenter.h"

#include <orc/stage/observation/observation_context.h>
#include <orc/stage/orc_source_parameters.h>

#include <variant>

namespace orc::presenters {

VideoParameterObservationView
VideoParameterObservationPresenter::extractObservations(
    FieldID field_id, const void* obs_context_ptr,
    const std::optional<orc::SourceParameters>& video_params) {
  const auto* ctx =
      static_cast<const orc::ObservationContext*>(obs_context_ptr);
  VideoParameterObservationView out{};

  if (video_params.has_value()) {
    out.video_params = toVideoParametersView(*video_params);
  }

  // BurstLevelObserver, ColourFramePhaseObserver, WhiteSNRObserver,
  // BlackPSNRObserver and FrameQualityObserver store their values keyed by the
  // first field of the frame.
  const orc::FieldID frame_fid(
      static_cast<uint64_t>((field_id.value() / 2) * 2));

  if (auto v = ctx->get(field_id, "colour_frame_phase", "field_phase_id");
      v && std::holds_alternative<int32_t>(*v)) {
    out.field_phase_id = std::get<int32_t>(*v);
  }

  if (auto v = ctx->get(frame_fid, "colour_frame_phase", "colour_frame_index");
      v && std::holds_alternative<int32_t>(*v)) {
    out.colour_frame_index = std::get<int32_t>(*v);
  }

  if (auto v = ctx->get(frame_fid, "burst_level", "median_burst_10bit");
      v && std::holds_alternative<double>(*v)) {
    out.burst_level_10bit = std::get<double>(*v);
  }

  if (auto v = ctx->get(frame_fid, "white_snr", "snr_db");
      v && std::holds_alternative<double>(*v)) {
    out.white_snr_db = std::get<double>(*v);
  }

  if (auto v = ctx->get(frame_fid, "black_psnr", "psnr_db");
      v && std::holds_alternative<double>(*v)) {
    out.black_psnr_db = std::get<double>(*v);
  }

  if (auto v = ctx->get(frame_fid, "disc_quality", "quality_score");
      v && std::holds_alternative<double>(*v)) {
    out.quality_score = std::get<double>(*v);
  }

  if (auto v = ctx->get(frame_fid, "disc_quality", "dropout_count");
      v && std::holds_alternative<int32_t>(*v)) {
    out.dropout_count = std::get<int32_t>(*v);
  }

  return out;
}

}  // namespace orc::presenters
