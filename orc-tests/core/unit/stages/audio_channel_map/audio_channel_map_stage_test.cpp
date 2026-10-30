/*
 * File:        audio_channel_map_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for AudioChannelMapStage (channel-pair delete,
 *              in-place SMPTE 272M mono, mono copy to a target pair, and
 *              L/R averaging where the VBI says mono)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/audio_channel_map/audio_channel_map_stage.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/support/vbi_types.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "../../include/video_frame_representation_artifact_mock.h"

namespace orc_unit_test {
namespace {

using ::testing::NiceMock;
using ::testing::Return;

const orc::ParameterDescriptor* find_descriptor(
    const std::vector<orc::ParameterDescriptor>& descs,
    const std::string& name) {
  auto it = std::find_if(
      descs.begin(), descs.end(),
      [&](const orc::ParameterDescriptor& d) { return d.name == name; });
  return it == descs.end() ? nullptr : &(*it);
}

// Two-pair source: "Analogue" pair 0 and "EFM digital audio" pair 1, each
// carrying two stereo pairs with distinct 24-bit L/R values for frame 0.
std::shared_ptr<NiceMock<MockVideoFrameRepresentationArtifact>>
make_two_pair_source() {
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();
  ON_CALL(*vfr, audio_channel_pair_count()).WillByDefault(Return(2u));
  ON_CALL(*vfr, get_audio_channel_pair_descriptor(0))
      .WillByDefault(Return(orc::AudioChannelPairDescriptor{
          "Analogue", orc::AudioOrigin::ANALOGUE}));
  ON_CALL(*vfr, get_audio_channel_pair_descriptor(1))
      .WillByDefault(Return(orc::AudioChannelPairDescriptor{
          "EFM digital audio", orc::AudioOrigin::EFM}));
  ON_CALL(*vfr, get_audio_samples(0, orc::FrameID(0)))
      .WillByDefault(
          Return(std::vector<int32_t>{100000, 200000, -300000, 400000}));
  ON_CALL(*vfr, get_audio_samples(1, orc::FrameID(0)))
      .WillByDefault(Return(std::vector<int32_t>{5000, 6000, 7000, 8000}));
  return vfr;
}

std::shared_ptr<const orc::VideoFrameRepresentation> run(
    orc::AudioChannelMapStage& stage, orc::ArtifactPtr input,
    const std::map<std::string, orc::ParameterValue>& params) {
  orc::ObservationContext ctx;
  auto outputs = stage.execute({std::move(input)}, params, ctx);
  EXPECT_EQ(outputs.size(), 1u);
  auto output = std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
      outputs[0]);
  EXPECT_NE(output, nullptr);
  return output;
}

TEST(AudioChannelMapStageTest, NodeTypeInfo_ReportsTransformWithOneInput) {
  orc::AudioChannelMapStage stage;
  const auto info = stage.get_node_type_info();

  EXPECT_EQ(info.type, orc::NodeType::TRANSFORM);
  EXPECT_EQ(info.stage_name, "audio_channel_map");
  EXPECT_EQ(stage.required_input_count(), 1u);
  EXPECT_EQ(stage.output_count(), 1u);
}

TEST(AudioChannelMapStageTest, Descriptors_DefaultsRoundTripThroughSetGet) {
  orc::AudioChannelMapStage stage;
  const auto descriptors = stage.get_parameter_descriptors();

  const auto* channel_pair = find_descriptor(descriptors, "channel_pair");
  const auto* operation = find_descriptor(descriptors, "operation");
  const auto* target_pair = find_descriptor(descriptors, "target_pair");
  ASSERT_NE(channel_pair, nullptr);
  ASSERT_NE(operation, nullptr);
  ASSERT_NE(target_pair, nullptr);

  // channel_pair is a string dropdown of the possible container pair slots.
  EXPECT_EQ(channel_pair->type, orc::ParameterType::STRING);
  EXPECT_EQ(channel_pair->constraints.allowed_strings.size(),
            orc::kMaxAudioChannelPairs);
  EXPECT_EQ(std::get<std::string>(*channel_pair->constraints.default_value),
            "0");

  EXPECT_EQ(operation->type, orc::ParameterType::STRING);
  EXPECT_EQ(std::get<std::string>(*operation->constraints.default_value),
            "left_to_mono");
  EXPECT_EQ(operation->constraints.allowed_strings.size(), 6u);

  // target_pair offers "new" plus the slots, and is shown only for the copy
  // ops.
  EXPECT_EQ(target_pair->type, orc::ParameterType::STRING);
  EXPECT_EQ(std::get<std::string>(*target_pair->constraints.default_value),
            "new");
  EXPECT_EQ(target_pair->constraints.allowed_strings.size(),
            orc::kMaxAudioChannelPairs + 1);
  ASSERT_TRUE(target_pair->constraints.depends_on.has_value());
  EXPECT_EQ(target_pair->constraints.depends_on->parameter_name, "operation");
  EXPECT_THAT(
      target_pair->constraints.depends_on->required_values,
      ::testing::ElementsAre("copy_left_to_target", "copy_right_to_target"));

  // set_description is a bool gate hidden only for delete; description is
  // free-form and shown only when set_description is true.
  const auto* set_description = find_descriptor(descriptors, "set_description");
  ASSERT_NE(set_description, nullptr);
  EXPECT_EQ(set_description->type, orc::ParameterType::BOOL);
  EXPECT_FALSE(std::get<bool>(*set_description->constraints.default_value));
  ASSERT_TRUE(set_description->constraints.depends_on.has_value());
  EXPECT_THAT(set_description->constraints.depends_on->required_values,
              ::testing::ElementsAre(
                  "left_to_mono", "right_to_mono", "copy_left_to_target",
                  "copy_right_to_target", "vbi_mono_average"));

  const auto* description = find_descriptor(descriptors, "description");
  ASSERT_NE(description, nullptr);
  EXPECT_EQ(description->type, orc::ParameterType::STRING);
  EXPECT_TRUE(description->constraints.allowed_strings.empty());
  ASSERT_TRUE(description->constraints.depends_on.has_value());
  EXPECT_EQ(description->constraints.depends_on->parameter_name,
            "set_description");
  EXPECT_THAT(description->constraints.depends_on->required_values,
              ::testing::ElementsAre("true"));

  auto params = stage.get_parameters();
  EXPECT_EQ(std::get<std::string>(params.at("channel_pair")), "0");
  EXPECT_EQ(std::get<std::string>(params.at("operation")), "left_to_mono");
  EXPECT_EQ(std::get<std::string>(params.at("target_pair")), "new");
  EXPECT_FALSE(std::get<bool>(params.at("set_description")));
  EXPECT_EQ(std::get<std::string>(params.at("description")), "");

  EXPECT_TRUE(
      stage.set_parameters({{"channel_pair", std::string("3")},
                            {"operation", std::string("copy_right_to_target")},
                            {"target_pair", std::string("2")},
                            {"set_description", true},
                            {"description", std::string("French language")}}));
  params = stage.get_parameters();
  EXPECT_EQ(std::get<std::string>(params.at("channel_pair")), "3");
  EXPECT_EQ(std::get<std::string>(params.at("operation")),
            "copy_right_to_target");
  EXPECT_EQ(std::get<std::string>(params.at("target_pair")), "2");
  EXPECT_TRUE(std::get<bool>(params.at("set_description")));
  EXPECT_EQ(std::get<std::string>(params.at("description")), "French language");
}

TEST(AudioChannelMapStageTest, Description_NamesTheResultPairWhenEnabled) {
  // The bilingual workflow: copy the right channel to a new pair named "French
  // language", then rename the source's left-mono to "English language".
  {
    orc::AudioChannelMapStage stage;
    auto vfr = make_two_pair_source();
    auto output = run(stage, vfr,
                      {{"channel_pair", std::string("0")},
                       {"operation", std::string("copy_right_to_target")},
                       {"target_pair", std::string("new")},
                       {"set_description", true},
                       {"description", std::string("French language")}});
    ASSERT_EQ(output->audio_channel_pair_count(), 3u);
    const auto appended = output->get_audio_channel_pair_descriptor(2);
    ASSERT_TRUE(appended.has_value());
    EXPECT_EQ(appended->name, "French language");
    EXPECT_EQ(appended->origin, orc::AudioOrigin::DERIVED);
    EXPECT_EQ(output->get_audio_samples(2, orc::FrameID(0)),
              (std::vector<int32_t>{200000, 0, 400000, 0}));
  }
  {
    orc::AudioChannelMapStage stage;
    auto vfr = make_two_pair_source();
    auto output = run(stage, vfr,
                      {{"channel_pair", std::string("0")},
                       {"operation", std::string("left_to_mono")},
                       {"set_description", true},
                       {"description", std::string("English language")}});
    const auto desc = output->get_audio_channel_pair_descriptor(0);
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(desc->name, "English language");
    EXPECT_EQ(desc->origin, orc::AudioOrigin::DERIVED);
  }
}

TEST(AudioChannelMapStageTest, Description_KeepsExistingNameWhenDisabled) {
  // With 'Add description' off, the result pair keeps the source's existing
  // name (no override, no derived suffix).
  orc::AudioChannelMapStage stage;
  auto vfr = make_two_pair_source();
  auto output = run(stage, vfr,
                    {{"channel_pair", std::string("0")},
                     {"operation", std::string("left_to_mono")},
                     {"set_description", false},
                     {"description", std::string("ignored")}});
  const auto desc = output->get_audio_channel_pair_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->name, "Analogue");
  EXPECT_EQ(desc->origin, orc::AudioOrigin::DERIVED);
}

TEST(AudioChannelMapStageTest, SetParameters_RejectsInvalidValues) {
  orc::AudioChannelMapStage stage;
  EXPECT_FALSE(stage.set_parameters({{"channel_pair", std::string("bogus")}}));
  EXPECT_FALSE(stage.set_parameters({{"channel_pair", std::string("-1")}}));
  EXPECT_FALSE(stage.set_parameters(
      {{"channel_pair",
        std::to_string(static_cast<int>(orc::kMaxAudioChannelPairs))}}));
  EXPECT_FALSE(stage.set_parameters({{"channel_pair", int32_t{1}}}));
  EXPECT_FALSE(
      stage.set_parameters({{"operation", std::string("swap_channels")}}));
  EXPECT_FALSE(stage.set_parameters({{"target_pair", std::string("bogus")}}));
  // "new" and valid indices are accepted.
  EXPECT_TRUE(stage.set_parameters({{"target_pair", std::string("new")}}));
  EXPECT_TRUE(stage.set_parameters({{"target_pair", std::string("1")}}));
}

TEST(AudioChannelMapStageTest, Execute_ThrowsOnMissingInput) {
  orc::AudioChannelMapStage stage;
  orc::ObservationContext ctx;
  EXPECT_THROW(stage.execute({}, {}, ctx), orc::DAGExecutionError);
}

TEST(AudioChannelMapStageTest, SourcePairOutOfRange_PassesInputThrough) {
  // A pair the input does not carry must not fail the DAG: video (and the
  // untouched audio) has to keep reaching downstream sinks and the preview.
  orc::AudioChannelMapStage stage;
  auto vfr = make_two_pair_source();
  auto output = run(stage, vfr,
                    {{"channel_pair", std::string("5")},
                     {"operation", std::string("left_to_mono")}});

  EXPECT_EQ(output.get(),
            static_cast<const orc::VideoFrameRepresentation*>(vfr.get()));
  EXPECT_EQ(output->audio_channel_pair_count(), 2u);
  EXPECT_EQ(output->get_audio_samples(0, orc::FrameID(0)),
            (std::vector<int32_t>{100000, 200000, -300000, 400000}));
}

TEST(AudioChannelMapStageTest, NoAudioPairs_PassesInputThrough) {
  // The reported case: a TBC source carrying no audio at all.
  orc::AudioChannelMapStage stage;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();
  ON_CALL(*vfr, audio_channel_pair_count()).WillByDefault(Return(0u));

  orc::ObservationContext ctx;
  std::vector<orc::ArtifactPtr> outputs;
  ASSERT_NO_THROW(outputs = stage.execute(
                      {vfr}, {{"channel_pair", std::string("0")}}, ctx));
  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_EQ(outputs[0].get(), static_cast<orc::Artifact*>(vfr.get()));
}

TEST(AudioChannelMapStageTest, LeftToMono_KeepsLeftAndSilencesRight) {
  orc::AudioChannelMapStage stage;
  auto vfr = make_two_pair_source();
  auto output = run(stage, vfr,
                    {{"channel_pair", std::string("0")},
                     {"operation", std::string("left_to_mono")}});

  EXPECT_EQ(output->audio_channel_pair_count(), 2u);
  const auto desc = output->get_audio_channel_pair_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->name, "Analogue");
  EXPECT_EQ(desc->origin, orc::AudioOrigin::DERIVED);

  EXPECT_EQ(output->get_audio_samples(0, orc::FrameID(0)),
            (std::vector<int32_t>{100000, 0, -300000, 0}));
  EXPECT_EQ(output->get_audio_samples(1, orc::FrameID(0)),
            (std::vector<int32_t>{5000, 6000, 7000, 8000}));
}

TEST(AudioChannelMapStageTest, RightToMono_MovesRightToLeftAndSilencesRight) {
  orc::AudioChannelMapStage stage;
  auto vfr = make_two_pair_source();
  auto output = run(stage, vfr,
                    {{"channel_pair", std::string("0")},
                     {"operation", std::string("right_to_mono")}});

  EXPECT_EQ(output->audio_channel_pair_count(), 2u);
  EXPECT_EQ(output->get_audio_samples(0, orc::FrameID(0)),
            (std::vector<int32_t>{200000, 0, 400000, 0}));
  EXPECT_EQ(output->get_audio_samples(1, orc::FrameID(0)),
            (std::vector<int32_t>{5000, 6000, 7000, 8000}));
}

TEST(AudioChannelMapStageTest, CopyLeftToNewTarget_AppendsMonoAndKeepsSource) {
  orc::AudioChannelMapStage stage;
  auto vfr = make_two_pair_source();
  auto output = run(stage, vfr,
                    {{"channel_pair", std::string("0")},
                     {"operation", std::string("copy_left_to_target")},
                     {"target_pair", std::string("new")}});

  // A new pair is appended; source and other pairs are untouched.
  ASSERT_EQ(output->audio_channel_pair_count(), 3u);
  EXPECT_EQ(output->get_audio_samples(0, orc::FrameID(0)),
            (std::vector<int32_t>{100000, 200000, -300000, 400000}));
  EXPECT_EQ(output->get_audio_samples(1, orc::FrameID(0)),
            (std::vector<int32_t>{5000, 6000, 7000, 8000}));
  EXPECT_EQ(output->get_audio_samples(2, orc::FrameID(0)),
            (std::vector<int32_t>{100000, 0, -300000, 0}));

  const auto appended = output->get_audio_channel_pair_descriptor(2);
  ASSERT_TRUE(appended.has_value());
  EXPECT_EQ(appended->name, "Analogue");
  EXPECT_EQ(appended->origin, orc::AudioOrigin::DERIVED);
  // Source descriptor unchanged.
  EXPECT_EQ(output->get_audio_channel_pair_descriptor(0)->name, "Analogue");
}

TEST(AudioChannelMapStageTest, CopyRightToNewTarget_AppendsRightChannelMono) {
  orc::AudioChannelMapStage stage;
  auto vfr = make_two_pair_source();
  auto output = run(stage, vfr,
                    {{"channel_pair", std::string("0")},
                     {"operation", std::string("copy_right_to_target")},
                     {"target_pair", std::string("new")}});

  ASSERT_EQ(output->audio_channel_pair_count(), 3u);
  EXPECT_EQ(output->get_audio_samples(2, orc::FrameID(0)),
            (std::vector<int32_t>{200000, 0, 400000, 0}));
}

TEST(AudioChannelMapStageTest, CopyLeftToExistingTarget_OverwritesTargetOnly) {
  orc::AudioChannelMapStage stage;
  auto vfr = make_two_pair_source();
  auto output = run(stage, vfr,
                    {{"channel_pair", std::string("0")},
                     {"operation", std::string("copy_left_to_target")},
                     {"target_pair", std::string("1")}});

  // No pair added; pair 1 becomes the source's left mono, source untouched.
  ASSERT_EQ(output->audio_channel_pair_count(), 2u);
  EXPECT_EQ(output->get_audio_samples(0, orc::FrameID(0)),
            (std::vector<int32_t>{100000, 200000, -300000, 400000}));
  EXPECT_EQ(output->get_audio_samples(1, orc::FrameID(0)),
            (std::vector<int32_t>{100000, 0, -300000, 0}));

  const auto target = output->get_audio_channel_pair_descriptor(1);
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->name, "Analogue");
  EXPECT_EQ(target->origin, orc::AudioOrigin::DERIVED);
}

TEST(AudioChannelMapStageTest, CopyToTarget_PassesThroughWhenTargetOutOfRange) {
  orc::AudioChannelMapStage stage;
  auto vfr = make_two_pair_source();
  auto output = run(stage, vfr,
                    {{"channel_pair", std::string("0")},
                     {"operation", std::string("copy_left_to_target")},
                     {"target_pair", std::string("5")}});

  // Nothing is appended or overwritten; the input reaches the output as-is.
  EXPECT_EQ(output->audio_channel_pair_count(), 2u);
  EXPECT_EQ(output->get_audio_samples(1, orc::FrameID(0)),
            (std::vector<int32_t>{5000, 6000, 7000, 8000}));
}

TEST(AudioChannelMapStageTest,
     CopyToNewTarget_PassesThroughWhenPairCapReached) {
  orc::AudioChannelMapStage stage;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();
  ON_CALL(*vfr, audio_channel_pair_count())
      .WillByDefault(Return(orc::kMaxAudioChannelPairs));

  auto output = run(stage, vfr,
                    {{"channel_pair", std::string("0")},
                     {"operation", std::string("copy_left_to_target")},
                     {"target_pair", std::string("new")}});

  // The cap still holds: no ninth pair appears.
  EXPECT_EQ(output->audio_channel_pair_count(), orc::kMaxAudioChannelPairs);
}

TEST(AudioChannelMapStageTest, Delete_RemovesTargetAndShiftsLaterPairsDown) {
  orc::AudioChannelMapStage stage;
  auto vfr = make_two_pair_source();
  auto output = run(stage, vfr,
                    {{"channel_pair", std::string("0")},
                     {"operation", std::string("delete")}});

  ASSERT_EQ(output->audio_channel_pair_count(), 1u);
  const auto desc = output->get_audio_channel_pair_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->name, "EFM digital audio");
  EXPECT_EQ(desc->origin, orc::AudioOrigin::EFM);
  EXPECT_EQ(output->get_audio_samples(0, orc::FrameID(0)),
            (std::vector<int32_t>{5000, 6000, 7000, 8000}));

  EXPECT_TRUE(output->get_audio_samples(1, orc::FrameID(0)).empty());
  EXPECT_FALSE(output->get_audio_channel_pair_descriptor(1).has_value());
}

TEST(AudioChannelMapStageTest, Delete_OfLaterPairKeepsEarlierPairIntact) {
  orc::AudioChannelMapStage stage;
  auto vfr = make_two_pair_source();
  auto output = run(stage, vfr,
                    {{"channel_pair", std::string("1")},
                     {"operation", std::string("delete")}});

  ASSERT_EQ(output->audio_channel_pair_count(), 1u);
  EXPECT_EQ(output->get_audio_channel_pair_descriptor(0)->name, "Analogue");
  EXPECT_EQ(output->get_audio_samples(0, orc::FrameID(0)),
            (std::vector<int32_t>{100000, 200000, -300000, 400000}));
}

TEST(AudioChannelMapStageTest, OutOfRangePair_ReturnsEmptyAndNullopt) {
  orc::AudioChannelMapStage stage;
  auto vfr = make_two_pair_source();
  auto output = run(stage, vfr,
                    {{"channel_pair", std::string("0")},
                     {"operation", std::string("left_to_mono")}});

  EXPECT_TRUE(output->get_audio_samples(2, orc::FrameID(0)).empty());
  EXPECT_FALSE(output->get_audio_channel_pair_descriptor(2).has_value());
}

// Programme status observations for one field, as the biphase observer
// writes them.
void set_status(orc::ObservationContext& ctx, uint64_t field, bool parity,
                orc::VbiSoundMode mode, orc::VbiSoundMode am2_mode) {
  const orc::FieldID id(field);
  ctx.set(id, "vbi", "programme_status_parity_valid", int32_t{parity ? 1 : 0});
  ctx.set(id, "vbi", "programme_status_sound_mode", static_cast<int32_t>(mode));
  ctx.set(id, "vbi", "amendment2_status_sound_mode",
          static_cast<int32_t>(am2_mode));
}

constexpr auto kMono = orc::VbiSoundMode::MONO;
constexpr auto kStereo = orc::VbiSoundMode::STEREO;

TEST(AudioChannelMapStageTest, VbiMono_BothFieldsMonoIsMono) {
  orc::ObservationContext ctx;
  set_status(ctx, 6, true, kMono, kMono);
  set_status(ctx, 7, true, kMono, kMono);
  EXPECT_TRUE(orc::vbi_frame_is_mono(ctx, orc::FrameID(3)));
  EXPECT_FALSE(orc::vbi_frame_is_mono(ctx, orc::FrameID(2)));
}

TEST(AudioChannelMapStageTest, VbiMono_OneUnreadFieldStillMono) {
  orc::ObservationContext ctx;
  set_status(ctx, 1, true, kMono, kMono);
  EXPECT_TRUE(orc::vbi_frame_is_mono(ctx, orc::FrameID(0)));
}

TEST(AudioChannelMapStageTest, VbiMono_FieldsDisagreeIsNotMono) {
  orc::ObservationContext ctx;
  set_status(ctx, 0, true, kMono, kMono);
  set_status(ctx, 1, true, kStereo, kStereo);
  EXPECT_FALSE(orc::vbi_frame_is_mono(ctx, orc::FrameID(0)));
}

TEST(AudioChannelMapStageTest, VbiMono_FailedParityIsIgnored) {
  orc::ObservationContext ctx;
  set_status(ctx, 0, false, kMono, kMono);
  EXPECT_FALSE(orc::vbi_frame_is_mono(ctx, orc::FrameID(0)));
  // A failed word does not outvote a good one either.
  set_status(ctx, 1, true, kMono, kMono);
  set_status(ctx, 0, false, kStereo, kStereo);
  EXPECT_TRUE(orc::vbi_frame_is_mono(ctx, orc::FrameID(0)));
}

TEST(AudioChannelMapStageTest, VbiMono_Amendment2MonoCounts) {
  // x34 set: the original table reads an FM-multiplex mode, Amendment 2 a
  // copy-permitted mono disc.
  orc::ObservationContext ctx;
  set_status(ctx, 0, true, orc::VbiSoundMode::STEREO_BILINGUAL, kMono);
  EXPECT_TRUE(orc::vbi_frame_is_mono(ctx, orc::FrameID(0)));
}

TEST(AudioChannelMapStageTest, VbiMono_NoStatusIsNotMono) {
  orc::ObservationContext ctx;
  EXPECT_FALSE(orc::vbi_frame_is_mono(ctx, orc::FrameID(0)));
}

// Interleaved stereo for one frame: |n| pairs of a sine at |left| and
// |right| amplitude, the right channel |phase| radians behind, plus |noise|
// independent samples on each channel.
std::vector<int32_t> stereo(double left, double right, double phase = 0.0,
                            double noise = 0.0, size_t n = 1920) {
  std::vector<int32_t> out;
  uint32_t seed = 12345;
  const auto next_noise = [&] {
    seed = seed * 1664525u + 1013904223u;
    return noise * ((static_cast<double>(seed >> 8) / 8388608.0) - 1.0);
  };
  for (size_t i = 0; i < n; ++i) {
    const double t = 2.0 * 3.14159265358979 * 1000.0 * i / 48000.0;
    out.push_back(static_cast<int32_t>(left * std::sin(t) + next_noise()));
    out.push_back(
        static_cast<int32_t>(right * std::sin(t - phase) + next_noise()));
  }
  return out;
}

constexpr double kLoud = 838861.0;  // -20 dBFS
constexpr double kQuiet = 8389.0;   // -60 dBFS

TEST(AudioChannelMapStageTest, MonoChannels_SameProgrammeAgrees) {
  EXPECT_TRUE(orc::mono_channels_agree(stereo(kLoud, kLoud)));
  // A slightly unequal carrier gain, and noise on each carrier.
  EXPECT_TRUE(orc::mono_channels_agree(stereo(kLoud, kLoud * 0.8, 0.0, 1e5)));
}

TEST(AudioChannelMapStageTest, MonoChannels_OneSilentChannelDisagrees) {
  // The programme on one carrier alone: the mean would halve it.
  EXPECT_FALSE(orc::mono_channels_agree(stereo(0.0, kLoud, 0.0, 100.0)));
  EXPECT_FALSE(orc::mono_channels_agree(stereo(kLoud, kLoud * 0.4)));
}

TEST(AudioChannelMapStageTest, MonoChannels_DifferentProgrammesDisagree) {
  // Equal levels but unrelated (90 degrees apart) or inverted.
  EXPECT_FALSE(orc::mono_channels_agree(stereo(kLoud, kLoud, 1.5708)));
  EXPECT_FALSE(orc::mono_channels_agree(stereo(kLoud, kLoud, 3.14159)));
}

TEST(AudioChannelMapStageTest, MonoChannels_QuietNeedsOnlyBalance) {
  // A silent passage is the carriers' independent noise, uncorrelated; the
  // mean is what makes it quieter.
  EXPECT_TRUE(orc::mono_channels_agree(stereo(0.0, 0.0, 0.0, kQuiet)));
  EXPECT_TRUE(orc::mono_channels_agree(stereo(kQuiet, kQuiet, 1.5708)));
  EXPECT_FALSE(orc::mono_channels_agree(stereo(kQuiet, kQuiet * 0.1)));
}

TEST(AudioChannelMapStageTest, MonoChannels_DigitalSilenceAgrees) {
  EXPECT_TRUE(orc::mono_channels_agree(std::vector<int32_t>(64, 0)));
  EXPECT_FALSE(orc::mono_channels_agree({}));
}

std::shared_ptr<const orc::VideoFrameRepresentation> run_mono_average(
    orc::AudioChannelMapStage& stage, orc::ArtifactPtr input, bool mono,
    int* probes = nullptr) {
  stage.set_mono_probe(
      [mono, probes](const orc::VideoFrameRepresentation&, orc::FrameID) {
        if (probes) ++*probes;
        return mono;
      });
  return run(stage, std::move(input),
             {{"channel_pair", std::string("0")},
              {"operation", std::string("vbi_mono_average")}});
}

// The two-pair source with a mono programme on pair 0: the right carrier a
// little louder and offset, the same programme.
std::shared_ptr<NiceMock<MockVideoFrameRepresentationArtifact>>
make_mono_source() {
  auto vfr = make_two_pair_source();
  ON_CALL(*vfr, get_audio_samples(0, orc::FrameID(0)))
      .WillByDefault(
          Return(std::vector<int32_t>{100000, 120000, -300000, -280000}));
  return vfr;
}

TEST(AudioChannelMapStageTest, VbiMonoAverage_MonoFrameGetsTheMeanOnBoth) {
  orc::AudioChannelMapStage stage;
  auto output = run_mono_average(stage, make_mono_source(), true);

  ASSERT_EQ(output->audio_channel_pair_count(), 2u);
  EXPECT_EQ(output->get_audio_samples(0, orc::FrameID(0)),
            (std::vector<int32_t>{110000, 110000, -290000, -290000}));
  // Other pairs pass through.
  EXPECT_EQ(output->get_audio_samples(1, orc::FrameID(0)),
            (std::vector<int32_t>{5000, 6000, 7000, 8000}));
  // Still the source's analogue programme.
  const auto desc = output->get_audio_channel_pair_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->name, "Analogue");
  EXPECT_EQ(desc->origin, orc::AudioOrigin::ANALOGUE);
}

TEST(AudioChannelMapStageTest, VbiMonoAverage_OtherFramesUntouched) {
  orc::AudioChannelMapStage stage;
  auto output = run_mono_average(stage, make_mono_source(), false);

  EXPECT_EQ(output->get_audio_samples(0, orc::FrameID(0)),
            (std::vector<int32_t>{100000, 120000, -300000, -280000}));
}

TEST(AudioChannelMapStageTest, VbiMonoAverage_DisagreeingChannelsUntouched) {
  // The status says mono but the programme is on the right carrier alone:
  // the frame passes through, and its video is never read for the status.
  auto vfr = make_two_pair_source();
  const auto one_sided = stereo(0.0, kLoud, 0.0, 100.0);
  ON_CALL(*vfr, get_audio_samples(0, orc::FrameID(0)))
      .WillByDefault(Return(one_sided));
  orc::AudioChannelMapStage stage;
  int probes = 0;
  auto output = run_mono_average(stage, vfr, true, &probes);

  EXPECT_EQ(output->get_audio_samples(0, orc::FrameID(0)), one_sided);
  EXPECT_EQ(probes, 0);
}

TEST(AudioChannelMapStageTest, VbiMonoAverage_FullScaleDoesNotOverflow) {
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();
  ON_CALL(*vfr, audio_channel_pair_count()).WillByDefault(Return(1u));
  ON_CALL(*vfr, get_audio_samples(0, orc::FrameID(0)))
      .WillByDefault(
          Return(std::vector<int32_t>{8388607, 8388607, -8388608, -8388608}));
  orc::AudioChannelMapStage stage;
  auto output = run_mono_average(stage, vfr, true);

  EXPECT_EQ(output->get_audio_samples(0, orc::FrameID(0)),
            (std::vector<int32_t>{8388607, 8388607, -8388608, -8388608}));
}

}  // namespace
}  // namespace orc_unit_test
