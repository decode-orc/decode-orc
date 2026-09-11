/*
 * File:        carrier_scope_extraction.cpp
 * Module:      orc-core
 * Purpose:     Vectorscope and histogram payloads from a colour frame carrier
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "include/carrier_scope_extraction.h"

#include <orc/stage/preview/colour_preview_provider.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "analysis/vectorscope/vectorscope_analysis.h"

namespace orc {

std::optional<VectorscopeData> extract_vectorscope_from_carrier(
    const ColourFrameCarrier& carrier, const PreviewScopeSelection& selection,
    std::uint64_t fallback_field_number) {
  std::optional<VectorscopeData> data;

  // The stage already extracted the active picture on the way past, so the
  // default request is answered from that rather than walked again. Any
  // narrower selection has to be taken from the planes.
  if (selection.isWholeActivePicture() &&
      carrier.vectorscope_data.has_value()) {
    data = carrier.vectorscope_data;
    // The stage reports the extent it sampled but not where it started;
    // for the active picture that is the carrier's own active line range.
    data->first_line = carrier.active_y_start;
    data->last_line = (carrier.active_y_end > carrier.active_y_start)
                          ? (carrier.active_y_end - 1)
                          : carrier.active_y_start;
  } else {
    const std::uint64_t field_number =
        carrier.vectorscope_data.has_value()
            ? carrier.vectorscope_data->field_number
            : fallback_field_number;

    data = VectorscopeAnalysisTool::extractFromColourFrameCarrier(
        carrier, field_number, 4, selection.active_area_only,
        selection.first_line, selection.last_line);
  }

  if (!data.has_value() || data->samples.empty()) {
    return std::nullopt;
  }

  data->acquisition_mode = VectorscopeAcquisitionMode::DecodedComponent;
  data->system = carrier.system;
  data->cvbs_white = static_cast<std::int32_t>(carrier.cvbs_white);
  data->cvbs_blanking = static_cast<std::int32_t>(carrier.cvbs_blanking);
  return data;
}

VideoHistogramData extract_histogram_from_carrier(
    const ColourFrameCarrier& carrier, std::uint64_t field_number) {
  VideoHistogramData data;
  data.field_number = field_number;
  data.system = carrier.system;
  data.cvbs_blanking = carrier.cvbs_blanking;
  data.cvbs_black = carrier.cvbs_black;
  data.cvbs_white = carrier.cvbs_white;
  data.width = carrier.width;
  data.height = carrier.height;

  if (!carrier.is_valid()) {
    return data;
  }

  const double range = std::max(1.0, carrier.cvbs_white - carrier.cvbs_black);
  const double uv_range =
      std::max(1.0, carrier.cvbs_white - carrier.cvbs_blanking);

  // Pre-compute NTSC IQ rotation constants.
  // SMPTE 170M-2004 §7.3: I is at 33° from V (R-Y), Q at 33° from U (B-Y).
  // In normalised (U,V) space: I = −U·sin33° + V·cos33°,
  //                             Q =  U·cos33° + V·sin33°.
  constexpr double kSin33 = 0.5446390350;
  constexpr double kCos33 = 0.8386705679;

  const bool is_ntsc = (carrier.system == VideoSystem::NTSC);

  const double bin_range =
      VideoHistogramData::kRangeMax - VideoHistogramData::kRangeMin;
  const double bins_per_percent =
      static_cast<double>(VideoHistogramData::kBinCount) / bin_range;

  const double chroma_bin_range =
      VideoHistogramData::kChromaRangeMax - VideoHistogramData::kChromaRangeMin;
  const double chroma_bins_per_percent =
      static_cast<double>(VideoHistogramData::kBinCount) / chroma_bin_range;

  auto to_bin = [&](double percent) -> std::optional<size_t> {
    const double offset = percent - VideoHistogramData::kRangeMin;
    const int bin = static_cast<int>(offset * bins_per_percent);
    if (bin < 0 || bin >= static_cast<int>(VideoHistogramData::kBinCount)) {
      return std::nullopt;
    }
    return static_cast<size_t>(bin);
  };

  auto to_chroma_bin = [&](double percent) -> std::optional<size_t> {
    const double offset = percent - VideoHistogramData::kChromaRangeMin;
    const int bin = static_cast<int>(offset * chroma_bins_per_percent);
    if (bin < 0 || bin >= static_cast<int>(VideoHistogramData::kBinCount)) {
      return std::nullopt;
    }
    return static_cast<size_t>(bin);
  };

  // The chroma sink always sets active_area_cropping_applied = true, which
  // causes decoders to remap active-picture data to 0-based indices within
  // the full-sized ComponentFrame buffer.  plane[0] is therefore the first
  // active pixel.  active_x/y_start hold the original pre-crop signal
  // coordinates and must NOT be used as absolute plane indices — only the
  // difference (active width / height) gives the correct iteration bounds.
  const uint32_t active_width =
      (carrier.active_x_end > carrier.active_x_start)
          ? (carrier.active_x_end - carrier.active_x_start)
          : carrier.width;
  const uint32_t active_height =
      (carrier.active_y_end > carrier.active_y_start)
          ? (carrier.active_y_end - carrier.active_y_start)
          : carrier.height;

  uint32_t pixel_count = 0;

  for (uint32_t row = 0; row < active_height; ++row) {
    for (uint32_t col = 0; col < active_width; ++col) {
      const size_t i = static_cast<size_t>(row) * carrier.width + col;

      // Y: normalised so that cvbs_black = 0 %, cvbs_white = 100 %.
      const double y_norm = (carrier.y_plane[i] - carrier.cvbs_black) / range;
      if (auto b = to_bin(y_norm * 100.0)) {
        data.y_bins[*b]++;
      }

      // U/V: bipolar, centred at 0 % (neutral chroma), binned over
      // [kChromaRangeMin, kChromaRangeMax] so the full swing is visible.
      const double u_norm = carrier.u_plane[i] / uv_range;
      const double v_norm = carrier.v_plane[i] / uv_range;

      if (auto b = to_chroma_bin(u_norm * 100.0)) {
        data.u_bins[*b]++;
      }
      if (auto b = to_chroma_bin(v_norm * 100.0)) {
        data.v_bins[*b]++;
      }

      // I/Q: SMPTE 170M rotation of (U, V), NTSC only.
      if (is_ntsc) {
        const double i_norm = (-u_norm * kSin33) + (v_norm * kCos33);
        const double q_norm = (u_norm * kCos33) + (v_norm * kSin33);
        if (auto b = to_chroma_bin(i_norm * 100.0)) {
          data.i_bins[*b]++;
        }
        if (auto b = to_chroma_bin(q_norm * 100.0)) {
          data.q_bins[*b]++;
        }
      }

      ++pixel_count;
    }
  }

  data.total_pixels = pixel_count;
  return data;
}
}  // namespace orc
