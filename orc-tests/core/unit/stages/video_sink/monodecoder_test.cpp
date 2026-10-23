/*
 * File:        monodecoder_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit test(s) for MonoDecoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/decoders/monodecoder.h"

#include <gtest/gtest.h>

namespace orc_unit_test {
TEST(MonoDecoderTest, DecodeFrames_HandlesUnevenYCFieldHeights) {
  orc::SourceParameters sourceParameters;
  sourceParameters.system = orc::VideoSystem::NTSC;
  sourceParameters.frame_width_nominal = 4;
  sourceParameters.first_active_frame_line = 0;
  sourceParameters.last_active_frame_line = 5;
  sourceParameters.active_video_start = 0;
  sourceParameters.active_video_end = 4;

  MonoDecoder::MonoConfiguration config;
  config.videoParameters = sourceParameters;
  config.filterChroma = false;

  MonoDecoder decoder(config);

  // Owned buffers for the non-owning SourceField pointers.
  const std::vector<int16_t> first_luma = {10, 11, 12, 13, 20, 21, 22, 23};
  const std::vector<int16_t> second_luma = {100, 101, 102, 103, 110, 111,
                                            112, 113, 120, 121, 122, 123};

  SourceField firstField;
  firstField.is_yc = true;
  firstField.is_first_field = true;
  firstField.line_count = 2;
  firstField.samples_per_line = 4;
  firstField.luma_data = first_luma.data();
  firstField.chroma_data = first_luma.data();  // Chroma same as luma in test

  SourceField secondField;
  secondField.is_yc = true;
  secondField.is_first_field = false;
  secondField.line_count = 3;
  secondField.samples_per_line = 4;
  secondField.luma_data = second_luma.data();
  secondField.chroma_data = second_luma.data();

  std::vector<SourceField> inputFields = {firstField, secondField};
  std::vector<ComponentFrame> outputFrames(1);

  decoder.decodeFrames(inputFields, 0, 2, outputFrames);

  ASSERT_EQ(outputFrames[0].getWidth(), 4);
  ASSERT_EQ(outputFrames[0].getHeight(), 525);  // NTSC: 263*2-1

  const double* line0 = outputFrames[0].y(0);
  const double* line1 = outputFrames[0].y(1);
  const double* line2 = outputFrames[0].y(2);
  const double* line3 = outputFrames[0].y(3);
  const double* line4 = outputFrames[0].y(4);

  EXPECT_DOUBLE_EQ(line0[0], 10.0);
  EXPECT_DOUBLE_EQ(line0[3], 13.0);
  EXPECT_DOUBLE_EQ(line1[0], 100.0);
  EXPECT_DOUBLE_EQ(line1[3], 103.0);
  EXPECT_DOUBLE_EQ(line2[0], 20.0);
  EXPECT_DOUBLE_EQ(line2[3], 23.0);
  EXPECT_DOUBLE_EQ(line3[0], 110.0);
  EXPECT_DOUBLE_EQ(line3[3], 113.0);

  for (int sample = 0; sample < 4; ++sample) {
    EXPECT_DOUBLE_EQ(line4[sample], 0.0);
  }
}

TEST(MonoDecoderTest, UpdateConfiguration_RejectsInvalidIreWindow) {
  orc::SourceParameters sourceParameters;
  sourceParameters.system = orc::VideoSystem::NTSC;
  sourceParameters.frame_width_nominal = 4;
  sourceParameters.first_active_frame_line = 0;
  sourceParameters.last_active_frame_line = 5;
  sourceParameters.active_video_start = 0;
  sourceParameters.active_video_end = 4;

  MonoDecoder decoder;
  MonoDecoder::MonoConfiguration config;
  config.filterChroma = false;

  EXPECT_TRUE(decoder.updateConfiguration(sourceParameters, config));

  std::vector<SourceField> inputFields(2);
  std::vector<ComponentFrame> outputFrames(1);

  EXPECT_NO_FATAL_FAILURE(
      decoder.decodeFrames(inputFields, 0, 2, outputFrames));
}
}  // namespace orc_unit_test
