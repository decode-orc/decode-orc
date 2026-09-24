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
#include "../../../../orc/core/include/project_to_dag.h"
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

// A pipe-aware SOURCE that can split stdin between several sinks: it
// declares the host-owned orc::kStreamReaderCountParameter, like the real
// cvbs/tbc stream sources.
class SharedStdinPipeSourceStage : public PipeSourceStage {
 public:
  orc::NodeTypeInfo get_node_type_info() const override {
    auto info = PipeSourceStage::get_node_type_info();
    info.stage_name = "unit_test_shared_stdin_source";
    return info;
  }
  std::vector<orc::ParameterDescriptor> get_parameter_descriptors(
      orc::VideoSystem system, orc::SourceType source_type) const override {
    auto descriptors =
        PipeSourceStage::get_parameter_descriptors(system, source_type);
    orc::ParameterDescriptor readers;
    readers.name = orc::kStreamReaderCountParameter;
    readers.display_name = "Stream Readers";
    readers.type = orc::ParameterType::UINT32;
    readers.constraints.default_value = static_cast<uint32_t>(1);
    descriptors.push_back(readers);
    return descriptors;
  }
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

// A pipe-aware SINK whose FILE_PATH descriptor does NOT set output_path —
// the realistic case: ParameterDescriptor::output_path (parameter_types.h)
// is documented as unnecessary for sink stages, which are "treated as
// output by default via a name heuristic" elsewhere (the GUI's own
// open/save file dialog choice, stageparameterdialog.cpp). Every core sink
// stage except one relies on exactly that and never sets the field, so a
// direction check reading only descriptor.output_path — as opposed to also
// consulting get_node_type_info().type == NodeType::SINK — misclassifies
// every one of them as an INPUT, not an output.
class RealisticPipeSinkStage : public orc::DAGStage,
                               public orc::ParameterizedStage,
                               public orc::TriggerableStage,
                               public orc::IStreamingCompatibility {
 public:
  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return orc::NodeTypeInfo{orc::NodeType::SINK,
                             "unit_test_realistic_pipe_sink",
                             "Realistic Pipe Sink (test-only)",
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
    // Deliberately NOT setting path_desc.output_path = true, matching every
    // real sink stage but one.
    return {path_desc};
  }
  std::map<std::string, orc::ParameterValue> get_parameters() const override {
    return {{"output_path", output_path_}};
  }
  bool set_parameters(
      const std::map<std::string, orc::ParameterValue>& params) override {
    if (auto it = params.find("output_path");
        it != params.end() && std::holds_alternative<std::string>(it->second)) {
      output_path_ = std::get<std::string>(it->second);
    }
    return true;
  }

  bool trigger(const std::vector<orc::ArtifactPtr>&,
               const std::map<std::string, orc::ParameterValue>&,
               orc::IObservationContext&) override {
    return true;
  }
  std::string get_trigger_status() const override { return "ok"; }

  bool supports_streaming_execution() const override { return true; }

 private:
  std::string output_path_;
};

// A streaming-compatible TRANSFORM, for graphs where a piped source reaches
// its sinks through an intermediate node.
class PipeTransformStage : public orc::DAGStage,
                           public orc::IStreamingCompatibility {
 public:
  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return orc::NodeTypeInfo{orc::NodeType::TRANSFORM,
                             "unit_test_pipe_transform",
                             "Pipe Transform (test-only)",
                             "Test-only stage",
                             1,
                             1,
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
  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 1; }

  bool supports_streaming_execution() const override { return true; }
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
    if (!registry.has_stage("unit_test_realistic_pipe_sink")) {
      registry.register_stage("unit_test_realistic_pipe_sink", [] {
        return std::make_shared<RealisticPipeSinkStage>();
      });
    }
    if (!registry.has_stage("unit_test_shared_stdin_source")) {
      registry.register_stage("unit_test_shared_stdin_source", [] {
        return std::make_shared<SharedStdinPipeSourceStage>();
      });
    }
    if (!registry.has_stage("unit_test_pipe_transform")) {
      registry.register_stage("unit_test_pipe_transform", [] {
        return std::make_shared<PipeTransformStage>();
      });
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

// Reproduces the real-world bug this project shipped: a sink whose
// descriptor never sets output_path (see RealisticPipeSinkStage above) piped
// stdin-to-stdout must not be reported as two nodes colliding over standard
// input, and the sink itself must not be misclassified as an unreachable
// "input" node requiring a forward-compatibility walk instead of the direct
// sink check it actually needs.
TEST(ValidatePipeExecutionTest,
     SingleStdinToRealisticSink_NotMisclassifiedAsInput_Passes) {
  ensure_pipe_test_stages_registered();
  auto project =
      orc::project_io::create_empty_project("realistic-single-stdin");
  auto src = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto sink = orc::project_io::add_node(
      project, "unit_test_realistic_pipe_sink", 100, 0);
  orc::project_io::set_node_parameters(project, src,
                                       {{"input_path", std::string("-")}});
  orc::project_io::set_node_parameters(project, sink,
                                       {{"output_path", std::string("-")}});
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

// A piped OUTPUT is a property of the sink's own writing behaviour, not of
// how its ancestors are read: a non-compliant (or not-even-implementing)
// ancestor upstream of a piped sink must NOT be flagged, since nothing
// upstream is itself piped and the sink's random-access-capable input has
// no bearing on whether the sink can write its own output forward-only.
TEST(ValidatePipeExecutionTest,
     StdoutSink_DoesNotWalkBackIntoNonCompliantAncestors) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("stdout-only");
  auto src = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto mid = orc::project_io::add_node(
      project, "unit_test_non_streaming_transform", 50, 0);
  auto sink = orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  orc::project_io::set_node_parameters(
      project, src, {{"input_path", std::string("in.cvbs")}});
  orc::project_io::set_node_parameters(project, sink,
                                       {{"output_path", std::string("-")}});
  orc::project_io::add_edge(project, src, mid);
  orc::project_io::add_edge(project, mid, sink);

  auto presenter = wrap(project);
  EXPECT_TRUE(presenter.validatePipeExecution().empty());
}

namespace {

// One stdin source feeding two sinks, directly.
struct FanOutProject {
  orc::Project project;
  orc::NodeID src;
  orc::NodeID sink1;
  orc::NodeID sink2;
};

FanOutProject make_stdin_fan_out(const std::string& source_stage) {
  ensure_pipe_test_stages_registered();
  FanOutProject p{orc::project_io::create_empty_project("fan-out"), {}, {}, {}};
  p.src = orc::project_io::add_node(p.project, source_stage, 0, 0);
  p.sink1 = orc::project_io::add_node(p.project, "unit_test_pipe_sink", 100, 0);
  p.sink2 =
      orc::project_io::add_node(p.project, "unit_test_pipe_sink", 100, 100);
  orc::project_io::set_node_parameters(p.project, p.src,
                                       {{"input_path", std::string("-")}});
  orc::project_io::set_node_parameters(
      p.project, p.sink1, {{"output_path", std::string("one.bin")}});
  orc::project_io::set_node_parameters(
      p.project, p.sink2, {{"output_path", std::string("two.bin")}});
  orc::project_io::add_edge(p.project, p.src, p.sink1);
  orc::project_io::add_edge(p.project, p.src, p.sink2);
  return p;
}

}  // namespace

// A source that splits stdin between its readers can feed several sinks:
// triggerAllSinks() runs them side by side, each with its own copy.
TEST(ValidatePipeExecutionTest, FanOutFromSharedStdinSource_Passes) {
  auto p = make_stdin_fan_out("unit_test_shared_stdin_source");
  auto presenter = wrap(p.project);
  EXPECT_TRUE(presenter.validatePipeExecution().empty());
}

// The DAG builder tells each instance of the source how many sinks share
// its stream, and triggerAllSinks() learns which sinks to run together.
TEST(ValidatePipeExecutionTest,
     FanOutFromSharedStdinSource_SetsReaderCountAndGroupsTheSinks) {
  auto p = make_stdin_fan_out("unit_test_shared_stdin_source");

  const auto dag = orc::project_to_dag(p.project);
  const auto& nodes = dag->nodes();
  const auto src_node =
      std::find_if(nodes.begin(), nodes.end(),
                   [&](const orc::DAGNode& n) { return n.node_id == p.src; });
  ASSERT_NE(src_node, nodes.end());
  const auto readers =
      src_node->parameters.find(orc::kStreamReaderCountParameter);
  ASSERT_NE(readers, src_node->parameters.end());
  EXPECT_EQ(std::get<uint32_t>(readers->second), 2u);

  const auto groups = orc::shared_pipe_sink_groups(p.project);
  ASSERT_EQ(groups.size(), 1u);
  EXPECT_EQ(groups[0], (std::vector<orc::NodeID>{p.sink1, p.sink2}));
}

// A source that reads stdin directly cannot hand each sink its own copy, so
// a second sink would get an empty output while the run reported success.
TEST(ValidatePipeExecutionTest,
     FanOutFromStdinSource_WithoutSharing_IsRejected) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("fan-out-stdin");
  auto src = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto sink1 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  auto sink2 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 100);
  orc::project_io::set_node_parameters(project, src,
                                       {{"input_path", std::string("-")}});
  orc::project_io::set_node_parameters(
      project, sink1, {{"output_path", std::string("one.bin")}});
  orc::project_io::set_node_parameters(
      project, sink2, {{"output_path", std::string("two.bin")}});
  orc::project_io::add_edge(project, src, sink1);
  orc::project_io::add_edge(project, src, sink2);

  auto presenter = wrap(project);
  const auto errors = presenter.validatePipeExecution();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors[0].find("only be consumed once"), std::string::npos);
  EXPECT_TRUE(any_error_mentions(errors, src));
  EXPECT_TRUE(any_error_mentions(errors, sink1));
  EXPECT_TRUE(any_error_mentions(errors, sink2));
}

