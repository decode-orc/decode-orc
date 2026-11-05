/*
 * File:        picture_truth.h
 * Module:      orc-videosynth-support
 * Purpose:     The component pictures of a still: videosynth's picture truth
 *              (.truth with its .truth.meta sidecar) and a video sink's raw
 *              YUV444P16 decode, both on the 4fsc active-window grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/common_types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "decoded_comparison.h"

namespace orc_videosynth {

// One frame's Y, U and V on the 4fsc grid, row-major, |width| samples by
// |height| rows. Y is 0 at black and 1 at white. U and V are the peak
// subcarrier amplitude of each colour-difference component on the same
// scale (U = 0.493 (B'-Y'), V = 0.877 (R'-Y')), with the PAL V-switch
// removed.
struct ComponentPicture {
  size_t width = 0;
  size_t height = 0;
  std::vector<double> y;
  std::vector<double> u;
  std::vector<double> v;

  ComponentPicture() = default;
  ComponentPicture(size_t w, size_t h)
      : width(w), height(h), y(w * h, 0.0), u(w * h, 0.0), v(w * h, 0.0) {}

  size_t index(size_t x, size_t row) const { return (row * width) + x; }
};

// The .truth.meta row (videosynth docs/reference/output.md, "truth").
struct TruthGeometry {
  orc::VideoSystem system = orc::VideoSystem::PAL;
  double sample_rate_hz = 0.0;
  size_t frame_count = 0;
  size_t samples_per_line = 0;
  size_t rows = 0;
  int first_sample = 0;  // first truth sample, in samples after 0H
  int first_line_field1 = 0;
  int first_line_field2 = 0;
  int upper_field = 1;  // the field supplying the even rows
  int half_picture_line = 0;
  int half_picture_end_sample = 0;  // sample after 0H; 0 when unused
  double black_mv = 0.0;
  double white_mv = 0.0;
};

// The .truth.meta row as stored: the geometry and the format columns this
// reader checks.
struct TruthMeta {
  std::string video_standard;
  TruthGeometry geometry;  // every field but |system|
  std::string plane_order;
  std::string sample_format;
  std::string units;
  std::string y_reference;
  std::string u_axis;
  std::string v_axis;
  std::string v_switch;
};

// |meta|'s geometry; nullopt, with |error| set, when it states an unknown
// standard, a format this reader does not know, an empty picture or no
// level range.
std::optional<TruthGeometry> truth_geometry(const TruthMeta& meta,
                                            std::string* error = nullptr);

// A videosynth .truth file: per frame, Y, U and V planes of float32 mV.
class TruthFile {
 public:
  // Reads <base>.truth and <base>.truth.meta; nullopt, with |error| set,
  // when either cannot be read, the sidecar states another format, or the
  // file's size does not match its frames.
  static std::optional<TruthFile> read(const std::filesystem::path& base,
                                       std::string* error = nullptr);

  // The truth from |geometry| and the .truth file's bytes (little-endian
  // float32); nullopt, with |error| set, when their size does not match
  // the geometry's frames.
  static std::optional<TruthFile> from_bytes(const TruthGeometry& geometry,
                                             const std::vector<uint8_t>& raw,
                                             std::string* error = nullptr);

  // From a geometry and samples already in memory, in the file's order.
  TruthFile(TruthGeometry geometry, std::vector<float> samples);

  const TruthGeometry& geometry() const { return geometry_; }

  // Frame |frame| scaled to ComponentPicture units by the sidecar's black
  // and white levels.
  ComponentPicture frame(size_t frame) const;

  // One byte per truth sample, non-zero where the frame carries picture:
  // everything but the tail of the PAL half-picture line.
  std::vector<uint8_t> picture_mask() const;

 private:
  TruthGeometry geometry_;
  std::vector<float> samples_;
};

// Where a video sink's raw decode sits on the 4fsc grid: its first column's
// sample after 0H, and the truth row its first row carries.
struct DecodeOrigin {
  int first_sample = 0;
  int first_row = 0;
};

// The decode's origin for |system|, from the cvbs_source active window that
// the sink crops to (and that videosynth's .meta states).
DecodeOrigin decode_origin(orc::VideoSystem system);

// Frame |frame| of |decoded| (|decoded_width| samples a row) placed on
// |geometry|'s truth grid, in ComponentPicture units. Truth samples the
// decode does not cover are zero, and are cleared in |covered| when given.
ComponentPicture decoded_picture(const DecodedFile& decoded, size_t frame,
                                 size_t decoded_width,
                                 const DecodeOrigin& origin,
                                 const TruthGeometry& geometry,
                                 std::vector<uint8_t>* covered = nullptr);

}  // namespace orc_videosynth
