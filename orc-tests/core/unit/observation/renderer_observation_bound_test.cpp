/*
 * File:        renderer_observation_bound_test.cpp
 * Module:      orc-core tests
 * Purpose:     DAGFrameRenderer keeps the observations of its most recently
 *              observed frames only, however many frames it renders
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/stage.h>
#include <orc/stage/video_frame_representation.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dag_frame_renderer.h"
#include "observation_test_doubles.h"

namespace orc {
namespace {

constexpr FrameID kFrames = 1200;

class SweepFrames final : public VideoFrameRepresentation, public Artifact {
 public:
  SweepFrames()
      : Artifact(ArtifactID("sweep-frames"), Provenance{}), samples_(16, 0) {}
  std::string type_name() const override { return "VideoFrameRepresentation"; }

  FrameIDRange frame_range() const override { return {0, kFrames - 1}; }
  size_t frame_count() const override { return kFrames; }
  bool has_frame(FrameID id) const override { return id < kFrames; }
  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID /*id*/) const override {
    return std::nullopt;
  }
  const sample_type* get_frame(FrameID /*id*/) const override {
    return samples_.data();
  }
  std::vector<sample_type> get_frame_copy(FrameID /*id*/) const override {
    return samples_;
  }

 private:
  std::vector<sample_type> samples_;
};

class SweepSource final : public DAGStage {
 public:
  std::string version() const override { return "1.0.0"; }
  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo(NodeType::SOURCE, "sweep_source", "sweep_source", "", 0,
                        0, 1, 1, VideoFormatCompatibility::ALL);
  }
  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& /*inputs*/,
      const std::map<std::string, ParameterValue>& /*parameters*/,
      ObservationContext& /*ctx*/) override {
    return {std::make_shared<SweepFrames>()};
  }
  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }
};

std::shared_ptr<DAG> sweep_dag() {
  auto dag = std::make_shared<DAG>();
  DAGNode node;
  node.node_id = NodeID(1);
  node.stage = std::make_shared<SweepSource>();
  dag->add_node(node);
  return dag;
}

bool observed(const ObservationContext& context, FrameID frame) {
  return context.has(FieldID(frame * 2), "obs", "i32") &&
         context.has(FieldID(frame * 2 + 1), "obs", "i32");
}

// A sweep renders every frame of a capture through one renderer. The context
// keeps the last kMaxObservedFrames frames' values, not every frame's.
TEST(DAGFrameRendererObservations, ContextStaysBoundedOverALongSweep) {
  auto spy = std::make_shared<test::SpyObservationService>();
  spy->observers = {test::make_observer_info("obs", "1")};
  DAGFrameRenderer renderer(sweep_dag());
  renderer.set_observation_service(spy);

  for (FrameID frame = 0; frame < kFrames; ++frame) {
    ASSERT_TRUE(renderer.render_frame_at_node(NodeID(1), frame).is_valid);
  }

  const auto& context = renderer.get_observation_context();
  const FrameID kept = DAGFrameRenderer::kMaxObservedFrames;
  size_t frames_held = 0;
  for (FrameID frame = 0; frame < kFrames; ++frame) {
    if (observed(context, frame)) ++frames_held;
  }
  EXPECT_EQ(frames_held, kept);
  EXPECT_FALSE(observed(context, kFrames - kept - 1));
  EXPECT_TRUE(observed(context, kFrames - kept));
  EXPECT_TRUE(observed(context, kFrames - 1));
}

// Rendering a frame whose values were cleared observes it again, so a caller
// reading the context after a render always finds that frame's values.
TEST(DAGFrameRendererObservations, ClearedFrameIsObservedAgainWhenRendered) {
  auto spy = std::make_shared<test::SpyObservationService>();
  spy->observers = {test::make_observer_info("obs", "1")};
  DAGFrameRenderer renderer(sweep_dag());
  renderer.set_observation_service(spy);

  for (FrameID frame = 0; frame < kFrames; ++frame) {
    ASSERT_TRUE(renderer.render_frame_at_node(NodeID(1), frame).is_valid);
  }
  const int runs = spy->total_runs;

  // Frame 0's values were cleared; rendering it brings them back.
  ASSERT_FALSE(observed(renderer.get_observation_context(), 0));
  ASSERT_TRUE(renderer.render_frame_at_node(NodeID(1), 0).is_valid);
  EXPECT_TRUE(observed(renderer.get_observation_context(), 0));
  EXPECT_EQ(spy->total_runs, runs + 1);

  // A frame still held is a cache hit and runs nothing.
  const auto again = renderer.render_frame_at_node(NodeID(1), kFrames - 1);
  EXPECT_TRUE(again.from_cache);
  EXPECT_EQ(spy->total_runs, runs + 1);
  EXPECT_TRUE(observed(renderer.get_observation_context(), kFrames - 1));
}

}  // namespace
}  // namespace orc
