/*
 * File:        frame_plane_conversion_test.cpp
 * Module:      orc-gui-tests
 * Purpose:     The plane payload and the uniforms the conversion shader reads
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/preview/orc_preview_carriers.h>
#include <orc/support/colour_preview_conversion.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "gpu/frame_plane_uniforms.h"

namespace {

using orc::gui::gpu::kFramePlaneUniformFloats;
using orc::gui::gpu::kFrameTransferLutWidth;
using orc::gui::gpu::packFramePlaneUniforms;
using orc::gui::gpu::packFrameTransferLut;
using orc::gui::gpu::packSignalPlaneUniforms;

// The composite modulation factors the conversion divides out, written here
// independently of the payload so that a change to either side is a test
// failure rather than a silent agreement.
// ITU-R BT.470-6 s1.1.2 / EBU Tech. 3280-E s2.1.
constexpr double kCompositeU = 0.49211104112248356308804691718185;
constexpr double kCompositeV = 0.87728321993817866838972487283129;

orc::ColourFrameCarrier makeCarrier(uint32_t width, uint32_t height) {
  orc::ColourFrameCarrier carrier{};
  carrier.width = width;
  carrier.height = height;
  carrier.system = orc::VideoSystem::PAL;
  carrier.cvbs_blanking = 256.0;
  carrier.cvbs_black = 256.0;
  carrier.cvbs_white = 844.0;
  carrier.colorimetry.matrix_coefficients =
      orc::ColorimetricMatrixCoefficients::BT601_625;
  carrier.colorimetry.transfer_characteristics =
      orc::ColorimetricTransferCharacteristics::Gamma22;

  const size_t samples = static_cast<size_t>(width) * height;
  carrier.y_plane.resize(samples);
  carrier.u_plane.resize(samples);
  carrier.v_plane.resize(samples);
  for (size_t i = 0; i < samples; ++i) {
    // A ramp across the picture excursion with chroma swinging through every
    // quadrant, so the matrix, the clamps and the transfer curve all get
    // exercised rather than just the middle of their range.
    const double across =
        static_cast<double>(i % width) / static_cast<double>(width);
    const double down =
        static_cast<double>(i / width) / static_cast<double>(height);
    carrier.y_plane[i] = 200.0 + (across * 700.0);
    carrier.u_plane[i] = std::sin((across + down) * 6.283185307) * 160.0;
    carrier.v_plane[i] = std::cos((across - down) * 6.283185307) * 160.0;
  }
  return carrier;
}

/**
 * @brief The arithmetic of frame_planes.frag, statement for statement.
 *
 * A QRhi::Null device accepts a draw without executing it, so no headless
 * test can read pixels out of the real shader. What can be checked without a
 * device is that the expression the shader evaluates - over the uniforms and
 * the table the surface actually uploads - is the conversion the CPU renderer
 * performs. This is that expression, in the float precision the device works
 * in.
 */
class ShaderModel {
 public:
  explicit ShaderModel(const orc::PreviewPlanes& planes)
      : uniforms_(packFramePlaneUniforms(planes)),
        table_(packFrameTransferLut(*planes.transfer_lut)) {}

  std::array<uint8_t, 3> convert(float y_sample, float u_sample,
                                 float v_sample) const {
    const float y = (y_sample - uniforms_[0]) * uniforms_[1];
    const float b_minus_y = u_sample * uniforms_[2];
    const float r_minus_y = v_sample * uniforms_[3];

    float red = y + r_minus_y;
    float green =
        y - (((uniforms_[5] * b_minus_y) + (uniforms_[4] * r_minus_y)) *
             uniforms_[6]);
    float blue = y + b_minus_y;

    red = std::clamp(red, 0.0F, 1.0F);
    green = std::clamp(green, 0.0F, 1.0F);
    blue = std::clamp(blue, 0.0F, 1.0F);

    return {toUnorm8(encodeTransfer(red)), toUnorm8(encodeTransfer(green)),
            toUnorm8(encodeTransfer(blue))};
  }

 private:
  float node(int index) const {
    const int width = static_cast<int>(uniforms_[8]);
    const int row = index / width;
    const int column = index % width;
    return table_[static_cast<size_t>((row * width) + column)];
  }

  float encodeTransfer(float non_linear) const {
    const int intervals = static_cast<int>(uniforms_[10]);
    const float scaled = non_linear * uniforms_[10];
    const int index = static_cast<int>(scaled);
    if (index >= intervals) {
      return node(intervals);
    }
    const float fraction = scaled - static_cast<float>(index);
    const float low = node(index);
    const float high = node(index + 1);
    return low + (fraction * (high - low));
  }

  /// What a render target of RGBA8 makes of a float colour: round to nearest
  /// over the 0-255 range.
  static uint8_t toUnorm8(float value) {
    const float scaled = std::clamp(value, 0.0F, 1.0F) * 255.0F;
    return static_cast<uint8_t>(std::lround(scaled));
  }

