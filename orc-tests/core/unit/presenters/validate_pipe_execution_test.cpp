/*
 * File:        validate_pipe_execution_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     Verify ProjectPresenter::validatePipeExecution() — the "-"
 *              stdio collision guard and the IStreamingCompatibility
 *              reachability check
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/streaming_capability.h>
#include <orc/stage/triggerable_stage.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../../../orc/core/include/project.h"
#include "../../../../orc/core/include/stage_registry.h"
#include "../../../../orc/presenters/include/project_presenter.h"

namespace orc_unit_test {
namespace {

class DummyArtifact : public orc::Artifact {
 public:
  DummyArtifact()
      : orc::Artifact(orc::ArtifactID("validate-pipe-execution-artifact"),
                      orc::Provenance{}) {}
  std::string type_name() const override { return "DummyArtifact"; }
};

// A pipe-aware SOURCE: one FILE_PATH input parameter, always reports
// streaming-safe (its own reachability is what several of these tests flag,
// via what's downstream of it, not via this stage's own answer).
class PipeSourceStage : public orc::DAGStage,
                        public orc::ParameterizedStage,
                        public orc::IStreamingCompatibility {
 public:
  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return orc::NodeTypeInfo{orc::NodeType::SOURCE,
                             "unit_test_pipe_source",
                             "Pipe Source (test-only)",
                             "Test-only stage",
                             0,
                             0,
                             1,
                             UINT32_MAX,
                             orc::VideoFormatCompatibility::ALL};
  }
  std::vector<orc::ArtifactPtr> execute(
      const std::vector<orc::ArtifactPtr>&,
      const std::map<std::string, orc::ParameterValue>&,
      orc::ObservationContext&) override {
    return {std::make_shared<DummyArtifact>()};
  }
  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }

  std::vector<orc::ParameterDescriptor> get_parameter_descriptors(
      orc::VideoSystem, orc::SourceType) const override {
    orc::ParameterDescriptor desc;
    desc.name = "input_path";
    desc.display_name = "Input Path";
    desc.type = orc::ParameterType::FILE_PATH;
    desc.output_path = false;
    return {desc};
  }
  std::map<std::string, orc::ParameterValue> get_parameters() const override {
    return {{"input_path", input_path_}};
  }
  bool set_parameters(
      const std::map<std::string, orc::ParameterValue>& params) override {
    if (auto it = params.find("input_path");
        it != params.end() && std::holds_alternative<std::string>(it->second)) {
      input_path_ = std::get<std::string>(it->second);
    }
    return true;
  }

  bool supports_streaming_execution() const override { return true; }

 private:
  std::string input_path_;
};

// A pipe-aware SINK: one FILE_PATH output parameter, plus a "streaming_ok"
// bool parameter that directly controls supports_streaming_execution() —
// exercising that the capability is read from the CURRENT configuration,
// not a fixed per-stage-type answer.
class PipeSinkStage : public orc::DAGStage,
                      public orc::ParameterizedStage,
                      public orc::TriggerableStage,
                      public orc::IStreamingCompatibility {
 public:
  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return orc::NodeTypeInfo{orc::NodeType::SINK,
                             "unit_test_pipe_sink",
                             "Pipe Sink (test-only)",
                             "Test-only stage",
                             1,
                             1,
                             0,
                             0,
                             orc::VideoFormatCompatibility::ALL};
  }
  std::vector<orc::ArtifactPtr> execute(
      const std::vector<orc::ArtifactPtr>&,
      const std::map<std::string, orc::ParameterValue>&,
      orc::ObservationContext&) override {
    return {};
  }
  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 0; }

  std::vector<orc::ParameterDescriptor> get_parameter_descriptors(
      orc::VideoSystem, orc::SourceType) const override {
    orc::ParameterDescriptor path_desc;
    path_desc.name = "output_path";
    path_desc.display_name = "Output Path";
    path_desc.type = orc::ParameterType::FILE_PATH;
    path_desc.output_path = true;

    orc::ParameterDescriptor streaming_desc;
    streaming_desc.name = "streaming_ok";
    streaming_desc.display_name = "Streaming OK";
    streaming_desc.type = orc::ParameterType::BOOL;
    return {path_desc, streaming_desc};
  }
  std::map<std::string, orc::ParameterValue> get_parameters() const override {
    return {{"output_path", output_path_}, {"streaming_ok", streaming_ok_}};
  }
  bool set_parameters(
      const std::map<std::string, orc::ParameterValue>& params) override {
    if (auto it = params.find("output_path");
        it != params.end() && std::holds_alternative<std::string>(it->second)) {
      output_path_ = std::get<std::string>(it->second);
    }
    if (auto it = params.find("streaming_ok");
        it != params.end() && std::holds_alternative<bool>(it->second)) {
      streaming_ok_ = std::get<bool>(it->second);
    }
    return true;
  }

  bool trigger(const std::vector<orc::ArtifactPtr>&,
               const std::map<std::string, orc::ParameterValue>&,
               orc::IObservationContext&) override {
    return true;
  }
  std::string get_trigger_status() const override { return "ok"; }

  bool supports_streaming_execution() const override { return streaming_ok_; }

 private:
  std::string output_path_;
  bool streaming_ok_ = true;
};

// A TRANSFORM that does not implement IStreamingCompatibility at all —
// exercising the "not implementing it means not streaming-safe" default.
class NonStreamingTransformStage : public orc::DAGStage {
 public:
  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return orc::NodeTypeInfo{orc::NodeType::TRANSFORM,
                             "unit_test_non_streaming_transform",
                             "Non-Streaming Transform (test-only)",
                             "Test-only stage",
                             1,
                             1,
                             1,
                             1,
                             orc::VideoFormatCompatibility::ALL};
  }
  std::vector<orc::ArtifactPtr> execute(
      const std::vector<orc::ArtifactPtr>&,
      const std::map<std::string, orc::ParameterValue>&,
      orc::ObservationContext&) override {
    return {std::make_shared<DummyArtifact>()};
  }
  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 1; }
};

void ensure_pipe_test_stages_registered() {
  static const bool registered = [] {
    auto& registry = orc::StageRegistry::instance();
    if (!registry.has_stage("unit_test_pipe_source")) {
      registry.register_stage("unit_test_pipe_source", [] {
        return std::make_shared<PipeSourceStage>();
      });
    }
    if (!registry.has_stage("unit_test_pipe_sink")) {
      registry.register_stage("unit_test_pipe_sink",
                              [] { return std::make_shared<PipeSinkStage>(); });
    }
    if (!registry.has_stage("unit_test_non_streaming_transform")) {
      registry.register_stage("unit_test_non_streaming_transform", [] {
        return std::make_shared<NonStreamingTransformStage>();
      });
    }
    return true;
  }();
  (void)registered;
}

orc::presenters::ProjectPresenter wrap(orc::Project& project) {
  return orc::presenters::ProjectPresenter(static_cast<void*>(&project));
}

bool any_error_mentions(const std::vector<std::string>& errors,
                        const orc::NodeID& node_id) {
  const std::string needle = node_id.to_string();
  return std::any_of(errors.begin(), errors.end(),
                     [&needle](const std::string& error) {
                       return error.find(needle) != std::string::npos;
                     });
}

}  // namespace

TEST(ValidatePipeExecutionTest, NoPipeUsage_ReturnsEmpty) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("no-pipe");
  auto src = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto sink = orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  orc::project_io::set_node_parameters(
      project, src, {{"input_path", std::string("real_file.cvbs")}});
  orc::project_io::set_node_parameters(
      project, sink, {{"output_path", std::string("out.bin")}});
  orc::project_io::add_edge(project, src, sink);

  auto presenter = wrap(project);
  EXPECT_TRUE(presenter.validatePipeExecution().empty());
}

TEST(ValidatePipeExecutionTest, SingleStdinToStreamingSink_Passes) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("single-stdin");
  auto src = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto sink = orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  orc::project_io::set_node_parameters(project, src,
                                       {{"input_path", std::string("-")}});
  orc::project_io::set_node_parameters(
      project, sink, {{"output_path", std::string("out.bin")}});
  orc::project_io::add_edge(project, src, sink);

  auto presenter = wrap(project);
  EXPECT_TRUE(presenter.validatePipeExecution().empty());
}

TEST(ValidatePipeExecutionTest, TwoNodesTargetStdin_ReportsCollision) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("two-stdin");
  auto src1 = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto sink1 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  auto src2 =
      orc::project_io::add_node(project, "unit_test_pipe_source", 0, 100);
  auto sink2 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 100);
  orc::project_io::set_node_parameters(project, src1,
                                       {{"input_path", std::string("-")}});
  orc::project_io::set_node_parameters(
      project, sink1, {{"output_path", std::string("out1.bin")}});
  orc::project_io::set_node_parameters(project, src2,
                                       {{"input_path", std::string("-")}});
  orc::project_io::set_node_parameters(
      project, sink2, {{"output_path", std::string("out2.bin")}});
  orc::project_io::add_edge(project, src1, sink1);
  orc::project_io::add_edge(project, src2, sink2);

  auto presenter = wrap(project);
  const auto errors = presenter.validatePipeExecution();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors[0].find("standard input"), std::string::npos);
  EXPECT_TRUE(any_error_mentions(errors, src1));
  EXPECT_TRUE(any_error_mentions(errors, src2));
}

TEST(ValidatePipeExecutionTest, TwoNodesTargetStdout_ReportsCollision) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("two-stdout");
  auto src = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto sink1 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  auto sink2 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 100);
  orc::project_io::set_node_parameters(
      project, src, {{"input_path", std::string("in.cvbs")}});
  orc::project_io::set_node_parameters(project, sink1,
                                       {{"output_path", std::string("-")}});
  orc::project_io::set_node_parameters(project, sink2,
                                       {{"output_path", std::string("-")}});
  orc::project_io::add_edge(project, src, sink1);
  orc::project_io::add_edge(project, src, sink2);

  auto presenter = wrap(project);
  const auto errors = presenter.validatePipeExecution();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors[0].find("standard output"), std::string::npos);
  EXPECT_TRUE(any_error_mentions(errors, sink1));
  EXPECT_TRUE(any_error_mentions(errors, sink2));
}

// One stdin source fanning out to two sinks: only the non-compliant sink
// should be flagged, not the compliant one sharing the same source — the
// whole point of checking the reachable subgraph rather than banning
// branching outright.
TEST(ValidatePipeExecutionTest, FanOut_OnlyFlagsTheNonCompliantSink) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("fan-out");
  auto src = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto good_sink =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  auto bad_sink =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 100);
  orc::project_io::set_node_parameters(project, src,
                                       {{"input_path", std::string("-")}});
  orc::project_io::set_node_parameters(
      project, good_sink,
      {{"output_path", std::string("good.bin")}, {"streaming_ok", true}});
  orc::project_io::set_node_parameters(
      project, bad_sink,
      {{"output_path", std::string("bad.bin")}, {"streaming_ok", false}});
  orc::project_io::add_edge(project, src, good_sink);
  orc::project_io::add_edge(project, src, bad_sink);

  auto presenter = wrap(project);
  const auto errors = presenter.validatePipeExecution();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_TRUE(any_error_mentions(errors, bad_sink));
  EXPECT_FALSE(any_error_mentions(errors, good_sink));
}

// A transform that never implements IStreamingCompatibility at all sits
// between a stdin source and an otherwise-compliant sink.
TEST(ValidatePipeExecutionTest,
     TransformWithoutInterface_IsFlaggedAsNonStreaming) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("non-streaming-mid");
  auto src = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto mid = orc::project_io::add_node(
      project, "unit_test_non_streaming_transform", 50, 0);
  auto sink = orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  orc::project_io::set_node_parameters(project, src,
                                       {{"input_path", std::string("-")}});
  orc::project_io::set_node_parameters(
      project, sink, {{"output_path", std::string("out.bin")}});
  orc::project_io::add_edge(project, src, mid);
  orc::project_io::add_edge(project, mid, sink);

  auto presenter = wrap(project);
  const auto errors = presenter.validatePipeExecution();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_TRUE(any_error_mentions(errors, mid));
}

}  // namespace orc_unit_test
