/*
 * File:        frame_quality_score.cpp
 * Module:      orc-core/analysis
 * Purpose:     Frame signal-quality scoring implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_quality_score.h"

#include <orc/stage/field_id.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <variant>

namespace orc {
namespace {

// Read a double-valued observation, ignoring absent and wrongly-typed entries.
std::optional<double> read_double(const IObservationContext& context,
                                  FieldID field_id,
                                  const std::string& namespace_,
                                  const std::string& key) {
  auto value = context.get(field_id, namespace_, key);
  if (!value || !std::holds_alternative<double>(*value)) {
    return std::nullopt;
  }
  return std::get<double>(*value);
}

// Map a decibel reading onto 0-100 across the [floor, ceiling] window.
double snr_sub_score(double snr_db) {
  const double normalised =
      (snr_db - kSnrFloorDb) / (kSnrCeilingDb - kSnrFloorDb);
  return 100.0 * std::clamp(normalised, 0.0, 1.0);
}

// Map a measured burst peak onto 0-100 by its fractional deviation from
// nominal. A reading at nominal scores 100; one at zero or at twice nominal
// scores 0.
double burst_sub_score(double measured_10bit, double nominal_10bit) {
  const double deviation =
      std::abs(measured_10bit - nominal_10bit) / nominal_10bit;
  return 100.0 * std::clamp(1.0 - deviation, 0.0, 1.0);
}

}  // namespace

std::optional<double> nominal_burst_peak_10bit(VideoSystem system,
                                               int32_t blanking_level,
                                               int32_t white_level) {
  if (blanking_level < 0 || white_level <= blanking_level) {
    return std::nullopt;
  }

  double peak_ire = kNtscBurstPeakIre;
  switch (system) {
    case VideoSystem::PAL:
      peak_ire = kPalBurstPeakIre;
      break;
    case VideoSystem::PAL_M:
      peak_ire = kPalMBurstPeakIre;
      break;
    case VideoSystem::NTSC:
      peak_ire = kNtscBurstPeakIre;
      break;
    default:
      return std::nullopt;
  }

  // 100 IRE spans (white - blanking) sample units in the CVBS_U10_4FSC domain.
  const double units_per_ire =
      static_cast<double>(white_level - blanking_level) / 100.0;
  return peak_ire * units_per_ire;
}

FrameQualityMetrics read_frame_quality_metrics(
    const IObservationContext& context, FrameID frame_id) {
  // The quality observers publish one value per frame, keyed on the frame's
  // first FieldID (FieldID = FrameID * 2 + field_index).
  const FieldID frame_field_id(frame_id * 2);

  FrameQualityMetrics metrics;
  metrics.median_burst_10bit =
      read_double(context, frame_field_id, "burst_level", "median_burst_10bit");
  metrics.white_snr_db =
      read_double(context, frame_field_id, "white_snr", "snr_db");
  metrics.black_psnr_db =
      read_double(context, frame_field_id, "black_psnr", "psnr_db");
  return metrics;
}

std::optional<double> dropout_fraction(const std::vector<DropoutRun>& runs,
                                       size_t frame_samples) {
  if (frame_samples == 0) return std::nullopt;
  uint64_t flagged = 0;
  for (const auto& run : runs) flagged += run.sample_count;
  return std::min(
      1.0, static_cast<double>(flagged) / static_cast<double>(frame_samples));
}

double compute_frame_quality_score(const FrameQualityMetrics& metrics,
                                   std::optional<double> nominal_burst_10bit) {
  double weighted_sum = 0.0;
  double total_weight = 0.0;

  if (metrics.median_burst_10bit && nominal_burst_10bit &&
      *nominal_burst_10bit > 0.0) {
    weighted_sum +=
        kBurstQualityWeight *
        burst_sub_score(*metrics.median_burst_10bit, *nominal_burst_10bit);
    total_weight += kBurstQualityWeight;
  }

  if (metrics.white_snr_db) {
    weighted_sum +=
        kWhiteSnrQualityWeight * snr_sub_score(*metrics.white_snr_db);
    total_weight += kWhiteSnrQualityWeight;
  }

  if (metrics.black_psnr_db) {
    weighted_sum +=
        kBlackPsnrQualityWeight * snr_sub_score(*metrics.black_psnr_db);
    total_weight += kBlackPsnrQualityWeight;
  }

  if (metrics.dropout_fraction) {
    const double sub_score =
        100.0 * (1.0 - std::min(1.0, *metrics.dropout_fraction /
                                         kDropoutFractionCeiling));
    weighted_sum += kDropoutQualityWeight * sub_score;
    total_weight += kDropoutQualityWeight;
  }

  if (total_weight <= 0.0) {
    return kNeutralFrameQualityScore;
  }

  return weighted_sum / total_weight;
}

}  // namespace orc
