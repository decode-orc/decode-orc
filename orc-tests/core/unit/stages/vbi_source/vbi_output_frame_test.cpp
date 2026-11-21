/*
 * File:        vbi_output_frame_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the emitted frame's lattice and amplitude domain
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_output_frame.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <cmath>
#include <string>

namespace orc {
namespace {

VBIOutputFrame pal_output_frame() {
  VBIOutputFrame frame;
  std::string error;
  EXPECT_TRUE(make_vbi_output_frame(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                    frame, error))
      << error;
  return frame;
}

VBIOutputFrame ntsc_output_frame(VBITeletextSystem tt_system) {
  VBIOutputFrame frame;
  std::string error;
  EXPECT_TRUE(
      make_vbi_output_frame(VBITVSystem::kNTSC, tt_system, frame, error))
      << error;
  return frame;
}

// A SECAM frame is a 625-line PAL frame in every figure but its system
// (ITU-R BT.1700-1 Annex 1 Part C).
TEST(VBIOutputFrame, SECAMFrameIsThePALFrameLabelledSECAM) {
  VBIOutputFrame secam;
  std::string error;
  ASSERT_TRUE(make_vbi_output_frame(VBITVSystem::kSECAM,
                                    VBITeletextSystem::kWST, secam, error))
      << error;
  const VBIOutputFrame pal = pal_output_frame();

  EXPECT_EQ(secam.system, VideoSystem::SECAM);
  EXPECT_EQ(pal.system, VideoSystem::PAL);
  EXPECT_EQ(secam.lines_per_frame, pal.lines_per_frame);
  EXPECT_EQ(secam.samples_per_frame, pal.samples_per_frame);
  EXPECT_EQ(secam.samples_per_line_nominal, pal.samples_per_line_nominal);
  EXPECT_DOUBLE_EQ(secam.sample_rate_hz, pal.sample_rate_hz);
  EXPECT_EQ(secam.levels.blanking, pal.levels.blanking);
  EXPECT_EQ(secam.levels.logic0, pal.levels.logic0);
  EXPECT_EQ(secam.levels.logic1, pal.levels.logic1);
}

// EBU Tech. 3280-E: a 625-line frame sampled at 4 x fsc holds 709 379 samples,
// being 625 lines of 1135.0064 rather than 625 lines of 1135.  A frame short of
// the normative count displaces every frame after it in the file.
TEST(VBIOutputFrame, PALFrameHoldsTheNormativeSampleCount) {
  const VBIOutputFrame frame = pal_output_frame();

  EXPECT_EQ(frame.lines_per_frame, 625u);
  EXPECT_EQ(frame.samples_per_frame, 709379u);
  EXPECT_EQ(frame.samples_per_line_nominal, 1135u);
  EXPECT_DOUBLE_EQ(frame.sample_rate_hz, kPalSampleRate);
}

TEST(VBIOutputFrame, LineOffsetsCoverTheFrameExactlyOnce) {
  const VBIOutputFrame frame = pal_output_frame();

  size_t written = 0;
  for (uint32_t line = 0; line < frame.lines_per_frame; ++line) {
    EXPECT_EQ(frame.line_offset(line), written) << "line " << line;
    written += frame.line_length(line);
  }
  EXPECT_EQ(written, static_cast<size_t>(frame.samples_per_frame));
}

// The lattice takes up its accumulated fraction at the foot of each field,
// which is the host's flat-frame convention and is below every teletext line.
TEST(VBIOutputFrame, TheExtraPALSamplesFallAtTheFootOfEachField) {
  const VBIOutputFrame frame = pal_output_frame();

  for (uint32_t line = 0; line < frame.lines_per_frame; ++line) {
    const size_t expected = (line == 312u || line == 624u) ? 1137u : 1135u;
    EXPECT_EQ(frame.line_length(line), expected) << "line " << line;
  }
}

// Every teletext line starts within a fraction of a sample of its true 0H, so
// placing data at a fixed offset from the line start is right on all of them
// (design §2.3).
TEST(VBIOutputFrame, TeletextLinesSitAtTheirTrue0HToWithinASixthOfASample) {
  const VBIOutputFrame frame = pal_output_frame();

  for (uint32_t line = 6; line <= 334u; ++line) {
    const bool teletext_line = (line <= 21u) || (line >= 319u);
    if (!teletext_line) continue;

    const double true_0h = static_cast<double>(line) * kPalSamplesPerLine;
    const double stored = static_cast<double>(frame.line_offset(line));
    EXPECT_LT(std::abs(stored - true_0h), 1.0 / 6.0) << "line " << line;
  }
}

// ETSI EN 300 706 Section 5: logic 0 at black, logic 1 at 66 % of the
// black-to-white excursion.
TEST(VBIOutputFrame, PALWSTLevelsFollowTheStandard) {
  const VBIOutputFrame frame = pal_output_frame();

  EXPECT_EQ(frame.levels.blanking, kPalBlanking);
  EXPECT_EQ(frame.levels.logic0, kPalBlack);
  EXPECT_EQ(frame.levels.logic1, 644);
  EXPECT_EQ(frame.levels.data_amplitude(), 388);
}

// SMPTE 244M-2003: a 525-line frame sampled at 4 x fsc is orthogonal — 910
// samples on every one of its 525 lines, 477 750 in the frame.
TEST(VBIOutputFrame, NTSCFrameHoldsTheNormativeSampleCount) {
  const VBIOutputFrame frame = ntsc_output_frame(VBITeletextSystem::kWST);

  EXPECT_EQ(frame.system, VideoSystem::NTSC);
  EXPECT_EQ(frame.lines_per_frame, 525u);
  EXPECT_EQ(frame.samples_per_frame, 477750u);
  EXPECT_EQ(frame.samples_per_line_nominal, 910u);
  EXPECT_DOUBLE_EQ(frame.sample_rate_hz, kNtscSampleRate);
}

// NTSC has no non-orthogonal lines, so a line's start is its index times the
// line length and every line is the same length.
TEST(VBIOutputFrame, NTSCLineOffsetsCoverTheFrameExactlyOnce) {
  const VBIOutputFrame frame = ntsc_output_frame(VBITeletextSystem::kWST);

  size_t written = 0;
  for (uint32_t line = 0; line < frame.lines_per_frame; ++line) {
    EXPECT_EQ(frame.line_length(line), 910u) << "line " << line;
    EXPECT_EQ(frame.line_offset(line), written) << "line " << line;
    written += frame.line_length(line);
  }
  EXPECT_EQ(written, static_cast<size_t>(frame.samples_per_frame));
}

// The two services measure their logic levels against different references,
// and on NTSC the two references are 7,5 IRE apart, so the pairing decides the
// amplitude the data is placed at (design §2.2).
TEST(VBIOutputFrame, NTSCLogicLevelsFollowTheConfiguredService) {
  const VBIOutputFrame wst = ntsc_output_frame(VBITeletextSystem::kWST);
  EXPECT_EQ(wst.levels.blanking, kNtscBlanking);
  EXPECT_EQ(wst.levels.logic0, kNtscBlack);
  EXPECT_EQ(wst.levels.logic1, 624);

  const VBIOutputFrame nabts = ntsc_output_frame(VBITeletextSystem::kNABTS);
  EXPECT_EQ(nabts.levels.blanking, kNtscBlanking);
  EXPECT_EQ(nabts.levels.logic0, kNtscBlanking);
  EXPECT_EQ(nabts.levels.logic1, 632);
}

// PAL-M is a 525-line system on a 909-sample lattice, so it is not NTSC with a
// different name and is refused rather than assumed to be one.
TEST(VBIOutputFrame, PALMIsNotEmitted) {
  VBIOutputFrame frame;
  std::string error;

  EXPECT_FALSE(make_vbi_output_frame(VBITVSystem::kPALM,
                                     VBITeletextSystem::kWST, frame, error));
  EXPECT_FALSE(error.empty());
}

// The sample encoding reserves the extremes of the 10-bit word; nothing the
// stage writes may land in either protected range (design §2.2).
TEST(VBIOutputSample, ClampingHoldsEverySampleInsideTheLegalRange) {
  EXPECT_EQ(clamp_vbi_output_sample(-100.0), kVBIOutputSampleMin);
  EXPECT_EQ(clamp_vbi_output_sample(0.0), kVBIOutputSampleMin);
  EXPECT_EQ(clamp_vbi_output_sample(2.0), kVBIOutputSampleMin);
  EXPECT_EQ(clamp_vbi_output_sample(5000.0), kVBIOutputSampleMax);
  EXPECT_EQ(clamp_vbi_output_sample(1019.4), kVBIOutputSampleMax);
}

TEST(VBIOutputSample, ClampingRoundsRatherThanTruncates) {
  EXPECT_EQ(clamp_vbi_output_sample(255.4), 255);
  EXPECT_EQ(clamp_vbi_output_sample(255.6), 256);
  EXPECT_EQ(clamp_vbi_output_sample(644.0), 644);
}

// A value that is not signal takes the same path as an under-range one rather
// than becoming an arbitrary word.
TEST(VBIOutputSample, ANonFiniteValueBecomesTheLowBound) {
  EXPECT_EQ(clamp_vbi_output_sample(std::nan("")), kVBIOutputSampleMin);
  EXPECT_EQ(clamp_vbi_output_sample(-std::numeric_limits<double>::infinity()),
            kVBIOutputSampleMin);
  EXPECT_EQ(clamp_vbi_output_sample(std::numeric_limits<double>::infinity()),
            kVBIOutputSampleMax);
}

}  // namespace
}  // namespace orc
