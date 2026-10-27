/*
 * File:        decoder_schedule_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the video sink's per-frame-range decoder
 *              schedule and its parameters
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/plugins/stages/sinks/common/decoder_schedule.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../../../../orc/plugins/stages/sinks/common/video_sink_stage.h"

namespace orc_unit_test {
namespace {

using orc::chroma_sink::DecoderSchedule;
using orc::chroma_sink::parse_decoder_schedule;

DecoderSchedule parse_ok(const std::string& spec) {
  auto result = parse_decoder_schedule(spec);
  EXPECT_TRUE(result.schedule.has_value()) << spec << ": " << result.error;
  return result.schedule.value_or(DecoderSchedule{});
}

std::string decoder_at(const DecoderSchedule& schedule, orc::FrameID id) {
  const std::string* type = schedule.decoder_for(id);
  return type ? *type : std::string("<none>");
}

const orc::ParameterDescriptor* find_parameter(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::string& name) {
  auto it = std::find_if(descriptors.begin(), descriptors.end(),
                         [&name](const orc::ParameterDescriptor& descriptor) {
                           return descriptor.name == name;
                         });
  return (it == descriptors.end()) ? nullptr : &(*it);
}

bool has_string(const std::vector<std::string>& values,
                const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

TEST(DecoderScheduleTest, Parse_RangesAndDefaultSelectPerFrame) {
  const auto schedule = parse_ok("transform3d, 100-199:transform2d, 250:mono");

  EXPECT_EQ(decoder_at(schedule, 0), "transform3d");
  EXPECT_EQ(decoder_at(schedule, 99), "transform3d");
  EXPECT_EQ(decoder_at(schedule, 100), "transform2d");
  EXPECT_EQ(decoder_at(schedule, 199), "transform2d");
  EXPECT_EQ(decoder_at(schedule, 200), "transform3d");
  EXPECT_EQ(decoder_at(schedule, 250), "mono");
  EXPECT_EQ(decoder_at(schedule, 251), "transform3d");
}

TEST(DecoderScheduleTest, Parse_SortsRangesGivenOutOfOrder) {
  const auto schedule = parse_ok("10-19:ntsc3d,0-9:ntsc2d");

  ASSERT_EQ(schedule.ranges.size(), 2U);
  EXPECT_EQ(schedule.ranges[0].first, 0U);
  EXPECT_EQ(schedule.ranges[1].first, 10U);
  EXPECT_EQ(decoder_at(schedule, 5), "ntsc2d");
  EXPECT_EQ(decoder_at(schedule, 15), "ntsc3d");
}

TEST(DecoderScheduleTest, DecoderFor_IsNullOutsideRangesWithoutDefault) {
  const auto schedule = parse_ok("10-19:pal2d");

  EXPECT_EQ(schedule.decoder_for(9), nullptr);
  EXPECT_EQ(decoder_at(schedule, 10), "pal2d");
  EXPECT_EQ(schedule.decoder_for(20), nullptr);
}

TEST(DecoderScheduleTest, DecoderTypes_ListsEachDecoderOnce) {
  const auto schedule =
      parse_ok("pal2d,0-9:transform3d,20-29:transform3d,40:pal2d");

  EXPECT_EQ(schedule.decoder_types(),
            (std::vector<std::string>{"pal2d", "transform3d"}));
}

TEST(DecoderScheduleTest, Single_DecodesEveryFrameWithOneDecoder) {
  const auto schedule = DecoderSchedule::single("ntsc3d");

  EXPECT_EQ(decoder_at(schedule, 0), "ntsc3d");
  EXPECT_EQ(decoder_at(schedule, 123456), "ntsc3d");
  EXPECT_FALSE(schedule.first_uncovered({0, 1000}).has_value());
}

TEST(DecoderScheduleTest, FirstUncovered_FindsGapsAndTail) {
  const auto schedule = parse_ok("0-9:pal2d,10-19:mono,25-30:pal2d");

  EXPECT_FALSE(schedule.first_uncovered({0, 19}).has_value());
  EXPECT_EQ(schedule.first_uncovered({0, 30}), orc::FrameID{20});
  EXPECT_EQ(schedule.first_uncovered({25, 31}), orc::FrameID{31});
  EXPECT_FALSE(schedule.first_uncovered({26, 29}).has_value());
  EXPECT_EQ(schedule.first_uncovered({21, 22}), orc::FrameID{21});
}

TEST(DecoderScheduleTest, FirstUncovered_DefaultCoversEverything) {
  const auto schedule = parse_ok("5-6:pal2d,transform2d");

  EXPECT_FALSE(schedule.first_uncovered({0, 100}).has_value());
}

TEST(DecoderScheduleTest, Parse_HeldEntryDecodesHeldFramesOutsideRanges) {
  const auto schedule =
      parse_ok("transform2d, held:transform3d, 100-199:pal2d");
  EXPECT_EQ(schedule.held_decoder, "transform3d");

  const auto at = [&schedule](orc::FrameID id, bool held) {
    const std::string* type = schedule.decoder_for(id, held);
    return type ? *type : std::string("<none>");
  };
  EXPECT_EQ(at(10, false), "transform2d");
  EXPECT_EQ(at(10, true), "transform3d");
  // A hand-written range overrules the held decoder.
  EXPECT_EQ(at(150, true), "pal2d");
  EXPECT_EQ(schedule.decoder_types(),
            (std::vector<std::string>{"transform2d", "transform3d", "pal2d"}));
}

TEST(DecoderScheduleTest, Parse_HeldEntryNeedsADefaultAndAppearsOnce) {
  EXPECT_FALSE(parse_decoder_schedule("held:transform3d").schedule);
  EXPECT_FALSE(parse_decoder_schedule("0-99:pal2d, held:transform3d").schedule);
  EXPECT_FALSE(
      parse_decoder_schedule("pal2d, held:transform3d, held:transform2d")
          .schedule);
  EXPECT_FALSE(parse_decoder_schedule("pal2d, held:bogus").schedule);
}

TEST(DecoderScheduleTest, Parse_RejectsMalformedSchedules) {
  for (const auto* spec :
       {"", " , ", "0-9:pal4d", "0-9", "a-9:pal2d", "9-0:pal2d",
        "pal2d,transform2d", "0-9:pal2d,5-12:mono", "0-9:multiple", "-5:pal2d",
        "5-:pal2d"}) {
    const auto result = parse_decoder_schedule(spec);
    EXPECT_FALSE(result.schedule.has_value()) << "'" << spec << "'";
    EXPECT_FALSE(result.error.empty()) << "'" << spec << "'";
  }
}

TEST(DecoderScheduleTest, Parse_ErrorsNameEntriesNotStoredFrameNumbers) {
  // Stored frame numbers are 0-based, which a user never sees, so an error
  // must not quote them.
  const auto result = parse_decoder_schedule("0-9:pal2d, 5-12:mono");

  ASSERT_FALSE(result.schedule.has_value());
  EXPECT_NE(result.error.find("entries 1 and 2"), std::string::npos)
      << result.error;
}

TEST(DecoderScheduleTest, StageDescriptors_OfferMultipleAndGateSchedule) {
  orc::VideoSinkStage stage;

  for (const auto system : {orc::VideoSystem::PAL, orc::VideoSystem::NTSC}) {
    const auto descriptors =
        stage.get_parameter_descriptors(system, orc::SourceType::Composite);

    const auto* decoder_type = find_parameter(descriptors, "decoder_type");
    ASSERT_NE(decoder_type, nullptr);
    EXPECT_TRUE(
        has_string(decoder_type->constraints.allowed_strings, "multiple"));

    const auto* schedule = find_parameter(descriptors, "decoder_schedule");
    ASSERT_NE(schedule, nullptr);
    EXPECT_EQ(schedule->type, orc::ParameterType::STRING);
    EXPECT_TRUE(schedule->constraints.allowed_strings.empty());
    ASSERT_TRUE(schedule->constraints.depends_on.has_value());
    EXPECT_EQ(schedule->constraints.depends_on->parameter_name, "decoder_type");
    EXPECT_EQ(schedule->constraints.depends_on->required_values,
              (std::vector<std::string>{"multiple"}));
  }
}

TEST(DecoderScheduleTest, StageDescriptors_KeepDecoderSettingsForMultiple) {
  // A schedule can name any decoder, so every decoder-specific setting stays
  // editable while decoder_type is "multiple".
  orc::VideoSinkStage stage;
  const auto pal = stage.get_parameter_descriptors(orc::VideoSystem::PAL,
                                                   orc::SourceType::Composite);
  const auto ntsc = stage.get_parameter_descriptors(orc::VideoSystem::NTSC,
                                                    orc::SourceType::Composite);

  for (const auto* name :
       {"simple_pal", "transform_threshold", "secam_temporal_chroma",
        "secam_cross_colour_suppression", "secam_field_identification"}) {
    const auto* descriptor = find_parameter(pal, name);
    ASSERT_NE(descriptor, nullptr) << name;
    ASSERT_TRUE(descriptor->constraints.depends_on.has_value()) << name;
    EXPECT_TRUE(has_string(descriptor->constraints.depends_on->required_values,
                           "multiple"))
        << name;
  }
  for (const auto* name : {"chroma_weight", "adapt_threshold"}) {
    const auto* descriptor = find_parameter(ntsc, name);
    ASSERT_NE(descriptor, nullptr) << name;
    ASSERT_TRUE(descriptor->constraints.depends_on.has_value()) << name;
    EXPECT_TRUE(has_string(descriptor->constraints.depends_on->required_values,
                           "multiple"))
        << name;
  }
}

TEST(DecoderScheduleTest, StageParameters_RoundTripSchedule) {
  orc::VideoSinkStage stage;
  ASSERT_TRUE(stage.set_parameters(
      {{"decoder_type", std::string("multiple")},
       {"decoder_schedule", std::string("transform3d,100-199:transform2d")}}));

  const auto params = stage.get_parameters();
  EXPECT_EQ(std::get<std::string>(params.at("decoder_type")), "multiple");
  EXPECT_EQ(std::get<std::string>(params.at("decoder_schedule")),
            "transform3d,100-199:transform2d");
}

TEST(DecoderScheduleTest, StageStatus_UnusableScheduleLeavesStageUnconfigured) {
  orc::VideoSinkStage stage;
  ASSERT_TRUE(
      stage.set_parameters({{"output_path", std::string("out.mkv")},
                            {"decoder_type", std::string("multiple")},
                            {"decoder_schedule", std::string("0-9:pal4d")}}));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Yellow);

  ASSERT_TRUE(stage.set_parameters(
      {{"decoder_schedule", std::string("pal2d,0-9:transform3d")}}));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Green);

  // A schedule left behind is not read once another decoder is chosen.
  ASSERT_TRUE(
      stage.set_parameters({{"decoder_type", std::string("pal2d")},
                            {"decoder_schedule", std::string("nonsense")}}));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Green);
}

}  // namespace orc_unit_test
