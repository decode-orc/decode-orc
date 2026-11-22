/*
 * File:        preview_dropout_override_test.cpp
 * Module:      orc-core-tests
 * Purpose:     A render can turn dropout highlighting off for itself without
 *              changing the renderer-wide setting
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/preview/stage_custom_preview_renderer.h>
#include <orc/stage/stage.h>
#include <orc/stage/video_frame_representation.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "preview_renderer.h"

namespace orc {
namespace {

constexpr uint32_t kWidth = 4;
constexpr uint32_t kHeight = 2;
constexpr uint8_t kGrey = 100;

/// Minimal VFR so the node has an output; the stage renders for itself.
class EmptyVfr final : public VideoFrameRepresentation, public Artifact {
 public:
  EmptyVfr() : Artifact(ArtifactID("dropout_override_probe"), Provenance{}) {}

  std::string type_name() const override { return "VideoFrameRepresentation"; }

  FrameIDRange frame_range() const override { return {0, 0}; }
  size_t frame_count() const override { return 1; }
  bool has_frame(FrameID id) const override { return id == 0; }
  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID /*id*/) const override {
    return std::nullopt;
  }
  const sample_type* get_frame(FrameID /*id*/) const override {
    return nullptr;
  }
  std::vector<sample_type> get_frame_copy(FrameID /*id*/) const override {
    return {};
  }
};

/// A flat grey image with one dropout region on line 0, samples 0 and 1.
class DropoutProbeStage final : public DAGStage,
                                public IStageCustomPreviewRenderer {
 public:
  std::string version() const override { return "1.0.0"; }

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo(NodeType::SOURCE, "dropout_override_probe",
                        "Dropout Override Probe", "", 0, 0, 1, 1,
                        VideoFormatCompatibility::ALL);
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& /*inputs*/,
      const std::map<std::string, ParameterValue>& /*parameters*/,
      ObservationContext& /*ctx*/) override {
    return {std::make_shared<EmptyVfr>()};
  }

  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }

  std::vector<PreviewOption> get_preview_options() const override {
    return {{"probe", "Probe", true, kWidth, kHeight, 1, 1.0}};
  }

  PreviewImage render_preview(const std::string& /*option_id*/,
                              uint64_t /*index*/,
                              PreviewNavigationHint /*hint*/) const override {
    PreviewImage image;
    image.width = kWidth;
    image.height = kHeight;
    image.rgb_data.assign(static_cast<size_t>(kWidth) * kHeight * 3, kGrey);
    DropoutRegion region;
    region.line = 0;
    region.start_sample = 0;
    region.end_sample = 2;
    image.dropout_regions.push_back(region);
    return image;
  }
};

std::shared_ptr<DAG> build_probe_dag() {
  DAGNode node;
  node.node_id = NodeID(1);
  node.stage = std::make_shared<DropoutProbeStage>();

  auto dag = std::make_shared<DAG>();
  dag->add_node(std::move(node));
  return dag;
}

PreviewRenderResult render(PreviewRenderer& renderer,
                           PreviewDropoutOverlay dropouts) {
  return renderer.render_output(
      NodeID(1), PreviewOutputType::Frame_Field1_First, 0, "probe",
      PreviewNavigationHint::Random, PreviewPixelDelivery::Rgb, dropouts);
}

/// True when the first sample of the dropout region is untouched grey.
bool dropout_sample_is_grey(const PreviewRenderResult& result) {
  return result.image.rgb_data.at(0) == kGrey &&
         result.image.rgb_data.at(1) == kGrey &&
         result.image.rgb_data.at(2) == kGrey;
}

}  // namespace

TEST(PreviewDropoutOverrideTest, FollowsTheSettingByDefault) {
  PreviewRenderer renderer(build_probe_dag());
  renderer.set_show_dropouts(true);

  const auto result = render(renderer, PreviewDropoutOverlay::FollowSetting);

  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_FALSE(dropout_sample_is_grey(result));
  // Outside the region the image is untouched.
  EXPECT_EQ(result.image.rgb_data.at(2 * 3), kGrey);
}

TEST(PreviewDropoutOverrideTest, OffSuppressesTheHighlightAndKeepsTheSetting) {
  PreviewRenderer renderer(build_probe_dag());
  renderer.set_show_dropouts(true);

  const auto result = render(renderer, PreviewDropoutOverlay::Off);

  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_TRUE(dropout_sample_is_grey(result));
  EXPECT_EQ(
      result.image.rgb_data,
      std::vector<uint8_t>(static_cast<size_t>(kWidth) * kHeight * 3, kGrey));
  // The regions still travel; only the burn-in is left out.
  EXPECT_EQ(result.image.dropout_regions.size(), 1u);
  EXPECT_TRUE(renderer.get_show_dropouts());
}

TEST(PreviewDropoutOverrideTest, TheNextRenderFollowsTheSettingAgain) {
  PreviewRenderer renderer(build_probe_dag());
  renderer.set_show_dropouts(true);

  (void)render(renderer, PreviewDropoutOverlay::Off);
  const auto result = render(renderer, PreviewDropoutOverlay::FollowSetting);

  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_FALSE(dropout_sample_is_grey(result));
}

TEST(PreviewDropoutOverrideTest, OffWithTheSettingOffStillShowsNone) {
  PreviewRenderer renderer(build_probe_dag());
  renderer.set_show_dropouts(false);

  const auto result = render(renderer, PreviewDropoutOverlay::Off);

  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_TRUE(dropout_sample_is_grey(result));
  EXPECT_FALSE(renderer.get_show_dropouts());
}

}  // namespace orc
