/*
 * File:        tbc_stream_source_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for FixedFormatTBCStreamSourceStage's parameter
 *              descriptors — specifically the black_16b_ire/white_16b_ire
 *              nominal defaults
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../../orc/plugins/stages/tbc_stream_source/tbc_stream_source_stage.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

const orc::ParameterDescriptor& find_descriptor(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::string& name) {
  auto it = std::find_if(
      descriptors.begin(), descriptors.end(),
      [&](const orc::ParameterDescriptor& d) { return d.name == name; });
  if (it == descriptors.end()) {
    ADD_FAILURE() << "No descriptor named '" << name << "'";
    static const orc::ParameterDescriptor empty{};
    return empty;
  }
  return *it;
}

}  // namespace

// black_16b_ire/white_16b_ire used to be required, with no default: a
// filtergraph piping a real .tbc file straight into this stage (the common
// case, since most captures are close to nominal) had no way to omit them.
// Now optional, defaulting to each system's nominal SMPTE/ITU-R level —
// exercised here per concrete subclass since the default is system-specific
// (system_ is only known once the constructor runs).

TEST(TBCStreamSourceStageParametersTest,
     NTSC_BlackWhiteDefaultToNominalNtscLevels_AndAreOptional) {
  orc::NTSCTBCStreamSourceStage stage;
  const auto descriptors =
      stage.get_parameter_descriptors(orc::VideoSystem::NTSC, {});

  const auto& black = find_descriptor(descriptors, "black_16b_ire");
  EXPECT_FALSE(black.constraints.required);
  ASSERT_TRUE(black.constraints.default_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*black.constraints.default_value),
            orc::kTbcNtscBlack);

  const auto& white = find_descriptor(descriptors, "white_16b_ire");
  EXPECT_FALSE(white.constraints.required);
  ASSERT_TRUE(white.constraints.default_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*white.constraints.default_value),
            orc::kTbcNtscWhite);
}

TEST(TBCStreamSourceStageParametersTest,
     PAL_BlackWhiteDefaultToNominalPalLevels_AndAreOptional) {
  orc::PALTBCStreamSourceStage stage;
  const auto descriptors =
      stage.get_parameter_descriptors(orc::VideoSystem::PAL, {});

  const auto& black = find_descriptor(descriptors, "black_16b_ire");
  EXPECT_FALSE(black.constraints.required);
  ASSERT_TRUE(black.constraints.default_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*black.constraints.default_value),
            orc::kTbcPalBlanking);

  const auto& white = find_descriptor(descriptors, "white_16b_ire");
  EXPECT_FALSE(white.constraints.required);
  ASSERT_TRUE(white.constraints.default_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*white.constraints.default_value),
            orc::kTbcPalWhite);
}

// PAL_M follows the NTSC setup-pedestal convention, not PAL's — see
// cvbs_signal_constants.h ("PAL_M signal levels are identical to NTSC") and
// tbc_level_derivation.h's derive_tbc_domain_levels().
TEST(TBCStreamSourceStageParametersTest,
     PALM_BlackWhiteDefaultToNominalNtscLevels_AndAreOptional) {
  orc::PALMTBCStreamSourceStage stage;
  const auto descriptors =
      stage.get_parameter_descriptors(orc::VideoSystem::PAL_M, {});

  const auto& black = find_descriptor(descriptors, "black_16b_ire");
  EXPECT_FALSE(black.constraints.required);
  ASSERT_TRUE(black.constraints.default_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*black.constraints.default_value),
            orc::kTbcNtscBlack);

  const auto& white = find_descriptor(descriptors, "white_16b_ire");
  EXPECT_FALSE(white.constraints.required);
  ASSERT_TRUE(white.constraints.default_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*white.constraints.default_value),
            orc::kTbcNtscWhite);
}

}  // namespace orc_unit_test
