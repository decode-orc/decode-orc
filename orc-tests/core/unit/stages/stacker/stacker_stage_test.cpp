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
  const auto* threshold = find_descriptor(descriptors, "smart_threshold");
  const auto* no_diff_dod = find_descriptor(descriptors, "no_diff_dod");
  const auto* passthrough = find_descriptor(descriptors, "passthrough");
  const auto* audio_stacking = find_descriptor(descriptors, "audio_stacking");
  const auto* efm_stacking = find_descriptor(descriptors, "efm_stacking");

  ASSERT_NE(mode, nullptr);
  ASSERT_NE(threshold, nullptr);
  ASSERT_NE(no_diff_dod, nullptr);
  ASSERT_NE(passthrough, nullptr);
  ASSERT_NE(audio_stacking, nullptr);
  ASSERT_NE(efm_stacking, nullptr);

  if (!mode->constraints.default_value.has_value() ||
      !threshold->constraints.default_value.has_value() ||
      !no_diff_dod->constraints.default_value.has_value() ||
      !passthrough->constraints.default_value.has_value() ||
      !audio_stacking->constraints.default_value.has_value() ||
      !efm_stacking->constraints.default_value.has_value()) {
    FAIL() << "Expected all descriptors to have default values";
    return;
  }

  EXPECT_EQ(std::get<std::string>(*mode->constraints.default_value),
            std::get<std::string>(params.at("mode")));
  EXPECT_EQ(std::get<int32_t>(*threshold->constraints.default_value),
            std::get<int32_t>(params.at("smart_threshold")));
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
      stage.set_parameters({{"mode", std::string("Smart Mean")},
                            {"smart_threshold", static_cast<int32_t>(17)},
                            {"no_diff_dod", true},
                            {"passthrough", true},
                            {"audio_stacking", std::string("Median")},
                            {"efm_stacking", std::string("Disabled")}});
  const auto params = stage.get_parameters();

  EXPECT_TRUE(result);
  EXPECT_EQ(std::get<std::string>(params.at("mode")), "Smart Mean");
  EXPECT_EQ(std::get<int32_t>(params.at("smart_threshold")), 17);
  EXPECT_TRUE(std::get<bool>(params.at("no_diff_dod")));
  EXPECT_TRUE(std::get<bool>(params.at("passthrough")));
  EXPECT_EQ(std::get<std::string>(params.at("audio_stacking")), "Median");
  EXPECT_EQ(std::get<std::string>(params.at("efm_stacking")), "Disabled");
}

TEST(StackerStageTest, SetParameters_AcceptsLegacyIntegerMode) {
  orc::StackerStage stage;

  ASSERT_TRUE(stage.set_parameters({{"mode", int32_t(2)}}));

  EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("mode")),
            "Smart Mean");
}

TEST(StackerStageTest, SetParameters_RejectsInvalidMode) {
  orc::StackerStage stage;
  EXPECT_FALSE(stage.set_parameters({{"mode", std::string("Nope")}}));
}

TEST(StackerStageTest, SetParameters_RejectsThresholdOutsideBounds) {
  orc::StackerStage stage;
  EXPECT_FALSE(stage.set_parameters({{"smart_threshold", int32_t(129)}}));
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

}  // namespace orc_unit_test
