/*
 * File:        weave_decision.h
 * Module:      orc-core
 * Purpose:     The video sink's per-frame choice between weaving a frame's
 *              two fields and deinterlacing them (deinterlace_mode)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace orc::chroma_sink {

// deinterlace_mode values, read only while apply_deinterlace is on: bwdif on
// every frame, or only on the frames whose fields are not one instant.
inline constexpr const char kDeinterlaceAll[] = "all";
inline constexpr const char kDeinterlaceMotionOnly[] = "motion_only";

enum class DeinterlaceMode { All, MotionOnly };

inline std::optional<DeinterlaceMode> parse_deinterlace_mode(
    const std::string& value) {
  if (value == kDeinterlaceAll) return DeinterlaceMode::All;
  if (value == kDeinterlaceMotionOnly) return DeinterlaceMode::MotionOnly;
  return std::nullopt;
}

// The deinterlacing filter the FFmpeg backend puts ahead of any user chain.
// "all" is the bare bwdif apply_deinterlace has always written (send_field:
// one output frame per field). In "motion_only" bwdif deinterlaces only the
// frames the backend flags as interlaced and passes the rest through
// bit-identical, but as one frame lasting two field periods; fps at the field
// rate repeats it, so the output keeps a constant field rate and two output
// frames per input frame.
inline std::string deinterlace_filter(DeinterlaceMode mode,
                                      VideoSystem system) {
  if (mode == DeinterlaceMode::All) return "bwdif";
  const bool field_rate_60 =
      system == VideoSystem::NTSC || system == VideoSystem::PAL_M;
  return std::string("bwdif=deint=interlaced,fps=") +
         (field_rate_60 ? "60000/1001" : "50");
}

// A frame's comb score (see comb_score() below): over the whole picture, and
// in its second-worst block, which finds motion confined to a small part of
// the picture (an animated graphic over a still) that the whole-picture
// score averages away. The second-worst rather than the worst block, so that
// one block holding a defect on one line (an unflagged dropout) is not taken
// for motion.
struct CombScore {
  double whole = 0.0;
  double local = 0.0;
};

// The scores below which a frame's fields are taken to be one instant,
// measured on Community South decodes, stacked and single (implementation
// plan, phase 2): maps, photographs and text pages score up to 0.69 whole and
// 1.04 in the second-worst block; interlaced motion 0.71 and more whole with
// 1.7 and more in a block, and an animated graphic over a still, which the
// whole-picture score misses (0.38-0.58), 1.37 and more in its second-worst
// block.
inline constexpr double kWeaveCombThreshold = 0.75;
inline constexpr double kWeaveLocalCombThreshold = 1.2;

// Whether a frame is woven: its picture is held (the Stacker's held hint), or
// its own fields are one instant by their comb score, whole and local.
inline bool weave_frame(bool held, const CombScore& score) {
  return held || (score.whole < kWeaveCombThreshold &&
                  score.local < kWeaveLocalCombThreshold);
}

// The blocks the local score is taken over, in frame lines by samples, and
// the fewest edge pixels a block needs to be scored.
inline constexpr int32_t kCombBlockLines = 32;
inline constexpr int32_t kCombBlockSamples = 64;
inline constexpr size_t kCombBlockMinEdges = 64;

namespace detail {

// The value at fraction q of the sorted values, interpolated linearly
// between neighbours as numpy's percentile does. Reorders values.
inline float quantile(std::vector<float>& values, double q) {
  const double position = q * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<size_t>(position);
  const auto lower_it = values.begin() + static_cast<std::ptrdiff_t>(lower);
  std::nth_element(values.begin(), lower_it, values.end());
  const float low = *lower_it;
  if (lower + 1 >= values.size()) return low;
  const float high = *std::min_element(lower_it + 1, values.end());
  return low + static_cast<float>(position - static_cast<double>(lower)) *
                   (high - low);
}

}  // namespace detail

// How much better a frame's lines are predicted from their own field than
// from the other field: the inter-field prediction error (the mean of the
// lines above and below, which are the other field's) over the intra-field
// one (the mean of the lines two above and two below), summed over the edge
// pixels, the top fifth by vertical plus horizontal gradient. On one instant
// the other field's lines are half as far away and predict better, so the
// score is low; between instants they carry the motion and it rises above 1.
//
// The luma is first low-passed along the line with a Gaussian of sigma 2
// samples (-43 dB at the subcarrier at 4fsc, PAL or NTSC), so that chroma
// residue, whose phase differs between the fields, cannot read as motion.
// The intra-field error is floored at twice the noise (1.4826 times its
// median, a robust sigma), so flat picture with no vertical detail reads as
// still rather than as noise against noise.
//
// plane holds frame lines (the two fields interleaved) of stride samples;
// the score covers samples [x_start, x_end) of lines [line_start, line_end).
// The local score is the same ratio within each kCombBlockLines by
// kCombBlockSamples block holding at least kCombBlockMinEdges edge pixels
// (the edges and the noise floor are the whole picture's); zero with fewer
// than two such blocks.
// After dp111/domesday-aiv-restore newdecode/combscore.py comb_score(),
// whose block form the local score follows.
inline CombScore comb_score(const double* plane, int32_t stride,
                            int32_t x_start, int32_t x_end, int32_t line_start,
                            int32_t line_end) {
  const int32_t width = x_end - x_start;
  const int32_t height = line_end - line_start;
  if (width < 3 || height < 5) return {};

  // scipy's gaussian_filter1d kernel: truncated at 4 sigma, reflected at the
  // edges (the edge sample repeats).
  constexpr double kSigma = 2.0;
  constexpr int32_t kRadius = 8;
  std::array<double, (2 * kRadius) + 1> kernel{};
  double kernel_sum = 0.0;
  for (int32_t k = -kRadius; k <= kRadius; ++k) {
    const double weight = std::exp(-0.5 * k * k / (kSigma * kSigma));
    kernel[static_cast<size_t>(k + kRadius)] = weight;
    kernel_sum += weight;
  }
  for (double& weight : kernel) weight /= kernel_sum;
  const auto reflect = [width](int32_t x) {
    while (x < 0 || x >= width) {
      x = x < 0 ? -x - 1 : (2 * width) - x - 1;
    }
    return x;
  };

  const auto w = static_cast<size_t>(width);
  std::vector<float> smooth(w * static_cast<size_t>(height));
  for (int32_t line = 0; line < height; ++line) {
    const double* row =
        plane + (static_cast<ptrdiff_t>(line_start + line) * stride) + x_start;
    float* out = smooth.data() + (static_cast<size_t>(line) * w);
    for (int32_t x = 0; x < width; ++x) {
      double sum = 0.0;
      for (int32_t k = -kRadius; k <= kRadius; ++k) {
        sum += kernel[static_cast<size_t>(k + kRadius)] * row[reflect(x + k)];
      }
      out[x] = static_cast<float>(sum);
    }
  }

  const size_t count = static_cast<size_t>(height - 4) * w;
  std::vector<float> inter(count);
  std::vector<float> intra(count);
  std::vector<float> gradient(count);
  for (int32_t line = 2; line < height - 2; ++line) {
    const auto at = [&](int32_t l) {
      return smooth.data() + (static_cast<size_t>(l) * w);
    };
    const float* c = at(line);
    const float* above = at(line - 1);
    const float* below = at(line + 1);
    const float* above2 = at(line - 2);
    const float* below2 = at(line + 2);
    const size_t base = static_cast<size_t>(line - 2) * w;
    for (int32_t x = 0; x < width; ++x) {
      const size_t i = base + static_cast<size_t>(x);
      inter[i] = std::abs(c[x] - (0.5F * (above[x] + below[x])));
      intra[i] = std::abs(c[x] - (0.5F * (above2[x] + below2[x])));
      // numpy's gradient: central differences, one-sided at the ends.
      float horizontal = 0.0F;
      if (x == 0) {
        horizontal = c[1] - c[0];
      } else if (x == width - 1) {
        horizontal = c[x] - c[x - 1];
      } else {
        horizontal = 0.5F * (c[x + 1] - c[x - 1]);
      }
      gradient[i] = std::abs(below[x] - above[x]) + std::abs(horizontal);
    }
  }

  std::vector<float> scratch = intra;
  const float noise_floor = 2.0F * 1.4826F * detail::quantile(scratch, 0.5);
  scratch = gradient;
  const float edge_threshold = detail::quantile(scratch, 0.8);

  // Sums over the edge pixels, of the whole picture and of each block.
  const int32_t rows = height - 4;
  const int32_t block_rows = rows / kCombBlockLines;
  const int32_t block_columns = width / kCombBlockSamples;
  const auto blocks =
      static_cast<size_t>(block_rows) * static_cast<size_t>(block_columns);
  std::vector<double> block_inter(blocks, 0.0);
  std::vector<double> block_intra(blocks, 0.0);
  std::vector<size_t> block_edges(blocks, 0);
  double inter_sum = 0.0;
  double intra_sum = 0.0;
  for (int32_t row = 0; row < rows; ++row) {
    const int32_t block_row = row / kCombBlockLines;
    for (int32_t x = 0; x < width; ++x) {
      const size_t i = (static_cast<size_t>(row) * w) + static_cast<size_t>(x);
      if (gradient[i] <= edge_threshold) continue;
      const double floored = std::max(intra[i], noise_floor);
      inter_sum += inter[i];
      intra_sum += floored;
      const int32_t block_column = x / kCombBlockSamples;
      if (block_row >= block_rows || block_column >= block_columns) continue;
      const size_t b = (static_cast<size_t>(block_row) *
                        static_cast<size_t>(block_columns)) +
                       static_cast<size_t>(block_column);
      block_inter[b] += inter[i];
      block_intra[b] += floored;
      ++block_edges[b];
    }
  }

  CombScore score;
  if (intra_sum > 0.0) score.whole = inter_sum / intra_sum;
  double worst = 0.0;
  double second = 0.0;
  size_t scored = 0;
  for (size_t b = 0; b < blocks; ++b) {
    if (block_edges[b] < kCombBlockMinEdges || block_intra[b] <= 0.0) {
      continue;
    }
    const double ratio = block_inter[b] / block_intra[b];
    ++scored;
    if (ratio > worst) {
      second = worst;
      worst = ratio;
    } else if (ratio > second) {
      second = ratio;
    }
  }
  if (scored >= 2) score.local = second;
  return score;
}

}  // namespace orc::chroma_sink