// Fan-out through an intermediate node is the same stream read twice.
TEST(ValidatePipeExecutionTest,
     FanOutFromStdinThroughTransform_WithoutSharing_IsRejected) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("fan-out-indirect");
  auto src = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto mid =
      orc::project_io::add_node(project, "unit_test_pipe_transform", 50, 0);
  auto sink1 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  auto sink2 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 100);
  orc::project_io::set_node_parameters(project, src,
                                       {{"input_path", std::string("-")}});
  orc::project_io::set_node_parameters(
      project, sink1, {{"output_path", std::string("one.bin")}});
  orc::project_io::set_node_parameters(
      project, sink2, {{"output_path", std::string("two.bin")}});
  orc::project_io::add_edge(project, src, mid);
  orc::project_io::add_edge(project, mid, sink1);
  orc::project_io::add_edge(project, mid, sink2);

  auto presenter = wrap(project);
  const auto errors = presenter.validatePipeExecution();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors[0].find("only be consumed once"), std::string::npos);
}

// Counted through intermediate nodes too: each sink downstream is a reader.
TEST(ValidatePipeExecutionTest,
     FanOutFromSharedStdinSourceThroughTransform_Passes) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("shared-indirect");
  auto src =
      orc::project_io::add_node(project, "unit_test_shared_stdin_source", 0, 0);
  auto mid =
      orc::project_io::add_node(project, "unit_test_pipe_transform", 50, 0);
  auto sink1 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  auto sink2 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 100);
  orc::project_io::set_node_parameters(project, src,
                                       {{"input_path", std::string("-")}});
  orc::project_io::set_node_parameters(
      project, sink1, {{"output_path", std::string("one.bin")}});
  orc::project_io::set_node_parameters(
      project, sink2, {{"output_path", std::string("two.bin")}});
  orc::project_io::add_edge(project, src, mid);
  orc::project_io::add_edge(project, mid, sink1);
  orc::project_io::add_edge(project, mid, sink2);

  auto presenter = wrap(project);
  EXPECT_TRUE(presenter.validatePipeExecution().empty());
  EXPECT_EQ(orc::shared_pipe_sink_groups(project).size(), 1u);
}

