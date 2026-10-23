/*
 * File:        pal_m_integration_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Integration test for PAL-M support across decoder pipeline
 *
 * Tests that PAL-M video parameters flow correctly through the shared
 * PAL decoder path and produce valid output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <memory>
#include <vector>

#include "../../../../orc/plugins/stages/sinks/common/decoders/componentframe.h"
#include "../../../../orc/plugins/stages/sinks/common/decoders/palcolour.h"
#include "../../../../orc/plugins/stages/sinks/common/decoders/paldecoder.h"
#include "../../../../orc/plugins/stages/sinks/common/decoders/sourcefield.h"

namespace orc_unit_test {
namespace {
// Create standard PAL-M video parameters matching Brazilian PAL-M specs
orc::SourceParameters make_pal_m_video_params() {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::PAL_M;
  p.frame_width_nominal = 909;
  p.active_video_start = 16;
  p.active_video_end = 893;
  p.first_active_frame_line = 0;
  p.last_active_frame_line = 520;  // 525 lines, -1 for 0-indexing adjustment
  p.active_area_cropping_applied = false;
  return p;
}

// Create standard PAL video parameters for comparison
orc::SourceParameters make_pal_video_params() {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::PAL;
  p.frame_width_nominal = 909;
  p.active_video_start = 16;
  p.active_video_end = 893;
  p.first_active_frame_line = 0;
  p.last_active_frame_line = 620;
  p.active_area_cropping_applied = false;
  return p;
}

// Owning wrapper for YC test fields; SourceField holds non-owning pointers.
struct OwnedYCField {
  std::vector<int16_t> luma_buf;
  std::vector<int16_t> chroma_buf;
  SourceField field;

  static OwnedYCField make(bool is_first, uint16_t base_y, uint16_t base_c) {
    OwnedYCField of;
    constexpr int width = 909;
    constexpr int height = 263;

    of.luma_buf.resize(width * height);
    of.chroma_buf.resize(width * height);

    for (int line = 0; line < height; ++line) {
      for (int x = 0; x < width; ++x) {
        const int idx = line * width + x;
        of.luma_buf[idx] = static_cast<int16_t>(base_y + (line % 256));
        of.chroma_buf[idx] = static_cast<int16_t>(base_c + ((x % 4) * 16));
      }
    }

    of.field.is_yc = true;
    of.field.is_first_field = is_first;
    of.field.line_count = static_cast<size_t>(height);
    of.field.samples_per_line = static_cast<size_t>(width);
    of.field.luma_data = of.luma_buf.data();
    of.field.chroma_data = of.chroma_buf.data();
    return of;
  }
};
}  // namespace

// =========================================================================
// PAL-M Decoder Configuration Tests
// =========================================================================

TEST(PalMIntegrationTest, palColourAcceptsPalM) {
  const auto params = make_pal_m_video_params();

  // Create decoder with standard PAL configuration
  PalColour::Configuration pal_config;
  pal_config.chromaFilter = PalColour::palColourFilter;
  PalColour decoder;

  // Configure with PAL-M parameters
  EXPECT_NO_THROW({ decoder.updateConfiguration(params, pal_config); })
      << "PalColour should accept PAL-M parameters";
}

// =========================================================================
// PAL-M vs PAL Parameter Validation Tests
// =========================================================================

TEST(PalMIntegrationTest, palMHasCorrectFieldHeight) {
  const auto pal_params = make_pal_video_params();
  const auto pal_m_params = make_pal_m_video_params();

  // PAL uses 625 lines (313 per field)
  EXPECT_EQ(calculate_padded_field_height(pal_params.system), 313u);
  EXPECT_EQ(
      static_cast<int32_t>(calculate_padded_field_height(pal_params.system)) *
              2 -
          1,
      625);

  // PAL-M uses 525 lines (263 per field)
  EXPECT_EQ(calculate_padded_field_height(pal_m_params.system), 263u);
  EXPECT_EQ(
      static_cast<int32_t>(calculate_padded_field_height(pal_m_params.system)) *
              2 -
          1,
      525);
}

TEST(PalMIntegrationTest, palMHasCorrectFsc) {
  const auto pal_params = make_pal_video_params();
  const auto pal_m_params = make_pal_m_video_params();

  // PAL FSC ~4.43 MHz
  EXPECT_NEAR(orc::fsc_from_system(pal_params.system), 4433618.75, 100.0);

  // PAL-M FSC ~3.58 MHz (distinct from PAL)
  EXPECT_NEAR(orc::fsc_from_system(pal_m_params.system), 3575611.89, 100.0);

  // Verify they are distinctly different
  EXPECT_NE(orc::fsc_from_system(pal_params.system),
            orc::fsc_from_system(pal_m_params.system));
}

// =========================================================================
// PAL-M Decoder Decoding Tests
// =========================================================================

TEST(PalMIntegrationTest, palColourDecodesYcFieldWithPalMParameters) {
  const auto params = make_pal_m_video_params();

  PalColour::Configuration config;
  config.chromaFilter = PalColour::palColourFilter;
  config.chromaGain = 1.0;
  config.chromaPhase = 0.0;

  PalColour decoder;
  decoder.updateConfiguration(params, config);

  // Create test fields
  auto owned_first = OwnedYCField::make(true, 50, 64);
  auto owned_second = OwnedYCField::make(false, 52, 64);
  std::vector<SourceField> input_fields = {owned_first.field,
                                           owned_second.field};

  // Decode frames
  std::vector<ComponentFrame> output_frames(1);
  ASSERT_NO_THROW({ decoder.decodeFrames(input_fields, 0, 2, output_frames); })
      << "Decoding YC fields with PAL-M parameters should not throw";

  // Verify output dimensions match input parameters
  EXPECT_EQ(output_frames[0].getWidth(), params.frame_width_nominal);
  const int32_t expected_height =
      static_cast<int32_t>(calculate_padded_field_height(params.system)) * 2 -
      1;
  EXPECT_EQ(output_frames[0].getHeight(), expected_height);
}

TEST(PalMIntegrationTest, palColourPreservesYcDataPath) {
  const auto params = make_pal_m_video_params();

  PalColour::Configuration config;
  config.chromaFilter = PalColour::palColourFilter;

  PalColour decoder;
  decoder.updateConfiguration(params, config);

  // Create test field with known luma values
  auto owned_first = OwnedYCField::make(true, 100, 64);
  auto owned_second = OwnedYCField::make(false, 102, 64);
  std::vector<SourceField> input_fields = {owned_first.field,
                                           owned_second.field};

  std::vector<ComponentFrame> output_frames(1);
  decoder.decodeFrames(input_fields, 0, 2, output_frames);

  // In YC path, luma should be copied directly (not filtered)
  // Sample a point in the active area
  const double* y_data = output_frames[0].y(10);
  ASSERT_NE(y_data, nullptr);

  // Luma values should be in the expected range
  for (int i = 16; i < 893; ++i) {  // active_video_start to active_video_end
    double luma = y_data[i];
    // Should be in the range of our test data (100-102 + line offset)
    EXPECT_GT(luma, 50.0) << "Luma values should be preserved in YC path";
    EXPECT_LT(luma, 250.0) << "Luma values should be in reasonable range";
  }
}

// =========================================================================
// PAL-M Transform Filter Configuration Tests
// =========================================================================

TEST(PalMIntegrationTest, palColourSupportsTransform2dFilterWithPalM) {
  const auto params = make_pal_m_video_params();

  PalColour::Configuration config;
  config.chromaFilter = PalColour::transform2DFilter;
  config.transformThreshold = 0.4;

  PalColour decoder;
  // Should configure without error
  ASSERT_NO_THROW({ decoder.updateConfiguration(params, config); })
      << "PalColour should support Transform 2D filter with PAL-M";
}

TEST(PalMIntegrationTest, palColourSupportsTransform3dFilterWithPalM) {
  const auto params = make_pal_m_video_params();

  PalColour::Configuration config;
  config.chromaFilter = PalColour::transform3DFilter;
  config.transformThreshold = 0.4;

  PalColour decoder;
  // Should configure without error
  ASSERT_NO_THROW({ decoder.updateConfiguration(params, config); })
      << "PalColour should support Transform 3D filter with PAL-M";
}

// =========================================================================
// PAL-M vs PAL Comparative Tests
// =========================================================================

TEST(PalMIntegrationTest, palAndPalMDecoderConfigureIndependently) {
  const auto pal_params = make_pal_video_params();
  const auto pal_m_params = make_pal_m_video_params();

  PalColour::Configuration config;
  config.chromaFilter = PalColour::palColourFilter;

  PalColour decoder;

  // Configure with PAL
  EXPECT_NO_THROW({ decoder.updateConfiguration(pal_params, config); })
      << "Should configure with PAL parameters";

  // Reconfigure with PAL-M
  EXPECT_NO_THROW({ decoder.updateConfiguration(pal_m_params, config); })
      << "Should reconfigure with PAL-M parameters";

  // Reconfigure back to PAL
  EXPECT_NO_THROW({ decoder.updateConfiguration(pal_params, config); })
      << "Should reconfigure back to PAL parameters";
}

TEST(PalMIntegrationTest, palMDecoderLookAroundValuesMatchPal) {
  // PAL-M should have same look-behind/look-ahead as PAL
  // since they share the same decoder implementation

  PalColour::Configuration pal_config;
  pal_config.chromaFilter = PalColour::palColourFilter;

  PalColour pal_decoder;
  pal_decoder.updateConfiguration(make_pal_video_params(), pal_config);
  auto pal_look_behind = pal_decoder.getConfiguration().getLookBehind();
  auto pal_look_ahead = pal_decoder.getConfiguration().getLookAhead();

  PalColour pal_m_decoder;
  pal_m_decoder.updateConfiguration(make_pal_m_video_params(), pal_config);
  auto pal_m_look_behind = pal_m_decoder.getConfiguration().getLookBehind();
  auto pal_m_look_ahead = pal_m_decoder.getConfiguration().getLookAhead();

  // Should match since both use palColourFilter
  EXPECT_EQ(pal_look_behind, pal_m_look_behind);
  EXPECT_EQ(pal_look_ahead, pal_m_look_ahead);
}
}  // namespace orc_unit_test
