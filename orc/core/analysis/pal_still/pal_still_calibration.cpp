/*
 * File:        pal_still_calibration.cpp
 * Module:      analysis
 * Purpose:     Measures a PAL source's line-alternating chroma matrix, the
 *              per-source correction the PAL Still decoder applies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "pal_still_calibration.h"

#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>

#include <algorithm>

namespace orc {

namespace {

// Field 2's first line in a PAL frame-flat buffer.
constexpr size_t kField2FirstLine = 313;

}  // namespace

PalStillCalibration calibrate_pal_still(
    const VideoFrameRepresentation& source,
    const PalStillCalibrationOptions& options,
    const PalStillCalibrationProgress& progress) {
  PalStillCalibration result;
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
  pal_still::FlatPairs pairs;
  pal_still::Plane raster;
  pal_still::Plane u;
  pal_still::Plane v;
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
        desc->samples_per_line_nominal < pal_still::kLine ||
        frame.size() < desc->samples_total ||
        frame_line_sample_offset(VideoSystem::PAL,
                                 desc->samples_per_line_nominal,
                                 kField2FirstLine + 312) > frame.size()) {
      continue;
    }
    pal_still::fill_raster(
        [&](size_t field, size_t line) {
          const size_t flat = (field == 0 ? 0 : kField2FirstLine) + line;
          return frame.data() +
                 frame_line_sample_offset(VideoSystem::PAL,
                                          desc->samples_per_line_nominal, flat);
        },
        params->blanking_level, params->white_level, raster);
    const auto lattice = pal_still::fit_lattice(raster);
    pal_still::demodulate_filtered(raster, lattice, 0.0, u, v);
    pal_still::add_flat_pairs(u, v, lattice, pairs);
    ++result.frames_read;
  }
  report(100, "Done");
  result.pairs = pairs.plus.size();
  result.matrix =
      pal_still::fit_matrix(pairs, options.minimum_pairs, &result.pairs_used);
  ORC_LOG_DEBUG("PAL Still calibration: {} frames, {} pairs, {} used",
                result.frames_read, result.pairs, result.pairs_used);
  return result;
}

}  // namespace orc