// Only stdin and named pipes are split between readers; a network stream is
// just as read-once, and stays limited to one sink even for a source that
// can share a pipe.
TEST(ValidatePipeExecutionTest, FanOutFromNetworkSource_IsRejected) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("fan-out-network");
  auto src =
      orc::project_io::add_node(project, "unit_test_shared_stdin_source", 0, 0);
  auto sink1 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  auto sink2 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 100);
  orc::project_io::set_node_parameters(
      project, src, {{"input_path", std::string("udp://239.1.1.1:1234")}});
  orc::project_io::set_node_parameters(
      project, sink1, {{"output_path", std::string("one.bin")}});
  orc::project_io::set_node_parameters(
      project, sink2, {{"output_path", std::string("two.bin")}});
  orc::project_io::add_edge(project, src, sink1);
  orc::project_io::add_edge(project, src, sink2);

  auto presenter = wrap(project);
  const auto errors = presenter.validatePipeExecution();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors[0].find("only be consumed once"), std::string::npos);
}

// Several sinks in one project stay fine as long as only one of them is fed
// by the piped source: a sink fed from a regular file is unaffected.
TEST(ValidatePipeExecutionTest,
     StdinSourceWithOneSink_PlusUnrelatedFileSink_Passes) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("stdin-plus-file");
  auto piped_src =
      orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto file_src =
      orc::project_io::add_node(project, "unit_test_pipe_source", 0, 100);
  auto piped_sink =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  auto file_sink =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 100);
  orc::project_io::set_node_parameters(project, piped_src,
                                       {{"input_path", std::string("-")}});
  orc::project_io::set_node_parameters(
      project, file_src, {{"input_path", std::string("in.cvbs")}});
  orc::project_io::set_node_parameters(
      project, piped_sink, {{"output_path", std::string("one.bin")}});
  orc::project_io::set_node_parameters(
      project, file_sink, {{"output_path", std::string("two.bin")}});
  orc::project_io::add_edge(project, piped_src, piped_sink);
  orc::project_io::add_edge(project, file_src, file_sink);

  auto presenter = wrap(project);
  EXPECT_TRUE(presenter.validatePipeExecution().empty());
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

