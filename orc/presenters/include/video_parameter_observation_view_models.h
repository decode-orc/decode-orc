/*
 * File:        video_parameter_observation_view_models.h
 * Module:      orc-presenters
 * Purpose:     View models for the video parameter observer dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <optional>

#include "hints_view_models.h"

namespace orc::presenters {

/**
 * @brief All observer-derived values for a single field, displayed by
 * VideoParameterObserverDialog.
 *
 * Each member is optional: absent means the corresponding observer did not
 * fire for this field (e.g. wrong video system, no burst present).
 */
struct VideoParameterObservationView {
  // Signal parameters from get_video_parameters() — the base signal geometry.
  std::optional<VideoParametersView> video_params;

  // Per-field colour-sequence phase ID (ColourFramePhaseObserver).
  // PAL/PAL_M: 1-8; NTSC: 1-4; -1 when absent or unclassifiable.
  std::optional<int32_t> field_phase_id;

  // Colour-frame sequence index derived from field_phase_id
  // (ColourFramePhaseObserver). -1 when absent or unclassifiable.
  std::optional<int32_t> colour_frame_index;

  // Median burst amplitude in 10-bit ADU (BurstLevelObserver).
  std::optional<double> burst_level_10bit;

  // Luminance signal-to-noise ratio in dB (WhiteSNRObserver).
  std::optional<double> white_snr_db;

  // Black-level peak signal-to-noise ratio in dB (BlackPSNRObserver).
  std::optional<double> black_psnr_db;

  // Composite disc quality score 0.0–1.0 for the frame (FrameQualityObserver).
  std::optional<double> quality_score;

  // Dropout region count for the frame (FrameQualityObserver).
  std::optional<int32_t> dropout_count;
};

}  // namespace orc::presenters
