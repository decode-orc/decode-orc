/*
 * File:        parallel_frame_observation_test.cpp
 * Module:      orc-core tests
 * Purpose:     observe_frames_in_parallel() (the GUI trigger precompute):
 *              its workers share node outputs, so each node executes once
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "observation_scheduler.h"
#include "observation_store.h"
#include "stage_registry.h"

namespace orc {
namespace {

constexpr FrameID kFrames = 64;

std::atomic<int>& source_executions() {
  static std::atomic<int> count{0};
  return count;
}
std::atomic<int>& transform_executions() {
  static std::atomic<int> count{0};
  return count;
}

class PrecomputeFrames final : public VideoFrameRepresentation,
                               public Artifact {
 public:
  PrecomputeFrames()
      : Artifact(ArtifactID("precompute-frames"), Provenance{}),
        samples_(16, 0) {}
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

NodeTypeInfo type_info(NodeType type, const char* name, uint32_t inputs) {
  return NodeTypeInfo(type, name, name, "", inputs, inputs, 1, 1,
                      VideoFormatCompatibility::ALL);
}

class PrecomputeSource final : public DAGStage {
 public:
  std::string version() const override { return "1.0"; }
  NodeTypeInfo get_node_type_info() const override {
    return type_info(NodeType::SOURCE, "unit_test_precompute_source", 0);
  }
  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& /*inputs*/,
      const std::map<std::string, ParameterValue>& /*parameters*/,
      ObservationContext& /*ctx*/) override {
    ++source_executions();
    return {std::make_shared<PrecomputeFrames>()};
  }
  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }
};

class PrecomputeTransform final : public DAGStage {
 public:
  std::string version() const override { return "1.0"; }
  NodeTypeInfo get_node_type_info() const override {
    return type_info(NodeType::TRANSFORM, "unit_test_precompute_transform", 1);
  }
  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& /*parameters*/,
      ObservationContext& /*ctx*/) override {
    ++transform_executions();
    return {inputs.at(0)};
  }
  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 1; }
};

// The workers render private copies of the DAG, which are made through the
// stage registry.
void ensure_stages_registered() {
  static const bool registered = [] {
    auto& registry = StageRegistry::instance();
    if (!registry.has_stage("unit_test_precompute_source")) {
      registry.register_stage("unit_test_precompute_source", [] {
        return std::make_shared<PrecomputeSource>();
      });
    }
    if (!registry.has_stage("unit_test_precompute_transform")) {
      registry.register_stage("unit_test_precompute_transform", [] {
        return std::make_shared<PrecomputeTransform>();
      });
    }
    return true;
  }();
  (void)registered;
}

std::shared_ptr<const DAG> precompute_dag() {
  auto dag = std::make_shared<DAG>();
  DAGNode source;
  source.node_id = NodeID(1);
  source.stage = std::make_shared<PrecomputeSource>();
  dag->add_node(source);
  DAGNode transform;
  transform.node_id = NodeID(2);
  transform.stage = std::make_shared<PrecomputeTransform>();
  transform.input_node_ids = {NodeID(1)};
  transform.input_indices = {0};
  dag->add_node(transform);
  return dag;
}

std::vector<FrameID> all_frames() {
  std::vector<FrameID> frames;
  for (FrameID id = 0; id < kFrames; ++id) frames.push_back(id);
  return frames;
}

// Every worker renders its own copy of the DAG, but they take node outputs
// from one pool: the source and the transform execute once between them, not
// once per worker, so the workers hold one set of frame caches.
TEST(ObserveFramesInParallel, WorkersShareNodeOutputs) {
  ensure_stages_registered();
  source_executions() = 0;
  transform_executions() = 0;

  auto fingerprints = std::make_shared<NodeFingerprintMap>();
  (*fingerprints)[NodeID(1)] = NodeFingerprint{"fp-source"};
  (*fingerprints)[NodeID(2)] = NodeFingerprint{"fp-transform"};
  auto store = std::make_shared<ObservationStore>();
  const std::atomic<bool> cancel{false};
  FrameObservationCounts counts;

  observe_frames_in_parallel(precompute_dag(), fingerprints, store, NodeID(2),
                             NodeFingerprint{"fp-transform"}, all_frames(),
                             /*workers=*/4, cancel, counts, nullptr);

  EXPECT_EQ(counts.done.load(), static_cast<std::uint64_t>(kFrames));
  EXPECT_EQ(counts.failed.load(), 0u);
  EXPECT_EQ(source_executions().load(), 1);
  EXPECT_EQ(transform_executions().load(), 1);
}

TEST(ObserveFramesInParallel, CancelledBeforeStartObservesNothing) {
  ensure_stages_registered();
  source_executions() = 0;

  auto fingerprints = std::make_shared<NodeFingerprintMap>();
  (*fingerprints)[NodeID(2)] = NodeFingerprint{"fp-transform"};
  auto store = std::make_shared<ObservationStore>();
  const std::atomic<bool> cancel{true};
  FrameObservationCounts counts;
  int ticks = 0;

  observe_frames_in_parallel(precompute_dag(), fingerprints, store, NodeID(2),
                             NodeFingerprint{"fp-transform"}, all_frames(),
                             /*workers=*/4, cancel, counts, [&] { ++ticks; });

  EXPECT_EQ(counts.done.load(), 0u);
  EXPECT_EQ(ticks, 0);
  EXPECT_EQ(source_executions().load(), 0);
}

}  // namespace
}  // namespace orc
