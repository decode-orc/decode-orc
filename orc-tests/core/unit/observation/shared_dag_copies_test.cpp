/*
 * File:        shared_dag_copies_test.cpp
 * Module:      orc-core tests
 * Purpose:     Unit tests for what private DAG copies share: node outputs
 *              (SharedNodeOutputs) and stage measurements kept in the
 *              observation store (NodeMeasurementStore)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/observation/stage_measurement_store.h>
#include <orc/stage/stage.h>

#include <atomic>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dag_executor.h"
#include "node_measurement_store.h"
#include "observation_store.h"
#include "shared_node_outputs.h"

namespace orc {
namespace {

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

class FakeArtifact final : public Artifact {
 public:
  explicit FakeArtifact(std::string id)
      : Artifact(ArtifactID(std::move(id)), Provenance{}) {}
  std::string type_name() const override { return "FakeArtifact"; }
};

// A stage of |type| with |inputs| inputs that counts its executions and
// records the measurement store it was last given.
class CountingStage final : public DAGStage, public IStageMeasurementConsumer {
 public:
  CountingStage(NodeType type, size_t inputs) : type_(type), inputs_(inputs) {}

  std::string version() const override { return "1.0"; }

  NodeTypeInfo get_node_type_info() const override {
    const auto in = static_cast<uint32_t>(inputs_);
    const uint32_t out = is_sink() ? 0 : 1;
    return NodeTypeInfo(type_, name(), name(), "", in, in, out, out,
                        VideoFormatCompatibility::ALL);
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& /*inputs*/,
      const std::map<std::string, ParameterValue>& /*parameters*/,
      ObservationContext& /*observation_context*/) override {
    ++execute_count;
    if (is_sink()) return {};
    return {std::make_shared<FakeArtifact>(name() + "_out")};
  }

  size_t required_input_count() const override { return inputs_; }
  size_t output_count() const override { return is_sink() ? 0 : 1; }

  void set_measurement_store(
      std::shared_ptr<IStageMeasurementStore> store) override {
    ++store_offers;
    measurement_store = std::move(store);
  }

  int execute_count = 0;
  int store_offers = 0;
  std::shared_ptr<IStageMeasurementStore> measurement_store;

 private:
  bool is_sink() const { return type_ == NodeType::SINK; }
  // Distinct per node type: the executor's own cache keys on stage name and
  // input artifact ids, so two stages alike in both would read as one.
  std::string name() const {
    return "counting_" + std::to_string(static_cast<int>(type_));
  }

  NodeType type_;
  size_t inputs_;
};

// One copy of a source -> transform -> sink DAG, with its own stages.
struct DagCopy {
  std::shared_ptr<CountingStage> source =
      std::make_shared<CountingStage>(NodeType::SOURCE, 0);
  std::shared_ptr<CountingStage> transform =
      std::make_shared<CountingStage>(NodeType::TRANSFORM, 1);
  std::shared_ptr<CountingStage> sink =
      std::make_shared<CountingStage>(NodeType::SINK, 1);
  DAG dag;

  DagCopy() {
    dag.add_node(DAGNode{NodeID(1), source, {}, {}, {}});
    dag.add_node(DAGNode{NodeID(2), transform, {}, {NodeID(1)}, {0}});
    dag.add_node(DAGNode{NodeID(3), sink, {}, {NodeID(2)}, {0}});
    dag.set_output_nodes({NodeID(3)});
  }
};

NodeFingerprintMap fingerprints_of_copy() {
  return {{NodeID(1), NodeFingerprint{"fp-source"}},
          {NodeID(2), NodeFingerprint{"fp-transform"}},
          {NodeID(3), NodeFingerprint{"fp-sink"}}};
}

// ---------------------------------------------------------------------------
// SharedNodeOutputs
// ---------------------------------------------------------------------------

