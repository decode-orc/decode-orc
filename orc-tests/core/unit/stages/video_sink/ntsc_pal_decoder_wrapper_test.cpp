/*
 * File:        ntsc_pal_decoder_wrapper_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit test(s) for NTSC/PAL decoder wrapper configuration and
 *              decoding
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <functional>
#include <vector>

#include "../../../../orc/plugins/stages/sinks/common/decoders/comb.h"
#include "../../../../orc/plugins/stages/sinks/common/decoders/monodecoder.h"
#include "../../../../orc/plugins/stages/sinks/common/decoders/ntscdecoder.h"
#include "../../../../orc/plugins/stages/sinks/common/decoders/palcolour.h"
#include "../../../../orc/plugins/stages/sinks/common/decoders/paldecoder.h"

namespace orc_unit_test {
namespace {
orc::SourceParameters make_ntsc_params() {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::NTSC;
  p.frame_width_nominal = 32;
  return p;
}

orc::SourceParameters make_pal_params() {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::PAL;
  p.frame_width_nominal = 32;
  return p;
}

// Video parameters with an active region, as required to decode rather than
// merely configure.
orc::SourceParameters make_ntsc_decode_params() {
  orc::SourceParameters p = make_ntsc_params();
  p.active_video_start = 16;
  p.active_video_end = 24;
  p.first_active_frame_line = 0;
  p.last_active_frame_line = 6;
  return p;
}

orc::SourceParameters make_pal_decode_params() {
  orc::SourceParameters p = make_pal_params();
  p.active_video_start = 16;
  p.active_video_end = 24;
  p.first_active_frame_line = 0;
  p.last_active_frame_line = 6;
  return p;
}

// Owning wrapper: holds the sample buffers and the non-owning SourceField
// that points into them.
struct OwnedField {
  std::vector<int16_t> composite_buf;
  std::vector<int16_t> luma_buf;
  std::vector<int16_t> chroma_buf;
  SourceField field;

  static OwnedField makeYc(bool is_first_field, int16_t luma_base,
                           int16_t chroma_base, int width, int height) {
    OwnedField of;
    of.field.is_yc = true;
    of.field.is_first_field = is_first_field;
    of.field.field_phase_id = is_first_field ? 1 : 2;
    of.field.line_count = static_cast<size_t>(height);
    of.field.samples_per_line = static_cast<size_t>(width);

    of.luma_buf.reserve(static_cast<size_t>(width * height));
    of.chroma_buf.reserve(static_cast<size_t>(width * height));
    for (int line = 0; line < height; ++line) {
      for (int x = 0; x < width; ++x) {
        of.luma_buf.push_back(static_cast<int16_t>(luma_base + line * 4 + x));
        of.chroma_buf.push_back(
            static_cast<int16_t>(chroma_base + ((x % 4) * 10)));
      }
    }
    of.field.luma_data = of.luma_buf.data();
    of.field.chroma_data = of.chroma_buf.data();
    return of;
  }

  static OwnedField makeComposite(bool is_first_field, int16_t base, int width,
                                  int height) {
    OwnedField of;
    of.field.is_yc = false;
    of.field.is_first_field = is_first_field;
    of.field.field_phase_id = is_first_field ? 1 : 2;
    of.field.line_count = static_cast<size_t>(height);
    of.field.samples_per_line = static_cast<size_t>(width);

    of.composite_buf.reserve(static_cast<size_t>(width * height));
    for (int line = 0; line < height; ++line) {
      for (int x = 0; x < width; ++x) {
        of.composite_buf.push_back(static_cast<int16_t>(base + line * 8 + x));
      }
    }
    of.field.data = of.composite_buf.data();
    return of;
  }
};

// Decodes separate Y/C input the explicit way: split each field into a
// luma-only and a chroma-only composite view, decode luma with a mono decoder
// and chroma with the colour decoder, then merge the luma plane into the colour
// frame. The wrapper's own Y/C handling must match this element for element.
void decode_yc_reference(
    const std::vector<SourceField>& inputFields, int32_t startIndex,
    int32_t endIndex, const orc::SourceParameters& videoParameters,
    MonoDecoder& lumaDecoder,
    const std::function<void(const std::vector<SourceField>&, int32_t, int32_t,
                             std::vector<ComponentFrame>&)>& decodeChroma,
    std::vector<ComponentFrame>& output) {
  std::vector<SourceField> lumaFields;
  std::vector<SourceField> chromaFields;
  lumaFields.reserve(inputFields.size());
  chromaFields.reserve(inputFields.size());

  for (const auto& field : inputFields) {
    SourceField yField = field;
    yField.is_yc = false;
    yField.data = field.luma_data;
    yField.luma_data = nullptr;
    yField.chroma_data = nullptr;
    yField.line_ptrs = field.luma_line_ptrs;
    yField.luma_line_ptrs.clear();
    yField.chroma_line_ptrs.clear();
    lumaFields.push_back(std::move(yField));

    SourceField cField = field;
    cField.is_yc = false;
    cField.data = field.chroma_data;
    cField.luma_data = nullptr;
    cField.chroma_data = nullptr;
    cField.line_ptrs = field.chroma_line_ptrs;
    cField.luma_line_ptrs.clear();
    cField.chroma_line_ptrs.clear();
    chromaFields.push_back(std::move(cField));
  }

  std::vector<ComponentFrame> lumaFrames(output.size());
  lumaDecoder.decodeFrames(lumaFields, startIndex, endIndex, lumaFrames);
  decodeChroma(chromaFields, startIndex, endIndex, output);

  for (size_t i = 0; i < output.size(); i++) {
    output[i].merge_luma_from(lumaFrames[i]);
  }
}

// Byte-identical comparison of two component frames over their full extent.
void expect_frames_equal(const ComponentFrame& a, const ComponentFrame& b) {
  ASSERT_EQ(a.getWidth(), b.getWidth());
  ASSERT_EQ(a.getHeight(), b.getHeight());
  const int32_t h = a.getHeight();
  for (int32_t line = 0; line < h; ++line) {
    const double* ay = a.y(line);
    const double* by = b.y(line);
    const double* au = a.u(line);
    const double* bu = b.u(line);
    const double* av = a.v(line);
    const double* bv = b.v(line);
    for (int32_t x = 0; x < a.getWidth(); ++x) {
      EXPECT_EQ(ay[x], by[x]) << "Y line " << line << " x " << x;
      EXPECT_EQ(au[x], bu[x]) << "U line " << line << " x " << x;
      EXPECT_EQ(av[x], bv[x]) << "V line " << line << " x " << x;
    }
  }
}
}  // namespace

TEST(NtscDecoderWrapperTest, Configure_AcceptsNtscAndRejectsPal) {
  Comb::Configuration config;
  NtscDecoder decoder(config);

  EXPECT_TRUE(decoder.configure(make_ntsc_params()));
  EXPECT_FALSE(decoder.configure(make_pal_params()));
}

TEST(NtscDecoderWrapperTest, Look_AroundFollowsCombConfiguration) {
  Comb::Configuration config;
  config.dimensions = 3;
  NtscDecoder decoder(config);

  EXPECT_EQ(decoder.getLookBehind(), 1);
  EXPECT_EQ(decoder.getLookAhead(), 1);
}

TEST(PalDecoderWrapperTest, Configure_AcceptsPalAndRejectsNtsc) {
  PalColour::Configuration config;
  PalDecoder decoder(config);

  EXPECT_TRUE(decoder.configure(make_pal_params()));
  EXPECT_FALSE(decoder.configure(make_ntsc_params()));
}

TEST(PalDecoderWrapperTest, Look_AroundDependsOnPalFilterMode) {
  PalColour::Configuration config_2d;
  config_2d.chromaFilter = PalColour::transform2DFilter;
  PalDecoder decoder_2d(config_2d);

  EXPECT_EQ(decoder_2d.getLookBehind(), 0);
  EXPECT_EQ(decoder_2d.getLookAhead(), 0);

  PalColour::Configuration config_3d;
  config_3d.chromaFilter = PalColour::transform3DFilter;
  PalDecoder decoder_3d(config_3d);

  EXPECT_GT(decoder_3d.getLookBehind(), 0);
  EXPECT_GT(decoder_3d.getLookAhead(), 0);
}

TEST(NtscDecoderWrapperTest, Configure_RejectsInvalidGeometry) {
  Comb::Configuration config;
  NtscDecoder decoder(config);

  auto params = make_ntsc_params();
  params.frame_width_nominal = 8;

  EXPECT_FALSE(decoder.configure(params));
}

TEST(PalDecoderWrapperTest, Configure_RejectsInvalidGeometry) {
  PalColour::Configuration config;
  PalDecoder decoder(config);

  auto params = make_pal_params();
  params.frame_width_nominal = 8;

  EXPECT_FALSE(decoder.configure(params));
}

TEST(NtscDecoderWrapperTest, DecodeFramesCompositePath_ProducesDecodedFrame) {
  Comb::Configuration config;
  config.dimensions = 2;
  config.phaseCompensation = false;

  NtscDecoder decoder(config);
  ASSERT_TRUE(decoder.configure(make_ntsc_decode_params()));

  auto first_owned = OwnedField::makeComposite(true, 1000, 32, 4);
  auto second_owned = OwnedField::makeComposite(false, 2000, 32, 4);
  std::vector<SourceField> fields = {first_owned.field, second_owned.field};
  std::vector<ComponentFrame> output(1);

  decoder.decodeFrames(fields, 0, 2, output);

  EXPECT_EQ(output[0].getWidth(), 32);
  EXPECT_EQ(output[0].getHeight(), 525);
}

TEST(NtscDecoderWrapperTest, DecodeFramesYcPath_PreservesLumaInActiveRegion) {
  Comb::Configuration config;
  config.dimensions = 2;
  config.phaseCompensation = false;

  NtscDecoder decoder(config);
  ASSERT_TRUE(decoder.configure(make_ntsc_decode_params()));

  auto first_owned = OwnedField::makeYc(true, 1000, 2000, 32, 4);
  auto second_owned = OwnedField::makeYc(false, 3000, 4000, 32, 4);
  std::vector<SourceField> fields = {first_owned.field, second_owned.field};
  std::vector<ComponentFrame> output(1);

  decoder.decodeFrames(fields, 0, 2, output);

  // Y/C luma must survive the split-decode-merge round trip unchanged.
  const double* line0 = output[0].y(0);
  const double* line1 = output[0].y(1);

  EXPECT_DOUBLE_EQ(line0[16],
                   static_cast<double>(first_owned.field.luma_data[16]));
  EXPECT_DOUBLE_EQ(line0[20],
                   static_cast<double>(first_owned.field.luma_data[20]));
  EXPECT_DOUBLE_EQ(line1[16],
                   static_cast<double>(second_owned.field.luma_data[16]));
  EXPECT_DOUBLE_EQ(line1[20],
                   static_cast<double>(second_owned.field.luma_data[20]));
}

TEST(PalDecoderWrapperTest, DecodeFramesYcPath_PreservesLumaInActiveRegion) {
  PalColour::Configuration config;
  config.chromaFilter = PalColour::palColourFilter;

  PalDecoder decoder(config);
  ASSERT_TRUE(decoder.configure(make_pal_decode_params()));

  auto first_owned = OwnedField::makeYc(true, 1000, 2000, 32, 4);
  auto second_owned = OwnedField::makeYc(false, 3000, 4000, 32, 4);
  std::vector<SourceField> fields = {first_owned.field, second_owned.field};
  std::vector<ComponentFrame> output(1);

  decoder.decodeFrames(fields, 0, 2, output);

  const double* line0 = output[0].y(0);
  const double* line1 = output[0].y(1);

  EXPECT_DOUBLE_EQ(line0[16],
                   static_cast<double>(first_owned.field.luma_data[16]));
  EXPECT_DOUBLE_EQ(line1[16],
                   static_cast<double>(second_owned.field.luma_data[16]));
}

// After a failed reconfigure the decoder must refuse, not decode with the
// previous system's stale configuration.
TEST(NtscDecoderWrapperTest,
     DecodeFrames_AfterFailedReconfigure_DoesNotDecode) {
  Comb::Configuration config;
  config.dimensions = 2;

  NtscDecoder decoder(config);
  ASSERT_TRUE(decoder.configure(make_ntsc_decode_params()));
  ASSERT_FALSE(decoder.configure(make_pal_params()));

  auto first_owned = OwnedField::makeComposite(true, 1000, 32, 4);
  auto second_owned = OwnedField::makeComposite(false, 2000, 32, 4);
  std::vector<SourceField> fields = {first_owned.field, second_owned.field};
  std::vector<ComponentFrame> output(1);

  decoder.decodeFrames(fields, 0, 2, output);

  // Left untouched: a default-constructed ComponentFrame has extent -1.
  EXPECT_EQ(output[0].getWidth(), -1);
  EXPECT_EQ(output[0].getHeight(), -1);
}

TEST(PalDecoderWrapperTest, DecodeFrames_AfterFailedReconfigure_DoesNotDecode) {
  PalColour::Configuration config;
  config.chromaFilter = PalColour::palColourFilter;

  PalDecoder decoder(config);
  ASSERT_TRUE(decoder.configure(make_pal_decode_params()));
  ASSERT_FALSE(decoder.configure(make_ntsc_params()));

  auto first_owned = OwnedField::makeComposite(true, 1000, 32, 4);
  auto second_owned = OwnedField::makeComposite(false, 2000, 32, 4);
  std::vector<SourceField> fields = {first_owned.field, second_owned.field};
  std::vector<ComponentFrame> output(1);

  decoder.decodeFrames(fields, 0, 2, output);

  EXPECT_EQ(output[0].getWidth(), -1);
  EXPECT_EQ(output[0].getHeight(), -1);
}

// NtscDecoder's Y/C handling must produce the same frame, element for element,
// as decoding luma and chroma separately and merging them.
TEST(NtscDecoderWrapperTest, DecodeFramesYcPath_MatchesSeparateLumaChroma) {
  const auto params = make_ntsc_decode_params();

  Comb::Configuration combConfig;
  combConfig.dimensions = 2;
  combConfig.phaseCompensation = false;

  auto first_owned = OwnedField::makeYc(true, 1000, 2000, 32, 4);
  auto second_owned = OwnedField::makeYc(false, 3000, 4000, 32, 4);
  std::vector<SourceField> fields = {first_owned.field, second_owned.field};

  NtscDecoder wrapper(combConfig);
  ASSERT_TRUE(wrapper.configure(params));
  std::vector<ComponentFrame> wrapperOut(1);
  wrapper.decodeFrames(fields, 0, 2, wrapperOut);

  MonoDecoder::MonoConfiguration lumaConfig;
  lumaConfig.filterChroma = false;
  lumaConfig.videoParameters = params;
  MonoDecoder lumaDecoder(lumaConfig);
  Comb chroma;
  chroma.updateConfiguration(params, combConfig);
  std::vector<ComponentFrame> referenceOut(1);
  decode_yc_reference(
      fields, 0, 2, params, lumaDecoder,
      [&chroma](const std::vector<SourceField>& f, int32_t s, int32_t e,
                std::vector<ComponentFrame>& o) {
        chroma.decodeFrames(f, s, e, o);
      },
      referenceOut);

  expect_frames_equal(wrapperOut[0], referenceOut[0]);
}

// PalDecoder's Y/C handling must produce the same frame, element for element,
// as decoding luma and chroma separately and merging them.
TEST(PalDecoderWrapperTest, DecodeFramesYcPath_MatchesSeparateLumaChroma) {
  const auto params = make_pal_decode_params();

  PalColour::Configuration palConfig;
  palConfig.chromaFilter = PalColour::palColourFilter;

  auto first_owned = OwnedField::makeYc(true, 1000, 2000, 32, 4);
  auto second_owned = OwnedField::makeYc(false, 3000, 4000, 32, 4);
  std::vector<SourceField> fields = {first_owned.field, second_owned.field};

  PalDecoder wrapper(palConfig);
  ASSERT_TRUE(wrapper.configure(params));
  std::vector<ComponentFrame> wrapperOut(1);
  wrapper.decodeFrames(fields, 0, 2, wrapperOut);

  MonoDecoder::MonoConfiguration lumaConfig;
  lumaConfig.filterChroma = false;
  lumaConfig.videoParameters = params;
  MonoDecoder lumaDecoder(lumaConfig);
  PalColour chroma;
  chroma.updateConfiguration(params, palConfig);
  std::vector<ComponentFrame> referenceOut(1);
  decode_yc_reference(
      fields, 0, 2, params, lumaDecoder,
      [&chroma](const std::vector<SourceField>& f, int32_t s, int32_t e,
                std::vector<ComponentFrame>& o) {
        chroma.decodeFrames(f, s, e, o);
      },
      referenceOut);

  expect_frames_equal(wrapperOut[0], referenceOut[0]);
}
}  // namespace orc_unit_test
