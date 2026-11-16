/*
 * File:        secamdecoder_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the 625-line SECAM chroma decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "../../../../orc/plugins/stages/sinks/common/decoders/secamdecoder.h"

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <optional>
#include <vector>

namespace orc_unit_test {
namespace {

constexpr double kSampleRate = 17734475.0;
constexpr double kFor = 4406250.0;
constexpr double kFob = 4250000.0;
constexpr int32_t kWidth = 1135;
constexpr int32_t kActiveStart = 185;
constexpr int32_t kActiveEnd = 1107;
constexpr int32_t kFieldLines = 313;
// The SECAM preset's 10-bit levels (equal to PAL's).
constexpr int16_t kBlanking = 256;
constexpr int16_t kWhite = 844;

// ITU-R BT.1700 Annex 1 Part C Table 4 item 10d bell gain at frequency f.
double bellGain(double f) {
  const double f0 = 4286.0e3;
  const double F = f / f0 - f0 / f;
  return std::abs(std::complex<double>(1.0, 16.0 * F) /
                  std::complex<double>(1.0, 1.26 * F));
}

// One line of SECAM chroma: undeviated subcarrier from 5.6 µs, then constant
// colour difference D from the active start (no precorrection), amplitude
// shaped by the bell so the decoder's cloche restores M0.
void synthesiseLine(bool drLine, double colourDifference, int16_t luma,
                    int16_t* out) {
  const double m0 = 0.115 * (kWhite - kBlanking);
  const double rest = drLine ? kFor : kFob;
  const double deviation = drLine ? 280.0e3 : 230.0e3;
  const int32_t gateStart = static_cast<int32_t>(5.6e-6 * kSampleRate);
  double phase = 0.0;
  for (int32_t x = 0; x < kWidth; ++x) {
    const double f =
        rest + ((x >= kActiveStart) ? deviation * colourDifference : 0.0);
    const double c = (x >= gateStart && x < kWidth - 26)
                         ? m0 * bellGain(f) * std::cos(phase)
                         : 0.0;
    out[x] = static_cast<int16_t>(std::lround(luma + c));
    phase += 2.0 * M_PI * f / kSampleRate;
  }
}

struct OwnedField {
  std::vector<int16_t> samples;
  SourceField field;
};

// The colour-frame observer's field_phase_id for a field whose even field
// lines carry D'R when evenIsDr: fields 1/2 of sequence frame 1 read 1/2, of
// frame 2 read 3/4, and in frame 1 the even stored frame lines carry D'R
// (field 2 starts on stored line 313, so there its odd field lines do).
int32_t observedFieldPhase(bool isFirst, bool evenIsDr) {
  const bool frameOne = (evenIsDr == isFirst);
  return (frameOne ? 0 : 2) + (isFirst ? 1 : 2);
}

// A composite field whose even field lines carry D'R when evenIsDr.
OwnedField makeField(bool isFirst, bool evenIsDr, double dr, double db,
                     int16_t luma) {
  OwnedField of;
  of.samples.resize(static_cast<size_t>(kWidth * kFieldLines));
  for (int32_t line = 0; line < kFieldLines; ++line) {
    const bool drLine = ((line % 2 == 0) == evenIsDr);
    synthesiseLine(drLine, drLine ? dr : db, luma,
                   of.samples.data() + static_cast<ptrdiff_t>(line * kWidth));
  }
  of.field.is_first_field = isFirst;
  of.field.field_phase_id = observedFieldPhase(isFirst, evenIsDr);
  of.field.line_count = kFieldLines;
  of.field.samples_per_line = kWidth;
  of.field.data = of.samples.data();
  return of;
}

// A composite field with a horizontal colour edge: field lines above
// edgeLine carry colour A (drA/dbA) on luma lumaA, the rest colour B on
// lumaB.
OwnedField makeEdgeField(bool isFirst, bool evenIsDr, int32_t edgeLine,
                         double drA, double dbA, double drB, double dbB,
                         int16_t lumaA = 450, int16_t lumaB = 450) {
  OwnedField of;
  of.samples.resize(static_cast<size_t>(kWidth * kFieldLines));
  for (int32_t line = 0; line < kFieldLines; ++line) {
    const bool drLine = ((line % 2 == 0) == evenIsDr);
    const bool above = line < edgeLine;
    const double d = drLine ? (above ? drA : drB) : (above ? dbA : dbB);
    synthesiseLine(drLine, d, above ? lumaA : lumaB,
                   of.samples.data() + static_cast<ptrdiff_t>(line * kWidth));
  }
  of.field.is_first_field = isFirst;
  of.field.field_phase_id = observedFieldPhase(isFirst, evenIsDr);
  of.field.line_count = kFieldLines;
  of.field.samples_per_line = kWidth;
  of.field.data = of.samples.data();
  return of;
}

orc::SourceParameters makeSecamParams() {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::SECAM;
  p.frame_width_nominal = kWidth;
  p.frame_height = 625;
  p.active_video_start = kActiveStart;
  p.active_video_end = kActiveEnd;
  p.first_active_frame_line = 44;
  p.last_active_frame_line = 620;
  p.blanking_level = kBlanking;
  p.black_level = kBlanking;
  p.white_level = kWhite;
  return p;
}

}  // namespace

TEST(SecamDecoderTest, FrequencyToColourDifference_UsesItem10eDeviation) {
  EXPECT_DOUBLE_EQ(SecamDecoder::frequencyToColourDifference(kFor, true), 0.0);
  EXPECT_DOUBLE_EQ(SecamDecoder::frequencyToColourDifference(kFob, false), 0.0);
  EXPECT_DOUBLE_EQ(
      SecamDecoder::frequencyToColourDifference(kFor + 280.0e3, true), 1.0);
  EXPECT_DOUBLE_EQ(
      SecamDecoder::frequencyToColourDifference(kFob - 230.0e3, false), -1.0);
}

TEST(SecamDecoderTest, LineSequenceFromFieldPhase_FollowsTheTwoFrameSequence) {
  // Sequence frame 1 (ids 1/2): even stored frame lines carry D'R, which are
  // field 1's even lines and field 2's odd lines.  Frame 2 (ids 3/4) inverts.
  EXPECT_EQ(SecamDecoder::lineSequenceFromFieldPhase(1, true), 1);
  EXPECT_EQ(SecamDecoder::lineSequenceFromFieldPhase(2, false), -1);
  EXPECT_EQ(SecamDecoder::lineSequenceFromFieldPhase(3, true), -1);
  EXPECT_EQ(SecamDecoder::lineSequenceFromFieldPhase(4, false), 1);
}

TEST(SecamDecoderTest, LineSequenceFromFieldPhase_UnknownKillsColour) {
  EXPECT_EQ(SecamDecoder::lineSequenceFromFieldPhase(std::nullopt, true), 0);
  EXPECT_EQ(SecamDecoder::lineSequenceFromFieldPhase(-1, false), 0);
  // A PAL id (5-8) is not a SECAM sequence position.
  EXPECT_EQ(SecamDecoder::lineSequenceFromFieldPhase(5, true), 0);
}

TEST(SecamDecoderTest, LineDemodulator_RecoversToneFrequencyAndEnvelope) {
  SecamLineDemodulator demodulator(kSampleRate);
  std::vector<int16_t> line(kWidth);
  const double f = 4.6e6;
  const double m0 = 67.6;
  for (int32_t x = 0; x < kWidth; ++x) {
    line[static_cast<size_t>(x)] = static_cast<int16_t>(std::lround(
        512.0 + m0 * bellGain(f) * std::cos(2.0 * M_PI * f * x / kSampleRate)));
  }
  std::vector<double> chroma;
  std::vector<double> freq;
  std::vector<double> amp;
  demodulator.separateChroma(line.data(), kWidth, chroma);
  demodulator.demodulate(chroma, freq, amp);
  // Integer sample quantisation adds per-sample phase noise, so the frequency
  // is checked as a short-window mean.
  for (int32_t x = 400; x < 800; x += 50) {
    double meanFreq = 0.0;
    for (int32_t i = 0; i < 32; ++i) {
      meanFreq += freq[static_cast<size_t>(x + i)] / 32.0;
    }
    EXPECT_NEAR(meanFreq, f, 3.0e3) << x;
    EXPECT_NEAR(amp[static_cast<size_t>(x)], m0, 0.06 * m0) << x;
  }
}

TEST(SecamDecoderTest, Configure_RejectsNonSecamSystems) {
  SecamDecoder decoder(SecamDecoder::SecamConfiguration{});
  orc::SourceParameters params = makeSecamParams();
  // A PAL-labelled source gets no SECAM sequence from the observer.
  params.system = orc::VideoSystem::PAL;
  EXPECT_FALSE(decoder.configure(params));
  params.system = orc::VideoSystem::NTSC;
  EXPECT_FALSE(decoder.configure(params));
  params.system = orc::VideoSystem::PAL_M;
  EXPECT_FALSE(decoder.configure(params));
  EXPECT_TRUE(decoder.configure(makeSecamParams()));
}

TEST(SecamDecoderTest, DecodeFrames_RecoversRedColourDifferences) {
  // 100 % red: R'−Y' = 0.701, B'−Y' = −0.299 (ITU-R BT.601 luma weights).
  // D'R = −1.902 × 0.701, D'B = 1.505 × −0.299 (BT.1700 Part C item 9).
  const double rMinusY = 0.701;
  const double bMinusY = -0.299;
  const double dr = -1.902 * rMinusY;
  const double db = 1.505 * bMinusY;

  SecamDecoder decoder(SecamDecoder::SecamConfiguration{});
  ASSERT_TRUE(decoder.configure(makeSecamParams()));

  OwnedField first = makeField(true, true, dr, db, 400);
  OwnedField second = makeField(false, false, dr, db, 400);
  std::vector<SourceField> fields = {first.field, second.field};
  std::vector<ComponentFrame> frames(1);
  decoder.decodeFrames(fields, 0, 2, frames);

  const double range = kWhite - kBlanking;
  const double expectedU = 0.492111 * bMinusY * range;
  const double expectedV = 0.877283 * rMinusY * range;
  for (const int32_t y : {100, 101, 300, 301}) {
    const double* u = frames[0].u(y);
    const double* v = frames[0].v(y);
    const double* luma = frames[0].y(y);
    for (int32_t x = 700; x < 1050; x += 70) {
      EXPECT_NEAR(u[x], expectedU, 0.05 * std::abs(expectedV)) << y << "/" << x;
      EXPECT_NEAR(v[x], expectedV, 0.05 * std::abs(expectedV)) << y << "/" << x;
      EXPECT_NEAR(luma[x], 400.0, 6.0) << y << "/" << x;
    }
  }
}

TEST(SecamDecoderTest, CrossColourWeight_PassesIntactSubcarrier) {
  EXPECT_DOUBLE_EQ(SecamDecoder::crossColourWeight(60.0, kFor, 60.0), 1.0);
  EXPECT_DOUBLE_EQ(SecamDecoder::crossColourWeight(70.0, 3.95e6, 60.0), 1.0);
  EXPECT_DOUBLE_EQ(SecamDecoder::crossColourWeight(50.0, 4.70e6, 60.0), 1.0);
}

TEST(SecamDecoderTest, CrossColourWeight_RejectsDisturbedEnvelope) {
  // Luma adding to the carrier (envelope ×2) or cancelling it (×0.5).
  EXPECT_DOUBLE_EQ(SecamDecoder::crossColourWeight(120.0, kFor, 60.0), 0.0);
  EXPECT_DOUBLE_EQ(SecamDecoder::crossColourWeight(30.0, kFor, 60.0), 0.0);
  // Partial fade between the 1.3 and 1.8 envelope ratios.
  const double partial = SecamDecoder::crossColourWeight(93.0, kFor, 60.0);
  EXPECT_GT(partial, 0.0);
  EXPECT_LT(partial, 1.0);
}

TEST(SecamDecoderTest, CrossColourWeight_RejectsFrequencyOutsideItem10eRange) {
  EXPECT_DOUBLE_EQ(SecamDecoder::crossColourWeight(60.0, 3.70e6, 60.0), 0.0);
  EXPECT_DOUBLE_EQ(SecamDecoder::crossColourWeight(60.0, 4.95e6, 60.0), 0.0);
}

TEST(SecamDecoderTest, SpreadWeight_CoversNeighbourhoodOfRejectedSample) {
  std::vector<double> weight(100, 1.0);
  weight[50] = 0.0;
  SecamDecoder::spreadWeight(weight);
  EXPECT_DOUBLE_EQ(weight[0], 1.0);
  EXPECT_DOUBLE_EQ(weight[99], 1.0);
  EXPECT_LT(weight[50], 0.1);
  EXPECT_LT(weight[45], 0.6);
  EXPECT_GT(weight[30], 0.99);
}

// Grey picture (D = 0) with a full-amplitude 4.4 MHz luma burst in the
// middle of every line, like a multiburst packet sitting on the subcarrier.
// Returns the worst |U| + |V| inside the burst.
double decodeLumaBurstFalseColour(bool suppression) {
  SecamDecoder::SecamConfiguration config;
  config.crossColourSuppression = suppression;
  SecamDecoder decoder(config);
  EXPECT_TRUE(decoder.configure(makeSecamParams()));

  OwnedField first = makeField(true, true, 0.0, 0.0, 550);
  OwnedField second = makeField(false, false, 0.0, 0.0, 550);
  const int32_t burstStart = 550;
  const int32_t burstEnd = 750;
  for (OwnedField* of : {&first, &second}) {
    for (int32_t line = 0; line < kFieldLines; ++line) {
      int16_t* samples =
          of->samples.data() + static_cast<ptrdiff_t>(line * kWidth);
      for (int32_t x = burstStart; x < burstEnd; ++x) {
        samples[x] = static_cast<int16_t>(std::lround(
            samples[x] +
            280.0 * std::sin(2.0 * M_PI * 4.4e6 * x / kSampleRate)));
      }
    }
  }
  std::vector<SourceField> fields = {first.field, second.field};
  std::vector<ComponentFrame> frames(1);
  decoder.decodeFrames(fields, 0, 2, frames);

  double worst = 0.0;
  for (const int32_t y : {150, 151, 400, 401}) {
    const double* u = frames[0].u(y);
    const double* v = frames[0].v(y);
    for (int32_t x = burstStart + 20; x < burstEnd - 20; ++x) {
      worst = std::max(worst, std::abs(u[x]) + std::abs(v[x]));
    }
  }
  return worst;
}

TEST(SecamDecoderTest, DecodeFrames_SuppressesCrossColourFromFineLuma) {
  // At most 3 % of the luma range of false colour with suppression on.
  EXPECT_LT(decodeLumaBurstFalseColour(true), 0.03 * (kWhite - kBlanking));
}

TEST(SecamDecoderTest, DecodeFrames_ShowsCrossColourWithSuppressionOff) {
  // The plain wide band-pass decodes the burst as strong false colour.
  EXPECT_GT(decodeLumaBurstFalseColour(false), 0.2 * (kWhite - kBlanking));
}

TEST(SecamDecoderTest, TemporalWeight_FollowsLumaMotion) {
  EXPECT_DOUBLE_EQ(SecamDecoder::temporalWeight(0.0, 588.0), 1.0);
  EXPECT_DOUBLE_EQ(SecamDecoder::temporalWeight(5.0, 588.0), 1.0);
  EXPECT_DOUBLE_EQ(SecamDecoder::temporalWeight(60.0, 588.0), 0.0);
  const double partial = SecamDecoder::temporalWeight(23.5, 588.0);
  EXPECT_GT(partial, 0.0);
  EXPECT_LT(partial, 1.0);
}

TEST(SecamDecoderTest, SceneChangeWeight_FollowsMovingShare) {
  EXPECT_DOUBLE_EQ(SecamDecoder::sceneChangeWeight(0.0), 1.0);
  EXPECT_DOUBLE_EQ(SecamDecoder::sceneChangeWeight(0.2), 1.0);
  EXPECT_DOUBLE_EQ(SecamDecoder::sceneChangeWeight(0.5), 0.0);
  EXPECT_DOUBLE_EQ(SecamDecoder::sceneChangeWeight(1.0), 0.0);
  const double partial = SecamDecoder::sceneChangeWeight(0.35);
  EXPECT_GT(partial, 0.0);
  EXPECT_LT(partial, 1.0);
}

TEST(SecamDecoderTest, CrossColourSuppression_IsOffByDefault) {
  EXPECT_FALSE(SecamDecoder::SecamConfiguration{}.crossColourSuppression);
}

TEST(SecamDecoderTest, GetLookBehind_IsOneFrameOnlyWithTemporalChroma) {
  SecamDecoder::SecamConfiguration config;
  EXPECT_EQ(SecamDecoder(config).getLookBehind(), 1);
  config.temporalChroma = false;
  EXPECT_EQ(SecamDecoder(config).getLookBehind(), 0);
}

// Decodes the frame [startIndex, startIndex + 2) of a still picture with a
// red-over-blue edge, where the frame's first field has even field lines on
// D'R when evenIsDr, and returns the V (R−Y) value of each frame line in a
// window around the edge.
std::vector<double> decodeEdgeV(bool temporal, bool evenIsDr) {
  // Red above (D'R −1.333, D'B −0.45), blue below (D'R +0.217, D'B +1.333).
  const double drA = -1.902 * 0.701, dbA = 1.505 * -0.299;
  const double drB = -1.902 * -0.114, dbB = 1.505 * 0.886;
  const int32_t edge = 150;
  SecamDecoder::SecamConfiguration config;
  config.temporalChroma = temporal;
  SecamDecoder decoder(config);
  EXPECT_TRUE(decoder.configure(makeSecamParams()));

  // Field 2 of a frame continues the line alternation of field 1, and the
  // next frame swaps it (625 lines per frame is odd).
  OwnedField p1 = makeEdgeField(true, !evenIsDr, edge, drA, dbA, drB, dbB);
  OwnedField p2 = makeEdgeField(false, evenIsDr, edge, drA, dbA, drB, dbB);
  OwnedField c1 = makeEdgeField(true, evenIsDr, edge, drA, dbA, drB, dbB);
  OwnedField c2 = makeEdgeField(false, !evenIsDr, edge, drA, dbA, drB, dbB);
  std::vector<SourceField> fields = {p1.field, p2.field, c1.field, c2.field};
  std::vector<ComponentFrame> frames(1);
  decoder.decodeFrames(fields, 2, 4, frames);

  std::vector<double> v;
  for (int32_t y = 2 * edge - 6; y < 2 * edge + 6; ++y) {
    v.push_back(frames[0].v(y)[700]);
  }
  return v;
}

TEST(SecamDecoderTest, DecodeFrames_TemporalChromaKeepsColourEdgeStill) {
  // Spatial reconstruction alone puts the edge one field line apart in the
  // two frame phases; with temporal chroma both phases decode alike.
  const std::vector<double> spatialA = decodeEdgeV(false, true);
  const std::vector<double> spatialB = decodeEdgeV(false, false);
  const std::vector<double> temporalA = decodeEdgeV(true, true);
  const std::vector<double> temporalB = decodeEdgeV(true, false);

  double spatialDifference = 0.0;
  double temporalDifference = 0.0;
  for (size_t i = 0; i < spatialA.size(); ++i) {
    spatialDifference =
        std::max(spatialDifference, std::abs(spatialA[i] - spatialB[i]));
    temporalDifference =
        std::max(temporalDifference, std::abs(temporalA[i] - temporalB[i]));
  }
  const double range = kWhite - kBlanking;
  EXPECT_GT(spatialDifference, 0.2 * range);
  EXPECT_LT(temporalDifference, 0.03 * range);
}

// Decodes a cut: the previous frame is red, bright above field line 220 and
// on luma 400 below it; the current frame is blue on luma 400 throughout, so
// below the edge its luma matches the previous frame's.  Returns the U and V
// of frame lines in the matching region, at one sample.
std::vector<double> decodeCutUV(bool temporal) {
  const double drRed = -1.902 * 0.701, dbRed = 1.505 * -0.299;
  const double drBlue = -1.902 * -0.114, dbBlue = 1.505 * 0.886;
  const int32_t edge = 220;
  SecamDecoder::SecamConfiguration config;
  config.temporalChroma = temporal;
  SecamDecoder decoder(config);
  EXPECT_TRUE(decoder.configure(makeSecamParams()));

  OwnedField p1 =
      makeEdgeField(true, false, edge, drRed, dbRed, drRed, dbRed, 700, 400);
  OwnedField p2 =
      makeEdgeField(false, true, edge, drRed, dbRed, drRed, dbRed, 700, 400);
  OwnedField c1 = makeField(true, true, drBlue, dbBlue, 400);
  OwnedField c2 = makeField(false, false, drBlue, dbBlue, 400);
  std::vector<SourceField> fields = {p1.field, p2.field, c1.field, c2.field};
  std::vector<ComponentFrame> frames(1);
  decoder.decodeFrames(fields, 2, 4, frames);

  std::vector<double> uv;
  for (int32_t y = 2 * edge + 40; y < 2 * edge + 60; ++y) {
    uv.push_back(frames[0].u(y)[700]);
    uv.push_back(frames[0].v(y)[700]);
  }
  return uv;
}

TEST(SecamDecoderTest, DecodeFrames_TemporalChromaTakesNoColourAcrossACut) {
  // Below the edge the luma has not changed, but most of the picture has:
  // the frame is a new scene, and every line's missing colour difference
  // comes from its neighbours, as with temporal chroma off.
  const std::vector<double> temporal = decodeCutUV(true);
  const std::vector<double> spatial = decodeCutUV(false);
  ASSERT_EQ(temporal.size(), spatial.size());
  for (size_t i = 0; i < temporal.size(); ++i) {
    EXPECT_DOUBLE_EQ(temporal[i], spatial[i]) << i;
  }
}

TEST(SecamDecoderTest, DecodeFrames_UnknownFieldPhaseDecodesWithoutColour) {
  // The porch reference is intact, but without the observer's id the decoder
  // does not work the sequence out for itself: luma only.
  SecamDecoder::SecamConfiguration config;
  config.temporalChroma = false;
  SecamDecoder decoder(config);
  ASSERT_TRUE(decoder.configure(makeSecamParams()));
  OwnedField first = makeField(true, true, -1.902 * 0.701, 1.505 * -0.299, 400);
  OwnedField second =
      makeField(false, false, -1.902 * 0.701, 1.505 * -0.299, 400);
  first.field.field_phase_id.reset();
  second.field.field_phase_id = -1;
  std::vector<SourceField> fields = {first.field, second.field};
  std::vector<ComponentFrame> frames(1);
  decoder.decodeFrames(fields, 0, 2, frames);
  for (const int32_t y : {200, 201}) {
    EXPECT_DOUBLE_EQ(frames[0].u(y)[800], 0.0) << y;
    EXPECT_DOUBLE_EQ(frames[0].v(y)[800], 0.0) << y;
    EXPECT_NEAR(frames[0].y(y)[800], 400.0, 6.0) << y;
  }
}

TEST(SecamDecoderTest, DecodeFrames_LineOrderComesFromTheFieldPhase) {
  // Ids naming the other sequence frame swap D'R and D'B: each line's
  // frequency is read against the other rest frequency and deviation, so red
  // decodes as another colour.  The id, not the porch, decides the line
  // order.  (With the right ids both components land within 5 %; see
  // DecodeFrames_RecoversRedColourDifferences.)
  const double dr = -1.902 * 0.701;
  const double db = 1.505 * -0.299;
  SecamDecoder::SecamConfiguration config;
  config.temporalChroma = false;
  SecamDecoder decoder(config);
  ASSERT_TRUE(decoder.configure(makeSecamParams()));
  OwnedField first = makeField(true, true, dr, db, 400);
  OwnedField second = makeField(false, false, dr, db, 400);
  first.field.field_phase_id = observedFieldPhase(true, false);
  second.field.field_phase_id = observedFieldPhase(false, true);
  std::vector<SourceField> fields = {first.field, second.field};
  std::vector<ComponentFrame> frames(1);
  decoder.decodeFrames(fields, 0, 2, frames);
  const double range = kWhite - kBlanking;
  const double expectedU = 0.492111 * -0.299 * range;
  const double expectedV = 0.877283 * 0.701 * range;
  EXPECT_GT(std::abs(frames[0].v(200)[800] - expectedV), 0.2 * expectedV);
  EXPECT_GT(std::abs(frames[0].u(200)[800] - expectedU),
            0.5 * std::abs(expectedU));
}

TEST(SecamDecoderTest, DecodeFrames_OutputsNoColourWithoutSubcarrier) {
  SecamDecoder decoder(SecamDecoder::SecamConfiguration{});
  ASSERT_TRUE(decoder.configure(makeSecamParams()));

  OwnedField first;
  first.samples.assign(static_cast<size_t>(kWidth * kFieldLines), 500);
  first.field.is_first_field = true;
  first.field.line_count = kFieldLines;
  first.field.samples_per_line = kWidth;
  first.field.data = first.samples.data();
  // A known sequence position, so only the missing sub-carrier kills colour.
  first.field.field_phase_id = 1;
  OwnedField second = first;
  second.field.is_first_field = false;
  second.field.field_phase_id = 2;
  second.field.data = second.samples.data();

  std::vector<SourceField> fields = {first.field, second.field};
  std::vector<ComponentFrame> frames(1);
  decoder.decodeFrames(fields, 0, 2, frames);

  const double* u = frames[0].u(200);
  const double* v = frames[0].v(200);
  const double* luma = frames[0].y(200);
  for (int32_t x = 300; x < 1000; x += 100) {
    EXPECT_DOUBLE_EQ(u[x], 0.0);
    EXPECT_DOUBLE_EQ(v[x], 0.0);
    EXPECT_NEAR(luma[x], 500.0, 1e-9);
  }
}

}  // namespace orc_unit_test