// A network stream URL (see orc::pipe_io::is_network_stream_url()) is exactly
// as non-seekable as "-", so a sink that cannot handle streaming execution
// must be flagged the same way it would be for "-".
TEST(ValidatePipeExecutionTest, NetworkUrlSink_NonStreamingSink_IsFlagged) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("network-url-sink");
  auto src = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto sink = orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  orc::project_io::set_node_parameters(
      project, src, {{"input_path", std::string("in.cvbs")}});
  orc::project_io::set_node_parameters(
      project, sink,
      {{"output_path", std::string("udp://239.1.1.1:1234")},
       {"streaming_ok", false}});
  orc::project_io::add_edge(project, src, sink);

  auto presenter = wrap(project);
  const auto errors = presenter.validatePipeExecution();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_TRUE(any_error_mentions(errors, sink));
}

// The streaming-compatible counterpart: a network URL sink that reports
// itself streaming-safe passes cleanly, same as "-" does.
TEST(ValidatePipeExecutionTest, NetworkUrlSink_StreamingCompatible_Passes) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("network-url-ok");
  auto src = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto sink = orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  orc::project_io::set_node_parameters(
      project, src, {{"input_path", std::string("in.cvbs")}});
  orc::project_io::set_node_parameters(
      project, sink,
      {{"output_path", std::string("rtmp://live.example.com/app")},
       {"streaming_ok", true}});
  orc::project_io::add_edge(project, src, sink);

  auto presenter = wrap(project);
  EXPECT_TRUE(presenter.validatePipeExecution().empty());
}