TEST(SharedNodeOutputs_ProducesEachNodeOnce, Basic) {
  SharedNodeOutputs shared;
  int produced = 0;
  const auto produce = [&] {
    ++produced;
    return SharedNodeOutputs::Entry{{std::make_shared<FakeArtifact>("a")},
                                    nullptr};
  };

  const auto first = shared.get_or_produce(NodeID(1), produce);
  const auto second = shared.get_or_produce(NodeID(1), produce);
  EXPECT_EQ(produced, 1);
  ASSERT_EQ(second.outputs.size(), 1u);
  EXPECT_EQ(second.outputs[0], first.outputs[0]);

  shared.get_or_produce(NodeID(2), produce);
  EXPECT_EQ(produced, 2);
  EXPECT_EQ(shared.size(), 2u);
}

// A copy arriving while another produces the node uses that copy's outputs,
// whether it arrived in time to wait or only once they were stored.
TEST(SharedNodeOutputs_ConcurrentCopyReusesTheOutputs, Basic) {
  SharedNodeOutputs shared;
  std::atomic<int> produced{0};
  const auto produce = [&] {
    ++produced;
    return SharedNodeOutputs::Entry{{std::make_shared<FakeArtifact>("a")},
                                    nullptr};
  };

  SharedNodeOutputs::Entry second;
  std::thread other;
  const auto first = shared.get_or_produce(NodeID(1), [&] {
    other = std::thread(
        [&] { second = shared.get_or_produce(NodeID(1), produce); });
    return produce();
  });
  other.join();

  EXPECT_EQ(produced.load(), 1);
  ASSERT_EQ(second.outputs.size(), 1u);
  EXPECT_EQ(second.outputs[0], first.outputs[0]);
}

TEST(SharedNodeOutputs_FailureLeavesTheNodeToTheNextCopy, Basic) {
  SharedNodeOutputs shared;
  EXPECT_THROW(shared.get_or_produce(NodeID(1),
                                     []() -> SharedNodeOutputs::Entry {
                                       throw std::runtime_error("failed");
                                     }),
               std::runtime_error);
  EXPECT_EQ(shared.size(), 0u);

  const auto entry = shared.get_or_produce(NodeID(1), [] {
    return SharedNodeOutputs::Entry{{std::make_shared<FakeArtifact>("a")},
                                    nullptr};
  });
  EXPECT_EQ(entry.outputs.size(), 1u);
}

TEST(SharedNodeOutputsPool_OneGenerationPerDag, Basic) {
  SharedNodeOutputsPool pool;
  auto first_dag = std::make_shared<const DAG>();
  auto second_dag = std::make_shared<const DAG>();

  const auto first = pool.for_dag(first_dag);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(pool.for_dag(first_dag), first);

  const auto second = pool.for_dag(second_dag);
  EXPECT_NE(second, first);
  EXPECT_EQ(pool.for_dag(second_dag), second);

  EXPECT_EQ(pool.for_dag(nullptr), nullptr);
}

// ---------------------------------------------------------------------------
// DAGExecutor with shared outputs
// ---------------------------------------------------------------------------

// Two copies of one DAG execute each node that is not a sink once between
// them, and the second copy reads the first copy's outputs.
TEST(DAGExecutor_SharedNodeOutputs_CopiesExecuteEachNodeOnce, Basic) {
  DagCopy first;
  DagCopy second;
  auto shared = std::make_shared<SharedNodeOutputs>();
  DAGExecutor first_executor;
  DAGExecutor second_executor;
  first_executor.set_shared_node_outputs(shared);
  second_executor.set_shared_node_outputs(shared);

  const auto first_outputs =
      first_executor.execute_to_node(first.dag, NodeID(3));
  const auto second_outputs =
      second_executor.execute_to_node(second.dag, NodeID(3));

  EXPECT_EQ(first.source->execute_count, 1);
  EXPECT_EQ(first.transform->execute_count, 1);
  EXPECT_EQ(second.source->execute_count, 0);
  EXPECT_EQ(second.transform->execute_count, 0);
  ASSERT_EQ(first_outputs.at(NodeID(2)).size(), 1u);
  ASSERT_EQ(second_outputs.at(NodeID(2)).size(), 1u);
  EXPECT_EQ(second_outputs.at(NodeID(2))[0], first_outputs.at(NodeID(2))[0]);

  // Sinks keep per-copy state, so each copy executes its own.
  EXPECT_EQ(first.sink->execute_count, 1);
  EXPECT_EQ(second.sink->execute_count, 1);
}

