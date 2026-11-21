/*
 * File:        pal_output_raster_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the PAL output raster shift: components decoded
 *              on the 4fsc lattice put onto an orthogonal raster
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/pal_output_raster.h"

#include <gtest/gtest.h>
#include <orc/support/pal_line_locked_resample.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "../../../../orc/plugins/stages/sinks/common/decoders/palcolour.h"

namespace orc_unit_test {
namespace {

using orc::chroma_sink::PalOutputRaster;
namespace pll = orc::pal_line_locked;

constexpr double kPi = 3.14159265358979323846;
constexpr int32_t kLine = orc::kPalSamplesPerLineNominal;  // 1135
constexpr double kLow = 200.0;
constexpr double kHigh = 800.0;
constexpr double kMid = (kLow + kHigh) / 2.0;
// The edge's 0H-relative position, in stored samples on field 1 row 0.
constexpr double kEdge = 500.3;
// Width of the raised-cosine edge, in samples: soft enough to sit well
// below the chroma band, so pal2d's luma passes it unchanged.
constexpr double kEdgeWidth = 12.0;

orc::SourceParameters pal_params(bool cropped) {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::PAL;
  p.frame_width_nominal = kLine;
  p.active_video_start = orc::kPalActiveVideoStart;
  p.active_video_end = orc::kPalActiveVideoEnd;
  p.first_active_frame_line = 44;
  p.last_active_frame_line = 620;
  p.active_area_cropping_applied = cropped;
  return p;
}

// The 0H offset of absolute frame line |line| (2 × row + field − 1).
double offset_of_frame_line(int32_t line) {
  return pll::row_0h_offset(line % 2 == 0 ? 1 : 2, line / 2);
}

// A raised-cosine step from kLow to kHigh centred on |centre|.
double edge(double x, double centre) {
  const double t = (x - centre) / kEdgeWidth;
  if (t <= -0.5) return kLow;
  if (t >= 0.5) return kHigh;
  return kMid + (kHigh - kLow) / 2.0 * std::sin(kPi * t);
}

// Where row |row| of |plane| crosses kMid, in frame indices, by linear
// interpolation; empty when it does not.
std::optional<double> crossing(const double* row, int32_t begin, int32_t end) {
  for (int32_t i = begin; i + 1 < end; ++i) {
    if (row[i] < kMid && row[i + 1] >= kMid) {
      return i + (kMid - row[i]) / (row[i + 1] - row[i]);
    }
  }
  return std::nullopt;
}

// A frame laid out as the decoders leave it for |decode|: stored sample x of
// each row at the frame index the cropping convention gives it, the window
// filled by |value|(x, row offset).
template <typename Value>
ComponentFrame decoded_frame(const orc::SourceParameters& decode, Value value) {
  ComponentFrame frame;
  frame.init(decode);
  const int32_t line_offset =
      decode.active_area_cropping_applied ? decode.first_active_frame_line : 0;
  const int32_t x_offset =
      decode.active_area_cropping_applied ? decode.active_video_start : 0;
  for (int32_t line = decode.first_active_frame_line;
       line < decode.last_active_frame_line; ++line) {
    const double d = offset_of_frame_line(line);
    for (std::vector<double>* plane :
         {frame.getY(), frame.getU(), frame.getV()}) {
      double* row = plane->data() + static_cast<size_t>(line - line_offset) *
                                        static_cast<size_t>(kLine);
      for (int32_t x = decode.active_video_start; x < decode.active_video_end;
           ++x) {
        row[x - x_offset] = value(static_cast<double>(x), d);
      }
    }
  }
  return frame;
}

void expect_edge_on_every_row(const ComponentFrame& frame,
                              const orc::SourceParameters& output) {
  const int32_t line_offset =
      output.active_area_cropping_applied ? output.first_active_frame_line : 0;
  const int32_t x_offset =
      output.active_area_cropping_applied ? output.active_video_start : 0;
  for (int32_t line = output.first_active_frame_line;
       line < output.last_active_frame_line; ++line) {
    for (const std::vector<double>* plane :
         {frame.getY(), frame.getU(), frame.getV()}) {
      const double* row =
          plane->data() +
          static_cast<size_t>(line - line_offset) * static_cast<size_t>(kLine);
      const auto at = crossing(row, output.active_video_start - x_offset,
                               output.active_video_end - x_offset);
      ASSERT_TRUE(at.has_value()) << "frame line " << line;
      EXPECT_NEAR(*at + x_offset, kEdge, 0.05) << "frame line " << line;
    }
  }
}

}  // namespace

TEST(PalOutputRasterTest, AppliesOnlyToTheNonOrthogonalLattice) {
  EXPECT_TRUE(PalOutputRaster::applies_to(orc::VideoSystem::PAL));
  EXPECT_FALSE(PalOutputRaster::applies_to(orc::VideoSystem::NTSC));
  EXPECT_FALSE(PalOutputRaster::applies_to(orc::VideoSystem::PAL_M));
}

TEST(PalOutputRasterTest, DecodeWindow_WidensPalAndLeavesOrthogonalSystems) {
  const auto pal = pal_params(true);
  const auto widened = PalOutputRaster::decode_window(pal);
  EXPECT_EQ(widened.active_video_start,
            pal.active_video_start - PalOutputRaster::kDecodeMarginLeft);
  EXPECT_EQ(widened.active_video_end,
            pal.active_video_end + PalOutputRaster::kDecodeMarginRight);

  auto ntsc = pal;
  ntsc.system = orc::VideoSystem::NTSC;
  ntsc.frame_width_nominal = orc::kNtscSamplesPerLine;
  const auto same = PalOutputRaster::decode_window(ntsc);
  EXPECT_EQ(same.active_video_start, ntsc.active_video_start);
  EXPECT_EQ(same.active_video_end, ntsc.active_video_end);
}

// An edge at a fixed time after 0H lands d samples later on each stored row;
// after the shift it is at one output position on every row.
TEST(PalOutputRasterTest, Shift_PutsAnEdgeAtZeroHPlusConstantInOneColumn) {
  for (const bool cropped : {true, false}) {
    SCOPED_TRACE(cropped ? "cropped frame" : "frame-flat frame");
    const auto output = pal_params(cropped);
    const auto decode = PalOutputRaster::decode_window(output);
    ComponentFrame frame = decoded_frame(
        decode, [](double x, double d) { return edge(x, kEdge + d); });

    PalOutputRaster(PalOutputRaster::geometry_for(output, decode)).apply(frame);
    expect_edge_on_every_row(frame, output);
  }
}

TEST(PalOutputRasterTest, Shift_KeepsAFlatFieldsLevel) {
  const auto output = pal_params(true);
  const auto decode = PalOutputRaster::decode_window(output);
  ComponentFrame frame =
      decoded_frame(decode, [](double, double) { return 512.0; });

  PalOutputRaster(PalOutputRaster::geometry_for(output, decode)).apply(frame);

  const int32_t width = output.active_video_end - output.active_video_start;
  for (int32_t line = 0;
       line < output.last_active_frame_line - output.first_active_frame_line;
       ++line) {
    for (int32_t x = 0; x < width; ++x) {
      ASSERT_NEAR(frame.y(line)[x], 512.0, 1.0) << line << "," << x;
      ASSERT_NEAR(frame.u(line)[x], 512.0, 1.0) << line << "," << x;
      ASSERT_NEAR(frame.v(line)[x], 512.0, 1.0) << line << "," << x;
    }
  }
}

// The decode margins held samples an unwidened decode never writes; the
// frame leaves the shift with them cleared.
TEST(PalOutputRasterTest, Shift_ClearsTheDecodeMargins) {
  const auto output = pal_params(false);
  const auto decode = PalOutputRaster::decode_window(output);
  ComponentFrame frame =
      decoded_frame(decode, [](double, double) { return 512.0; });

  PalOutputRaster(PalOutputRaster::geometry_for(output, decode)).apply(frame);

  const double* row = frame.y(output.first_active_frame_line + 1);
  for (int32_t x = decode.active_video_start; x < output.active_video_start;
       ++x) {
    EXPECT_EQ(row[x], 0.0) << x;
  }
  for (int32_t x = output.active_video_end; x < decode.active_video_end; ++x) {
    EXPECT_EQ(row[x], 0.0) << x;
  }
}

// Field 1 row 0 is the reference: its shift is zero and its samples come
// through bit for bit.
TEST(PalOutputRasterTest, Shift_LeavesTheReferenceRowExact) {
  auto output = pal_params(false);
  output.first_active_frame_line = 0;
  const auto decode = PalOutputRaster::decode_window(output);
  ComponentFrame frame = decoded_frame(
      decode, [](double x, double) { return std::fmod(x * 37.0, 101.0); });

  PalOutputRaster(PalOutputRaster::geometry_for(output, decode)).apply(frame);

  for (int32_t x = output.active_video_start; x < output.active_video_end;
       ++x) {
    ASSERT_DOUBLE_EQ(frame.y(0)[x], std::fmod(x * 37.0, 101.0)) << x;
  }
}

// The decoder path: a lattice frame with a luma edge at 0H + constant on
// every line, decoded by pal2d over the widened window and shifted, has the
// edge at one output position on every line.
TEST(PalOutputRasterTest, Pal2dDecode_PutsALatticeEdgeInOneColumn) {
  const auto output = pal_params(true);
  const auto decode = PalOutputRaster::decode_window(output);

  std::vector<std::vector<int16_t>> buffers;
  std::vector<SourceField> fields;
  for (const int field : {1, 2}) {
    const int32_t rows = pll::field_lines(field);
    std::vector<int16_t> samples(static_cast<size_t>(rows) * kLine);
    for (int32_t row = 0; row < rows; ++row) {
      const double d = pll::row_0h_offset(field, row);
      for (int32_t x = 0; x < kLine; ++x) {
        samples[static_cast<size_t>(row) * kLine + x] =
            static_cast<int16_t>(std::lround(edge(x, kEdge + d)));
      }
    }
    buffers.push_back(std::move(samples));
    SourceField f;
    f.is_first_field = field == 1;
    f.field_phase_id = field;
    f.line_count = static_cast<size_t>(rows);
    f.samples_per_line = static_cast<size_t>(kLine);
    f.data = buffers.back().data();
    fields.push_back(f);
  }
  // buffers no longer reallocates; re-point the views at the final storage.
  for (size_t i = 0; i < fields.size(); ++i) fields[i].data = buffers[i].data();

  PalColour decoder;
  PalColour::Configuration config;
  config.chromaFilter = PalColour::palColourFilter;
  decoder.updateConfiguration(decode, config);
  std::vector<ComponentFrame> frames(1);
  decoder.decodeFrames(fields, 0, 2, frames);

  PalOutputRaster(PalOutputRaster::geometry_for(output, decode))
      .apply(frames[0]);

  // Luma only: the decode puts no chroma on a soft luma edge worth measuring.
  // Lines next to the top and bottom of each field see the decoder's
  // vertical filter run off the field, so they are left out.
  constexpr int32_t kFieldEdgeLines = 8;
  for (int32_t line = output.first_active_frame_line + kFieldEdgeLines;
       line < output.last_active_frame_line - kFieldEdgeLines; ++line) {
    const double* row = frames[0].y(line - output.first_active_frame_line);
    const auto at =
        crossing(row, 0, output.active_video_end - output.active_video_start);
    ASSERT_TRUE(at.has_value()) << "frame line " << line;
    EXPECT_NEAR(*at + output.active_video_start, kEdge, 0.05)
        << "frame line " << line;
  }
}

}  // namespace orc_unit_test
