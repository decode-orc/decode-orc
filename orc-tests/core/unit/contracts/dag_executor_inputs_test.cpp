/*
 * File:        dag_executor_inputs_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     DAGExecutor::execute_inputs_of hands a node (a stage tool
 *              job) exactly what its own execute() would receive, without
 *              running the node itself
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/stage.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dag_executor.h"

namespace orc {
namespace {

class NamedArtifact final : public Artifact {
 public:
  explicit NamedArtifact(std::string id)
      : Artifact(ArtifactID(std::move(id)), Provenance{}) {}
  std::string type_name() const override { return "NamedArtifact"; }
};

// A stage with @p inputs inputs and @p outputs outputs named
// "<name>_out<i>", which counts its executions.
class NamedStage final : public DAGStage {
 public:
  NamedStage(std::string name, NodeType type, size_t inputs, size_t outputs)
      : name_(std::move(name)),
        type_(type),
        inputs_(inputs),
        outputs_(outputs) {}

  std::string version() const override { return "1.0"; }

  NodeTypeInfo get_node_type_info() const override {
    const auto in = static_cast<uint32_t>(inputs_);
    const auto out = static_cast<uint32_t>(outputs_);
    return NodeTypeInfo(type_, name_, name_, "", in, in, out, out,
                        VideoFormatCompatibility::ALL);
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& /*parameters*/,
      ObservationContext& /*observation_context*/) override {
    ++execute_count;
    received = inputs;
    std::vector<ArtifactPtr> result;
    for (size_t i = 0; i < outputs_; ++i) {
      result.push_back(
          std::make_shared<NamedArtifact>(name_ + "_out" + std::to_string(i)));
    }
    return result;
  }

  size_t required_input_count() const override { return inputs_; }
  size_t output_count() const override { return outputs_; }

  int execute_count = 0;
  std::vector<ArtifactPtr> received;

 private:
  std::string name_;
  NodeType type_;
  size_t inputs_;
  size_t outputs_;
};

std::vector<std::string> ids_of(const std::vector<ArtifactPtr>& artifacts) {
  std::vector<std::string> ids;
  for (const auto& artifact : artifacts) {
    ids.push_back(artifact->id().value());
  }
  return ids;
}

// Two sources, the second with two outputs, feed one two-input transform:
// source A output 0 first, then source B output 1.
struct TwoInputGraph {
  std::shared_ptr<NamedStage> a =
      std::make_shared<NamedStage>("a", NodeType::SOURCE, 0, 1);
  std::shared_ptr<NamedStage> b =
      std::make_shared<NamedStage>("b", NodeType::SOURCE, 0, 2);
  std::shared_ptr<NamedStage> join =
      std::make_shared<NamedStage>("join", NodeType::TRANSFORM, 2, 1);
  DAG dag;

  TwoInputGraph() {
    dag.add_node(DAGNode{NodeID(1), a, {}, {}, {}});
    dag.add_node(DAGNode{NodeID(2), b, {}, {}, {}});
    dag.add_node(DAGNode{NodeID(3), join, {}, {NodeID(1), NodeID(2)}, {0, 1}});
    dag.set_output_nodes({NodeID(3)});
  }
};

TEST(DagExecutorInputs, GathersTheInputsInTheOrderAndOutputsTheNodeUses) {
  TwoInputGraph graph;
  DAGExecutor executor;

  const auto inputs = executor.execute_inputs_of(graph.dag, NodeID(3));

  EXPECT_EQ(ids_of(inputs), (std::vector<std::string>{"a_out0", "b_out1"}));
}

TEST(DagExecutorInputs, MatchesWhatTheNodesOwnExecuteReceives) {
  TwoInputGraph graph;
  DAGExecutor executor;
  executor.execute_to_node(graph.dag, NodeID(3));
  const auto received = ids_of(graph.join->received);

  TwoInputGraph fresh;
  DAGExecutor other;
  EXPECT_EQ(ids_of(other.execute_inputs_of(fresh.dag, NodeID(3))), received);
}

TEST(DagExecutorInputs, DoesNotRunTheNodeItself) {
  TwoInputGraph graph;
  DAGExecutor executor;

  executor.execute_inputs_of(graph.dag, NodeID(3));

  EXPECT_EQ(graph.join->execute_count, 0);
  EXPECT_EQ(graph.a->execute_count, 1);
  EXPECT_EQ(graph.b->execute_count, 1);
}

TEST(DagExecutorInputs, GivesASourceNodeNoInputs) {
  TwoInputGraph graph;
  DAGExecutor executor;

  EXPECT_TRUE(executor.execute_inputs_of(graph.dag, NodeID(1)).empty());
  EXPECT_EQ(graph.a->execute_count, 0);
}

TEST(DagExecutorInputs, RefusesANodeThatIsNotInTheGraph) {
  TwoInputGraph graph;
  DAGExecutor executor;

  EXPECT_THROW(executor.execute_inputs_of(graph.dag, NodeID(99)),
               DAGExecutionError);
}

}  // namespace
}  // namespace orc