TEST(DAGExecutor_WithoutSharedNodeOutputs_EachCopyExecutesItsOwn, Basic) {
  DagCopy first;
  DagCopy second;
  DAGExecutor first_executor;
  DAGExecutor second_executor;

  first_executor.execute_to_node(first.dag, NodeID(2));
  second_executor.execute_to_node(second.dag, NodeID(2));

  EXPECT_EQ(first.transform->execute_count, 1);
  EXPECT_EQ(second.transform->execute_count, 1);
}

// ---------------------------------------------------------------------------
// Measurement store
// ---------------------------------------------------------------------------

TEST(NodeMeasurementStore_KeysOnTheNodeFingerprintAndVersion, Basic) {
  auto store = std::make_shared<ObservationStore>();
  NodeMeasurementStore measurements(store, NodeFingerprint{"fp-stack"});

  int measured = 0;
  const auto measure = [&] {
    ++measured;
    return StageMeasurement{{"groups", std::string("0,0,1")},
                            {"inputs", static_cast<int32_t>(3)}};
  };

  const auto got =
      measurements.get_or_measure("stacker.pressings", "1", measure);
  EXPECT_EQ(std::get<std::string>(got.at("groups")), "0,0,1");
  EXPECT_EQ(std::get<int32_t>(got.at("inputs")), 3);

  // Kept in the store as the node's record...
  const ObservationRecordKey key{NodeFingerprint{"fp-stack"}, FieldID(0),
                                 "stage:stacker.pressings", "1"};
  EXPECT_EQ(measurements.key_for("stacker.pressings", "1"), key);
  const auto record = store->get(key);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->at("stacker.pressings"), got);

  // ...so a second store over the same content does not measure again...
  NodeMeasurementStore other_copy(store, NodeFingerprint{"fp-stack"});
  other_copy.get_or_measure("stacker.pressings", "1", measure);
  EXPECT_EQ(measured, 1);

  // ...while another version, or other content, does.
  measurements.get_or_measure("stacker.pressings", "2", measure);
  NodeMeasurementStore other_content(store, NodeFingerprint{"fp-other"});
  other_content.get_or_measure("stacker.pressings", "1", measure);
  EXPECT_EQ(measured, 3);
}

TEST(DAGExecutor_MeasurementStore_IsBoundToTheExecutingNode, Basic) {
  auto store = std::make_shared<ObservationStore>();
  DagCopy copy;
  DAGExecutor executor;
  executor.set_measurement_store(
      store,
      std::make_shared<const NodeFingerprintMap>(fingerprints_of_copy()));

  executor.execute_to_node(copy.dag, NodeID(2));

  auto* bound = dynamic_cast<NodeMeasurementStore*>(
      copy.transform->measurement_store.get());
  ASSERT_NE(bound, nullptr);
  EXPECT_EQ(bound->key_for("m", "1").fingerprint,
            NodeFingerprint{"fp-transform"});
}

// Without a store (the CLI) a consumer is told so, and measures for itself.
TEST(DAGExecutor_MeasurementStore_AbsentGivesTheStageNone, Basic) {
  DagCopy copy;
  DAGExecutor executor;

  executor.execute_to_node(copy.dag, NodeID(2));

  EXPECT_EQ(copy.transform->store_offers, 1);
  EXPECT_EQ(copy.transform->measurement_store, nullptr);
}

TEST(DAGExecutor_MeasurementStore_NodeWithoutFingerprintGetsNone, Basic) {
  DagCopy copy;
  DAGExecutor executor;
  executor.set_measurement_store(std::make_shared<ObservationStore>(),
                                 std::make_shared<const NodeFingerprintMap>());

  executor.execute_to_node(copy.dag, NodeID(2));

  EXPECT_EQ(copy.transform->store_offers, 1);
  EXPECT_EQ(copy.transform->measurement_store, nullptr);
}

}  // namespace
}  // namespace orc
