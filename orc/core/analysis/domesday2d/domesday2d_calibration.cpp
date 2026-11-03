/*
 * File:        domesday2d_calibration.cpp
 * Module:      analysis
 * Purpose:     Measures a PAL source's line-alternating chroma matrix, the
 *              per-source correction the Domesday2D decoder applies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "domesday2d_calibration.h"

#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>

#include <algorithm>

namespace orc {

namespace {

// Field 2's first line in a PAL frame-flat buffer.
constexpr size_t kField2FirstLine = 313;

}  // namespace

Domesday2DCalibration calibrate_domesday2d(
    const VideoFrameRepresentation& source,
    const Domesday2DCalibrationOptions& options,
    const Domesday2DCalibrationProgress& progress) {
  Domesday2DCalibration result;
  const auto params = source.get_video_parameters();
  if (!params || params->system != VideoSystem::PAL ||
      source.has_separate_channels() ||
      params->white_level <= params->blanking_level) {
    return result;
  }
  auto report = [&](int percent, const std::string& status) {
    return !progress || progress(percent, status);
  };

  const uint64_t count = source.frame_count();
  const FrameID first = source.frame_range().first;
  const size_t reads =
      static_cast<size_t>(std::min<uint64_t>(options.frames, count));
  domesday2d::FlatPairs pairs;
  domesday2d::Plane raster;
  domesday2d::Plane u;
  domesday2d::Plane v;
  for (size_t k = 0; k < reads; ++k) {
    if (!report(static_cast<int>(100 * k / std::max<size_t>(reads, 1)),
                "Finding flat colour")) {
      return {};
    }
    const FrameID id = first + ((k * count) / reads);
    const auto desc = source.get_frame_descriptor(id);
    // A copy: a source's frame pointer is not held across the frame's work.
    const auto frame = source.get_frame_copy(id);
    if (!desc || desc->system != VideoSystem::PAL ||
        desc->samples_per_line_nominal < domesday2d::kLine ||
        frame.size() < desc->samples_total ||
        frame_line_sample_offset(VideoSystem::PAL,
                                 desc->samples_per_line_nominal,
                                 kField2FirstLine + 312) > frame.size()) {
      continue;
    }
    domesday2d::fill_raster(
        [&](size_t field, size_t line) {
          const size_t flat = (field == 0 ? 0 : kField2FirstLine) + line;
          return frame.data() +
                 frame_line_sample_offset(VideoSystem::PAL,
                                          desc->samples_per_line_nominal, flat);
        },
        params->blanking_level, params->white_level, raster);
    const auto lattice = domesday2d::fit_lattice(raster);
    domesday2d::demodulate_filtered(raster, lattice, 0.0, u, v);
    domesday2d::add_flat_pairs(u, v, lattice, pairs);
    ++result.frames_read;
  }
  report(100, "Done");
  result.pairs = pairs.plus.size();
  result.matrix =
      domesday2d::fit_matrix(pairs, options.minimum_pairs, &result.pairs_used);
  ORC_LOG_DEBUG("Domesday2D calibration: {} frames, {} pairs, {} used",
                result.frames_read, result.pairs, result.pairs_used);
  return result;
}

}  // namespace orc
