/*
 * File:        frame_content_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the frame content measurements: field
 *              signatures, same-picture and colour-phase tests, and noise
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_content.h>
#include <orc/support/frame_line_util.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::ColourPhaseRelation;
using orc::DropoutRun;
using orc::PictureMatch;
using orc::StackSample;
using orc::VideoSystem;

constexpr double kPi = 3.14159265358979323846;

struct Format {
  VideoSystem system;
  size_t width;
  size_t height;
  size_t frame_samples;
};

const Format kPal{VideoSystem::PAL, 1135, 625, 709379};
const Format kNtsc{VideoSystem::NTSC, 910, 525, 477750};

// Picture value at (field, field line, sample) of a frame, before chroma and
// noise.
using Picture = std::function<double(size_t field, size_t line, size_t n)>;

struct Chroma {
  double u = 0.0;
  double v = 0.0;
};

// A flat frame of |picture| plus a subcarrier of |chroma| at the phase it has
// in frame |index| of a continuous 4FSC recording, plus white noise of
// |sigma| codes.
//   PAL: the V component switches sign every line (ITU-R BT.1700 Annex 1
//   Part B); 625 lines a frame, so every frame on a given line too.
std::vector<StackSample> make_frame(const Format& fmt, size_t index,
                                    const Picture& picture, Chroma chroma,
                                    double sigma, uint32_t seed) {
  std::vector<StackSample> frame(fmt.frame_samples, 0);
  std::mt19937 rng(seed);
  std::normal_distribution<double> noise(0.0, sigma);
  const size_t field1 = orc::field1_line_count(fmt.system);
  for (size_t line = 0; line < fmt.height; ++line) {
    const size_t start =
        orc::frame_line_sample_offset(fmt.system, fmt.width, line);
    const size_t length =
        orc::frame_line_sample_count(fmt.system, fmt.width, line);
    const double v_sign =
        fmt.system == VideoSystem::PAL && (index * fmt.height + line) % 2 ? -1.0
                                                                          : 1.0;
    for (size_t n = 0; n < length; ++n) {
      const size_t s = index * fmt.frame_samples + start + n;
      const double theta = 2.0 * kPi * static_cast<double>(s % 4) / 4.0;
      const size_t field = line < field1 ? 0 : 1;
      const size_t field_line = field == 0 ? line : line - field1;
      double value = picture(field, field_line, n) +
                     chroma.u * std::sin(theta) +
                     v_sign * chroma.v * std::cos(theta);
      if (sigma > 0.0) value += noise(rng);
      frame[start + n] = static_cast<StackSample>(std::lround(value));
    }
  }
  return frame;
}

Picture flat(double level) {
  return [level](size_t, size_t, size_t) { return level; };
}

// Texture with structure at block scale, so a picture is not blank.
Picture textured() {
  return [](size_t, size_t line, size_t n) {
    return 500.0 + 120.0 * std::sin(static_cast<double>(n) / 37.0) *
                       std::cos(static_cast<double>(line) / 23.0);
  };
}

// Texture plus vertical bars every 5 samples (3.5 MHz in PAL), luma detail
// near the subcarrier that a raw chroma band would mistake for colour.
Picture textured_with_fine_bars() {
  const Picture base = textured();
  return [base](size_t field, size_t line, size_t n) {
    return base(field, line, n) + 60.0 * std::sin(2.0 * kPi * n / 5.0);
  };
}

std::array<orc::FieldSignature, 2> signatures(
    const Format& fmt, const std::vector<StackSample>& frame,
    const std::vector<DropoutRun>& dropouts = {}) {
  return orc::build_field_signatures(frame, fmt.system, fmt.width, fmt.height,
                                     dropouts,
                                     orc::standard_picture_levels(fmt.system));
}

orc::ChromaSignature chroma(const Format& fmt,
                            const std::vector<StackSample>& frame) {
  return orc::build_chroma_signature(frame, fmt.system, fmt.width, fmt.height,
                                     {});
}

// Dropout runs flagging samples [first, end) of every active line of both
// fields from field line |from_line| down.
std::vector<DropoutRun> flag_region(const Format& fmt, size_t from_line,
                                    size_t to_line, size_t first, size_t end) {
  std::vector<DropoutRun> runs;
  const size_t field1 = orc::field1_line_count(fmt.system);
  for (size_t field = 0; field < 2; ++field) {
    for (size_t line = from_line; line < to_line; ++line) {
      const size_t flat = field == 0 ? line : field1 + line;
      DropoutRun run;
      run.sample_start =
          orc::frame_line_sample_offset(fmt.system, fmt.width, flat) + first;
      run.sample_count = static_cast<uint32_t>(end - first);
      runs.push_back(run);
    }
  }
  return runs;
}

// ── Field signature ─────────────────────────────────────────────────────────

TEST(FrameContent, Signature_BlockMeans_MatchTheSyntheticLevel) {
  const auto frame = make_frame(kPal, 0, flat(500.0), {}, 5.0, 1);
  const auto sig = signatures(kPal, frame);
  for (size_t field = 0; field < 2; ++field) {
    for (size_t block = 0; block < orc::kSignatureBlocks; ++block) {
      ASSERT_TRUE(sig[field].judged(block));
      // σ = 5 over ~430 box positions moves a block mean by about 0.25.
      EXPECT_NEAR(sig[field].mean[block], 500.0, 1.5);
    }
  }
}

TEST(FrameContent, Signature_ChromaDoesNotMoveTheBlockMeans) {
  const auto plain = make_frame(kPal, 0, flat(500.0), {}, 0.0, 0);
  const auto coloured = make_frame(kPal, 0, flat(500.0), {80.0, 50.0}, 0.0, 0);
  const auto a = signatures(kPal, plain);
  const auto b = signatures(kPal, coloured);
  for (size_t block = 0; block < orc::kSignatureBlocks; ++block) {
    EXPECT_NEAR(a[0].mean[block], b[0].mean[block], 0.5);
  }
}

// ── Same picture ────────────────────────────────────────────────────────────

TEST(FrameContent, SamePicture_IndependentNoise_Matches) {
  const auto a = signatures(kPal, make_frame(kPal, 0, textured(), {}, 5.0, 6));
  const auto b = signatures(kPal, make_frame(kPal, 4, textured(), {}, 5.0, 7));
  for (size_t field = 0; field < 2; ++field) {
    const auto result = orc::same_picture(a[field], b[field], 5.0);
    EXPECT_EQ(result.match, PictureMatch::Same) << "field " << field;
    EXPECT_EQ(result.judged_blocks, orc::kSignatureBlocks);
    EXPECT_EQ(result.failed_blocks, 0U);
    EXPECT_NEAR(result.pair_sigma, 5.0, 0.75);
  }
}

TEST(FrameContent, SamePicture_GainDriftDownTheField_StillMatches) {
  const Picture base = textured();
  // The top of the picture 4 % darker above blanking, fading out by a third
  // of the way down, as repeats on Volcanoes side 1 drift.
  const Picture drifted = [&](size_t field, size_t line, size_t n) {
    const double above = base(field, line, n) - orc::kPalBlanking;
    const double depth = std::max(0.0, 1.0 - static_cast<double>(line) / 100.0);
    return orc::kPalBlanking + above * (1.0 - 0.04 * depth);
  };
  const auto a = signatures(kPal, make_frame(kPal, 0, base, {}, 5.0, 16));
  const auto b = signatures(kPal, make_frame(kPal, 4, drifted, {}, 5.0, 17));
  const auto result = orc::same_picture(a[0], b[0], 5.0);
  EXPECT_EQ(result.match, PictureMatch::Same);
}

TEST(FrameContent, SamePicture_ChangeInOneBlock_IsRejected) {
  const Picture base = textured();
  // A 40-sample by 20-line patch 30 codes brighter, as a cursor would be.
  const Picture changed = [&](size_t field, size_t line, size_t n) {
    const bool patch = line >= 150 && line < 170 && n >= 600 && n < 640;
    return base(field, line, n) + (patch ? 30.0 : 0.0);
  };
  const auto a = signatures(kPal, make_frame(kPal, 0, base, {}, 5.0, 8));
  const auto b = signatures(kPal, make_frame(kPal, 4, changed, {}, 5.0, 9));
  const auto result = orc::same_picture(a[0], b[0], 5.0);
  EXPECT_EQ(result.match, PictureMatch::Different);
  EXPECT_GE(result.failed_blocks, 1U);
  // The patch straddles at most 2 rows and 3 columns of blocks.
  EXPECT_LE(result.failed_blocks, 6U);
}

TEST(FrameContent, SamePicture_UnflaggedSpikeOnTwoLines_IsExcused) {
  const Picture base = textured();
  // A 24-sample streak 300 codes bright on two lines, as an unflagged
  // dropout leaves.
  const Picture spiked = [&](size_t field, size_t line, size_t n) {
    const bool spike = (line == 120 || line == 121) && n >= 500 && n < 524;
    return base(field, line, n) + (spike ? 300.0 : 0.0);
  };
  const auto a = signatures(kPal, make_frame(kPal, 0, base, {}, 5.0, 18));
  const auto b = signatures(kPal, make_frame(kPal, 4, spiked, {}, 5.0, 19));
  const auto result = orc::same_picture(a[0], b[0], 5.0);
  EXPECT_EQ(result.match, PictureMatch::Same);
  EXPECT_GE(result.disturbed_blocks, 1U);
  EXPECT_EQ(result.failed_blocks, 0U);
}

TEST(FrameContent, SamePicture_Fade_IsRejected) {
  const Picture base = textured();
  // 2 % darker above blanking everywhere: four frames of a 200-frame fade.
  const Picture faded = [&](size_t field, size_t line, size_t n) {
    return orc::kPalBlanking +
           (base(field, line, n) - orc::kPalBlanking) * 0.98;
  };
  const auto a = signatures(kPal, make_frame(kPal, 0, base, {}, 5.0, 10));
  const auto b = signatures(kPal, make_frame(kPal, 4, faded, {}, 5.0, 11));
  const auto result = orc::same_picture(a[0], b[0], 5.0);
  EXPECT_NEAR(result.level_change, -0.02, 0.003);
  EXPECT_EQ(result.match, PictureMatch::Different);
}

TEST(FrameContent, SamePicture_ChangeEverywhere_IsRejectedOnItsNoise) {
  const Picture base = textured();
  // Every block moved by a different amount: a cut to a similar picture.
  const Picture other = [&](size_t field, size_t line, size_t n) {
    return base(field, line, n) +
           12.0 * std::sin(static_cast<double>(n) / 11.0 +
                           static_cast<double>(line) / 7.0);
  };
  const auto a = signatures(kPal, make_frame(kPal, 0, base, {}, 5.0, 20));
  const auto b = signatures(kPal, make_frame(kPal, 4, other, {}, 5.0, 21));
  const auto result = orc::same_picture(a[0], b[0], 5.0);
  EXPECT_GT(result.pair_sigma, orc::kSamePictureNoiseRatio * 5.0);
  EXPECT_EQ(result.match, PictureMatch::Different);
}

TEST(FrameContent, SamePicture_BlocksReadingFlaggedSamples_AreNotJudged) {
  const Picture base = textured();
  // A change hidden under a dropout that covers it.
  const Picture changed = [&](size_t field, size_t line, size_t n) {
    const bool hit = line >= 100 && line < 140 && n >= 400 && n < 520;
    return base(field, line, n) + (hit ? 200.0 : 0.0);
  };
  const auto flagged = flag_region(kPal, 100, 140, 400, 520);
  const auto a = signatures(kPal, make_frame(kPal, 0, base, {}, 5.0, 12));
  const auto b =
      signatures(kPal, make_frame(kPal, 4, changed, {}, 5.0, 13), flagged);
  const auto result = orc::same_picture(a[0], b[0], 5.0);
  EXPECT_EQ(result.match, PictureMatch::Same);
  EXPECT_LT(result.judged_blocks, orc::kSignatureBlocks);
}

TEST(FrameContent, SamePicture_TooFewJudgedBlocks_IsUndecided) {
  const auto flagged = flag_region(kPal, 100, 312, 0, 1135);
  const auto a = signatures(kPal, make_frame(kPal, 0, textured(), {}, 5.0, 14));
  const auto b =
      signatures(kPal, make_frame(kPal, 4, textured(), {}, 5.0, 15), flagged);
  const auto result = orc::same_picture(a[0], b[0], 5.0);
  EXPECT_EQ(result.match, PictureMatch::Undecided);
}

// ── Colour phase ────────────────────────────────────────────────────────────

class ColourPhaseTest : public ::testing::Test {
 protected:
  static constexpr Chroma kColour{80.0, 50.0};

  ColourPhaseRelation relation(const Format& fmt, size_t index_a,
                               size_t index_b) {
    const auto a = make_frame(fmt, index_a, textured_with_fine_bars(), kColour,
                              5.0, 20 + static_cast<uint32_t>(index_a));
    const auto b = make_frame(fmt, index_b, textured_with_fine_bars(), kColour,
                              5.0, 40 + static_cast<uint32_t>(index_b));
    return orc::colour_phase_relation(chroma(fmt, a), chroma(fmt, b)).relation;
  }
};

TEST_F(ColourPhaseTest, Pal_FourFramesApart_IsTheSamePhase) {
  EXPECT_EQ(relation(kPal, 0, 4), ColourPhaseRelation::Same);
  EXPECT_EQ(relation(kPal, 3, 11), ColourPhaseRelation::Same);
}

TEST_F(ColourPhaseTest, Pal_OneFrameApart_IsQuadrature) {
  EXPECT_EQ(relation(kPal, 0, 1), ColourPhaseRelation::Quadrature);
  EXPECT_EQ(relation(kPal, 5, 4), ColourPhaseRelation::Quadrature);
}

TEST_F(ColourPhaseTest, Pal_TwoFramesApart_IsOpposite) {
  EXPECT_EQ(relation(kPal, 0, 2), ColourPhaseRelation::Opposite);
}

TEST_F(ColourPhaseTest, Ntsc_TwoFramesApart_IsTheSamePhase) {
  EXPECT_EQ(relation(kNtsc, 0, 2), ColourPhaseRelation::Same);
}

TEST_F(ColourPhaseTest, Ntsc_OneFrameApart_IsOpposite) {
  EXPECT_EQ(relation(kNtsc, 0, 1), ColourPhaseRelation::Opposite);
}

TEST_F(ColourPhaseTest, OneSampleGridSlip_IsNotTheSamePhase) {
  const auto a =
      make_frame(kPal, 0, textured_with_fine_bars(), kColour, 5.0, 60);
  auto b = make_frame(kPal, 4, textured_with_fine_bars(), kColour, 5.0, 61);
  orc::shift_frame_samples(b, kPal.system, kPal.width, kPal.height, 1);
  const auto measure =
      orc::colour_phase_relation(chroma(kPal, a), chroma(kPal, b));
  EXPECT_NE(measure.relation, ColourPhaseRelation::Same);
}

TEST_F(ColourPhaseTest, ChromaFreeFrames_AreUnknown) {
  const auto a = make_frame(kPal, 0, textured_with_fine_bars(), {}, 5.0, 62);
  const auto b = make_frame(kPal, 4, textured_with_fine_bars(), {}, 5.0, 63);
  const auto measure =
      orc::colour_phase_relation(chroma(kPal, a), chroma(kPal, b));
  EXPECT_EQ(measure.relation, ColourPhaseRelation::Unknown);
  EXPECT_LT(measure.expected, orc::kMinChromaCorrelation);
}

TEST(FrameContent, ColourFrameCadence_IsFourForPalAndTwoForNtsc) {
  EXPECT_EQ(orc::colour_frame_cadence(VideoSystem::PAL), 4U);
  EXPECT_EQ(orc::colour_frame_cadence(VideoSystem::PAL_M), 4U);
  EXPECT_EQ(orc::colour_frame_cadence(VideoSystem::NTSC), 2U);
}

// ── Duplicate reads ─────────────────────────────────────────────────────────

TEST(FrameContent, DuplicateRead_BitIdenticalFrames_AreDuplicates) {
  const auto a = make_frame(kPal, 0, textured(), {}, 5.0, 70);
  auto b = a;
  EXPECT_TRUE(orc::is_duplicate_read(a, b));
  EXPECT_EQ(orc::frame_sample_hash(a), orc::frame_sample_hash(b));
  b[400000] = static_cast<StackSample>(b[400000] + 1);
  EXPECT_FALSE(orc::is_duplicate_read(a, b));
  EXPECT_NE(orc::frame_sample_hash(a), orc::frame_sample_hash(b));
}

TEST(FrameContent, SampleHash_ChangesWithAnySample) {
  // An odd length, so the last samples fall outside the word lanes.
  std::vector<StackSample> a(1001, 512);
  for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<StackSample>(i);
  const uint64_t hash = orc::frame_sample_hash(a);
  for (const size_t i : {size_t{0}, size_t{7}, size_t{999}, size_t{1000}}) {
    auto b = a;
    b[i] = static_cast<StackSample>(b[i] ^ 0x100);  // the high byte only
    EXPECT_NE(orc::frame_sample_hash(b), hash) << "sample " << i;
  }
  a.pop_back();
  EXPECT_NE(orc::frame_sample_hash(a), hash);
}

TEST(FrameContent, DuplicateRead_IndependentReadsOfOnePicture_AreNot) {
  const auto a = make_frame(kPal, 0, textured(), {}, 5.0, 71);
  const auto b = make_frame(kPal, 0, textured(), {}, 5.0, 72);
  EXPECT_FALSE(orc::is_duplicate_read(a, b));
}

// ── Noise ───────────────────────────────────────────────────────────────────

class NoiseEstimateTest : public ::testing::TestWithParam<double> {};

TEST_P(NoiseEstimateTest, BlockNoiseMeter_TracksTheSyntheticSigma) {
  const double sigma = GetParam();
  const auto a =
      signatures(kPal, make_frame(kPal, 0, textured(), {}, sigma, 80));
  const auto b =
      signatures(kPal, make_frame(kPal, 4, textured(), {}, sigma, 81));
  orc::BlockNoiseMeter meter;
  meter.add(a[0], b[0]);
  meter.add(a[1], b[1]);
  const auto estimate = meter.sigma();
  ASSERT_TRUE(estimate.has_value());
  EXPECT_NEAR(*estimate, sigma, 0.1 * sigma);
}

TEST_P(NoiseEstimateTest, WithinFrameNoise_TracksTheSyntheticSigma) {
  const double sigma = GetParam();
  // Flat, so that only the noise separates samples 8 apart.
  const auto frame = make_frame(kPal, 0, flat(500.0), {}, sigma, 82);
  const auto estimate =
      orc::within_frame_noise(frame, kPal.system, kPal.width, kPal.height, {});
  ASSERT_TRUE(estimate.has_value());
  EXPECT_NEAR(*estimate, sigma, 0.1 * sigma);
}

INSTANTIATE_TEST_SUITE_P(FrameContent, NoiseEstimateTest,
                         ::testing::Values(5.0, 20.0));

TEST(FrameContent, BlockNoiseMeter_TooFewBlocks_GivesNoEstimate) {
  orc::BlockNoiseMeter meter;
  EXPECT_FALSE(meter.sigma().has_value());
}

}  // namespace
}  // namespace orc_unit_test