  std::array<float, kFramePlaneUniformFloats> uniforms_;
  std::vector<float> table_;
};

/// The arithmetic of frame_planes_signal.frag, in the same order and the same
/// precision, over the uniforms the surface uploads.
uint8_t shaderGrey(const std::array<float, kFramePlaneUniformFloats>& uniforms,
                   float sample_value) {
  if (uniforms[1] <= 0.0F) {
    return 0;
  }
  float code = std::floor((sample_value - uniforms[0]) * 255.0F / uniforms[1]);
  code = std::clamp(code, 0.0F, 255.0F);
  // code / 255 through an 8-bit UNORM target comes back as the code itself.
  return static_cast<uint8_t>(code);
}

/// scale_10bit_to_8bit(), which is what the shader has to reproduce.
uint8_t cpuGrey(int32_t sample, bool clamped, int32_t black, int32_t white,
                int32_t sync_tip, int32_t peak) {
  const int32_t low = clamped ? black : sync_tip;
  const int32_t high = clamped ? white : peak;
  const int32_t range = high - low;
  if (range <= 0) {
    return 0;
  }
  const int32_t scaled = ((sample - low) * 255) / range;
  return static_cast<uint8_t>(std::clamp(scaled, 0, 255));
}

orc::PreviewPlanes makeSignalPlanes(bool clamped) {
  orc::PreviewPlanes planes;
  planes.domain = orc::PreviewPlaneDomain::Signal;
  planes.width = 4;
  planes.height = 1;
  planes.y_plane.assign(4, 0.0F);
  // PAL normative levels.
  planes.apply_level_scaling = clamped;
  planes.black_level = 256.0F;
  planes.white_level = 844.0F;
  planes.sync_tip_level = 0.0F;
  planes.peak_level = 1023.0F;
  return planes;
}

}  // namespace

