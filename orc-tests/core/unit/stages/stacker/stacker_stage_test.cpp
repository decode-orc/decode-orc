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

#include <algorithm>
#include <cstdint>
#include <map>

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
  orc::StackerStage stage;  // efm_stacking defaults to Mean
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

// A single contributing source is still an EFM-stacking output, so it is
// reported the same way: t-values with no doubt of their own.
TEST(StackerStageTest, EfmStacking_StripsDoubtEvenFromASingleSource) {
  orc::StackerStage stage;  // Mean
  auto src0 = make_efm_stack_source({orc::efm_pack(4, 12)});
  auto src1 = make_audio_stack_source();  // no EFM to contribute

  const orc::StackedVideoFrameRepresentation stacked({src0, src1}, &stage);

  EXPECT_EQ(stacked.get_efm_samples(orc::FrameID{0}),
            (std::vector<uint8_t>{4}));
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

}  // namespace orc_unit_test
