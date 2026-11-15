/*
 * File:        pal_output_raster.h
 * Module:      orc-core
 * Purpose:     Puts PAL components decoded on the 4fsc lattice onto an
 *              orthogonal raster: each decoded line moves by its own 0H offset
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_CHROMA_SINK_PAL_OUTPUT_RASTER_H
#define ORC_CORE_CHROMA_SINK_PAL_OUTPUT_RASTER_H

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/orc_source_parameters.h>
#include <orc/support/pal_line_locked_resample.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "decoders/componentframe.h"

namespace orc::chroma_sink {

// The PAL decoders take the CVBS_U10_4FSC lattice as it is stored: the only
// PAL grid the specification calls phase locked (CVBS File Format
// Specification v1.8.1, signal-state-presets, "Phase locked";
// video-standard-presets, Preset PAL, "non-orthogonal 4fsc sampling
// structure"). On it 0H moves 4/625 sample later on each row, so the decoded
// picture leans by up to 2 samples through a field. That lean belongs to the
// raster, not to the decode, so it is taken out of the decoded components:
// output sample x of a row is the decoded row evaluated at x + d, where d is
// the row's 0H offset (pal_line_locked::row_0h_offset). Every row then has 0H
// where field 1 row 0 has it.
//
// The decoders compare a row only with the same row in other frames, where d
// is the same, so they need no change; the shift runs on each decoded frame
// before anything that treats it as a picture (the weave decision, the noise
// filter, the output writers and the preview).
//
// Reach: the kernel reads kKernelLeft (7) samples before the position and
// 8 after, and d is under 2, so the decoders fill kDecodeMarginLeft samples
// before the output window and kDecodeMarginRight after it. Both are
// multiples of the 8-sample Transform PAL tile hop, so widening the window
// adds whole tiles at its edges and leaves the tiles under the output window
// where they were.
class PalOutputRaster {
 public:
  static constexpr int32_t kDecodeMarginLeft = 8;
  static constexpr int32_t kDecodeMarginRight = 16;
  static_assert(kDecodeMarginLeft >= pal_line_locked::detail::kKernelLeft);
  static_assert(kDecodeMarginRight >= 2 + pal_line_locked::detail::kKernelTaps -
                                          pal_line_locked::detail::kKernelLeft);

  // Where the decoded samples sit in a ComponentFrame and where the output
  // goes. Frame index i of a row is shifted when out_begin <= i < out_end;
  // it takes the decoded row at i + source_offset + d, whose samples are at
  // frame indices [valid_begin, valid_end). Frame line l is absolute frame
  // line l + line_offset (absolute line 2 × row + 0 for field 1, + 1 for
  // field 2).
  struct Geometry {
    int32_t line_offset = 0;
    int32_t line_begin = 0;
    int32_t line_end = 0;
    int32_t out_begin = 0;
    int32_t out_end = 0;
    int32_t source_offset = 0;
    int32_t valid_begin = 0;
    int32_t valid_end = 0;
  };

  // Whether a source of |system| is stored on the non-orthogonal lattice.
  // NTSC and PAL_M are orthogonal and are left as decoded.
  static bool applies_to(VideoSystem system) {
    return system == VideoSystem::PAL;
  }

  // The decode window for |output|: its active window widened by the
  // shift's reach. The caller clamps it to what the decoders accept; the
  // geometry below uses whatever window they were given.
  static SourceParameters decode_window(const SourceParameters& output) {
    SourceParameters decode = output;
    if (!applies_to(output.system)) return decode;
    decode.active_video_start -= kDecodeMarginLeft;
    decode.active_video_end += kDecodeMarginRight;
    return decode;
  }

  // Geometry for frames decoded with |decode| that the rest of the sink reads
  // with |output|. Both follow the decoders' active_area_cropping_applied
  // convention: a cropped frame is indexed from the window's first sample
  // and first line, an uncropped one in frame-flat coordinates.
  static Geometry geometry_for(const SourceParameters& output,
                               const SourceParameters& decode) {
    Geometry g;
    const int32_t lines =
        output.last_active_frame_line - output.first_active_frame_line;
    if (output.active_area_cropping_applied) {
      g.line_offset = output.first_active_frame_line;
      g.line_begin = 0;
      g.line_end = lines;
      g.out_begin = 0;
      g.out_end = output.active_video_end - output.active_video_start;
      g.source_offset = output.active_video_start - decode.active_video_start;
      g.valid_begin = 0;
      g.valid_end = decode.active_video_end - decode.active_video_start;
    } else {
      g.line_offset = 0;
      g.line_begin = output.first_active_frame_line;
      g.line_end = output.last_active_frame_line;
      g.out_begin = output.active_video_start;
      g.out_end = output.active_video_end;
      g.source_offset = 0;
      g.valid_begin = decode.active_video_start;
      g.valid_end = decode.active_video_end;
    }
    return g;
  }

  explicit PalOutputRaster(const Geometry& geometry) : geometry_(geometry) {}

  const Geometry& geometry() const { return geometry_; }

  // Shifts every row of |frame|'s Y, U and V planes in the geometry's line
  // range (planes a mono decode left empty are skipped), and clears the
  // decode margins outside the output window so the frame holds what an
  // unwidened decode would. Rows outside the 625-line frame are left alone.
  //
  // O(lines × width × taps). Thread-safe: const, with per-call scratch, so
  // the export's workers each shift their own frames.
  void apply(ComponentFrame& frame) const {
    const int32_t width = frame.getWidth();
    if (width <= 0) return;
    const Geometry& g = geometry_;
    Span span;
    span.valid_begin = std::max(g.valid_begin, 0);
    span.valid_end = std::min(g.valid_end, width);
    span.out_begin = std::max(g.out_begin, 0);
    span.out_end = std::min(g.out_end, width);
    span.source_offset = g.source_offset;
    if (span.valid_end <= span.valid_begin || span.out_end <= span.out_begin) {
      return;
    }
    assert(span.out_begin + g.source_offset >= span.valid_begin);
    assert(span.out_end + g.source_offset <= span.valid_end);

    std::vector<float> scratch(
        static_cast<size_t>(span.valid_end - span.valid_begin + 2 * kPad));
    const auto shift_plane = [&](std::vector<double>* plane, auto& taps) {
      const int32_t height =
          static_cast<int32_t>(plane->size() / static_cast<size_t>(width));
      const int32_t line_end = std::min(g.line_end, height);
      for (int32_t line = std::max(g.line_begin, 0); line < line_end; ++line) {
        const int32_t absolute = line + g.line_offset;
        if (absolute < 0 || absolute >= kPalFrameLines) continue;
        shift_row(plane->data() + static_cast<size_t>(line) * width,
                  taps[static_cast<size_t>(absolute)], span, scratch);
      }
    };
    shift_plane(frame.getY(), luma_taps());
    shift_plane(frame.getU(), chroma_taps());
    shift_plane(frame.getV(), chroma_taps());
  }

 private:
  // Luma: the 16-tap kernel TBC Source and TBC Sink resample with, within
  // 0.2 % in gain and 0.2° in phase up to 0.4 fs (7.1 MHz). Chroma is
  // baseband (the decoders band-limit it to about 1.3 MHz), so it takes an
  // 8-tap kernel of the same form: within 0.08 % in gain and 0.09° in phase
  // up to 1.3 MHz, 0.14 % at 2 MHz. Both are cos²-windowed sincs normalised
  // to unity DC gain.
  static constexpr int kLumaTaps = pal_line_locked::detail::kKernelTaps;
  static constexpr int kChromaTaps = 8;
  // Edge padding of the scratch row: covers the kernel's reach past a
  // window the decoders could not widen in full.
  static constexpr int32_t kPad = 16;
  static_assert(kPad >= kLumaTaps);
  // Outputs accumulated per pass of the tap loop (a stack buffer, so the
  // compiler sees it cannot alias the input).
  static constexpr int32_t kChunk = 256;

  template <int N>
  struct RowKernel {
    static constexpr int kTaps = N;
    static constexpr int kLeft = N / 2 - 1;  // taps base−kLeft … base+N−1−kLeft
    int32_t whole = 0;                       // floor(d)
    bool copy = false;                       // d is a whole number of samples
    std::array<float, static_cast<size_t>(N)> h{};
  };

  struct Span {
    int32_t valid_begin = 0;
    int32_t valid_end = 0;
    int32_t out_begin = 0;
    int32_t out_end = 0;
    int32_t source_offset = 0;
  };

  // The kernel for each absolute frame line, built once per process: d
  // depends only on the field and row.
  template <int N>
  static std::array<RowKernel<N>, kPalFrameLines> build_kernels() {
    constexpr double kPi = 3.14159265358979323846;
    std::array<RowKernel<N>, kPalFrameLines> t{};
    for (int32_t line = 0; line < kPalFrameLines; ++line) {
      const int field = (line % 2 == 0) ? 1 : 2;
      const int64_t d625 =
          pal_line_locked::row_0h_offset_625ths(field, line / 2);
      RowKernel<N>& r = t[static_cast<size_t>(line)];
      r.whole = static_cast<int32_t>(d625 / kPalFrameLines);
      const double frac =
          static_cast<double>(d625 % kPalFrameLines) / kPalFrameLines;
      // A whole-sample shift (only field 1 row 0, where d is 0) is a plain
      // copy: the kernel's zero taps are not exactly zero in floating point.
      r.copy = (frac == 0.0);
      double h[N];
      double sum = 0.0;
      for (int k = 0; k < N; ++k) {
        const double x = static_cast<double>(k - RowKernel<N>::kLeft) - frac;
        const double sinc = (x == 0.0) ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double w = std::cos(kPi * x / (N + 1));
        h[k] = sinc * w * w;
        sum += h[k];
      }
      for (int k = 0; k < N; ++k) {
        r.h[static_cast<size_t>(k)] = static_cast<float>(h[k] / sum);
      }
    }
    return t;
  }

  static const std::array<RowKernel<kLumaTaps>, kPalFrameLines>& luma_taps() {
    static const auto table = build_kernels<kLumaTaps>();
    return table;
  }
  static const std::array<RowKernel<kChromaTaps>, kPalFrameLines>&
  chroma_taps() {
    static const auto table = build_kernels<kChromaTaps>();
    return table;
  }

  // Shifts one row in place. The arithmetic is in float, four samples to a
  // SIMD register: its rounding (under 1e-4 of a 10-bit code) is far below
  // the 16-bit output.
  template <int N>
  static void shift_row(double* row, const RowKernel<N>& kernel,
                        const Span& span, std::vector<float>& scratch) {
    const int32_t count = span.out_end - span.out_begin;
    double* out = row + span.out_begin;
    const int32_t source = span.out_begin + span.source_offset + kernel.whole;
    if (kernel.copy && source >= span.valid_begin &&
        source + count <= span.valid_end) {
      // The ranges overlap when the frame is cropped (source is ahead of
      // out by the left margin).
      std::memmove(out, row + source,
                   static_cast<size_t>(count) * sizeof(double));
    } else {
      // Edge-padded float copy of the decoded samples, so the tap loop never
      // bounds-checks. The padding is only read where the decoders could not
      // be given the whole margin (a window at the edge of the line).
      const int32_t n = span.valid_end - span.valid_begin;
      float* s = scratch.data();
      std::fill(s, s + kPad, static_cast<float>(row[span.valid_begin]));
      for (int32_t i = 0; i < n; ++i) {
        s[kPad + i] = static_cast<float>(row[span.valid_begin + i]);
      }
      std::fill(s + kPad + n, s + kPad + n + kPad,
                static_cast<float>(row[span.valid_end - 1]));

      // Scratch index of the first tap of the first output.
      const int32_t first =
          source - RowKernel<N>::kLeft - span.valid_begin + kPad;
      const int32_t highest = n + 2 * kPad - N;
      const float* h = kernel.h.data();
      if (first >= 0 && first + count - 1 <= highest) {
        // Taps outer, outputs inner: no reduction in the inner loop, so it
        // vectorises.
        float acc[kChunk];
        for (int32_t c = 0; c < count; c += kChunk) {
          const int32_t len = std::min(kChunk, count - c);
          const float* sc = s + first + c;
          for (int32_t i = 0; i < len; ++i) acc[i] = h[0] * sc[i];
          for (int k = 1; k < N; ++k) {
            const float hk = h[k];
            const float* sk = sc + k;
            for (int32_t i = 0; i < len; ++i) acc[i] += hk * sk[i];
          }
          for (int32_t i = 0; i < len; ++i) out[c + i] = acc[i];
        }
      } else {
        for (int32_t i = 0; i < count; ++i) {
          const int32_t b = std::clamp(first + i, 0, highest);
          float acc = 0.0f;
          for (int k = 0; k < N; ++k) acc += h[k] * s[b + k];
          out[i] = acc;
        }
      }
    }

    // The margins held decode-only samples; an unwidened decode leaves them
    // at zero.
    std::fill(row + span.valid_begin,
              row + std::min(span.out_begin, span.valid_end), 0.0);
    std::fill(row + std::max(span.out_end, span.valid_begin),
              row + span.valid_end, 0.0);
  }

  Geometry geometry_;
};

}  // namespace orc::chroma_sink

#endif  // ORC_CORE_CHROMA_SINK_PAL_OUTPUT_RASTER_H