// Two sinks each targeting their OWN distinct network URL must not be
// reported as a collision: unlike "-", which names one real OS-level
// singleton stream shared by the whole process, two different network
// URLs are two independent destinations with nothing to collide over. This
// is the property that makes it necessary to track "is this literally the
// '-' token" separately from "is this some non-seekable destination" (see
// PipeParameterMatch in project_presenter.cpp).
TEST(ValidatePipeExecutionTest,
     TwoDistinctNetworkUrlSinks_DoesNotReportCollision) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("two-network-urls");
  auto src = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto sink1 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  auto sink2 =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 100);
  orc::project_io::set_node_parameters(
      project, src, {{"input_path", std::string("in.cvbs")}});
  orc::project_io::set_node_parameters(
      project, sink1,
      {{"output_path", std::string("udp://239.1.1.1:1234")},
       {"streaming_ok", true}});
  orc::project_io::set_node_parameters(
      project, sink2,
      {{"output_path", std::string("udp://239.1.1.1:5678")},
       {"streaming_ok", true}});
  orc::project_io::add_edge(project, src, sink1);
  orc::project_io::add_edge(project, src, sink2);

  auto presenter = wrap(project);
  EXPECT_TRUE(presenter.validatePipeExecution().empty());
}

// A "-" sink and a network-URL sink coexisting is not a collision either —
// they are two different kinds of non-seekable destination, not two
// claimants of the same one.
TEST(ValidatePipeExecutionTest, StdoutSinkAndNetworkUrlSink_DoesNotCollide) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("stdout-and-network");
  auto src = orc::project_io::add_node(project, "unit_test_pipe_source", 0, 0);
  auto stdout_sink =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 0);
  auto network_sink =
      orc::project_io::add_node(project, "unit_test_pipe_sink", 100, 100);
  orc::project_io::set_node_parameters(
      project, src, {{"input_path", std::string("in.cvbs")}});
  orc::project_io::set_node_parameters(
      project, stdout_sink,
      {{"output_path", std::string("-")}, {"streaming_ok", true}});
  orc::project_io::set_node_parameters(
      project, network_sink,
      {{"output_path", std::string("srt://host:9000")},
       {"streaming_ok", true}});
  orc::project_io::add_edge(project, src, stdout_sink);
  orc::project_io::add_edge(project, src, network_sink);

  auto presenter = wrap(project);
  EXPECT_TRUE(presenter.validatePipeExecution().empty());
}

////////////////////////////////////////////////////////////////////////////////////////////
// ProjectPresenter::getNodeConfigurationStatus() — the GUI's per-node status
// dot must not read a "-" or network-URL FILE_PATH as configured, since the
// GUI has no equivalent of validatePipeExecution() at trigger time (see
// render_presenter.cpp's triggerStage()) and a project can carry one of
// these values even though stageparameterdialog.cpp refuses to let a user
// type one in directly (produced by --export-project, or hand-edited).
////////////////////////////////////////////////////////////////////////////////////////////

TEST(GetNodeConfigurationStatusTest, RealPath_ReportsTheStagesOwnStatus) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("real-path");
  auto sink = orc::project_io::add_node(project, "unit_test_pipe_sink", 0, 0);
  orc::project_io::set_node_parameters(
      project, sink, {{"output_path", std::string("out.bin")}});

  auto presenter = wrap(project);
  EXPECT_EQ(presenter.getNodeConfigurationStatus(sink),
            orc::ConfigurationStatus::Green);
}

TEST(GetNodeConfigurationStatusTest, StdioToken_ReportsRedEvenThoughSet) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("stdio-token");
  auto sink = orc::project_io::add_node(project, "unit_test_pipe_sink", 0, 0);
  orc::project_io::set_node_parameters(project, sink,
                                       {{"output_path", std::string("-")}});

  auto presenter = wrap(project);
  EXPECT_EQ(presenter.getNodeConfigurationStatus(sink),
            orc::ConfigurationStatus::Red);
}

TEST(GetNodeConfigurationStatusTest, NetworkStreamUrl_ReportsRedEvenThoughSet) {
  ensure_pipe_test_stages_registered();
  auto project = orc::project_io::create_empty_project("network-url-token");
  auto sink = orc::project_io::add_node(project, "unit_test_pipe_sink", 0, 0);
  orc::project_io::set_node_parameters(
      project, sink, {{"output_path", std::string("udp://239.1.1.1:1234")}});

  auto presenter = wrap(project);
  EXPECT_EQ(presenter.getNodeConfigurationStatus(sink),
            orc::ConfigurationStatus::Red);
}

}  // namespace orc_unit_test
