/*
 * File:        preview_plane_delivery_test.cpp
 * Module:      orc-core-tests
 * Purpose:     PreviewRenderer produces the pixel representation it is asked
 * for
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/preview/colour_preview_provider.h>
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

/// Minimal VFR so a source node has something to hand downstream. The renders
/// under test never read it - the stages below answer for themselves.
class EmptyVfr final : public VideoFrameRepresentation, public Artifact {
 public:
  EmptyVfr() : Artifact(ArtifactID("plane_delivery_probe"), Provenance{}) {}

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

constexpr uint32_t kProbeWidth = 2;
constexpr uint32_t kProbeHeight = 4;

/// A stage on the colour-carrier path, which is the only path that can answer
/// with planes.
class ColourProbeStage final : public DAGStage,
                               public IStagePreviewCapability,
                               public IColourPreviewProvider {
 public:
  std::string version() const override { return "1.0.0"; }

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo(NodeType::SOURCE, "colour_plane_probe",
                        "Colour Plane Probe", "", 0, 0, 1, 1,
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

  StagePreviewCapability get_preview_capability() const override {
    StagePreviewCapability capability{};
    capability.supported_data_types = {VideoDataType::ColourNTSC};
    capability.navigation_extent = {1, 1, "frame"};
    capability.geometry = {static_cast<int>(kProbeWidth),
                           static_cast<int>(kProbeHeight), 4.0 / 3.0, 1.0};
    return capability;
  }

  std::optional<ColourFrameCarrier> get_colour_preview_carrier(
      uint64_t frame_index, PreviewNavigationHint /*hint*/) const override {
    ColourFrameCarrier carrier{};
    carrier.data_type = VideoDataType::ColourNTSC;
    carrier.colorimetry = ColorimetricMetadata::default_ntsc();
    carrier.frame_index = frame_index;
    carrier.width = kProbeWidth;
    carrier.height = kProbeHeight;
    carrier.cvbs_blanking = 0.0;
    carrier.cvbs_black = 0.0;
    carrier.cvbs_white = 65535.0;

    const size_t samples = static_cast<size_t>(kProbeWidth) * kProbeHeight;
    for (size_t i = 0; i < samples; ++i) {
      // A distinct value per row, so the sequential re-ordering below is
      // visible in the plane rather than having to be inferred.
      carrier.y_plane.push_back(8192.0 * static_cast<double>(i + 1));
      carrier.u_plane.push_back(0.0);
      carrier.v_plane.push_back(0.0);
    }
    return carrier;
  }
};

std::shared_ptr<DAG> build_probe_dag() {
  DAGNode node;
  node.node_id = NodeID(1);
  node.stage = std::make_shared<ColourProbeStage>();

  auto dag = std::make_shared<DAG>();
  dag->add_node(std::move(node));
  return dag;
}

PreviewRenderResult render(PreviewRenderer& renderer,
                           PreviewPixelDelivery delivery,
                           const std::string& option_id = "") {
  return renderer.render_output(
      NodeID(1), PreviewOutputType::Frame_Field1_First, 0, option_id,
      PreviewNavigationHint::Random, delivery);
}

}  // namespace

// The two representations are exclusive: producing both would pay the
// conversion the plane payload exists to avoid.
TEST(PreviewPlaneDeliveryTest, PlanesRequestProducesPlanesAndNoImage) {
  PreviewRenderer renderer(build_probe_dag());

  const auto result = render(renderer, PreviewPixelDelivery::Planes);

  ASSERT_TRUE(result.success) << result.error_message;
  ASSERT_TRUE(result.planes.is_valid());
  EXPECT_EQ(result.planes.domain, PreviewPlaneDomain::Colour);
  EXPECT_EQ(result.planes.width, kProbeWidth);
  EXPECT_EQ(result.planes.height, kProbeHeight);
  EXPECT_TRUE(result.image.rgb_data.empty());
  EXPECT_TRUE(result.is_valid());
}

TEST(PreviewPlaneDeliveryTest, RgbRequestProducesAnImageAndNoPlanes) {
  PreviewRenderer renderer(build_probe_dag());

  const auto result = render(renderer, PreviewPixelDelivery::Rgb);

  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_TRUE(result.image.is_valid());
  EXPECT_FALSE(result.planes.is_valid());
  EXPECT_EQ(result.planes.domain, PreviewPlaneDomain::None);
}

// Nothing asks for planes unless it means to, so the export path and every
// existing caller keep the image they have always been given.
TEST(PreviewPlaneDeliveryTest, DefaultsToTheImageWhenTheCallerSaysNothing) {
  PreviewRenderer renderer(build_probe_dag());

  const auto result = renderer.render_output(
      NodeID(1), PreviewOutputType::Frame_Field1_First, 0);

  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_TRUE(result.image.is_valid());
  EXPECT_FALSE(result.planes.is_valid());
}

// The layout is chosen before the conversion, so it has to be applied to
// whichever representation the caller asked for.
TEST(PreviewPlaneDeliveryTest, SequentialLayoutReachesThePlanesToo) {
  PreviewRenderer renderer(build_probe_dag());

  const auto weaved = render(renderer, PreviewPixelDelivery::Planes);
  const auto sequential = render(renderer, PreviewPixelDelivery::Planes,
                                 "phase2_colour_carrier_sequential");
  ASSERT_TRUE(weaved.planes.is_valid());
  ASSERT_TRUE(sequential.planes.is_valid());

  // Weaved row 2 (field 1, third line) becomes sequential row 1.
  for (uint32_t x = 0; x < kProbeWidth; ++x) {
    EXPECT_FLOAT_EQ(sequential.planes.y_plane[kProbeWidth + x],
                    weaved.planes.y_plane[(2 * kProbeWidth) + x]);
  }
  // Weaved row 1 (field 2, first line) becomes the first row of the second
  // block.
  const uint32_t field1_rows = (kProbeHeight + 1) / 2;
  for (uint32_t x = 0; x < kProbeWidth; ++x) {
    EXPECT_FLOAT_EQ(sequential.planes.y_plane[(field1_rows * kProbeWidth) + x],
                    weaved.planes.y_plane[kProbeWidth + x]);
  }
}

}  // namespace orc