namespace orc_unit_test {

// ---------------------------------------------------------------------------
// The payload
// ---------------------------------------------------------------------------

TEST(PreviewPlanePayload, CarriesEverySampleOfTheCarrierNarrowedToFloat) {
  const orc::ColourFrameCarrier carrier = makeCarrier(8, 4);
  const orc::PreviewPlanes planes =
      orc::preview_planes_from_colour_carrier(carrier);

  ASSERT_TRUE(planes.is_valid());
  EXPECT_EQ(planes.domain, orc::PreviewPlaneDomain::Colour);
  EXPECT_EQ(planes.width, carrier.width);
  EXPECT_EQ(planes.height, carrier.height);
  ASSERT_EQ(planes.y_plane.size(), carrier.y_plane.size());

  for (size_t i = 0; i < carrier.y_plane.size(); ++i) {
    EXPECT_FLOAT_EQ(planes.y_plane[i], static_cast<float>(carrier.y_plane[i]));
    EXPECT_FLOAT_EQ(planes.u_plane[i], static_cast<float>(carrier.u_plane[i]));
    EXPECT_FLOAT_EQ(planes.v_plane[i], static_cast<float>(carrier.v_plane[i]));
  }
}

TEST(PreviewPlanePayload, CarriesTheExcursionsTheConversionNormalisesBy) {
  const orc::ColourFrameCarrier carrier = makeCarrier(4, 2);
  const orc::PreviewPlanes planes =
      orc::preview_planes_from_colour_carrier(carrier);

  // Picture black, not blanking, and the picture excursion for both Y and
  // U/V - the anchors render_preview_from_colour_carrier() uses.
  EXPECT_FLOAT_EQ(planes.cvbs_black, 256.0F);
  EXPECT_FLOAT_EQ(planes.y_range, 844.0F - 256.0F);
  EXPECT_FLOAT_EQ(planes.uv_range, planes.y_range);
  EXPECT_FLOAT_EQ(planes.matrix_kr, 0.299F);
  EXPECT_FLOAT_EQ(planes.matrix_kb, 0.114F);
  EXPECT_FLOAT_EQ(planes.composite_u, static_cast<float>(kCompositeU));
  EXPECT_FLOAT_EQ(planes.composite_v, static_cast<float>(kCompositeV));
}

TEST(PreviewPlanePayload, IsEmptyWhenTheCarrierIsInvalid) {
  orc::ColourFrameCarrier carrier = makeCarrier(4, 2);
  carrier.y_plane.pop_back();

  const orc::PreviewPlanes planes =
      orc::preview_planes_from_colour_carrier(carrier);
  EXPECT_EQ(planes.domain, orc::PreviewPlaneDomain::None);
  EXPECT_FALSE(planes.is_valid());
}

TEST(PreviewPlanePayload, SharesOneTransferTablePerCharacteristic) {
  const orc::PreviewPlanes first =
      orc::preview_planes_from_colour_carrier(makeCarrier(2, 2));
  const orc::PreviewPlanes second =
      orc::preview_planes_from_colour_carrier(makeCarrier(4, 4));

  // Identity, not equality: it is what lets a surface skip the upload.
  EXPECT_EQ(first.transfer_lut.get(), second.transfer_lut.get());
  EXPECT_NE(
      first.transfer_lut.get(),
      orc::preview_transfer_lut(orc::ColorimetricTransferCharacteristics::BT709)
          .get());
}

TEST(PreviewPlanePayload, SequentialReorderMovesTheSameRowsAsTheImagePath) {
  const orc::ColourFrameCarrier carrier = makeCarrier(3, 6);

  orc::PreviewImage image = orc::render_preview_from_colour_carrier(carrier);
  orc::PreviewPlanes planes = orc::preview_planes_from_colour_carrier(carrier);
  const std::vector<float> weaved_y = planes.y_plane;

  orc::reorder_preview_image_to_sequential_fields(image);
  orc::reorder_preview_planes_to_sequential_fields(planes);

  // Field 1 (even weaved rows) forms the top block, field 2 the bottom.
  const uint32_t field1_rows = (carrier.height + 1) / 2;
  for (uint32_t row = 0; row < carrier.height; ++row) {
    const uint32_t destination =
        (row % 2 == 0) ? (row / 2) : (field1_rows + (row / 2));
    for (uint32_t x = 0; x < carrier.width; ++x) {
      const size_t from = (static_cast<size_t>(row) * carrier.width) + x;
      const size_t to = (static_cast<size_t>(destination) * carrier.width) + x;
      EXPECT_FLOAT_EQ(planes.y_plane[to], weaved_y[from])
          << "row " << row << " column " << x;
    }
  }
}

// ---------------------------------------------------------------------------
// The uniforms and the table the shader reads
// ---------------------------------------------------------------------------

TEST(FramePlaneUniforms, CarryTheReciprocalsOfTheCarriersOwnExcursions) {
  const orc::PreviewPlanes planes =
      orc::preview_planes_from_colour_carrier(makeCarrier(4, 2));
  const auto uniforms = packFramePlaneUniforms(planes);

  EXPECT_FLOAT_EQ(uniforms[0], planes.cvbs_black);
  EXPECT_FLOAT_EQ(uniforms[1], 1.0F / planes.y_range);
  EXPECT_FLOAT_EQ(uniforms[2], 1.0F / (planes.uv_range * planes.composite_u));
  EXPECT_FLOAT_EQ(uniforms[3], 1.0F / (planes.uv_range * planes.composite_v));
}

TEST(FramePlaneUniforms, CarryTheMatrixTheColorimetryNames) {
  const orc::PreviewPlanes planes =
      orc::preview_planes_from_colour_carrier(makeCarrier(4, 2));
  const auto uniforms = packFramePlaneUniforms(planes);

  EXPECT_FLOAT_EQ(uniforms[4], 0.299F);
  EXPECT_FLOAT_EQ(uniforms[5], 0.114F);
  EXPECT_FLOAT_EQ(uniforms[6], 1.0F / (1.0F - 0.299F - 0.114F));
}

TEST(FramePlaneUniforms, DescribeTheShapeOfTheTransferTable) {
  const orc::PreviewPlanes planes =
      orc::preview_planes_from_colour_carrier(makeCarrier(2, 2));
  const auto uniforms = packFramePlaneUniforms(planes);

  const int entries = static_cast<int>(planes.transfer_lut->size());
  EXPECT_FLOAT_EQ(uniforms[8], static_cast<float>(kFrameTransferLutWidth));
  EXPECT_FLOAT_EQ(uniforms[9],
                  static_cast<float>((entries + kFrameTransferLutWidth - 1) /
                                     kFrameTransferLutWidth));
  // One fewer interval than there are nodes: the shader interpolates between
  // neighbours, as TransferLut::encode() does.
  EXPECT_FLOAT_EQ(uniforms[10], static_cast<float>(entries - 1));
}

TEST(FramePlaneUniforms, SubstituteUnityForAnExcursionOfZero) {
  orc::PreviewPlanes planes;
  planes.domain = orc::PreviewPlaneDomain::Colour;
  planes.y_range = 0.0F;
  planes.uv_range = 0.0F;
  planes.matrix_kr = 0.5F;
  planes.matrix_kb = 0.5F;  // kg == 0

  const auto uniforms = packFramePlaneUniforms(planes);
  for (float value : uniforms) {
    EXPECT_TRUE(std::isfinite(value));
  }
}

TEST(FrameTransferTable, KeepsEveryNodeAtItsOwnIndex) {
  const auto lut = orc::preview_transfer_lut(
      orc::ColorimetricTransferCharacteristics::Gamma22);
  const std::vector<float> packed = packFrameTransferLut(*lut);

  ASSERT_GE(packed.size(), lut->size());
  for (size_t i = 0; i < lut->size(); ++i) {
    EXPECT_FLOAT_EQ(packed[i], (*lut)[i]) << "node " << i;
  }
}

TEST(FrameTransferTable, PadsTheRemainderOfTheRectangleWithTheEndpoint) {
  const auto lut = orc::preview_transfer_lut(
      orc::ColorimetricTransferCharacteristics::Gamma22);
  const std::vector<float> packed = packFrameTransferLut(*lut);

  // Whole rows of texels, so a fetch past the last node reads the curve's
  // endpoint rather than uninitialised memory.
  EXPECT_EQ(packed.size() % static_cast<size_t>(kFrameTransferLutWidth), 0U);
  for (size_t i = lut->size(); i < packed.size(); ++i) {
    EXPECT_FLOAT_EQ(packed[i], lut->back());
  }
}

// ---------------------------------------------------------------------------
// The signal domain
// ---------------------------------------------------------------------------

TEST(SignalPlaneUniforms, CarryTheClampedRangeForAClampedPreview) {
  const auto uniforms = packSignalPlaneUniforms(makeSignalPlanes(true));

  EXPECT_FLOAT_EQ(uniforms[0], 256.0F);
  EXPECT_FLOAT_EQ(uniforms[1], 844.0F - 256.0F);
}

TEST(SignalPlaneUniforms, CarryTheFullExcursionForARawPreview) {
  const auto uniforms = packSignalPlaneUniforms(makeSignalPlanes(false));

  EXPECT_FLOAT_EQ(uniforms[0], 0.0F);
  EXPECT_FLOAT_EQ(uniforms[1], 1023.0F);
}

// The CPU mapping is integer throughout - a multiply by 255 and a truncating
// divide - so this is the assertion that the shader's floor() and its single
// division land on the same code for every sample the domain can hold.
TEST(SignalPlaneConversion, ReproducesTheCpuMappingOverTheWholeDomain) {
  for (const bool clamped : {true, false}) {
    const orc::PreviewPlanes planes = makeSignalPlanes(clamped);
    const auto uniforms = packSignalPlaneUniforms(planes);

    for (int32_t sample = -2048; sample < 3072; ++sample) {
      const uint8_t expected = cpuGrey(sample, clamped, 256, 844, 0, 1023);
      const uint8_t produced = shaderGrey(uniforms, static_cast<float>(sample));
      ASSERT_EQ(produced, expected)
          << (clamped ? "clamped" : "raw") << " sample " << sample;
    }
  }
}

TEST(SignalPlaneConversion, ProducesBlackWhenTheLevelsGiveNoRange) {
  orc::PreviewPlanes planes = makeSignalPlanes(true);
  planes.white_level = planes.black_level;

  const auto uniforms = packSignalPlaneUniforms(planes);
  EXPECT_EQ(shaderGrey(uniforms, 500.0F), 0);
}

// ---------------------------------------------------------------------------
// The conversion itself
// ---------------------------------------------------------------------------

TEST(FramePlaneConversion, ReproducesTheCpuConversionOverAWholeFrame) {
  const orc::ColourFrameCarrier carrier = makeCarrier(160, 120);
  const orc::PreviewImage reference =
      orc::render_preview_from_colour_carrier(carrier);
  const orc::PreviewPlanes planes =
      orc::preview_planes_from_colour_carrier(carrier);
  ASSERT_TRUE(reference.is_valid());
  ASSERT_TRUE(planes.is_valid());

  const ShaderModel shader(planes);

  size_t exact = 0;
  size_t total = 0;
  int worst = 0;
  for (size_t i = 0; i < planes.y_plane.size(); ++i) {
    const std::array<uint8_t, 3> produced =
        shader.convert(planes.y_plane[i], planes.u_plane[i], planes.v_plane[i]);
    for (size_t channel = 0; channel < 3; ++channel) {
      const int expected =
          static_cast<int>(reference.rgb_data[(i * 3) + channel]);
      const int actual = static_cast<int>(produced[channel]);
      worst = std::max(worst, std::abs(expected - actual));
      exact += (expected == actual) ? 1 : 0;
      ++total;
    }
  }

  // The expression is evaluated in single precision here and in double in the
  // CPU renderer, so a sample within a rounding step of a code boundary could
  // in principle land on the neighbouring code. Anything further than that
  // would mean the expression is wrong rather than merely less precise.
  EXPECT_LE(worst, 1);
  // In practice nothing moves at all: every one of this frame's samples comes
  // out on the same code as the double-precision conversion.
  EXPECT_EQ(exact, total);
}

}  // namespace orc_unit_test
