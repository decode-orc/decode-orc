/*
 * File:        ntsc3d_comb_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NTSC motion-adaptive 3D comb and its
 *              fractional-slope 2D fallback
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/decoders/ntsc3d_comb.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "../../../../orc/plugins/stages/sinks/common/decoders/fractional_comb.h"

namespace orc_unit_test {
namespace {

using ntsc_comb::ActivePicture;
using ntsc_comb::FieldLayout;
using ntsc_comb::FractionalComb;
using ntsc_comb::Ntsc3dComb;
using ntsc_comb::Plane;

// SMPTE 170M-2004 Section 11.3: 525 lines a frame, 263 in the first field
// as stored; 910 samples a line at 4fsc.
constexpr int32_t kRows = 525;
constexpr int32_t kCols = 910;
constexpr int32_t kField1Rows = 263;
constexpr int32_t kFirstActiveLine = orc::kNtscFirstActiveFrameLine;
constexpr int32_t kLastActiveLine = orc::kNtscLastActiveFrameLine;
constexpr int32_t kActiveStart = orc::kNtscActiveVideoStart;
constexpr int32_t kActiveEnd = orc::kNtscActiveVideoEnd;
// SMPTE 170M-2004 Section 8: horizontal sync is 4.7 us (67.3 samples at
// 4fsc) from 0H, which falls 57/90 of a sample before stored sample 0 (CVBS
// File Format Specification v1.8.1, NTSC "Stored line alignment"): stored
// samples 0-66.
constexpr int32_t kSyncEnd = 67;
constexpr double kPi = 3.14159265358979323846;
// Sync tip level in 10-bit codes (below blanking), and the per-sample noise
// every synthetic frame carries, so the comb's noise floors measure
// something.
constexpr double kSyncTip = 16.0;
constexpr double kNoise = 1.5;

FieldLayout layout() {
  FieldLayout l;
  l.rows = kRows;
  l.cols = kCols;
  l.field1Rows = kField1Rows;
  return l;
}

ActivePicture picture() {
  return ActivePicture::fromFrameLines(
      layout(), kFirstActiveLine, kLastActiveLine, kActiveStart, kActiveEnd);
}

int32_t frameLineOf(int32_t row) {
  return row < kField1Rows ? 2 * row : (2 * (row - kField1Rows)) + 1;
}

// A deterministic value in [-1, 1) from three integers.
double hashUniform(uint64_t a, uint64_t b, uint64_t c) {
  uint64_t x = (a * 0x9E3779B97F4A7C15ULL) ^ (b * 0xC2B2AE3D27D4EB4FULL) ^
               (c * 0x165667B19E3779F9ULL);
  x ^= x >> 31;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBULL;
  x ^= x >> 31;
  return (static_cast<double>(x >> 11) / static_cast<double>(1ULL << 52)) - 1.0;
}

// Luma above black (codes) at frame k, interlaced frame line, sample.
using LumaFn = std::function<double(int32_t, int32_t, int32_t)>;

// The subcarrier runs continuously through the transmitted sequence: field
// 1's lines then field 2's, frame after frame. 910 samples a line and 525
// lines a frame are both 2 modulo 4, so it inverts from line to line and
// from frame to frame.
int64_t sampleTime(int32_t k, int32_t row, int32_t h) {
  return ((static_cast<int64_t>(k) * kRows) + row) * kCols + h;
}

double trueChroma(int32_t k, int32_t row, int32_t h, double amplitude,
                  double theta) {
  return amplitude *
         std::cos((kPi / 2.0) * static_cast<double>(sampleTime(k, row, h) % 4) +
                  theta);
}

// One composite frame: sync tip, burst, and in the picture black-level luma
// plus |luma| plus a flat colour, all with per-frame noise.
Plane makeFrame(int32_t k, const LumaFn& luma, double amplitude, double theta) {
  const double ireScale =
      static_cast<double>(orc::kNtscWhite - orc::kNtscBlanking) / 100.0;
  Plane frame(kRows, kCols);
  for (int32_t r = 0; r < kRows; r++) {
    const int32_t line = frameLineOf(r);
    for (int32_t h = 0; h < kCols; h++) {
      double value = orc::kNtscBlanking;
      if (h < kSyncEnd) {
        value = kSyncTip;
      } else if (h >= orc::kNtscColourBurstStart &&
                 h < orc::kNtscColourBurstEnd) {
        value += 20.0 * ireScale *
                 std::sin((kPi / 2.0) *
                          static_cast<double>(sampleTime(k, r, h) % 4));
      } else if (line >= kFirstActiveLine && line < kLastActiveLine &&
                 h >= kActiveStart && h < kActiveEnd) {
        value = orc::kNtscBlack + luma(k, line, h) +
                trueChroma(k, r, h, amplitude, theta);
      }
      value += kNoise * hashUniform(static_cast<uint64_t>(k) + 1000, r, h);
      frame.set(r, h, static_cast<double>(std::lround(value)));
    }
  }
  return frame;
}

// Fine luma texture with energy across the band, the same in every frame
// unless |shift| moves it along the line.
LumaFn texture(uint64_t seed, int32_t shift = 0) {
  return [seed, shift](int32_t k, int32_t line, int32_t h) {
    return 60.0 + 40.0 * hashUniform(seed, line, h - (shift * k));
  };
}

LumaFn flat(double level) {
  return [level](int32_t, int32_t, int32_t) { return level; };
}

// Samples well inside the picture.
template <typename F>
void forInterior(F f) {
  const ActivePicture p = picture();
  for (int32_t r = 0; r < kRows; r++) {
    if (!p.containsRow(r) || !p.containsRow(r - 4) || !p.containsRow(r + 4)) {
      continue;
    }
    for (int32_t h = kActiveStart + 16; h < kActiveEnd - 16; h++) f(r, h);
  }
}

double frameCombShare(const Ntsc3dComb::Result& result) {
  double total = 0.0;
  int64_t count = 0;
  forInterior([&](int32_t r, int32_t h) {
    total += result.frameCombWeight.at(r, h);
    count++;
  });
  return total / static_cast<double>(count);
}

double chromaRmsError(const Plane& chroma, int32_t k, double amplitude,
                      double theta) {
  double total = 0.0;
  int64_t count = 0;
  forInterior([&](int32_t r, int32_t h) {
    const double e = chroma.at(r, h) - trueChroma(k, r, h, amplitude, theta);
    total += e * e;
    count++;
  });
  return std::sqrt(total / static_cast<double>(count));
}

struct Sequence {
  std::vector<Plane> frames;  // frames 0..4; frame 2 is decoded

  Ntsc3dComb::Result separate(const Ntsc3dComb::Settings& settings = {}) const {
    Ntsc3dComb::Frames f;
    f.before2 = &frames[0];
    f.before = &frames[1];
    f.current = &frames[2];
    f.after = &frames[3];
    f.after2 = &frames[4];
    return Ntsc3dComb::separate(f, layout(), picture(), settings);
  }
};

constexpr double kAmplitude = 40.0;
constexpr double kTheta = 0.6;

Sequence makeSequence(const std::function<LumaFn(int32_t)>& lumaOf,
                      const std::function<double(int32_t)>& thetaOf) {
  Sequence s;
  for (int32_t k = 0; k < 5; k++) {
    s.frames.push_back(makeFrame(k, lumaOf(k), kAmplitude, thetaOf(k)));
  }
  return s;
}

double steadyTheta(int32_t) { return kTheta; }

}  // namespace

TEST(Ntsc3dCombTest, StillFineTexture_UsesTheFrameComb_AndRemovesCrossColour) {
  const auto still =
      makeSequence([](int32_t) { return texture(7); }, steadyTheta);
  const auto result = still.separate();

  EXPECT_GT(frameCombShare(result), 0.8);

  // The 2D comb takes much of the texture for chroma; the frame comb does
  // not.
  const Plane twoD =
      FractionalComb::separate(still.frames[2], layout(), picture());
  const double frameError =
      chromaRmsError(result.chroma, 2, kAmplitude, kTheta);
  const double twoDError = chromaRmsError(twoD, 2, kAmplitude, kTheta);
  EXPECT_LT(frameError, 0.3 * twoDError);
}

TEST(Ntsc3dCombTest, MovingTexture_FallsBackToThe2DComb) {
  const auto moving =
      makeSequence([](int32_t) { return texture(7, 6); }, steadyTheta);
  EXPECT_LT(frameCombShare(moving.separate()), 0.1);
}

TEST(Ntsc3dCombTest, FlashInTheCurrentFrame_IsNotFrameCombed) {
  // The frames either side and two away carry one picture, the current frame
  // another.
  const auto flash = makeSequence(
      [](int32_t k) { return texture(k == 2 ? 9 : 7); }, steadyTheta);
  EXPECT_LT(frameCombShare(flash.separate()), 0.1);
}

TEST(Ntsc3dCombTest, ColourChangingFrameToFrameAndBack_KeepsEachFramesColour) {
  // A, B, A, B, A at constant luma: every comparison between frames of the
  // current frame's chroma phase matches, yet a frame comb would average A
  // and B. The 2D comb, whose chroma is consistent from line to line here,
  // must be used.
  const auto flicker = makeSequence(
      [](int32_t) { return flat(30.0); },
      [](int32_t k) { return (k % 2 == 0) ? kTheta : kTheta + 1.8; });
  const auto result = flicker.separate();

  EXPECT_LT(frameCombShare(result), 0.1);
  EXPECT_LT(chromaRmsError(result.chroma, 2, kAmplitude, kTheta), 3.0);
}

TEST(Ntsc3dCombTest, FlatStillColour_NeedsNoFrameComb) {
  // The 2D comb is right on flat colour and averages the noise across lines,
  // so the frame comb is not used.
  const auto still =
      makeSequence([](int32_t) { return flat(30.0); }, steadyTheta);
  const auto result = still.separate();
  EXPECT_LT(frameCombShare(result), 0.1);
  EXPECT_LT(chromaRmsError(result.chroma, 2, kAmplitude, kTheta), 3.0);
}

TEST(Ntsc3dCombTest, DetailThresholdZero_FrameCombsWhereverStill) {
  const auto still =
      makeSequence([](int32_t) { return flat(30.0); }, steadyTheta);
  Ntsc3dComb::Settings settings;
  settings.detailThreshold = 0.0;
  EXPECT_GT(frameCombShare(still.separate(settings)), 0.8);
}

TEST(Ntsc3dCombTest, OneSidedStillColourBars_FrameCombsTheColourEdges) {
  // The first frame of a sequence, frames after it only: still fine texture
  // under bars of colour 64 samples wide, which the 2D comb gets wrong. Against
  // a still neighbour the current frame's chroma, inverted, doubles at each
  // colour edge and leaks through the low-frequency luma test; with the frame
  // two away confirming the picture is still, that leak is allowed for, as it
  // is with both neighbours.
  Sequence bars;
  for (int32_t k = 0; k < 5; k++) {
    Plane frame = makeFrame(k, texture(7), 0.0, 0.0);
    for (int32_t r = 0; r < kRows; r++) {
      const int32_t line = frameLineOf(r);
      if (line < kFirstActiveLine || line >= kLastActiveLine) continue;
      for (int32_t h = kActiveStart; h < kActiveEnd; h++) {
        const double theta = 1.3 * static_cast<double>((h - kActiveStart) / 64);
        frame.set(r, h,
                  frame.at(r, h) +
                      std::lround(trueChroma(k, r, h, kAmplitude, theta)));
      }
    }
    bars.frames.push_back(std::move(frame));
  }
  Ntsc3dComb::Frames f;
  f.current = &bars.frames[2];
  f.after = &bars.frames[3];
  f.after2 = &bars.frames[4];
  const auto result = Ntsc3dComb::separate(f, layout(), picture(), {});

  // The samples within 4 of a colour edge (without the allowance, about a
  // fifth of them are frame-combed).
  double total = 0.0;
  int64_t count = 0;
  forInterior([&](int32_t r, int32_t h) {
    const int32_t inBar = (h - kActiveStart) % 64;
    if (inBar >= 4 && inBar < 60) return;
    total += result.frameCombWeight.at(r, h);
    count++;
  });
  EXPECT_GT(total / static_cast<double>(count), 0.6);
  EXPECT_GT(frameCombShare(result), 0.9);
}

TEST(Ntsc3dCombTest,
     ChromaChannelAcrossACut_MeasuredWithTheLuma_KeepsItsColour) {
  // A Y/C source's chroma, with a cut after the current frame: the colour
  // shifts a little, the luma (in the Y channel) changes. On the chroma alone
  // the luma tests see nothing, so the frame passes as moving steadily, and
  // the two frame combs disagree by less than their allowance: averaged, a
  // quarter of the next scene's colour would come in. Measured on Y + C, the
  // cut is plain, and the previous frame's comb alone is used.
  const double nextTheta = kTheta + 0.8;
  auto thetaOf = [&](int32_t k) { return k < 3 ? kTheta : nextTheta; };
  const auto chroma = makeSequence([](int32_t) { return flat(0.0); }, thetaOf);
  const auto measured =
      makeSequence([](int32_t k) { return texture(k < 3 ? 7 : 9); }, thetaOf);
  auto frames = [](const Sequence& s) {
    Ntsc3dComb::Frames f;
    f.before2 = &s.frames[0];
    f.before = &s.frames[1];
    f.current = &s.frames[2];
    f.after = &s.frames[3];
    f.after2 = &s.frames[4];
    return f;
  };
  // (Frame-combed wherever still, so the 2D comb's own accuracy on flat
  // colour does not hide which neighbours were used.)
  Ntsc3dComb::Settings settings;
  settings.detailThreshold = 0.0;

  const auto alone =
      Ntsc3dComb::separate(frames(chroma), layout(), picture(), settings);
  const auto withLuma = Ntsc3dComb::separate(frames(chroma), frames(measured),
                                             layout(), picture(), settings);

  // A quarter of the shift is about 8 codes RMS; noise alone about 1.
  EXPECT_GT(chromaRmsError(alone.chroma, 2, kAmplitude, kTheta), 5.0);
  EXPECT_LT(chromaRmsError(withLuma.chroma, 2, kAmplitude, kTheta), 2.0);
  EXPECT_GT(frameCombShare(withLuma), 0.8);
}

TEST(Ntsc3dCombTest, NoNeighbours_GivesThe2DComb) {
  const Plane frame = makeFrame(2, texture(7), kAmplitude, kTheta);
  Ntsc3dComb::Frames f;
  f.current = &frame;
  const auto result = Ntsc3dComb::separate(f, layout(), picture(), {});
  const Plane twoD = FractionalComb::separate(frame, layout(), picture());

  ASSERT_EQ(result.chroma.v.size(), twoD.v.size());
  for (size_t i = 0; i < twoD.v.size(); i++) {
    ASSERT_DOUBLE_EQ(result.chroma.v[i], twoD.v[i]);
    ASSERT_DOUBLE_EQ(result.frameCombWeight.v[i], 0.0);
  }
}

TEST(FractionalCombTest, Design_ZeroSlope_IsCloseToTheThreeLineComb) {
  const auto design = FractionalComb::design(0.0);
  EXPECT_GT(design.quality, 0.9);
  for (const auto& [position, weight] : design.taps) {
    const auto [line, sample] = position;
    double expected = 0.0;
    if (line == 0 && sample == 0) expected = 0.5;
    if (line != 0 && sample == 0) expected = -0.25;
    EXPECT_NEAR(weight, expected, 0.05)
        << "tap (" << line << ", " << sample << ")";
  }
}

TEST(FractionalCombTest, Design_NearTwoSamplesPerLine_IsNotTrusted) {
  // Chroma and luma line up the same way at +-2 samples per field line, so no
  // filter separates them there.
  EXPECT_DOUBLE_EQ(FractionalComb::design(2.0).quality, 0.0);
  EXPECT_DOUBLE_EQ(FractionalComb::design(-2.0).quality, 0.0);
}

TEST(FractionalCombTest, SlopedLumaEdge_IsCancelledAlongItsSlope) {
  // A sharp luma edge leaning one sample per field line and no chroma: the
  // 3-line comb leaves much of the edge in its chroma, the fractional comb
  // little.
  const LumaFn edge = [](int32_t, int32_t line, int32_t h) {
    return (h - (line / 2)) > 400 ? 100.0 : 0.0;
  };
  const Plane frame = makeFrame(0, edge, 0.0, 0.0);
  const Plane chroma = FractionalComb::separate(frame, layout(), picture());
  const Plane bp = ntsc_comb::bandpass(frame);

  double fractional = 0.0, threeLine = 0.0;
  forInterior([&](int32_t r, int32_t h) {
    // Only near the edge; elsewhere both carry just the noise.
    if (std::abs(h - (frameLineOf(r) / 2) - 400) > 12) return;
    const double base =
        (0.5 * bp.at(r, h)) - (0.25 * (bp.at(r - 1, h) + bp.at(r + 1, h)));
    fractional += chroma.at(r, h) * chroma.at(r, h);
    threeLine += base * base;
  });
  EXPECT_LT(fractional, 0.1 * threeLine);
}

TEST(FractionalCombTest, ThinStripesOfColour_TakeEachLinesOwnChroma) {
  // Horizontal stripes two field lines high, light and coloured in turn: the
  // 3-line comb averages each line's chroma with its neighbours' (left in the
  // luma as dots), where along the stripes the bandpass takes the line's own.
  const Plane light = makeFrame(0, flat(80.0), 0.0, 0.0);
  const Plane colour = makeFrame(0, flat(20.0), kAmplitude, kTheta);
  auto coloured = [](int32_t r) { return (frameLineOf(r) / 2) % 4 >= 2; };
  Plane frame = light;
  for (int32_t r = 0; r < kRows; r++) {
    if (!coloured(r)) continue;
    for (int32_t h = 0; h < kCols; h++) frame.set(r, h, colour.at(r, h));
  }
  const Plane chroma = FractionalComb::separate(frame, layout(), picture());
  const Plane bp = ntsc_comb::bandpass(frame);

  double fractional = 0.0, threeLine = 0.0;
  forInterior([&](int32_t r, int32_t h) {
    const double truth =
        coloured(r) ? trueChroma(0, r, h, kAmplitude, kTheta) : 0.0;
    const double base =
        (0.5 * bp.at(r, h)) - (0.25 * (bp.at(r - 1, h) + bp.at(r + 1, h)));
    fractional += (chroma.at(r, h) - truth) * (chroma.at(r, h) - truth);
    threeLine += (base - truth) * (base - truth);
  });
  EXPECT_LT(fractional, 0.1 * threeLine);
}

TEST(FractionalCombTest,
     ThinStripesWithDetailAlongThem_KeepTheDetailInTheLuma) {
  // As above, the light stripes carrying fine luma detail along them, the
  // same on each of their lines (lettering on a trim strip): the bandpass
  // would take it for chroma, the comb with the matching line leaves it.
  const LumaFn lettered = [](int32_t, int32_t, int32_t h) {
    return 80.0 + (10.0 * hashUniform(5, 0, h));
  };
  const Plane light = makeFrame(0, lettered, 0.0, 0.0);
  const Plane colour = makeFrame(0, flat(20.0), kAmplitude, kTheta);
  auto coloured = [](int32_t r) { return (frameLineOf(r) / 2) % 4 >= 2; };
  Plane frame = light;
  for (int32_t r = 0; r < kRows; r++) {
    if (!coloured(r)) continue;
    for (int32_t h = 0; h < kCols; h++) frame.set(r, h, colour.at(r, h));
  }
  const Plane chroma = FractionalComb::separate(frame, layout(), picture());
  const Plane bp = ntsc_comb::bandpass(frame);

  double fractional = 0.0, threeLine = 0.0, bandpass = 0.0;
  forInterior([&](int32_t r, int32_t h) {
    const double truth =
        coloured(r) ? trueChroma(0, r, h, kAmplitude, kTheta) : 0.0;
    const double base =
        (0.5 * bp.at(r, h)) - (0.25 * (bp.at(r - 1, h) + bp.at(r + 1, h)));
    fractional += (chroma.at(r, h) - truth) * (chroma.at(r, h) - truth);
    threeLine += (base - truth) * (base - truth);
    bandpass += (bp.at(r, h) - truth) * (bp.at(r, h) - truth);
  });
  EXPECT_LT(fractional, 0.25 * threeLine);
  EXPECT_LT(fractional, 0.25 * bandpass);
}

}  // namespace orc_unit_test
