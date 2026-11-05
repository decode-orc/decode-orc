/*
 * File:        palstilldecoder_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the PAL Still decoder on a PAL composite built
 *              in the test: colour bars, flat colour, a colour edge and a
 *              luma edge decoded within tolerance of their truth, the same
 *              output from every platform, and the colour killer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/decoders/palstilldecoder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../../../../orc/plugins/stages/sinks/common/decoders/monodecoder.h"
#include "../../../../orc/plugins/stages/sinks/common/decoders/pal_still.h"

namespace orc_unit_test {
namespace {

namespace d2d = orc::pal_still;

constexpr double kPi = 3.14159265358979323846;
constexpr double kBlanking = 256.0;
constexpr double kWhite = 844.0;
// Transmission-order lines: field 1's 313, then field 2's 312. Lines 312
// and 624 carry 1137 samples on the 4fsc grid, the others 1135.
constexpr size_t kFrameLines = 625;
constexpr size_t kField1Lines = 313;
constexpr size_t kLine = d2d::kLine;
// The picture: raster rows of field lines 23-310, the decoder's samples.
constexpr size_t kPictureFirstSample = 157;
constexpr size_t kPictureEndSample = 157 + 948;
// Nominal PAL burst, 0.3 V peak to peak of 0.7 V white - blanking.
constexpr double kBurst = 0.15 / 0.7;

struct Yuv {
  double y;
  double u;
  double v;
};

// EBU 75 % bars: white, yellow, cyan, green, magenta, red, blue, black.
constexpr std::array<Yuv, 8> kBars{{{0.75, 0.0, 0.0},
                                    {0.665, -0.327, 0.075},
                                    {0.526, 0.110, -0.460},
                                    {0.441, -0.217, -0.385},
                                    {0.309, 0.217, 0.385},
                                    {0.224, -0.110, 0.460},
                                    {0.085, 0.327, -0.075},
                                    {0.0, 0.0, 0.0}}};
constexpr Yuv kFlat{0.5, 0.12, -0.08};
constexpr Yuv kGreen{0.45, -0.15, -0.20};
constexpr Yuv kMagenta{0.40, 0.15, 0.20};

// Raster row bands of the picture.
constexpr size_t kBarsEnd = 240;
constexpr size_t kFlatEnd = 400;
constexpr size_t kColourEdgeRow = 470;  // green above, magenta below
constexpr size_t kColourEnd = 540;      // then the luma edge to the end
constexpr size_t kLumaEdgeSample = 630;

// The picture's truth at raster |row|, sample |n| (rows outside the picture
// and samples outside the decoder's are blanking).
Yuv truth(size_t row, size_t n) {
  if (row < d2d::kFirstActiveRow || row >= d2d::kEndActiveRow ||
      n < kPictureFirstSample || n >= kPictureEndSample) {
    return {0.0, 0.0, 0.0};
  }
  if (row < kBarsEnd) {
    const size_t bar = (n - kPictureFirstSample) * kBars.size() /
                       (kPictureEndSample - kPictureFirstSample);
    return kBars[bar];
  }
  if (row < kFlatEnd) return kFlat;
  if (row < kColourEnd) return row < kColourEdgeRow ? kGreen : kMagenta;
  return {n < kLumaEdgeSample ? 0.2 : 0.8, 0.0, 0.0};
}

// A deterministic small noise (an LCG), so that no transform bin of the
// picture ties exactly between its two reflections and the decision is the
// same on every platform.
class Noise {
 public:
  double next() {
    state_ = (state_ * 6364136223846793005ULL) + 1442695040888963407ULL;
    return (static_cast<double>(state_ >> 11) / 9007199254740992.0) - 0.5;
  }

 private:
  uint64_t state_ = 1;
};

// A frame's two fields of 10-bit codes, laid out 1135 samples a line. The
// carrier runs on over the whole transmission-order frame, so field 2 is
// 90 degrees on and its V-switch inverted, as on a PAL 4fsc capture.
struct Frame {
  std::vector<int16_t> field1;
  std::vector<int16_t> field2;
};

Frame make_frame(bool colour) {
  Frame frame;
  frame.field1.assign(kField1Lines * kLine, static_cast<int16_t>(kBlanking));
  frame.field2.assign((kFrameLines - kField1Lines) * kLine,
                      static_cast<int16_t>(kBlanking));
  Noise noise;
  size_t start = 0;  // the line's first sample in transmission order
  for (size_t line = 0; line < kFrameLines; ++line) {
    const bool first = line < kField1Lines;
    const size_t field_line = first ? line : line - kField1Lines;
    const size_t row = (2 * field_line) + (first ? 0 : 1);
    // The U carrier's phase at the line's sample 0, and the V-switch.
    const double phi = static_cast<double>(start % 4) * kPi / 2.0;
    const double s = (line % 2 == 0) ? 1.0 : -1.0;
    int16_t* out = (first ? frame.field1.data() : frame.field2.data()) +
                   (field_line * kLine);
    for (size_t n = 0; n < kLine; ++n) {
      const double theta = (static_cast<double>(n) * kPi / 2.0) + phi;
      double x = 0.0;
      if (n >= d2d::kBurstStart && n < d2d::kBurstEnd && colour) {
        x = kBurst * std::cos(theta + ((s > 0.0 ? 45.0 : 135.0) * kPi / 180.0));
      }
      const Yuv t = truth(row, n);
      x += t.y;
      if (colour) x += (t.u * std::sin(theta)) + (s * t.v * std::cos(theta));
      const double code =
          kBlanking + (x * (kWhite - kBlanking)) + (0.6 * noise.next());
      out[n] = static_cast<int16_t>(std::lround(code));
    }
    start += (line == 312 || line == 624) ? 1137 : 1135;
  }
  return frame;
}

orc::SourceParameters pal_params() {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::PAL;
  p.frame_width_nominal = static_cast<int32_t>(kLine);
  p.frame_height = static_cast<int32_t>(kFrameLines);
  p.blanking_level = static_cast<int32_t>(kBlanking);
  p.black_level = static_cast<int32_t>(kBlanking);
  p.white_level = static_cast<int32_t>(kWhite);
  p.active_video_start = 185;
  p.active_video_end = 1107;
  p.first_active_frame_line = 44;
  p.last_active_frame_line = 620;
  return p;
}

std::vector<SourceField> fields_of(const Frame& frame) {
  SourceField a;
  a.is_first_field = true;
  a.field_phase_id = 1;
  a.line_count = kField1Lines;
  a.samples_per_line = kLine;
  a.data = frame.field1.data();
  SourceField b = a;
  b.is_first_field = false;
  b.field_phase_id = 2;
  b.line_count = kFrameLines - kField1Lines;
  b.data = frame.field2.data();
  return {a, b};
}

// The decoder's planes for |frame| on the frame raster.
PalStillDecoder::Planes decode(const Frame& frame,
                               const PalStillDecoder::Configuration& c) {
  d2d::Plane raster;
  d2d::fill_raster(
      [&](size_t field, size_t line) {
        return (field == 0 ? frame.field1.data() : frame.field2.data()) +
               (line * kLine);
      },
      kBlanking, kWhite, raster);
  PalStillDecoder decoder(c);
  PalStillDecoder::Planes planes;
  decoder.decodeRaster(raster, planes);
  return planes;
}

// Away from the bands' edges and the bars' transitions.
bool inside(size_t row, size_t n) {
  const auto clear = [](size_t x, size_t edge, size_t margin) {
    return x + margin <= edge || x >= edge + margin;
  };
  const size_t bar_width =
      (kPictureEndSample - kPictureFirstSample) / kBars.size();
  bool ok = row >= d2d::kFirstActiveRow + 8 && row + 8 < d2d::kEndActiveRow &&
            n >= kPictureFirstSample + 24 && n + 24 < kPictureEndSample;
  for (const size_t edge : {kBarsEnd, kFlatEnd, kColourEdgeRow, kColourEnd}) {
    ok = ok && clear(row, edge, 16);
  }
  if (row < kBarsEnd) {
    for (size_t b = 1; b < kBars.size(); ++b) {
      ok = ok && clear(n, kPictureFirstSample + (b * bar_width), 32);
    }
  }
  return ok && clear(n, kLumaEdgeSample, 16);
}

// Every part of the picture decodes within a few codes of its truth: the
// transform, the lattice, the vertical fit and the reassignment together.
TEST(PalStillDecoderTest, DecodesTheTestFrameWithinTolerance) {
  const auto planes = decode(make_frame(true), {});
  double worst_y = 0.0;
  double worst_uv = 0.0;
  size_t worst_y_at = 0;
  size_t worst_uv_at = 0;
  size_t checked = 0;
  for (size_t row = 0; row < d2d::kRows; ++row) {
    for (size_t n = 0; n < kLine; ++n) {
      if (!inside(row, n)) continue;
      const Yuv t = truth(row, n);
      const size_t k = (row * kLine) + n;
      const double dy = std::abs(planes.y[k] - t.y);
      const double duv =
          std::max(std::abs(planes.u[k] - t.u), std::abs(planes.v[k] - t.v));
      if (dy > worst_y) {
        worst_y = dy;
        worst_y_at = k;
      }
      if (duv > worst_uv) {
        worst_uv = duv;
        worst_uv_at = k;
      }
      ++checked;
    }
  }
  EXPECT_GT(checked, 200000u);
  // A percent and a quarter of white for the luma (about 3 8-bit codes),
  // under half a percent for the chroma. With kDetailGate at 0.03 the
  // reassignment returns less of the residual on this noise-free frame's
  // sharp bars than a smaller gate would (worst 0.0103 against 0.0057 at
  // 0.003, bars r.m.s. 0.00075 against 0.00048), while on stills softened
  // and jittered as the discs are, and on the discs, 0.03 does better.
  EXPECT_LT(worst_y, 0.0125)
      << "row " << worst_y_at / kLine << " sample " << worst_y_at % kLine;
  EXPECT_LT(worst_uv, 0.005)
      << "row " << worst_uv_at / kLine << " sample " << worst_uv_at % kLine;
}

// The colour edge stays sharp: one field line (two raster rows) either
// side of it, the chroma is within a tenth of the step of its own side.
TEST(PalStillDecoderTest, KeepsTheVerticalColourEdge) {
  const auto planes = decode(make_frame(true), {});
  const double step = kMagenta.u - kGreen.u;
  for (size_t n = 300; n < 1000; n += 50) {
    const double above = planes.u[((kColourEdgeRow - 3) * kLine) + n];
    const double below = planes.u[((kColourEdgeRow + 2) * kLine) + n];
    EXPECT_LT(std::abs(above - kGreen.u), 0.1 * step) << n;
    EXPECT_LT(std::abs(below - kMagenta.u), 0.1 * step) << n;
  }
}

// Recorded on x86-64 Linux (GCC, the AVX2 fit): every platform's build
// (SSE2 or AVX2, any FFT codelets) must give the same output to far below
// an output code (a 16-bit code is 2e-5), or the decode depends on where it
// ran. The single-precision fit is built without fused multiply-adds, which
// would move these points by up to 5e-7 (the decoders' CMakeLists.txt).
TEST(PalStillDecoderTest, SameOutputOnEveryPlatform) {
  const auto planes = decode(make_frame(true), {});
  struct Point {
    size_t row;
    size_t n;
    double y;
    double u;
    double v;
  };
  // clang-format off
  constexpr std::array<Point, 8> kRecorded{{
      {100, 300, 0.66221553160576918, -0.32699856162071228, 0.073663830757141113},
      {101, 777, 0.2235961576700817, -0.11007777601480484, 0.45997759699821472},
      {300, 500, 0.49997432064967728, 0.12019510567188263, -0.079934254288673401},
      {468, 640, 0.44682168342424938, -0.14773984253406525, -0.19609023630619049},
      {471, 640, 0.40015384669000847, 0.14771571755409241, 0.19605129957199097},
      {472, 900, 0.39957126787623259, 0.14832678437232971, 0.19741004705429077},
      {580, 626, 0.20052480533851133, 1.1538037142599933e-05, -6.0951060731895268e-05},
      {581, 634, 0.79946530687417883, -5.4616073612123728e-05, -2.003962617891375e-05},
  }};
  // clang-format on
  for (const auto& p : kRecorded) {
    const size_t k = (p.row * kLine) + p.n;
    EXPECT_NEAR(planes.y[k], p.y, 1e-7) << p.row << " " << p.n;
    EXPECT_NEAR(planes.u[k], p.u, 1e-7) << p.row << " " << p.n;
    EXPECT_NEAR(planes.v[k], p.v, 1e-7) << p.row << " " << p.n;
  }
}

// Two decoders, the same input: the same output to the bit.
TEST(PalStillDecoderTest, Reproducible) {
  const auto frame = make_frame(true);
  const auto a = decode(frame, {});
  const auto b = decode(frame, {});
  EXPECT_EQ(a.y, b.y);
  EXPECT_EQ(a.u, b.u);
  EXPECT_EQ(a.v, b.v);
}

// A monochrome frame has no burst: no colour comes out, and the luma is the
// composite, exactly as the mono decoder gives it. (pal2d has no such path:
// its luma is always the composite less what its Y filter takes as chroma,
// which softens luma edges and, on this frame, reads 0.6 % of the sample
// code low on flat picture.)
TEST(PalStillDecoderTest, ColourKillerOnAMonochromeFrame) {
  const auto frame = make_frame(false);
  const auto planes = decode(frame, {});
  for (size_t row = d2d::kFirstActiveRow; row < d2d::kEndActiveRow; ++row) {
    for (size_t n = kPictureFirstSample; n < kPictureEndSample; n += 7) {
      const size_t k = (row * kLine) + n;
      ASSERT_EQ(planes.u[k], 0.0);
      ASSERT_EQ(planes.v[k], 0.0);
    }
  }

  const auto params = pal_params();
  PalStillDecoder domesday({});
  ASSERT_TRUE(domesday.configure(params));
  MonoDecoder mono;
  ASSERT_TRUE(mono.configure(params));
  const auto fields = fields_of(frame);
  std::vector<ComponentFrame> ours(1);
  std::vector<ComponentFrame> plain(1);
  domesday.decodeFrames(fields, 0, 2, ours);
  mono.decodeFrames(fields, 0, 2, plain);
  double worst = 0.0;
  for (int32_t line = params.first_active_frame_line;
       line < params.last_active_frame_line; ++line) {
    for (int32_t i = params.active_video_start; i < params.active_video_end;
         ++i) {
      worst =
          std::max(worst, std::abs(ours[0].y(line)[i] - plain[0].y(line)[i]));
      EXPECT_EQ(ours[0].u(line)[i], 0.0);
      EXPECT_EQ(ours[0].v(line)[i], 0.0);
    }
  }
  EXPECT_LT(worst, 1e-9);
}

// A weak burst still decodes colour: the killer acts only below PalColour's
// threshold, a quarter of the nominal burst.
TEST(PalStillDecoderTest, ColourKillerSparesAWeakBurst) {
  auto frame = make_frame(true);
  // Halve every sample's distance from blanking: burst and picture alike.
  for (auto* field : {&frame.field1, &frame.field2}) {
    for (auto& code : *field) {
      code = static_cast<int16_t>(
          std::lround(kBlanking + ((code - kBlanking) * 0.5)));
    }
  }
  const auto planes = decode(frame, {});
  const size_t k = (300 * kLine) + 500;
  EXPECT_NEAR(planes.u[k], kFlat.u / 2.0, 0.005);
  EXPECT_NEAR(planes.v[k], kFlat.v / 2.0, 0.005);
}

}  // namespace
}  // namespace orc_unit_test
