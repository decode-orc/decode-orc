/*
 * File:        trigger_nodes_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     project_io::trigger_nodes() executes each upstream stage once
 *              for any number of sinks, runs sinks that share it side by side
 *              with paced reads, and lets the others finish when one fails
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../../../orc/core/include/project.h"
#include "../../../orc/core/include/stage_registry.h"

namespace orc_unit_test {
namespace {

constexpr orc::FrameID kFrames = 200;
constexpr size_t kSourceBound = 100;

std::atomic<int>& source_executions() {
  static std::atomic<int> count{0};
  return count;
}
std::atomic<int>& transform_executions() {
  static std::atomic<int> count{0};
  return count;
}

// What each sink saw, by its "name" parameter.
struct SinkRecord {
  size_t frames_read = 0;
  size_t input_bound = 0;
};
std::mutex& records_mutex() {
  static std::mutex mutex;
  return mutex;
}
std::map<std::string, SinkRecord>& records() {
  static std::map<std::string, SinkRecord> value;
  return value;
}

void reset() {
  source_executions() = 0;
  transform_executions() = 0;
  std::lock_guard<std::mutex> lock(records_mutex());
  records().clear();
}

class Frames final : public orc::VideoFrameRepresentation,
                     public orc::Artifact {
 public:
  Frames()
      : orc::Artifact(orc::ArtifactID("trigger-nodes-frames"),
                      orc::Provenance{}),
        samples_(16, 0) {}
  std::string type_name() const override { return "Frames"; }

  orc::FrameIDRange frame_range() const override { return {0, kFrames - 1}; }
  size_t frame_count() const override { return kFrames; }
  bool has_frame(orc::FrameID id) const override { return id < kFrames; }
  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID) const override {
    return std::nullopt;
  }
  const sample_type* get_frame(orc::FrameID id) const override {
    return id < kFrames ? samples_.data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID) const override {
    return samples_;
  }
  size_t max_concurrent_frame_requests() const override { return kSourceBound; }

 private:
  std::vector<sample_type> samples_;
};

orc::NodeTypeInfo type_info(orc::NodeType type, const char* name,
                            uint32_t inputs, uint32_t outputs) {
  return orc::NodeTypeInfo{type,
                           name,
                           name,
                           "Test-only stage",
                           inputs,
                           inputs,
                           outputs,
                           outputs,
                           orc::VideoFormatCompatibility::ALL};
}

class FramesSource : public orc::DAGStage {
 public:
  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return type_info(orc::NodeType::SOURCE, "unit_test_frames_source", 0, 1);
  }
  std::vector<orc::ArtifactPtr> execute(
      const std::vector<orc::ArtifactPtr>&,
      const std::map<std::string, orc::ParameterValue>&,
      orc::ObservationContext&) override {
    ++source_executions();
    return {std::make_shared<Frames>()};
  }
  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }
};

class PassTransform : public orc::DAGStage {
 public:
  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return type_info(orc::NodeType::TRANSFORM, "unit_test_pass_transform", 1,
                     1);
  }
  std::vector<orc::ArtifactPtr> execute(
      const std::vector<orc::ArtifactPtr>& inputs,
      const std::map<std::string, orc::ParameterValue>&,
      orc::ObservationContext&) override {
    ++transform_executions();
    return {inputs.at(0)};
  }
  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 1; }
};

int32_t int_parameter(const std::map<std::string, orc::ParameterValue>& p,
                      const char* name, int32_t fallback) {
  const auto it = p.find(name);
  if (it == p.end()) return fallback;
  if (const auto* v = std::get_if<int32_t>(&it->second)) return *v;
  return fallback;
}

// Reads every frame of its input in order. Parameters: name, stop_at (fails
// on reaching that frame), throw_at (throws there), delay_us (per frame).
class ReadingSink : public orc::DAGStage, public orc::TriggerableStage {
 public:
  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return type_info(orc::NodeType::SINK, "unit_test_reading_sink", 1, 0);
  }
  std::vector<orc::ArtifactPtr> execute(
      const std::vector<orc::ArtifactPtr>&,
      const std::map<std::string, orc::ParameterValue>&,
      orc::ObservationContext&) override {
    return {};
  }
  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 0; }

  bool trigger(const std::vector<orc::ArtifactPtr>& inputs,
               const std::map<std::string, orc::ParameterValue>& parameters,
               orc::IObservationContext&) override {
    std::string name = "sink";
    if (const auto it = parameters.find("name"); it != parameters.end()) {
      name = std::get<std::string>(it->second);
    }
    const int32_t stop_at = int_parameter(parameters, "stop_at", -1);
    const int32_t throw_at = int_parameter(parameters, "throw_at", -1);
    const auto delay =
        std::chrono::microseconds(int_parameter(parameters, "delay_us", 0));

    auto vfr = std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
        inputs.at(0));
    if (!vfr) {
      status_ = "no frames";
      return false;
    }
    SinkRecord record;
    record.input_bound = vfr->max_concurrent_frame_requests();
    const auto range = vfr->frame_range();
    bool ok = true;
    for (orc::FrameID id = range.first; id <= range.last; ++id) {
      if (static_cast<int32_t>(id) == stop_at) {
        status_ = "stopped";
        ok = false;
        break;
      }
      if (static_cast<int32_t>(id) == throw_at) {
        throw std::runtime_error("sink threw");
      }
      if (vfr->get_frame(id) != nullptr) ++record.frames_read;
      if (delay.count() > 0) std::this_thread::sleep_for(delay);
    }
    if (ok) status_ = "ok";
    std::lock_guard<std::mutex> lock(records_mutex());
    records()[name] = record;
    return ok;
  }
  std::string get_trigger_status() const override { return status_; }

 private:
  std::string status_;
};

void ensure_stages_registered() {
  static const bool registered = [] {
    auto& registry = orc::StageRegistry::instance();
    if (!registry.has_stage("unit_test_frames_source")) {
      registry.register_stage("unit_test_frames_source",
                              [] { return std::make_shared<FramesSource>(); });
    }
    if (!registry.has_stage("unit_test_pass_transform")) {
      registry.register_stage("unit_test_pass_transform",
                              [] { return std::make_shared<PassTransform>(); });
    }
    if (!registry.has_stage("unit_test_reading_sink")) {
      registry.register_stage("unit_test_reading_sink",
                              [] { return std::make_shared<ReadingSink>(); });
    }
    return true;
  }();
  (void)registered;
}

std::string node(int id, const char* stage, const char* type,
                 const std::string& parameters = "") {
  return "    - id: " + std::to_string(id) + "\n      stage: " + stage +
         "\n      node_type: " + type + "\n      display_name: N" +
         std::to_string(id) + "\n      user_label: N" + std::to_string(id) +
         "\n      x: 0\n      y: 0\n" + parameters;
}

std::string sink_parameters(const std::string& name, int32_t stop_at = -1,
                            int32_t throw_at = -1, int32_t delay_us = 0) {
  return "      parameters:\n        name:\n          type: string\n"
         "          value: " +
         name + "\n        stop_at:\n          type: int32\n          value: " +
         std::to_string(stop_at) +
         "\n        throw_at:\n          type: int32\n          value: " +
         std::to_string(throw_at) +
         "\n        delay_us:\n          type: int32\n          value: " +
         std::to_string(delay_us) + "\n";
}

std::string project_yaml(const std::string& nodes, const std::string& edges) {
  return "project:\n  name: trigger-nodes\n  version: \"2.0\"\n"
         "  video_format: PAL\n  source_format: Composite\n"
         "  amplitude_unit: mV\ndag:\n  nodes:\n" +
         nodes + "  edges:\n" + edges;
}

std::string edge(int from, int to) {
  return "    - from: " + std::to_string(from) +
         "\n      to: " + std::to_string(to) + "\n";
}

// Source 1 -> transform 2 -> one sink per entry of |sinks|, ids 3, 4, ...
std::vector<orc::project_io::NodeTriggerResult> run_shared(
    const std::vector<std::string>& sinks) {
  ensure_stages_registered();
  std::string nodes = node(1, "unit_test_frames_source", "SOURCE") +
                      node(2, "unit_test_pass_transform", "TRANSFORM");
  std::string edges = edge(1, 2);
  std::vector<orc::NodeID> ids;
  for (size_t i = 0; i < sinks.size(); ++i) {
    const int id = static_cast<int>(3 + i);
    nodes += node(id, "unit_test_reading_sink", "SINK", sinks[i]);
    edges += edge(2, id);
    ids.emplace_back(id);
  }
  auto project = orc::project_io::load_project_from_yaml(
      project_yaml(nodes, edges), "trigger-nodes.orcprj");
  return orc::project_io::trigger_nodes(project, ids, nullptr, nullptr);
}

SinkRecord record_of(const std::string& name) {
  std::lock_guard<std::mutex> lock(records_mutex());
  const auto it = records().find(name);
  return it == records().end() ? SinkRecord{} : it->second;
}

}  // namespace

TEST(TriggerNodesTest, EachUpstreamStageExecutesOnceForEverySink) {
  reset();
  const auto results = run_shared(
      {sink_parameters("a"), sink_parameters("b"), sink_parameters("c")});
  ASSERT_EQ(results.size(), 3u);
  for (const auto& result : results) {
    EXPECT_TRUE(result.success) << result.status;
  }
  EXPECT_EQ(source_executions(), 1);
  EXPECT_EQ(transform_executions(), 1);
  for (const char* name : {"a", "b", "c"}) {
    EXPECT_EQ(record_of(name).frames_read, kFrames) << name;
  }
}

TEST(TriggerNodesTest, SinksSharingAnInputReadItPaced) {
  reset();
  const auto results = run_shared({sink_parameters("a"), sink_parameters("b")});
  ASSERT_TRUE(results[0].success && results[1].success);
  // The paced view leaves the caches room for the window between the sinks.
  EXPECT_LT(record_of("a").input_bound, kSourceBound);
  EXPECT_LT(record_of("b").input_bound, kSourceBound);
}

TEST(TriggerNodesTest, ALoneSinkReadsItsInputUnpaced) {
  reset();
  const auto results = run_shared({sink_parameters("a")});
  ASSERT_TRUE(results[0].success);
  EXPECT_EQ(record_of("a").input_bound, kSourceBound);
}

TEST(TriggerNodesTest, SinksWithNothingInCommonRunUnpaced) {
  reset();
  ensure_stages_registered();
  const std::string nodes =
      node(1, "unit_test_frames_source", "SOURCE") +
      node(2, "unit_test_frames_source", "SOURCE") +
      node(3, "unit_test_reading_sink", "SINK", sink_parameters("a")) +
      node(4, "unit_test_reading_sink", "SINK", sink_parameters("b"));
  auto project = orc::project_io::load_project_from_yaml(
      project_yaml(nodes, edge(1, 3) + edge(2, 4)), "separate.orcprj");
  const auto results = orc::project_io::trigger_nodes(
      project, {orc::NodeID(3), orc::NodeID(4)}, nullptr, nullptr);
  ASSERT_TRUE(results[0].success && results[1].success);
  EXPECT_EQ(record_of("a").input_bound, kSourceBound);
  EXPECT_EQ(record_of("b").input_bound, kSourceBound);
}

TEST(TriggerNodesTest, ATrailingSinkThatFailsReleasesTheLeader) {
  reset();
  // "slow" holds "fast" within the window until it fails at frame 50; the
  // leader must then read on to the end.
  const auto results = run_shared(
      {sink_parameters("fast"), sink_parameters("slow", 50, -1, 200)});
  EXPECT_TRUE(results[0].success) << results[0].status;
  EXPECT_FALSE(results[1].success);
  EXPECT_EQ(results[1].status, "stopped");
  EXPECT_EQ(record_of("fast").frames_read, kFrames);
}

TEST(TriggerNodesTest, ALeadingSinkThatFailsLeavesTheOthersRunning) {
  reset();
  const auto results = run_shared(
      {sink_parameters("fast", 50), sink_parameters("slow", -1, -1, 200)});
  EXPECT_FALSE(results[0].success);
  EXPECT_TRUE(results[1].success) << results[1].status;
  EXPECT_EQ(record_of("slow").frames_read, kFrames);
}

TEST(TriggerNodesTest, ASinkThatThrowsReleasesTheOthers) {
  reset();
  const auto results = run_shared(
      {sink_parameters("fast"), sink_parameters("slow", -1, 30, 200)});
  EXPECT_TRUE(results[0].success) << results[0].status;
  EXPECT_FALSE(results[1].success);
  EXPECT_EQ(results[1].status, "sink threw");
  EXPECT_EQ(record_of("fast").frames_read, kFrames);
}

TEST(TriggerNodesTest, AnUnknownNodeFailsAloneAndTheRestRun) {
  reset();
  ensure_stages_registered();
  const std::string nodes =
      node(1, "unit_test_frames_source", "SOURCE") +
      node(3, "unit_test_reading_sink", "SINK", sink_parameters("a"));
  auto project = orc::project_io::load_project_from_yaml(
      project_yaml(nodes, edge(1, 3)), "unknown.orcprj");
  const auto results = orc::project_io::trigger_nodes(
      project, {orc::NodeID(9), orc::NodeID(3)}, nullptr, nullptr);
  EXPECT_FALSE(results[0].success);
  EXPECT_EQ(results[0].status, "Node not found");
  EXPECT_TRUE(results[1].success);
}

}  // namespace orc_unit_test
