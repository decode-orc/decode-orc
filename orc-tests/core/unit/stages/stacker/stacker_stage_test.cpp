/*
 * File:        stacker_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for StackerStage defaults and parameter validation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/stacker/stacker_stage.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/support/frame_line_util.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "../../mocks/mock_video_frame_representation.h"

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

namespace orc_unit_test {

namespace {
const orc::ParameterDescriptor* find_descriptor(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::string& name) {
  auto it = std::find_if(descriptors.begin(), descriptors.end(),
                         [&](const orc::ParameterDescriptor& descriptor) {
                           return descriptor.name == name;
                         });

  return it == descriptors.end() ? nullptr : &(*it);
}
}  // namespace

TEST(StackerStageTest, RequiredInputCount_IsOne) {
  orc::StackerStage stage;
  EXPECT_EQ(stage.required_input_count(), 1u);
}

TEST(StackerStageTest, OutputCount_IsOne) {
  orc::StackerStage stage;
  EXPECT_EQ(stage.output_count(), 1u);
}

TEST(StackerStageTest, NodeTypeInfo_HasExpectedMetadata) {
  orc::StackerStage stage;
  auto info = stage.get_node_type_info();

  EXPECT_EQ(info.type, orc::NodeType::MERGER);
  EXPECT_EQ(info.stage_name, "stacker");
  EXPECT_EQ(info.compatible_formats, orc::VideoFormatCompatibility::ALL);
}

TEST(StackerStageTest, Descriptor_DefaultsMatchRuntimeDefaults) {
  orc::StackerStage stage;
  const auto descriptors = stage.get_parameter_descriptors();
  const auto params = stage.get_parameters();

  const auto* mode = find_descriptor(descriptors, "mode");
  const auto* no_diff_dod = find_descriptor(descriptors, "no_diff_dod");
  const auto* passthrough = find_descriptor(descriptors, "passthrough");
  const auto* audio_stacking = find_descriptor(descriptors, "audio_stacking");
  const auto* efm_stacking = find_descriptor(descriptors, "efm_stacking");

  ASSERT_NE(mode, nullptr);
  ASSERT_NE(no_diff_dod, nullptr);
  ASSERT_NE(passthrough, nullptr);
  ASSERT_NE(audio_stacking, nullptr);
  ASSERT_NE(efm_stacking, nullptr);

  if (!mode->constraints.default_value.has_value() ||
      !no_diff_dod->constraints.default_value.has_value() ||
      !passthrough->constraints.default_value.has_value() ||
      !audio_stacking->constraints.default_value.has_value() ||
      !efm_stacking->constraints.default_value.has_value()) {
    FAIL() << "Expected all descriptors to have default values";
    return;
  }

  EXPECT_EQ(std::get<std::string>(*mode->constraints.default_value),
            std::get<std::string>(params.at("mode")));
  EXPECT_EQ(std::get<bool>(*no_diff_dod->constraints.default_value),
            std::get<bool>(params.at("no_diff_dod")));
  EXPECT_EQ(std::get<bool>(*passthrough->constraints.default_value),
            std::get<bool>(params.at("passthrough")));
  EXPECT_EQ(std::get<std::string>(*audio_stacking->constraints.default_value),
            std::get<std::string>(params.at("audio_stacking")));
  EXPECT_EQ(std::get<std::string>(*efm_stacking->constraints.default_value),
            std::get<std::string>(params.at("efm_stacking")));
}

TEST(StackerStageTest, SetParameters_AcceptsValidStringValues) {
  orc::StackerStage stage;

  const bool result =
      stage.set_parameters({{"mode", std::string("Median")},
                            {"no_diff_dod", true},
                            {"passthrough", true},
                            {"audio_stacking", std::string("Median")},
                            {"efm_stacking", std::string("Disabled")}});
  const auto params = stage.get_parameters();

  EXPECT_TRUE(result);
  EXPECT_EQ(std::get<std::string>(params.at("mode")), "Median");
  EXPECT_TRUE(std::get<bool>(params.at("no_diff_dod")));
  EXPECT_TRUE(std::get<bool>(params.at("passthrough")));
  EXPECT_EQ(std::get<std::string>(params.at("audio_stacking")), "Median");
  EXPECT_EQ(std::get<std::string>(params.at("efm_stacking")), "Disabled");
}

TEST(StackerStageTest, SetParameters_AcceptsLegacyIntegerMode) {
  orc::StackerStage stage;

  ASSERT_TRUE(stage.set_parameters({{"mode", int32_t(1)}}));

  EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("mode")), "Median");
}

// Auto and Smart Mean are deprecated and no longer offered; a caller that
// still sets one, as a string or as its legacy integer, gets Weighted.
TEST(StackerStageTest, SetParameters_ReadsTheDeprecatedModesAsWeighted) {
  const std::vector<orc::ParameterValue> deprecated = {
      std::string("Auto"), std::string("Smart Mean"), int32_t(-1), int32_t(2)};
  for (const auto& mode : deprecated) {
    orc::StackerStage stage;
    ASSERT_TRUE(stage.set_parameters({{"mode", std::string("Mean")}}));
    ASSERT_TRUE(stage.set_parameters({{"mode", mode}}));
    EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("mode")),
              "Weighted");
  }
}

TEST(StackerStageTest, Descriptor_NoLongerOffersTheDeprecatedModes) {
  orc::StackerStage stage;
  const auto descriptors = stage.get_parameter_descriptors();
  const auto* mode = find_descriptor(descriptors, "mode");

  ASSERT_NE(mode, nullptr);
  const auto& allowed = mode->constraints.allowed_strings;
  EXPECT_EQ(std::find(allowed.begin(), allowed.end(), "Auto"), allowed.end());
  EXPECT_EQ(std::find(allowed.begin(), allowed.end(), "Smart Mean"),
            allowed.end());
  EXPECT_EQ(find_descriptor(descriptors, "smart_threshold"), nullptr);
}

// The Smart Mean threshold is no longer a parameter; a stored value is
// accepted and ignored, so older projects still load.
TEST(StackerStageTest, SetParameters_IgnoresTheSmartMeanThreshold) {
  orc::StackerStage stage;
  EXPECT_TRUE(stage.set_parameters({{"smart_threshold", int32_t(129)}}));
  EXPECT_EQ(stage.get_parameters().count("smart_threshold"), 0u);
}

TEST(StackerStageTest, SetParameters_RejectsInvalidMode) {
  orc::StackerStage stage;
  EXPECT_FALSE(stage.set_parameters({{"mode", std::string("Nope")}}));
}

TEST(StackerStageTest, Process_ReturnsNullWhenSourcesEmpty) {
  orc::StackerStage stage;
  EXPECT_EQ(stage.process({}), nullptr);
}

TEST(StackerStageTest, Process_ReturnsOnlySourceInPassthroughMode) {
  orc::StackerStage stage;
  auto source = std::make_shared<MockVideoFrameRepresentation>();

  EXPECT_CALL(*source, has_separate_channels()).WillRepeatedly(Return(false));

  std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>> sources = {
      source};

  const auto result = stage.process(sources);

  EXPECT_EQ(result.get(), source.get());
}

TEST(StackerStageTest, Process_ReturnsWrappedOutputForMultipleSources) {
  orc::StackerStage stage;
  auto src0 = std::make_shared<MockVideoFrameRepresentation>();
  auto src1 = std::make_shared<MockVideoFrameRepresentation>();

  EXPECT_CALL(*src0, has_separate_channels()).WillRepeatedly(Return(false));
  EXPECT_CALL(*src1, has_separate_channels()).WillRepeatedly(Return(false));

  std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>> sources = {
      src0, src1};

  const auto result = stage.process(sources);

  ASSERT_NE(result, nullptr);
  EXPECT_NE(result.get(), src0.get());
  EXPECT_NE(result.get(), src1.get());
}

namespace {

// Minimal in-memory composite source: one frame filled with a constant value.
// NTSC geometry keeps every line at kStackWidth samples.
using sample_type = orc::VideoFrameRepresentation::sample_type;
constexpr size_t kStackWidth = 16;
constexpr size_t kStackHeight = 8;

class FakeConstantSource : public orc::VideoFrameRepresentation {
 public:
  explicit FakeConstantSource(sample_type value)
      : frame_(kStackWidth * kStackHeight, value) {}

  orc::FrameIDRange frame_range() const override {
    return {orc::FrameID{0}, orc::FrameID{0}};
  }
  size_t frame_count() const override { return 1; }
  bool has_frame(orc::FrameID id) const override {
    return id == orc::FrameID{0};
  }

  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    orc::FrameDescriptor desc;
    desc.frame_id = id;
    desc.system = orc::VideoSystem::NTSC;
    desc.height = kStackHeight;
    desc.samples_total = frame_.size();
    desc.samples_per_line_nominal = kStackWidth;
    return desc;
  }

  const sample_type* get_frame(orc::FrameID id) const override {
    return has_frame(id) ? frame_.data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return has_frame(id) ? frame_ : std::vector<sample_type>{};
  }

  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters params;
    params.system = orc::VideoSystem::NTSC;
    params.frame_width_nominal = static_cast<int32_t>(kStackWidth);
    params.frame_height = static_cast<int32_t>(kStackHeight);
    params.black_level = 282;
    params.number_of_sequential_frames = 1;
    return params;
  }

 private:
  std::vector<sample_type> frame_;
};

}  // namespace

// Regression: get_line_samples()/get_line() on the stacked representation
// must return the stacked output, never the first source's raw samples
// (observers and analysis sinks read lines through these accessors).
TEST(StackerStageTest, LineReads_ReturnStackedOutputNotFirstSource) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"mode", std::string("Mean")}}));

  auto src0 = std::make_shared<FakeConstantSource>(100);
  auto src1 = std::make_shared<FakeConstantSource>(200);

  const auto stacked = stage.process({src0, src1});
  ASSERT_NE(stacked, nullptr);

  const sample_type* frame = stacked->get_frame(orc::FrameID{0});
  ASSERT_NE(frame, nullptr);

  for (size_t line = 0; line < kStackHeight; ++line) {
    const auto samples = stacked->get_line_samples(orc::FrameID{0}, line);
    ASSERT_EQ(samples.size(), kStackWidth) << "line " << line;
    for (size_t i = 0; i < kStackWidth; ++i) {
      // Mean of 100 and 200 — and definitely not the first source's value.
      EXPECT_EQ(samples[i], 150) << "line " << line << " sample " << i;
      EXPECT_EQ(samples[i], frame[line * kStackWidth + i]);
    }
  }
}

namespace {

// Constant-valued NTSC source of |frames| frames whose first |head_padding|
// are padding, as source_align and frame_map produce them.
class FakePaddedSource : public orc::VideoFrameRepresentation {
 public:
  FakePaddedSource(sample_type value, size_t frames, size_t head_padding)
      : frames_(frames),
        head_padding_(head_padding),
        frame_(kStackWidth * kStackHeight, value),
        padding_(kStackWidth * kStackHeight, kBlanking) {}

  static constexpr sample_type kBlanking = 256;

  orc::FrameIDRange frame_range() const override {
    return {orc::FrameID{0}, orc::FrameID{frames_ - 1}};
  }
  size_t frame_count() const override { return frames_; }
  bool has_frame(orc::FrameID id) const override { return id < frames_; }

  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    orc::FrameDescriptor desc;
    desc.frame_id = id;
    desc.system = orc::VideoSystem::NTSC;
    desc.height = kStackHeight;
    desc.samples_total = frame_.size();
    desc.samples_per_line_nominal = kStackWidth;
    desc.is_padding_frame = id < head_padding_;
    return desc;
  }

  const sample_type* get_frame(orc::FrameID id) const override {
    if (!has_frame(id)) return nullptr;
    return id < head_padding_ ? padding_.data() : frame_.data();
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    if (!has_frame(id)) return {};
    return id < head_padding_ ? padding_ : frame_;
  }

  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters params;
    params.system = orc::VideoSystem::NTSC;
    params.frame_width_nominal = static_cast<int32_t>(kStackWidth);
    params.frame_height = static_cast<int32_t>(kStackHeight);
    params.black_level = 282;
    params.blanking_level = kBlanking;
    params.number_of_sequential_frames = static_cast<int32_t>(frames_);
    return params;
  }

 private:
  size_t frames_;
  size_t head_padding_;
  std::vector<sample_type> frame_;
  std::vector<sample_type> padding_;
};

}  // namespace

// The first input is padded at the head and ends early. The stack must still
// cover every frame any input carries, stack the frames the first input lacks
// from the others, and describe them as real pictures.
TEST(StackerStageTest, StackRange_IsTheUnionOfTheInputs) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters(
      {{"mode", std::string("Mean")}, {"sample_align", false}}));

  auto first = std::make_shared<FakePaddedSource>(100, 10, 3);
  auto second = std::make_shared<FakePaddedSource>(200, 13, 0);
  auto third = std::make_shared<FakePaddedSource>(300, 13, 0);

  const auto stacked = stage.process({first, second, third});
  ASSERT_NE(stacked, nullptr);

  EXPECT_EQ(stacked->frame_count(), 13u);
  EXPECT_EQ(stacked->frame_range().first, orc::FrameID{0});
  EXPECT_EQ(stacked->frame_range().last, orc::FrameID{12});
  EXPECT_TRUE(stacked->has_frame(orc::FrameID{12}));
  EXPECT_FALSE(stacked->has_frame(orc::FrameID{13}));

  // Head: the first input is padding, the others are real.
  // Tail: the first input has no frame at all.
  for (orc::FrameID id :
       {orc::FrameID{0}, orc::FrameID{2}, orc::FrameID{10}, orc::FrameID{12}}) {
    const auto desc = stacked->get_frame_descriptor(id);
    ASSERT_TRUE(desc.has_value()) << "frame " << id;
    EXPECT_FALSE(desc->is_padding_frame) << "frame " << id;
    EXPECT_EQ(desc->frame_id, id);

    const auto samples = stacked->get_frame_copy(id);
    ASSERT_EQ(samples.size(), kStackWidth * kStackHeight) << "frame " << id;
    EXPECT_EQ(samples[0], 250) << "frame " << id;
  }

  // Where all three are real, all three are stacked.
  EXPECT_EQ(stacked->get_frame_copy(orc::FrameID{5})[0], 200);
}

// A frame is padding only when every input is padding there, and it still
// reads back as a full blanking-level frame.
TEST(StackerStageTest, StackRange_AllPaddingFrameIsPadding) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters(
      {{"mode", std::string("Mean")}, {"sample_align", false}}));

  auto first = std::make_shared<FakePaddedSource>(100, 6, 2);
  auto second = std::make_shared<FakePaddedSource>(200, 6, 1);

  const auto stacked = stage.process({first, second});
  ASSERT_NE(stacked, nullptr);

  const auto padding = stacked->get_frame_descriptor(orc::FrameID{0});
  ASSERT_TRUE(padding.has_value());
  EXPECT_TRUE(padding->is_padding_frame);
  EXPECT_EQ(padding->samples_total, kStackWidth * kStackHeight);

  const auto samples = stacked->get_frame_copy(orc::FrameID{0});
  ASSERT_EQ(samples.size(), kStackWidth * kStackHeight);
  EXPECT_TRUE(std::all_of(samples.begin(), samples.end(), [](sample_type v) {
    return v == FakePaddedSource::kBlanking;
  }));
  ASSERT_NE(stacked->get_frame(orc::FrameID{0}), nullptr);

  const auto one_real = stacked->get_frame_descriptor(orc::FrameID{1});
  ASSERT_TRUE(one_real.has_value());
  EXPECT_FALSE(one_real->is_padding_frame);
  EXPECT_EQ(stacked->get_frame_copy(orc::FrameID{1})[0], 200);
}

namespace {

// FakeConstantSource that is also an Artifact, so it can be fed to execute()
// (which receives ArtifactPtr inputs and dynamic-casts them back to
// VideoFrameRepresentation).
class FakeArtifactSource : public FakeConstantSource, public orc::Artifact {
 public:
  explicit FakeArtifactSource(sample_type value)
      : FakeConstantSource(value),
        orc::Artifact(orc::ArtifactID("fake_source"), orc::Provenance{}) {}

  std::string type_name() const override { return "fake_source"; }
};

}  // namespace

// Regression: execute() must not discard its cached stacked wrapper when the
// parameters are unchanged. The DAG passes the node's stored parameters on
// every execute(); an unconditional reset threw away the wrapper — and its warm
// per-frame stack cache — on each call, forcing every analysis-sink sweep to
// re-stack the whole source from scratch.
TEST(StackerStageTest, Execute_ReusesCachedWrapper_WhenParametersUnchanged) {
  orc::StackerStage stage;
  auto src0 = std::make_shared<FakeArtifactSource>(100);
  auto src1 = std::make_shared<FakeArtifactSource>(200);
  std::vector<orc::ArtifactPtr> inputs = {src0, src1};

  const std::map<std::string, orc::ParameterValue> params = {
      {"mode", std::string("Mean")}};

  orc::ObservationContext ctx;
  const auto out1 = stage.execute(inputs, params, ctx);
  const auto out2 = stage.execute(inputs, params, ctx);

  ASSERT_EQ(out1.size(), 1u);
  ASSERT_EQ(out2.size(), 1u);
  // Same wrapper instance => its per-frame stack cache survives the second
  // call.
  EXPECT_EQ(out1[0].get(), out2[0].get());
}

// Complement: changing the stacking parameters must invalidate the cache and
// produce a fresh wrapper, so stale stacked frames are never served.
TEST(StackerStageTest, Execute_RebuildsWrapper_WhenParametersChange) {
  orc::StackerStage stage;
  auto src0 = std::make_shared<FakeArtifactSource>(100);
  auto src1 = std::make_shared<FakeArtifactSource>(200);
  std::vector<orc::ArtifactPtr> inputs = {src0, src1};

  orc::ObservationContext ctx;
  const auto out1 = stage.execute(inputs, {{"mode", std::string("Mean")}}, ctx);
  const auto out2 =
      stage.execute(inputs, {{"mode", std::string("Median")}}, ctx);

  ASSERT_EQ(out1.size(), 1u);
  ASSERT_EQ(out2.size(), 1u);
  EXPECT_NE(out1[0].get(), out2[0].get());
}

namespace {

// FakeConstantSource with multiple frames and a dropout hint present on every
// frame (line 1, samples 4..11 = flat offset 20, count 8).
class FakeDropoutSource : public FakeConstantSource {
 public:
  FakeDropoutSource(sample_type value, size_t frame_count)
      : FakeConstantSource(value), frame_count_(frame_count) {}

  orc::FrameIDRange frame_range() const override {
    return {orc::FrameID{0}, orc::FrameID{frame_count_ - 1}};
  }
  size_t frame_count() const override { return frame_count_; }
  bool has_frame(orc::FrameID id) const override {
    return id < static_cast<orc::FrameID>(frame_count_);
  }

  std::vector<orc::DropoutRun> get_dropout_hints(
      orc::FrameID id) const override {
    if (!has_frame(id)) return {};
    return {orc::DropoutRun{id, kStackWidth + 4, 8u, 100}};
  }

 private:
  size_t frame_count_;
};

}  // namespace

// Regression: residual dropout runs on the stacked output must carry the
// stacked frame's ID. They used to be emitted with a hard-coded frame_id of
// 0, which broke downstream consumers keying on the field (e.g. dropout_map
// removals).
TEST(StackerStageTest, StackedDropoutHints_CarryStackedFrameId) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"mode", std::string("Mean")}}));

  // Both sources drop out over the same span, so the stacked output has a
  // residual dropout there on every frame.
  auto src0 = std::make_shared<FakeDropoutSource>(100, 2);
  auto src1 = std::make_shared<FakeDropoutSource>(200, 2);

  const auto stacked = stage.process({src0, src1});
  ASSERT_NE(stacked, nullptr);

  const auto hints = stacked->get_dropout_hints(orc::FrameID{1});
  ASSERT_FALSE(hints.empty());
  for (const auto& run : hints) {
    EXPECT_EQ(run.frame_id, orc::FrameID{1});
  }
}

// ── Channel-pair audio ───────────────────────────────────────────────────────

namespace {

// One-frame source scaffold for audio stacking tests: frame 0 present with
// no colour-frame index (temporal alignment) and no dropouts by default.
std::shared_ptr<NiceMock<MockVideoFrameRepresentation>>
make_audio_stack_source() {
  auto src = std::make_shared<NiceMock<MockVideoFrameRepresentation>>();
  ON_CALL(*src, has_frame(orc::FrameID{0})).WillByDefault(Return(true));
  orc::FrameDescriptor desc;
  desc.frame_id = orc::FrameID{0};
  ON_CALL(*src, get_frame_descriptor(orc::FrameID{0}))
      .WillByDefault(Return(desc));
  ON_CALL(*src, frame_range()).WillByDefault(Return(orc::FrameIDRange{0u, 0u}));
  ON_CALL(*src, frame_count()).WillByDefault(Return(1u));
  return src;
}

const orc::AudioChannelPairDescriptor kAnaloguePair{"Analogue",
                                                    orc::AudioOrigin::ANALOGUE};

// 24-bit two's-complement bounds carried in int32_t (audio_channel_pair.h).
constexpr int32_t kAudioMax = 8388607;
constexpr int32_t kAudioMin = -8388608;

}  // namespace

TEST(StackerStageTest, CommonChannelPairs_MeanStacksEveryPair) {
  orc::StackerStage stage;  // audio_stacking defaults to Mean
  auto src0 = make_audio_stack_source();
  auto src1 = make_audio_stack_source();
  for (const auto& src : {src0, src1}) {
    ON_CALL(*src, audio_channel_pair_count()).WillByDefault(Return(2u));
    ON_CALL(*src, get_audio_channel_pair_descriptor(_))
        .WillByDefault(Return(kAnaloguePair));
  }
  ON_CALL(*src0, get_audio_samples(0, orc::FrameID{0}))
      .WillByDefault(Return(std::vector<int32_t>{0, 0}));
  ON_CALL(*src1, get_audio_samples(0, orc::FrameID{0}))
      .WillByDefault(Return(std::vector<int32_t>{2000000, 2000000}));
  ON_CALL(*src0, get_audio_samples(1, orc::FrameID{0}))
      .WillByDefault(Return(std::vector<int32_t>{100000, -200000}));
  ON_CALL(*src1, get_audio_samples(1, orc::FrameID{0}))
      .WillByDefault(Return(std::vector<int32_t>{300000, -400000}));

  const orc::StackedVideoFrameRepresentation stacked({src0, src1}, &stage);

  ASSERT_EQ(stacked.audio_channel_pair_count(), 2u);
  const auto descriptor = stacked.get_audio_channel_pair_descriptor(0);
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->name, "Analogue");
  // Every channel pair common to all inputs is stacked — not just pair 0.
  EXPECT_EQ(stacked.get_audio_samples(0, orc::FrameID{0}),
            (std::vector<int32_t>{1000000, 1000000}));
  EXPECT_EQ(stacked.get_audio_samples(1, orc::FrameID{0}),
            (std::vector<int32_t>{200000, -300000}));
  // Out-of-range pairs answer with {}.
  EXPECT_TRUE(stacked.get_audio_samples(2, orc::FrameID{0}).empty());
}

TEST(StackerStageTest, CommonChannelPairs_MedianStacksEveryPair) {
  orc::StackerStage stage;
  ASSERT_TRUE(
      stage.set_parameters({{"audio_stacking", std::string("Median")}}));
  auto src0 = make_audio_stack_source();
  auto src1 = make_audio_stack_source();
  auto src2 = make_audio_stack_source();
  for (const auto& src : {src0, src1, src2}) {
    ON_CALL(*src, audio_channel_pair_count()).WillByDefault(Return(2u));
    ON_CALL(*src, get_audio_channel_pair_descriptor(_))
        .WillByDefault(Return(kAnaloguePair));
  }
  ON_CALL(*src0, get_audio_samples(0, orc::FrameID{0}))
      .WillByDefault(Return(std::vector<int32_t>{100, kAudioMin}));
  ON_CALL(*src1, get_audio_samples(0, orc::FrameID{0}))
      .WillByDefault(Return(std::vector<int32_t>{200, 0}));
  ON_CALL(*src2, get_audio_samples(0, orc::FrameID{0}))
      .WillByDefault(Return(std::vector<int32_t>{3000000, kAudioMax}));
  ON_CALL(*src0, get_audio_samples(1, orc::FrameID{0}))
      .WillByDefault(Return(std::vector<int32_t>{-50, -50}));
  ON_CALL(*src1, get_audio_samples(1, orc::FrameID{0}))
      .WillByDefault(Return(std::vector<int32_t>{-10, -10}));
  ON_CALL(*src2, get_audio_samples(1, orc::FrameID{0}))
      .WillByDefault(Return(std::vector<int32_t>{-90, -90}));

  const orc::StackedVideoFrameRepresentation stacked({src0, src1, src2},
                                                     &stage);

  EXPECT_EQ(stacked.get_audio_samples(0, orc::FrameID{0}),
            (std::vector<int32_t>{200, 0}));
  EXPECT_EQ(stacked.get_audio_samples(1, orc::FrameID{0}),
            (std::vector<int32_t>{-50, -50}));
}

TEST(StackerStageTest, PairNotInAllSources_PassesThroughFromBestSource) {
  orc::StackerStage stage;
  auto src0 = make_audio_stack_source();
  auto src1 = make_audio_stack_source();
  ON_CALL(*src0, audio_channel_pair_count()).WillByDefault(Return(2u));
  ON_CALL(*src0, get_audio_channel_pair_descriptor(_))
      .WillByDefault(Return(kAnaloguePair));
  ON_CALL(*src1, audio_channel_pair_count()).WillByDefault(Return(1u));
  ON_CALL(*src1, get_audio_channel_pair_descriptor(0))
      .WillByDefault(Return(kAnaloguePair));
  ON_CALL(*src0, get_audio_samples(1, orc::FrameID{0}))
      .WillByDefault(Return(std::vector<int32_t>{100000, 200000}));
  // src1 has more dropouts, so src0 is the best source.
  ON_CALL(*src1, get_dropout_hints(orc::FrameID{0}))
      .WillByDefault(Return(
          std::vector<orc::DropoutRun>{{orc::FrameID{0}, 0u, 10u, 128}}));

  const orc::StackedVideoFrameRepresentation stacked({src0, src1}, &stage);

  // Pair 1 exists only in src0: no combining, pass through from best.
  EXPECT_EQ(stacked.get_audio_samples(1, orc::FrameID{0}),
            (std::vector<int32_t>{100000, 200000}));
}

TEST(StackerStageTest, MeanStacking_SaturatesAtTwentyFourBitBounds) {
  orc::StackerStage stage;  // Mean
  auto src0 = make_audio_stack_source();
  auto src1 = make_audio_stack_source();
  for (const auto& src : {src0, src1}) {
    ON_CALL(*src, audio_channel_pair_count()).WillByDefault(Return(1u));
    ON_CALL(*src, get_audio_channel_pair_descriptor(_))
        .WillByDefault(Return(kAnaloguePair));
    // Both inputs deliver values beyond the 24-bit range; the combined
    // result must be clamped to the carrier bounds.
    ON_CALL(*src, get_audio_samples(0, orc::FrameID{0}))
        .WillByDefault(Return(
            std::vector<int32_t>{9000000, -9000000, kAudioMax, kAudioMin}));
  }

  const orc::StackedVideoFrameRepresentation stacked({src0, src1}, &stage);

  EXPECT_EQ(stacked.get_audio_samples(0, orc::FrameID{0}),
            (std::vector<int32_t>{kAudioMax, kAudioMin, kAudioMax, kAudioMin}));
}

TEST(StackerStageTest, MedianStacking_SaturatesAtTwentyFourBitBounds) {
  orc::StackerStage stage;
  ASSERT_TRUE(
      stage.set_parameters({{"audio_stacking", std::string("Median")}}));
  auto src0 = make_audio_stack_source();
  auto src1 = make_audio_stack_source();
  for (const auto& src : {src0, src1}) {
    ON_CALL(*src, audio_channel_pair_count()).WillByDefault(Return(1u));
    ON_CALL(*src, get_audio_channel_pair_descriptor(_))
        .WillByDefault(Return(kAnaloguePair));
  }
  // Even count: the median averages the two middle values, which can land
  // outside the 24-bit range when the inputs do.
  ON_CALL(*src0, get_audio_samples(0, orc::FrameID{0}))
      .WillByDefault(Return(std::vector<int32_t>{9000000, -9000000}));
  ON_CALL(*src1, get_audio_samples(0, orc::FrameID{0}))
      .WillByDefault(Return(std::vector<int32_t>{9500000, -9500000}));

  const orc::StackedVideoFrameRepresentation stacked({src0, src1}, &stage);

  EXPECT_EQ(stacked.get_audio_samples(0, orc::FrameID{0}),
            (std::vector<int32_t>{kAudioMax, kAudioMin}));
}

// ── EFM ──────────────────────────────────────────────────────────────────────

namespace {

// One-frame EFM source: |packed| is delivered verbatim as the frame's EFM
// bytes, so a test can control the doubt nibble of each t-value.
std::shared_ptr<NiceMock<MockVideoFrameRepresentation>> make_efm_stack_source(
    std::vector<uint8_t> packed) {
  auto src = make_audio_stack_source();
  ON_CALL(*src, has_efm()).WillByDefault(Return(true));
  ON_CALL(*src, get_efm_sample_count(orc::FrameID{0}))
      .WillByDefault(Return(static_cast<uint32_t>(packed.size())));
  ON_CALL(*src, get_efm_samples(orc::FrameID{0}))
      .WillByDefault(Return(std::move(packed)));
  return src;
}

}  // namespace

// Each pipeline EFM byte packs the t-value into its low nibble and the
// producer's doubt into the high one. Stacking combines the t-values alone:
// mean/median of the whole bytes would fold the doubt into the t-value field.
TEST(StackerStageTest, EfmMeanStacking_CombinesTValuesAndDropsDoubt) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"efm_stacking", std::string("Mean")}}));
  // t-values 3 and 5 (mean 4), 11 and 7 (mean 9) - carried with wildly
  // different doubt, which must not reach the result.
  auto src0 =
      make_efm_stack_source({orc::efm_pack(3, 9), orc::efm_pack(11, 0)});
  auto src1 =
      make_efm_stack_source({orc::efm_pack(5, 0), orc::efm_pack(7, 15)});

  const orc::StackedVideoFrameRepresentation stacked({src0, src1}, &stage);

  // Byte-wise means would be 0x4C and 0x7F.
  EXPECT_EQ(stacked.get_efm_samples(orc::FrameID{0}),
            (std::vector<uint8_t>{4, 9}));
}

TEST(StackerStageTest, EfmMedianStacking_CombinesTValuesAndDropsDoubt) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"efm_stacking", std::string("Median")}}));
  auto src0 = make_efm_stack_source({orc::efm_pack(3, 15)});
  auto src1 = make_efm_stack_source({orc::efm_pack(8, 1)});
  auto src2 = make_efm_stack_source({orc::efm_pack(9, 0)});

  const orc::StackedVideoFrameRepresentation stacked({src0, src1, src2},
                                                     &stage);

  // Median of the t-values 3, 8, 9 - not of the bytes 0xF3, 0x18, 0x09.
  EXPECT_EQ(stacked.get_efm_samples(orc::FrameID{0}),
            (std::vector<uint8_t>{8}));
}

// Mean and median produce a value no source vouched for, so a single
// contributing source is reported the same way as any other of their outputs:
// t-values with no doubt of their own.
TEST(StackerStageTest, EfmMeanStacking_StripsDoubtEvenFromASingleSource) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"efm_stacking", std::string("Mean")}}));
  auto src0 = make_efm_stack_source({orc::efm_pack(4, 12)});
  auto src1 = make_audio_stack_source();  // no EFM to contribute

  const orc::StackedVideoFrameRepresentation stacked({src0, src1}, &stage);

  EXPECT_EQ(stacked.get_efm_samples(orc::FrameID{0}),
            (std::vector<uint8_t>{4}));
}

// EFM combining is off by default: no mode has yet been shown to beat passing
// the best source's t-values through untouched. With Confidence selected and
// one contributing source nothing was combined either, so its bytes - doubt
// included - stand as they are.
TEST(StackerStageTest, EfmStacking_DefaultsToDisabled) {
  orc::StackerStage stage;
  EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("efm_stacking")),
            "Disabled");
}

TEST(StackerStageTest, EfmConfidenceStacking_KeepsSingleSourceDoubt) {
  orc::StackerStage stage;
  ASSERT_TRUE(
      stage.set_parameters({{"efm_stacking", std::string("Confidence")}}));

  const std::vector<uint8_t> packed = {orc::efm_pack(4, 12)};
  auto src0 = make_efm_stack_source(packed);
  auto src1 = make_audio_stack_source();  // no EFM to contribute

  const orc::StackedVideoFrameRepresentation stacked({src0, src1}, &stage);

  EXPECT_EQ(stacked.get_efm_samples(orc::FrameID{0}), packed);
}

// Two sources whose t-values carry no frame sync give confidence stacking no
// axis to combine on, so it returns a source unaltered rather than blending on
// an assumption that does not hold. The stage-level cover for the combining
// itself is in efm_confidence_stack_test.cpp.
TEST(StackerStageTest, EfmConfidenceStacking_FallsBackWithoutASyncGrid) {
  orc::StackerStage stage;
  ASSERT_TRUE(
      stage.set_parameters({{"efm_stacking", std::string("Confidence")}}));
  const std::vector<uint8_t> packed = {orc::efm_pack(3, 9),
                                       orc::efm_pack(4, 15)};
  auto src0 = make_efm_stack_source(packed);
  auto src1 = make_efm_stack_source(packed);

  const orc::StackedVideoFrameRepresentation stacked({src0, src1}, &stage);

  EXPECT_EQ(stacked.get_efm_samples(orc::FrameID{0}), packed);
}

// Disabled does not combine anything, so the chosen source's bytes - doubt
// included - pass through untouched.
TEST(StackerStageTest, EfmStackingDisabled_PassesPackedBytesThrough) {
  orc::StackerStage stage;
  ASSERT_TRUE(
      stage.set_parameters({{"efm_stacking", std::string("Disabled")}}));
  const std::vector<uint8_t> packed = {orc::efm_pack(3, 9),
                                       orc::efm_pack(11, 15)};
  // Identical bytes in both sources, so the result does not depend on which
  // one is picked as best.
  auto src0 = make_efm_stack_source(packed);
  auto src1 = make_efm_stack_source(packed);

  const orc::StackedVideoFrameRepresentation stacked({src0, src1}, &stage);

  EXPECT_EQ(stacked.get_efm_samples(orc::FrameID{0}), packed);
}

// ============================================================================
// Frame alignment and sample-grid alignment
// ============================================================================

namespace {

// A concrete PAL source carrying synthetic frame data, so the sample-grid
// measurement and the shift it drives can be exercised end to end.
//
// |content_shift| moves the picture within the sample grid: sample n holds
// the pattern value for n + content_shift, which is what a decode that placed
// its line starts |content_shift| samples early produces.
class FakePalSource : public orc::VideoFrameRepresentation {
 public:
  using sample_type = orc::VideoFrameRepresentation::sample_type;

  static constexpr size_t kWidth = 1135;
  static constexpr size_t kHeight = 625;
  static constexpr size_t kFrames = 64;

  explicit FakePalSource(int32_t content_shift = 0) {
    total_ =
        orc::frame_line_sample_offset(orc::VideoSystem::PAL, kWidth, kHeight);
    samples_.assign(total_, 0);
    for (size_t line = 0; line < kHeight; ++line) {
      const size_t start =
          orc::frame_line_sample_offset(orc::VideoSystem::PAL, kWidth, line);
      const size_t length =
          orc::frame_line_sample_count(orc::VideoSystem::PAL, kWidth, line);
      for (size_t n = 0; n < length; ++n) {
        samples_[start + n] =
            pattern(line, static_cast<int64_t>(n) + content_shift);
      }
    }
  }

  // Deterministic broadband content, so cross-correlation has a single clear
  // peak rather than the ambiguity a periodic signal would give.
  static sample_type pattern(size_t line, int64_t n) {
    if (n < 0) n = 0;
    uint64_t h = static_cast<uint64_t>(n) * 6364136223846793005ULL +
                 static_cast<uint64_t>(line) * 1442695040888963407ULL;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 29;
    return static_cast<sample_type>(256 + static_cast<int>(h % 512));
  }

  orc::FrameIDRange frame_range() const override {
    return orc::FrameIDRange{orc::FrameID{0}, orc::FrameID{kFrames - 1}};
  }
  size_t frame_count() const override { return kFrames; }
  bool has_frame(orc::FrameID id) const override { return id < kFrames; }

  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    orc::FrameDescriptor d;
    d.frame_id = id;
    d.system = orc::VideoSystem::PAL;
    d.height = kHeight;
    d.samples_total = total_;
    d.samples_per_line_nominal = kWidth;
    return d;
  }

  const sample_type* get_frame(orc::FrameID id) const override {
    return has_frame(id) ? samples_.data() : nullptr;
  }
  const sample_type* get_line(orc::FrameID id, size_t line) const override {
    if (!has_frame(id) || line >= kHeight) return nullptr;
    return samples_.data() +
           orc::frame_line_sample_offset(orc::VideoSystem::PAL, kWidth, line);
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return has_frame(id) ? samples_ : std::vector<sample_type>{};
  }

  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters p;
    p.system = orc::VideoSystem::PAL;
    p.frame_width_nominal = static_cast<int32_t>(kWidth);
    p.frame_height = static_cast<int32_t>(kHeight);
    p.number_of_sequential_frames = static_cast<int32_t>(kFrames);
    p.blanking_level = 256;
    p.black_level = 256;
    return p;
  }

 private:
  std::vector<sample_type> samples_;
  size_t total_ = 0;
};

std::shared_ptr<const orc::StackedVideoFrameRepresentation> stack_of(
    orc::StackerStage& stage,
    const std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>>&
        sources) {
  return std::dynamic_pointer_cast<const orc::StackedVideoFrameRepresentation>(
      stage.process(sources));
}

// Compare well inside the active picture, away from the line ends where a
// shift replicates the edge sample.
void expect_active_video_equal(
    const std::vector<orc::VideoFrameRepresentation::sample_type>& actual,
    const std::vector<orc::VideoFrameRepresentation::sample_type>& expected,
    bool equal) {
  ASSERT_EQ(actual.size(), expected.size());
  size_t differences = 0;
  for (size_t line = 20; line < FakePalSource::kHeight; line += 37) {
    const size_t start = orc::frame_line_sample_offset(
        orc::VideoSystem::PAL, FakePalSource::kWidth, line);
    for (size_t n = 300; n < 1000; ++n) {
      if (actual[start + n] != expected[start + n]) ++differences;
    }
  }
  if (equal) {
    EXPECT_EQ(differences, 0u);
  } else {
    EXPECT_GT(differences, 0u);
  }
}

}  // namespace

// Sources arrive frame-aligned from frame_map / source_align, so every source
// contributes the frame with the same id. The stage must not go looking for a
// different frame of its own accord: the colour_frame_index it used to search
// on is measured against each source's own sample grid, so a source whose
// grid differs reports a mismatch on the very same picture.
TEST(StackerStageTest, FrameAlignment_ReadsTheSameFrameIdFromEverySource) {
  orc::StackerStage stage;
  auto aligned = std::make_shared<FakePalSource>(0);
  auto shifted = std::make_shared<FakePalSource>(1);

  auto stacked = stack_of(stage, {aligned, shifted});
  ASSERT_NE(stacked, nullptr);

  for (orc::FrameID id : {orc::FrameID{0}, orc::FrameID{7}, orc::FrameID{31}}) {
    EXPECT_EQ(stacked->resolve_source_frame(0, id), id);
    EXPECT_EQ(stacked->resolve_source_frame(1, id), id);
  }
}

// A source placed one sample early reads back as offset -1: reference sample
// n is that source's sample n - 1.
TEST(StackerStageTest, SampleAlign_MeasuresAShiftedSourcesGridOffset) {
  orc::StackerStage stage;
  auto reference = std::make_shared<FakePalSource>(0);
  auto same_grid = std::make_shared<FakePalSource>(0);
  auto one_early = std::make_shared<FakePalSource>(1);
  auto two_late = std::make_shared<FakePalSource>(-2);

  auto stacked = stack_of(stage, {reference, same_grid, one_early, two_late});
  ASSERT_NE(stacked, nullptr);

  const auto offsets = stacked->sample_offsets(orc::FrameID{5});
  ASSERT_EQ(offsets.size(), 4u);
  EXPECT_EQ(offsets[0], 0);
  EXPECT_EQ(offsets[1], 0);
  EXPECT_EQ(offsets[2], -1);
  EXPECT_EQ(offsets[3], 2);
}

// With the grids brought into line, stacking a source against a shifted copy
// of itself returns the reference exactly — the two agree sample for sample
// once shifted, so their mean is the original.
TEST(StackerStageTest, SampleAlign_ShiftedCopyStacksBackToTheReference) {
  orc::StackerStage stage;
  auto reference = std::make_shared<FakePalSource>(0);
  auto one_early = std::make_shared<FakePalSource>(1);

  auto stacked = stack_of(stage, {reference, one_early});
  ASSERT_NE(stacked, nullptr);

  expect_active_video_equal(stacked->get_frame_copy(orc::FrameID{5}),
                            reference->get_frame_copy(orc::FrameID{5}), true);
}

// The negative control for the test above: combining on the sources' own
// grids averages each sample with its neighbour, which is what erodes chroma
// on real captures.
TEST(StackerStageTest, SampleAlign_Disabled_CombinesOnTheSourcesOwnGrids) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"sample_align", false}}));
  auto reference = std::make_shared<FakePalSource>(0);
  auto one_early = std::make_shared<FakePalSource>(1);

  auto stacked = stack_of(stage, {reference, one_early});
  ASSERT_NE(stacked, nullptr);

  EXPECT_EQ(stacked->sample_offsets(orc::FrameID{5})[1], 0);
  expect_active_video_equal(stacked->get_frame_copy(orc::FrameID{5}),
                            reference->get_frame_copy(orc::FrameID{5}), false);
}

// ============================================================================
// Segmented sample-grid measurement and field agreement
// ============================================================================

namespace {

constexpr size_t kPalWidth = 1135;
constexpr size_t kPalHeight = 625;
constexpr size_t kPalField1Lines = 313;

uint64_t mix(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 29;
  return h;
}

// A picture: the sample value at (line, n) of a PAL frame.
using Picture = std::function<sample_type(size_t, int64_t)>;

// Composite picture as a decode delivers it: smooth luma, a strong 4FSC
// subcarrier that repeats every 4 samples, and noise of the decode's own.
// |content_shift| moves the picture within the sample grid as
// FakePalSource's does, so a source built with -L sits at grid offset +L.
Picture composite_picture(int32_t content_shift, uint64_t noise_seed) {
  return [=](size_t line, int64_t n) {
    const int64_t m = std::max<int64_t>(0, n + content_shift);
    // Luma: hashed knots every 16 samples, linearly interpolated.
    const auto knot = [line](int64_t k) {
      return static_cast<double>(
          mix(static_cast<uint64_t>(k) * 0x9e3779b97f4a7c15ULL +
              static_cast<uint64_t>(line) * 0xc2b2ae3d27d4eb4fULL) %
          300);
    };
    const int64_t k = m / 16;
    const double t = static_cast<double>(m % 16) / 16.0;
    const double luma = knot(k) * (1.0 - t) + knot(k + 1) * t;
    static constexpr int kSubcarrier[4] = {0, 120, 0, -120};
    const int chroma = kSubcarrier[m % 4];
    const int noise =
        static_cast<int>(
            mix(static_cast<uint64_t>(n) * 0x2545f4914f6cdd1dULL +
                static_cast<uint64_t>(line) * 0x9e3779b97f4a7c15ULL +
                noise_seed) %
            21) -
        10;
    return static_cast<sample_type>(300 + static_cast<int>(luma) + chroma +
                                    noise);
  };
}

// FakePalSource's broadband pattern, re-seeded and raised by |dc|.
Picture broadband_picture(uint64_t seed, int dc = 0) {
  return [=](size_t line, int64_t n) {
    return static_cast<sample_type>(
        FakePalSource::pattern(line + static_cast<size_t>(seed) * 7919, n) +
        dc);
  };
}

// |first| on field 1 and |second| on field 2.
Picture fields_picture(Picture first, Picture second) {
  return [=](size_t line, int64_t n) {
    return line < kPalField1Lines ? first(line, n) : second(line, n);
  };
}

Picture flat_picture() {
  return [](size_t, int64_t) { return static_cast<sample_type>(400); };
}

// PAL source whose content changes at given frames: |spans| lists each
// span's first frame and its picture, in order, starting at frame 0.
class FakeSyntheticPalSource : public orc::VideoFrameRepresentation {
 public:
  FakeSyntheticPalSource(size_t frames,
                         std::vector<std::pair<size_t, Picture>> spans)
      : frames_(frames) {
    total_ = orc::frame_line_sample_offset(orc::VideoSystem::PAL, kPalWidth,
                                           kPalHeight);
    for (const auto& [first, picture] : spans) {
      std::vector<sample_type> samples(total_, 0);
      for (size_t line = 0; line < kPalHeight; ++line) {
        const size_t start = orc::frame_line_sample_offset(
            orc::VideoSystem::PAL, kPalWidth, line);
        const size_t length = orc::frame_line_sample_count(
            orc::VideoSystem::PAL, kPalWidth, line);
        for (size_t n = 0; n < length; ++n) {
          samples[start + n] = picture(line, static_cast<int64_t>(n));
        }
      }
      spans_.emplace_back(first, std::move(samples));
    }
  }

  orc::FrameIDRange frame_range() const override {
    return {orc::FrameID{0}, orc::FrameID{frames_ - 1}};
  }
  size_t frame_count() const override { return frames_; }
  bool has_frame(orc::FrameID id) const override { return id < frames_; }

  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    orc::FrameDescriptor d;
    d.frame_id = id;
    d.system = orc::VideoSystem::PAL;
    d.height = kPalHeight;
    d.samples_total = total_;
    d.samples_per_line_nominal = kPalWidth;
    return d;
  }

  const sample_type* get_frame(orc::FrameID id) const override {
    return has_frame(id) ? samples_for(id).data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return has_frame(id) ? samples_for(id) : std::vector<sample_type>{};
  }

  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters p;
    p.system = orc::VideoSystem::PAL;
    p.frame_width_nominal = static_cast<int32_t>(kPalWidth);
    p.frame_height = static_cast<int32_t>(kPalHeight);
    p.number_of_sequential_frames = static_cast<int32_t>(frames_);
    p.blanking_level = 256;
    p.black_level = 256;
    return p;
  }

 private:
  const std::vector<sample_type>& samples_for(orc::FrameID id) const {
    const auto* found = &spans_.front().second;
    for (const auto& [first, samples] : spans_) {
      if (id >= first) found = &samples;
    }
    return *found;
  }

  size_t frames_;
  size_t total_ = 0;
  std::vector<std::pair<size_t, std::vector<sample_type>>> spans_;
};

std::shared_ptr<FakeSyntheticPalSource> synthetic(size_t frames,
                                                  Picture picture) {
  return std::make_shared<FakeSyntheticPalSource>(
      frames, std::vector<std::pair<size_t, Picture>>{{0, std::move(picture)}});
}

// Sample |n| of frame-flat |line| of a PAL frame buffer.
sample_type at(const std::vector<sample_type>& frame, size_t line, size_t n) {
  return frame[orc::frame_line_sample_offset(orc::VideoSystem::PAL, kPalWidth,
                                             line) +
               n];
}

}  // namespace

// Real captures measure up to 12 samples apart. Composite repeats every 4
// samples, so a search on the raw signal confuses L with L±4; the measurement
// must find both offsets exactly, well outside the old ±4 window.
TEST(StackerStageTest, SampleAlign_FindsOffsetsBeyondASubcarrierCycle) {
  orc::StackerStage stage;
  auto reference = synthetic(64, composite_picture(0, 1));
  auto twelve_late = synthetic(64, composite_picture(-12, 2));
  auto seven_early = synthetic(64, composite_picture(7, 3));

  auto stacked = stack_of(stage, {reference, twelve_late, seven_early});
  ASSERT_NE(stacked, nullptr);

  const auto offsets = stacked->sample_offsets(orc::FrameID{30});
  ASSERT_EQ(offsets.size(), 3u);
  EXPECT_EQ(offsets[0], 0);
  EXPECT_EQ(offsets[1], 12);
  EXPECT_EQ(offsets[2], -7);
}

// The offset drifts across a side, so it is measured per segment: a source
// that moves from 0 to +4 halfway through is shifted by the right amount on
// each side of the change.
TEST(StackerStageTest, SampleAlign_FollowsAnOffsetThatChangesPartWay) {
  constexpr size_t kSegment =
      orc::StackedVideoFrameRepresentation::kSampleAlignSegmentFrames;
  orc::StackerStage stage;
  auto reference = synthetic(2 * kSegment, composite_picture(0, 1));
  auto drifting = std::make_shared<FakeSyntheticPalSource>(
      2 * kSegment,
      std::vector<std::pair<size_t, Picture>>{
          {0, composite_picture(0, 2)}, {kSegment, composite_picture(-4, 2)}});

  auto stacked = stack_of(stage, {reference, drifting});
  ASSERT_NE(stacked, nullptr);

  EXPECT_EQ(stacked->sample_offsets(orc::FrameID{0})[1], 0);
  EXPECT_EQ(stacked->sample_offsets(orc::FrameID{kSegment - 1})[1], 0);
  EXPECT_EQ(stacked->sample_offsets(orc::FrameID{kSegment})[1], 4);
  EXPECT_EQ(stacked->sample_offsets(orc::FrameID{2 * kSegment - 1})[1], 4);
}

// A segment with nothing to correlate holds the offset of the nearest segment
// that could be measured, the earlier one on a tie.
TEST(StackerStageTest, SampleAlign_HoldsTheNearestMeasuredSegment) {
  constexpr size_t kSegment =
      orc::StackedVideoFrameRepresentation::kSampleAlignSegmentFrames;
  orc::StackerStage stage;
  auto reference = synthetic(3 * kSegment, composite_picture(0, 1));
  auto source = std::make_shared<FakeSyntheticPalSource>(
      3 * kSegment, std::vector<std::pair<size_t, Picture>>{
                        {0, composite_picture(-3, 2)},
                        {kSegment, flat_picture()},
                        {2 * kSegment, composite_picture(-5, 2)}});

  auto stacked = stack_of(stage, {reference, source});
  ASSERT_NE(stacked, nullptr);

  EXPECT_EQ(stacked->sample_offsets(orc::FrameID{kSegment + 200})[1], 3);
  EXPECT_EQ(stacked->sample_offsets(orc::FrameID{2 * kSegment + 1})[1], 5);
}

// One input's field 2 holds a different picture, as a mid-frame player skip
// leaves it. That field is left out, and the same input's field 1 is still
// stacked: its +30 offset lifts the field 1 mean by 10.
TEST(StackerStageTest, FieldCheck_LeavesAForeignFieldOut) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters(
      {{"mode", std::string("Mean")}, {"sample_align", false}}));
  auto first = synthetic(4, broadband_picture(0));
  auto second = synthetic(4, broadband_picture(0));
  auto mixed = synthetic(
      4, fields_picture(broadband_picture(0, 30), broadband_picture(99)));

  auto stacked = stack_of(stage, {first, second, mixed});
  ASSERT_NE(stacked, nullptr);

  const auto output = stacked->get_frame_copy(orc::FrameID{1});
  const auto reference = first->get_frame_copy(orc::FrameID{1});
  ASSERT_EQ(output.size(), reference.size());
  for (size_t line : {40u, 200u, 400u, 600u}) {
    const int expected_lift = line < kPalField1Lines ? 10 : 0;
    for (size_t n = 300; n < 1000; n += 37) {
      EXPECT_EQ(at(output, line, n), at(reference, line, n) + expected_lift)
          << "line " << line << " sample " << n;
    }
  }
  EXPECT_TRUE(stacked->get_dropout_hints(orc::FrameID{1}).empty());
}

// With two inputs there is no majority: both are kept, and the field they
// disagree on is marked as dropout so a later stage can repair it.
TEST(StackerStageTest, FieldCheck_TwoInputsThatDisagreeAreMarkedAsDropout) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters(
      {{"mode", std::string("Mean")}, {"sample_align", false}}));
  auto first = synthetic(4, broadband_picture(0));
  auto mixed =
      synthetic(4, fields_picture(broadband_picture(0), broadband_picture(99)));

  auto stacked = stack_of(stage, {first, mixed});
  ASSERT_NE(stacked, nullptr);

  const auto output = stacked->get_frame_copy(orc::FrameID{2});
  const auto a = first->get_frame_copy(orc::FrameID{2});
  const auto b = mixed->get_frame_copy(orc::FrameID{2});
  for (size_t n = 300; n < 1000; n += 37) {
    EXPECT_EQ(at(output, 400, n), (at(a, 400, n) + at(b, 400, n)) / 2);
  }

  const auto hints = stacked->get_dropout_hints(orc::FrameID{2});
  const uint64_t field2_start = orc::frame_line_sample_offset(
      orc::VideoSystem::PAL, kPalWidth, kPalField1Lines);
  uint64_t covered = 0;
  for (const auto& run : hints) {
    EXPECT_GE(run.sample_start, field2_start);
    covered += run.sample_count;
  }
  EXPECT_EQ(covered, output.size() - field2_start);
}

// ============================================================================
// Input noise and the Weighted mode
// ============================================================================

namespace {

// Gaussian noise of standard deviation |sigma|, fixed by |seed|, line and n.
double gaussian(double sigma, uint64_t seed, size_t line, int64_t n) {
  const uint64_t h = mix(static_cast<uint64_t>(n) * 0x2545f4914f6cdd1dULL +
                         static_cast<uint64_t>(line) * 0x9e3779b97f4a7c15ULL +
                         seed * 0xc2b2ae3d27d4eb4fULL);
  const uint64_t g = mix(h + 0x632be59bd9b4e019ULL);
  const double u1 =
      (static_cast<double>(h >> 11) + 1.0) / 9007199254740993.0;  // (0, 1)
  const double u2 = static_cast<double>(g >> 11) / 9007199254740992.0;
  return sigma * std::sqrt(-2.0 * std::log(u1)) *
         std::cos(6.283185307179586 * u2);
}

// FakePalSource's broadband pattern plus Gaussian noise of |sigma|.
Picture noisy_picture(double sigma, uint64_t seed) {
  return [=](size_t line, int64_t n) {
    const double value =
        FakePalSource::pattern(line, n) + gaussian(sigma, seed, line, n);
    return static_cast<sample_type>(std::clamp(std::lround(value), 0L, 1023L));
  };
}

// A 64-frame source with noise |sigma|, drawn afresh on each frame the noise
// measurement probes (7, 14, 21 and 28), so those frames are independent.
std::shared_ptr<FakeSyntheticPalSource> noisy_source(double sigma,
                                                     uint64_t seed) {
  std::vector<std::pair<size_t, Picture>> spans;
  for (size_t first : {0u, 14u, 21u, 28u}) {
    spans.emplace_back(first, noisy_picture(sigma, seed * 100 + first));
  }
  return std::make_shared<FakeSyntheticPalSource>(64, std::move(spans));
}

// A 64-frame source from a pressing: noise |pressing_sigma| shared by every
// source built with the same |pressing_seed|, plus noise |own_sigma| of its
// own, both drawn afresh on each probed frame.
std::shared_ptr<FakeSyntheticPalSource> pressed_source(double pressing_sigma,
                                                       uint64_t pressing_seed,
                                                       double own_sigma,
                                                       uint64_t own_seed) {
  std::vector<std::pair<size_t, Picture>> spans;
  for (size_t first : {0u, 14u, 21u, 28u}) {
    const uint64_t pressing = pressing_seed * 1000 + first;
    const uint64_t own = own_seed * 100 + first;
    spans.emplace_back(first, [=](size_t line, int64_t n) {
      const double value = FakePalSource::pattern(line, n) +
                           gaussian(pressing_sigma, pressing, line, n) +
                           gaussian(own_sigma, own, line, n);
      return static_cast<sample_type>(
          std::clamp(std::lround(value), 0L, 1023L));
    });
  }
  return std::make_shared<FakeSyntheticPalSource>(64, std::move(spans));
}

// Three discs of one pressing and two of another, whose stamper noise (σ 12)
// outweighs each disc's own (σ 4), as on the study's NatB set.
std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>>
uneven_pressings() {
  return {pressed_source(12.0, 1, 4.0, 1), pressed_source(12.0, 1, 4.0, 2),
          pressed_source(12.0, 1, 4.0, 3), pressed_source(12.0, 2, 4.0, 4),
          pressed_source(12.0, 2, 4.0, 5)};
}

// Variance of |output| against the clean pattern over the active picture.
double error_variance(const std::vector<sample_type>& output) {
  double sum = 0.0;
  double sum_sq = 0.0;
  double count = 0.0;
  for (size_t line = 30; line < kPalHeight; line += 3) {
    if (line >= kPalField1Lines && line < kPalField1Lines + 30) continue;
    for (size_t n = 300; n < 1000; ++n) {
      const double e = at(output, line, n) -
                       FakePalSource::pattern(line, static_cast<int64_t>(n));
      sum += e;
      sum_sq += e * e;
      count += 1.0;
    }
  }
  const double mean = sum / count;
  return sum_sq / count - mean * mean;
}

// Total samples |hints| covers with |severity|, and with any other severity.
std::pair<uint64_t, uint64_t> covered_by_severity(
    const std::vector<orc::DropoutRun>& hints, uint8_t severity) {
  uint64_t matching = 0;
  uint64_t other = 0;
  for (const auto& run : hints) {
    (run.severity == severity ? matching : other) += run.sample_count;
  }
  return {matching, other};
}

// Stack constant-valued inputs (one 16x8 NTSC frame each) and return the
// stacked value of sample 0.  These frames are too small to measure noise
// on, so every input carries the nominal noise and equal weight, and values
// are rejected beyond 60 codes of the median.
sample_type stack_constants(orc::StackerStage& stage,
                            const std::vector<sample_type>& values,
                            std::vector<orc::DropoutRun>* hints = nullptr) {
  std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>> sources;
  for (sample_type v : values) {
    sources.push_back(std::make_shared<FakeConstantSource>(v));
  }
  const auto stacked = stage.process(sources);
  if (hints) *hints = stacked->get_dropout_hints(orc::FrameID{0});
  return stacked->get_frame_copy(orc::FrameID{0})[0];
}

}  // namespace

// Each input's noise is solved from the variances of its differences with
// the others, so inputs of very different quality are told apart.
TEST(StackerStageTest, Noise_RecoversEachInputsSigma) {
  orc::StackerStage stage;
  const std::vector<double> sigmas = {3.0, 5.0, 10.0, 15.0};
  std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>> sources;
  for (size_t i = 0; i < sigmas.size(); ++i) {
    sources.push_back(noisy_source(sigmas[i], i + 1));
  }

  auto stacked = stack_of(stage, sources);
  ASSERT_NE(stacked, nullptr);

  const auto measured = stacked->noise_model(orc::FrameID{10});
  ASSERT_EQ(measured.size(), sigmas.size());
  for (size_t i = 0; i < sigmas.size(); ++i) {
    EXPECT_NEAR(measured[i].sigma, sigmas[i], 0.1 * sigmas[i]) << "input " << i;
    // Independent noise: no pressing is shared.
    EXPECT_EQ(measured[i].group, measured[0].group) << "input " << i;
  }
}

// Discs of one pressing differ only by their own noise, discs of different
// pressings by the pressings' noise as well, so the inputs are grouped by
// pressing and each σ counts both.
TEST(StackerStageTest, Noise_GroupsInputsByPressing) {
  orc::StackerStage stage;
  auto stacked = stack_of(stage, uneven_pressings());
  ASSERT_NE(stacked, nullptr);

  const auto measured = stacked->noise_model(orc::FrameID{10});
  ASSERT_EQ(measured.size(), 5u);
  EXPECT_EQ(measured[1].group, measured[0].group);
  EXPECT_EQ(measured[2].group, measured[0].group);
  EXPECT_EQ(measured[4].group, measured[3].group);
  EXPECT_NE(measured[3].group, measured[0].group);
  const double expected_sigma = std::sqrt(12.0 * 12.0 + 4.0 * 4.0);
  for (size_t i = 0; i < measured.size(); ++i) {
    EXPECT_NEAR(measured[i].sigma, expected_sigma, 0.15 * expected_sigma)
        << "input " << i;
  }

  // Each pressing counts about once, however many of its discs are stacked.
  double first = 0.0;
  double total = 0.0;
  for (size_t i = 0; i < measured.size(); ++i) {
    total += measured[i].weight;
    if (i < 3) first += measured[i].weight;
  }
  EXPECT_NEAR(first / total, 0.5, 0.07);
}

// A source of |frames| frames with its own noise σ 4 throughout, plus, from
// frame |shared_first| to |shared_last| only, noise σ 6 drawn alike for every
// source given |shares| — something the pair differences carry in one
// stretch that is not a pressing.
std::shared_ptr<FakeSyntheticPalSource> locally_shared_source(
    size_t frames, size_t shared_first, size_t shared_last, bool shares,
    uint64_t seed) {
  auto picture = [=](uint64_t own, bool shared) -> Picture {
    return [=](size_t line, int64_t n) {
      double value =
          FakePalSource::pattern(line, n) + gaussian(4.0, own, line, n);
      if (shared) value += gaussian(6.0, 7777, line, n);
      return static_cast<sample_type>(
          std::clamp(std::lround(value), 0L, 1023L));
    };
  };
  std::vector<std::pair<size_t, Picture>> spans;
  spans.emplace_back(0, picture(seed * 100, false));
  spans.emplace_back(shared_first, picture(seed * 100 + 1, shares));
  if (shared_last + 1 < frames) {
    spans.emplace_back(shared_last + 1, picture(seed * 100 + 2, false));
  }
  return std::make_shared<FakeSyntheticPalSource>(frames, std::move(spans));
}

// Five discs of one pressing, of which the last two share extra noise in
// frames |shared_first|..|shared_last| of |frames|.
std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>>
one_pressing_with_local_sharing(size_t frames, size_t shared_first,
                                size_t shared_last) {
  std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>> sources;
  for (size_t i = 0; i < 5; ++i) {
    sources.push_back(locally_shared_source(frames, shared_first, shared_last,
                                            i >= 3, i + 1));
  }
  return sources;
}

// Pressings do not change within a side, so they are found once from frames
// across the whole stack.  One segment whose pair differences alone would
// split a pressing must not split it there and hand the inputs it separates
// most of the weight.
TEST(StackerStageTest, Noise_PressingsAreFoundAcrossTheWholeStack) {
  // The disturbed stretch on its own reads as two pressings...
  {
    orc::StackerStage stage;
    ASSERT_TRUE(stage.set_parameters({{"sample_align", false}}));
    auto stacked =
        stack_of(stage, one_pressing_with_local_sharing(500, 0, 499));
    ASSERT_NE(stacked, nullptr);
    const auto measured = stacked->noise_model(orc::FrameID{200});
    ASSERT_EQ(measured.size(), 5u);
    EXPECT_NE(measured[3].group, measured[0].group);
  }

  // ...but is one segment of sixteen in a long stack of one pressing.
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"sample_align", false}}));
  auto stacked =
      stack_of(stage, one_pressing_with_local_sharing(8000, 3500, 3999));
  ASSERT_NE(stacked, nullptr);
  const auto measured = stacked->noise_model(orc::FrameID{3700});
  ASSERT_EQ(measured.size(), 5u);
  double total = 0.0;
  for (const auto& noise : measured) total += noise.weight;
  for (size_t i = 0; i < measured.size(); ++i) {
    EXPECT_EQ(measured[i].group, measured[0].group) << "input " << i;
    EXPECT_GT(measured[i].weight / total, 0.1) << "input " << i;
    EXPECT_LT(measured[i].weight / total, 0.35) << "input " << i;
  }
}

// With grouping off every input is taken as one pressing, even when they
// come from two.  The discs of the smaller pressing then carry the noise
// between the pressings as their own, measure noisier, and are weighted down,
// so the larger pressing takes more than its share by count.
TEST(StackerStageTest, Noise_GroupPressingsOffTakesOnePressing) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"group_pressings", false}}));
  auto stacked = stack_of(stage, uneven_pressings());
  ASSERT_NE(stacked, nullptr);

  const auto measured = stacked->noise_model(orc::FrameID{10});
  ASSERT_EQ(measured.size(), 5u);
  double first = 0.0;
  double total = 0.0;
  for (size_t i = 0; i < measured.size(); ++i) {
    EXPECT_EQ(measured[i].group, measured[0].group) << "input " << i;
    total += measured[i].weight;
    if (i < 3) first += measured[i].weight;
  }
  EXPECT_GT(measured[3].sigma, 1.2 * measured[0].sigma);
  EXPECT_GT(first / total, 0.7);
}

// Shared pressing noise only averages out across pressings, so weighting an
// under-represented pressing down would leave more of the larger pressing's
// noise in the stack than Mean does.
TEST(StackerStageTest, Weighted_UnevenPressingsAreNoWorseThanTheMean) {
  const auto sources = uneven_pressings();

  orc::StackerStage weighted;
  ASSERT_TRUE(weighted.set_parameters({{"mode", std::string("Weighted")}}));
  orc::StackerStage mean;
  ASSERT_TRUE(mean.set_parameters({{"mode", std::string("Mean")}}));

  const double weighted_variance = error_variance(
      stack_of(weighted, sources)->get_frame_copy(orc::FrameID{20}));
  const double mean_variance =
      error_variance(stack_of(mean, sources)->get_frame_copy(orc::FrameID{20}));

  EXPECT_LE(weighted_variance, mean_variance);
}

// Weighting by noise lets two clean inputs outweigh a noisy one; Mean gives
// all three a third each.
TEST(StackerStageTest, Weighted_UnequalNoiseBeatsTheMean) {
  std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>> sources = {
      noisy_source(3.0, 1), noisy_source(3.0, 2), noisy_source(15.0, 3)};

  orc::StackerStage weighted;
  ASSERT_TRUE(weighted.set_parameters({{"mode", std::string("Weighted")}}));
  orc::StackerStage mean;
  ASSERT_TRUE(mean.set_parameters({{"mode", std::string("Mean")}}));

  const double weighted_variance = error_variance(
      stack_of(weighted, sources)->get_frame_copy(orc::FrameID{20}));
  const double mean_variance =
      error_variance(stack_of(mean, sources)->get_frame_copy(orc::FrameID{20}));

  EXPECT_LT(weighted_variance, 0.5 * mean_variance);
}

TEST(StackerStageTest, Weighted_RejectsAnOutlierBeyondTheThreshold) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"mode", std::string("Weighted")}}));

  std::vector<orc::DropoutRun> hints;
  EXPECT_EQ(stack_constants(stage, {100, 104, 400}, &hints), 102);
  EXPECT_TRUE(hints.empty());
}

// Captures of different pressings differ by more than the old fixed Smart
// Mean threshold of 15 but less than the rejection distance; both are
// averaged, not chosen between.
TEST(StackerStageTest, Weighted_AveragesGroupsWithinTheThreshold) {
  orc::StackerStage weighted;
  ASSERT_TRUE(weighted.set_parameters({{"mode", std::string("Weighted")}}));

  EXPECT_EQ(stack_constants(weighted, {100, 100, 140}), 113);
}

// reject_sigma scales the rejection distance: at 2 the same outlier group is
// left out.
TEST(StackerStageTest, Weighted_RejectSigmaSetsTheDistance) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters(
      {{"mode", std::string("Weighted")}, {"reject_sigma", 2.0}}));

  EXPECT_EQ(stack_constants(stage, {100, 100, 130}), 100);
}

TEST(StackerStageTest, Weighted_RoundsInsteadOfTruncating) {
  orc::StackerStage weighted;
  ASSERT_TRUE(weighted.set_parameters({{"mode", std::string("Weighted")}}));
  orc::StackerStage mean;
  ASSERT_TRUE(mean.set_parameters({{"mode", std::string("Mean")}}));

  EXPECT_EQ(stack_constants(weighted, {1, 2}), 2);
  // The existing modes keep their truncating divide.
  EXPECT_EQ(stack_constants(mean, {1, 2}), 1);
}

TEST(StackerStageTest, Weighted_HasNoBiasOverRandomInputs) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"mode", std::string("Weighted")}}));

  double bias = 0.0;
  constexpr int kTrials = 400;
  for (int t = 0; t < kTrials; ++t) {
    const auto a = static_cast<sample_type>(
        200 + mix(static_cast<uint64_t>(t) * 0x9e3779b97f4a7c15ULL + 1) % 40);
    const auto b = static_cast<sample_type>(
        200 + mix(static_cast<uint64_t>(t) * 0xc2b2ae3d27d4eb4fULL + 2) % 40);
    bias += stack_constants(stage, {a, b}) - 0.5 * (a + b);
  }
  EXPECT_NEAR(bias / kTrials, 0.0, 0.05);
}

// Two inputs cannot outvote each other: where they differ by more than the
// rejection distance the output is marked as low-confidence dropout.
TEST(StackerStageTest, Weighted_MarksTwoInputsThatDisagree) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"mode", std::string("Weighted")}}));

  std::vector<orc::DropoutRun> hints;
  stack_constants(stage, {100, 200}, &hints);

  const auto [low, other] = covered_by_severity(hints, 25);
  EXPECT_EQ(low, kStackWidth * kStackHeight);
  EXPECT_EQ(other, 0u);
}

// Three inputs that each disagree with the others leave no majority.
TEST(StackerStageTest, Weighted_MarksASplitVote) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"mode", std::string("Weighted")}}));

  std::vector<orc::DropoutRun> hints;
  stack_constants(stage, {100, 300, 500}, &hints);

  EXPECT_EQ(covered_by_severity(hints, 25).first, kStackWidth * kStackHeight);
}

TEST(StackerStageTest, Weighted_AgreeingInputsAreNotMarked) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"mode", std::string("Weighted")}}));

  for (const std::vector<sample_type>& values :
       {std::vector<sample_type>{100, 140},
        std::vector<sample_type>{300, 305, 310, 296}}) {
    std::vector<orc::DropoutRun> hints;
    stack_constants(stage, values, &hints);
    EXPECT_TRUE(hints.empty());
  }
}

// Where every input is flagged, passthrough keeps their combined value,
// still marked, rather than black.
TEST(StackerStageTest, Passthrough_KeepsTheFlaggedValues) {
  for (const bool passthrough : {false, true}) {
    orc::StackerStage stage;
    ASSERT_TRUE(stage.set_parameters(
        {{"mode", std::string("Mean")}, {"passthrough", passthrough}}));
    auto src0 = std::make_shared<FakeDropoutSource>(100, 1);
    auto src1 = std::make_shared<FakeDropoutSource>(200, 1);

    const auto stacked = stage.process({src0, src1});
    const auto frame = stacked->get_frame_copy(orc::FrameID{0});
    EXPECT_EQ(frame[kStackWidth + 4], passthrough ? 150 : 282);
    EXPECT_EQ(frame[0], 150);

    const auto hints = stacked->get_dropout_hints(orc::FrameID{0});
    ASSERT_EQ(hints.size(), 1u);
    EXPECT_EQ(hints[0].sample_start, kStackWidth + 4);
    EXPECT_EQ(hints[0].sample_count, 8u);
  }
}

// diff_dod recovers flagged values within 10 % of their median, as
// ld-disc-stacker does; 600 lies outside 540 ± 54.
TEST(StackerStageTest, DiffDod_KeepsValuesWithinTenPercentOfTheMedian) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters({{"mode", std::string("Mean")}}));
  auto src0 = std::make_shared<FakeDropoutSource>(500, 1);
  auto src1 = std::make_shared<FakeDropoutSource>(540, 1);
  auto src2 = std::make_shared<FakeDropoutSource>(600, 1);

  const auto stacked = stage.process({src0, src1, src2});
  EXPECT_EQ(stacked->get_frame_copy(orc::FrameID{0})[kStackWidth + 4], 520);
  EXPECT_FALSE(stacked->get_dropout_hints(orc::FrameID{0}).empty());
}

// The neighbour modes were Median in all but name; they are still read, as
// Median, but no longer offered.
TEST(StackerStageTest, NeighbourModes_AreReadAsMedian) {
  for (const char* legacy : {"Neighbor", "Smart Neighbor"}) {
    orc::StackerStage stage;
    ASSERT_TRUE(stage.set_parameters({{"mode", std::string(legacy)}}));
    EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("mode")),
              "Median");
  }

  orc::StackerStage stage;
  const auto descriptors = stage.get_parameter_descriptors();
  const auto* mode = find_descriptor(descriptors, "mode");
  ASSERT_NE(mode, nullptr);
  EXPECT_EQ(std::count(mode->constraints.allowed_strings.begin(),
                       mode->constraints.allowed_strings.end(), "Neighbor"),
            0);
}

// ============================================================================
// Chroma gain matching
// ============================================================================

namespace {

constexpr double kPi = 3.14159265358979323846;

// Smooth luma under a subcarrier of amplitude |subcarrier| on every sample,
// burst window included, as a decode whose MTF correction lifted the whole
// subcarrier band delivers it.
Picture coloured_picture(int subcarrier) {
  return [=](size_t line, int64_t n) {
    const double luma =
        150.0 * std::sin(2.0 * kPi * static_cast<double>(n) / 400.0 +
                         static_cast<double>(line) * 0.01);
    static constexpr int kPhase[4] = {0, 1, 0, -1};
    return static_cast<sample_type>(400 + std::lround(luma) +
                                    subcarrier * kPhase[n % 4]);
  };
}

std::vector<sample_type> frame_of(Picture picture) {
  return synthetic(1, std::move(picture))->get_frame_copy(orc::FrameID{0});
}

std::optional<double> burst_of(const std::vector<sample_type>& frame) {
  return orc::StackerStage::burst_amplitude(frame, {}, orc::VideoSystem::PAL,
                                            kPalWidth, kPalHeight);
}

}  // namespace

TEST(StackerStageTest, BurstAmplitude_MeasuresTheSubcarrierInTheBurstWindow) {
  const auto amplitude = burst_of(frame_of(coloured_picture(100)));
  ASSERT_TRUE(amplitude.has_value());
  EXPECT_NEAR(*amplitude, 100.0, 1.0);
}

// Picture or noise in the burst window is not a burst, and a frame without
// one gives no amplitude rather than a wrong one.
TEST(StackerStageTest, BurstAmplitude_NoBurstWithoutASubcarrier) {
  EXPECT_FALSE(burst_of(frame_of(broadband_picture(3))).has_value());
  EXPECT_FALSE(burst_of(frame_of(flat_picture())).has_value());
}

TEST(StackerStageTest, BurstAmplitude_SkipsLinesFlaggedInTheBurstWindow) {
  auto frame = frame_of(coloured_picture(100));
  std::vector<orc::DropoutRun> runs;
  for (size_t line = 30; line < 200; ++line) {
    const size_t start =
        orc::frame_line_sample_offset(orc::VideoSystem::PAL, kPalWidth, line);
    for (size_t n = 100; n < 136; ++n) frame[start + n] = 1000;
    orc::DropoutRun run;
    run.sample_start = start + 100;
    run.sample_count = 36;
    runs.push_back(run);
  }
  const auto amplitude = orc::StackerStage::burst_amplitude(
      frame, runs, orc::VideoSystem::PAL, kPalWidth, kPalHeight);
  ASSERT_TRUE(amplitude.has_value());
  EXPECT_NEAR(*amplitude, 100.0, 1.0);
}

TEST(StackerStageTest, ChromaGain_BringsTheBurstToTheReference) {
  const auto reference = orc::StackerStage::burst_reference(
      {100.0, std::nullopt, 120.0, 99.0, 101.0});
  ASSERT_TRUE(reference.has_value());
  EXPECT_DOUBLE_EQ(*reference, 100.5);

  EXPECT_NEAR(orc::StackerStage::chroma_gain(120.0, 100.0), 100.0 / 120.0,
              1e-12);
  EXPECT_NEAR(orc::StackerStage::chroma_gain(80.0, 100.0), 1.25, 1e-12);
  // Within the measurement's scatter, missing, or too large to be a
  // correction: left alone.
  EXPECT_EQ(orc::StackerStage::chroma_gain(103.0, 100.0), 1.0);
  EXPECT_EQ(orc::StackerStage::chroma_gain(std::nullopt, 100.0), 1.0);
  EXPECT_EQ(orc::StackerStage::chroma_gain(100.0, std::nullopt), 1.0);
  EXPECT_EQ(orc::StackerStage::chroma_gain(40.0, 100.0), 1.0);
  EXPECT_FALSE(orc::StackerStage::burst_reference({std::nullopt}).has_value());
}

// The subcarrier is scaled by the gain and the luma under it is left as it
// was.
TEST(StackerStageTest, ApplyChromaGain_ScalesTheSubcarrierBandOnly) {
  auto lifted = frame_of(coloured_picture(120));
  const auto expected = frame_of(coloured_picture(100));
  orc::StackerStage::apply_chroma_gain(lifted, 100.0 / 120.0);
  for (size_t line : {40u, 300u, 500u}) {
    for (size_t n = 10; n < 1120; ++n) {
      EXPECT_NEAR(at(lifted, line, n), at(expected, line, n), 1)
          << "line " << line << " sample " << n;
    }
  }
}

// One input carries its chroma, burst and all, a fifth stronger than the
// other two.  Matched to their burst it stacks back to their picture;
// unmatched, Mean lifts the stack's chroma by a fifteenth.
TEST(StackerStageTest, ChromaGainMatch_StacksALiftedInputBackToTheOthers) {
  auto first = synthetic(4, coloured_picture(100));
  auto second = synthetic(4, coloured_picture(100));
  auto lifted = synthetic(4, coloured_picture(120));
  const auto reference = first->get_frame_copy(orc::FrameID{1});

  for (bool match : {true, false}) {
    orc::StackerStage stage;
    ASSERT_TRUE(stage.set_parameters({{"mode", std::string("Mean")},
                                      {"sample_align", false},
                                      {"chroma_gain_match", match}}));
    auto stacked = stack_of(stage, {first, second, lifted});
    ASSERT_NE(stacked, nullptr);
    const auto output = stacked->get_frame_copy(orc::FrameID{1});

    const auto amplitude = burst_of(output);
    ASSERT_TRUE(amplitude.has_value());
    EXPECT_NEAR(*amplitude, match ? 100.0 : 320.0 / 3.0, 1.0)
        << "chroma_gain_match " << match;
    if (!match) continue;
    for (size_t line : {40u, 400u}) {
      for (size_t n = 300; n < 1000; n += 7) {
        EXPECT_NEAR(at(output, line, n), at(reference, line, n), 1)
            << "line " << line << " sample " << n;
      }
    }
  }
}

TEST(StackerStageTest, ChromaGainMatch_RoundTripsAndDefaultsOn) {
  orc::StackerStage stage;
  EXPECT_TRUE(std::get<bool>(stage.get_parameters().at("chroma_gain_match")));
  const auto* descriptor =
      find_descriptor(stage.get_parameter_descriptors(), "chroma_gain_match");
  ASSERT_NE(descriptor, nullptr);
  ASSERT_TRUE(descriptor->constraints.default_value.has_value());
  EXPECT_TRUE(std::get<bool>(*descriptor->constraints.default_value));

  ASSERT_TRUE(stage.set_parameters({{"chroma_gain_match", false}}));
  EXPECT_FALSE(std::get<bool>(stage.get_parameters().at("chroma_gain_match")));

  EXPECT_FALSE(stage.set_parameters({{"chroma_gain_match", int32_t{0}}}));
}

TEST(StackerStageTest, RejectSigma_RoundTripsAndIsBounded) {
  orc::StackerStage stage;
  EXPECT_EQ(std::get<double>(stage.get_parameters().at("reject_sigma")), 5.0);

  ASSERT_TRUE(stage.set_parameters({{"reject_sigma", 3.5}}));
  EXPECT_EQ(std::get<double>(stage.get_parameters().at("reject_sigma")), 3.5);

  EXPECT_FALSE(stage.set_parameters({{"reject_sigma", 1.0}}));
  EXPECT_FALSE(stage.set_parameters({{"reject_sigma", 11.0}}));
  EXPECT_FALSE(stage.set_parameters({{"reject_sigma", std::string("5")}}));
}

TEST(StackerStageTest, GroupPressings_RoundTripsAndDefaultsOn) {
  orc::StackerStage stage;
  EXPECT_TRUE(std::get<bool>(stage.get_parameters().at("group_pressings")));

  ASSERT_TRUE(stage.set_parameters({{"group_pressings", false}}));
  EXPECT_FALSE(std::get<bool>(stage.get_parameters().at("group_pressings")));

  EXPECT_FALSE(stage.set_parameters({{"group_pressings", int32_t{0}}}));
}

TEST(StackerStageTest, DefaultMode_IsWeighted) {
  orc::StackerStage stage;
  EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("mode")),
            "Weighted");
}

}  // namespace orc_unit_test
