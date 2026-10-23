/*
 * File:        frame_provenance_test.cpp
 * Module:      orc-core tests
 * Purpose:     Unit tests for static DAG node provenance fingerprints
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_provenance.h"

#include <gtest/gtest.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/stage.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dag_executor.h"

namespace orc {
namespace {

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

// Minimal concrete artifact carrying a fixed identity.
class FakeArtifact final : public Artifact {
 public:
  explicit FakeArtifact(std::string id)
      : Artifact(ArtifactID(std::move(id)), Provenance{}) {}
  std::string type_name() const override { return "FakeArtifact"; }
};

// Non-source stage: implements only the DAGStage contract.
class MockStage : public DAGStage {
 public:
  MockStage(NodeType type, std::string name, std::string version,
            size_t input_count, size_t output_count)
      : type_(type),
        name_(std::move(name)),
        version_(std::move(version)),
        input_count_(input_count),
        output_count_(output_count) {}

  std::string version() const override { return version_; }

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo(
        type_, name_, name_, "", static_cast<uint32_t>(input_count_),
        static_cast<uint32_t>(input_count_),
        static_cast<uint32_t>(output_count_),
        static_cast<uint32_t>(output_count_), VideoFormatCompatibility::ALL);
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& /*inputs*/,
      const std::map<std::string, ParameterValue>& /*parameters*/,
      ObservationContext& /*observation_context*/) override {
    ++execute_count;
    std::vector<ArtifactPtr> outputs;
    outputs.reserve(output_count_);
    for (size_t i = 0; i < output_count_; ++i) {
      outputs.push_back(std::make_shared<FakeArtifact>(name_ + "_out"));
    }
    return outputs;
  }

  size_t required_input_count() const override { return input_count_; }
  size_t output_count() const override { return output_count_; }

  int execute_count = 0;

 private:
  NodeType type_;
  std::string name_;
  std::string version_;
  size_t input_count_;
  size_t output_count_;
};

// Source stage exposing a single FILE_PATH input parameter named "file".
class MockSourceStage final : public DAGStage, public ParameterizedStage {
 public:
  explicit MockSourceStage(std::string version = "1.0.0")
      : version_(std::move(version)) {}

  std::string version() const override { return version_; }

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo(NodeType::SOURCE, "MockSource", "MockSource", "", 0, 0,
                        1, 1, VideoFormatCompatibility::ALL);
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& /*inputs*/,
      const std::map<std::string, ParameterValue>& /*parameters*/,
      ObservationContext& /*observation_context*/) override {
    return {std::make_shared<FakeArtifact>("source_out")};
  }

  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }

  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem /*project_format*/,
      SourceType /*source_type*/) const override {
    ParameterDescriptor desc;
    desc.name = "file";
    desc.display_name = "File";
    desc.type = ParameterType::FILE_PATH;
    return {desc};
  }
  using ParameterizedStage::get_parameter_descriptors;

  std::map<std::string, ParameterValue> get_parameters() const override {
    return {};
  }
  bool set_parameters(
      const std::map<std::string, ParameterValue>& /*params*/) override {
    return true;
  }

 private:
  std::string version_;
};

class MockFileIdentityProvider final : public IFileIdentityProvider {
 public:
  std::map<std::string, FileIdentity> identities;

  FileIdentity identify(const std::string& path) const override {
    auto it = identities.find(path);
    return it != identities.end() ? it->second : FileIdentity{};
  }
};

// Identity provider that always reports "no such file" — used to assert that a
// missing file never throws.
class NullFileIdentityProvider final : public IFileIdentityProvider {
 public:
  FileIdentity identify(const std::string& /*path*/) const override {
    return FileIdentity{};
  }
};

// ---------------------------------------------------------------------------
// DAG builders
// ---------------------------------------------------------------------------

DAGStagePtr make_source() {
  return std::make_shared<MockStage>(NodeType::SOURCE, "Source", "1.0.0", 0, 1);
}

DAGStagePtr make_transform(std::string name = "Transform") {
  return std::make_shared<MockStage>(NodeType::TRANSFORM, std::move(name),
                                     "1.0.0", 1, 1);
}

DAGNode make_node(NodeID id, DAGStagePtr stage,
                  std::map<std::string, ParameterValue> params = {},
                  std::vector<NodeID> inputs = {}) {
  DAGNode node;
  node.node_id = id;
  node.stage = std::move(stage);
  node.parameters = std::move(params);
  node.input_indices.assign(inputs.size(), 0);
  node.input_node_ids = std::move(inputs);
  return node;
}

// ---------------------------------------------------------------------------
// serialize_node_provenance
// ---------------------------------------------------------------------------

TEST(SerializeNodeProvenance_IsDeterministic_ForIdenticalInputs, Basic) {
  std::map<std::string, ParameterValue> params{{"gain", 1.5},
                                               {"name", std::string("x")}};
  const std::string a =
      serialize_node_provenance("Stage", "1.0.0", {"in0"}, params);
  const std::string b =
      serialize_node_provenance("Stage", "1.0.0", {"in0"}, params);
  EXPECT_EQ(a, b);
}

TEST(SerializeNodeProvenance_DistinguishesCloseDoubles, PrecisionPreserved) {
  const std::string a =
      serialize_node_provenance("Stage", "1.0.0", {}, {{"gain", 1.0000001}});
  const std::string b =
      serialize_node_provenance("Stage", "1.0.0", {}, {{"gain", 1.0000002}});
  EXPECT_NE(a, b);
}

TEST(SerializeNodeProvenance_DistinguishesVariantTypes, TypeTagged) {
  const std::string as_int = serialize_node_provenance(
      "Stage", "1.0.0", {}, {{"v", static_cast<int32_t>(1)}});
  const std::string as_bool =
      serialize_node_provenance("Stage", "1.0.0", {}, {{"v", true}});
  const std::string as_string = serialize_node_provenance(
      "Stage", "1.0.0", {}, {{"v", std::string("1")}});
  EXPECT_NE(as_int, as_bool);
  EXPECT_NE(as_int, as_string);
  EXPECT_NE(as_bool, as_string);
}

TEST(SerializeNodeProvenance_SourceIdentityMatters, AppendedWhenPresent) {
  const std::string without = serialize_node_provenance("Src", "1.0.0", {}, {});
  const std::string with =
      serialize_node_provenance("Src", "1.0.0", {}, {}, "file=10:20");
  EXPECT_NE(without, with);
}

// ---------------------------------------------------------------------------
// compute_node_fingerprints — Task 1.1
// ---------------------------------------------------------------------------

TEST(ComputeNodeFingerprints_IdenticalDag_ProducesIdenticalMap, Stable) {
  MockFileIdentityProvider provider;

  auto build = [] {
    auto dag = std::make_shared<DAG>();
    dag->add_node(make_node(NodeID(1), make_source()));
    dag->add_node(
        make_node(NodeID(2), make_transform(), {{"gain", 2.0}}, {NodeID(1)}));
    return dag;
  };

  const auto map_a = compute_node_fingerprints(*build(), provider);
  const auto map_b = compute_node_fingerprints(*build(), provider);

  ASSERT_EQ(map_a.size(), map_b.size());
  for (const auto& [id, fp] : map_a) {
    ASSERT_TRUE(map_b.count(id));
    EXPECT_EQ(fp, map_b.at(id));
    EXPECT_EQ(fp.value.size(), 64u);  // SHA-256 hex
  }
}

TEST(ComputeNodeFingerprints_ParameterChange_AffectsDescendantsOnly, Scoped) {
  MockFileIdentityProvider provider;

  // Chain: 1 -> 2 -> 3, plus an unrelated branch: 4 -> 5.
  auto build = [](double gain_on_node2) {
    auto dag = std::make_shared<DAG>();
    dag->add_node(make_node(NodeID(1), make_source()));
    dag->add_node(make_node(NodeID(2), make_transform("T2"),
                            {{"gain", gain_on_node2}}, {NodeID(1)}));
    dag->add_node(make_node(NodeID(3), make_transform("T3"), {}, {NodeID(2)}));
    dag->add_node(make_node(NodeID(4), make_source()));
    dag->add_node(make_node(NodeID(5), make_transform("T5"), {}, {NodeID(4)}));
    return dag;
  };

  const auto before = compute_node_fingerprints(*build(1.0), provider);
  const auto after = compute_node_fingerprints(*build(2.0), provider);

  // Edited node and its descendant change.
  EXPECT_NE(before.at(NodeID(2)), after.at(NodeID(2)));
  EXPECT_NE(before.at(NodeID(3)), after.at(NodeID(3)));

  // Ancestor and the unrelated branch are byte-identical.
  EXPECT_EQ(before.at(NodeID(1)), after.at(NodeID(1)));
  EXPECT_EQ(before.at(NodeID(4)), after.at(NodeID(4)));
  EXPECT_EQ(before.at(NodeID(5)), after.at(NodeID(5)));
}

TEST(ComputeNodeFingerprints_NodeInsertion_ChangesAffectedChainOnly, Scoped) {
  MockFileIdentityProvider provider;

  // Base: 1 -> 2.
  auto base = std::make_shared<DAG>();
  base->add_node(make_node(NodeID(1), make_source()));
  base->add_node(make_node(NodeID(2), make_transform("T2"), {}, {NodeID(1)}));

  // Modified: 1 -> 3(new) -> 2.
  auto modified = std::make_shared<DAG>();
  modified->add_node(make_node(NodeID(1), make_source()));
  modified->add_node(
      make_node(NodeID(3), make_transform("T3"), {}, {NodeID(1)}));
  modified->add_node(
      make_node(NodeID(2), make_transform("T2"), {}, {NodeID(3)}));

  const auto before = compute_node_fingerprints(*base, provider);
  const auto after = compute_node_fingerprints(*modified, provider);

  // The untouched source keeps its fingerprint.
  EXPECT_EQ(before.at(NodeID(1)), after.at(NodeID(1)));
  // The node whose input changed is re-fingerprinted.
  EXPECT_NE(before.at(NodeID(2)), after.at(NodeID(2)));
  // The inserted node exists only in the modified map.
  EXPECT_EQ(before.count(NodeID(3)), 0u);
  EXPECT_EQ(after.count(NodeID(3)), 1u);
}

// ---------------------------------------------------------------------------
// Source content identity — Task 1.2
// ---------------------------------------------------------------------------

TEST(ComputeNodeFingerprints_ChangedFileIdentity_ChangesDownstream, SourceId) {
  MockFileIdentityProvider provider;
  provider.identities["/media/a.tbc"] = FileIdentity{true, 100, 1000};

  auto build = [] {
    auto dag = std::make_shared<DAG>();
    DAGNode source = make_node(NodeID(1), std::make_shared<MockSourceStage>(),
                               {{"file", std::string("/media/a.tbc")}});
    dag->add_node(std::move(source));
    dag->add_node(make_node(NodeID(2), make_transform(), {}, {NodeID(1)}));
    return dag;
  };

  const auto before = compute_node_fingerprints(*build(), provider);

  // Simulate the file being rewritten (size + mtime change).
  provider.identities["/media/a.tbc"] = FileIdentity{true, 200, 2000};
  const auto after = compute_node_fingerprints(*build(), provider);

  EXPECT_NE(before.at(NodeID(1)), after.at(NodeID(1)));
  EXPECT_NE(before.at(NodeID(2)), after.at(NodeID(2)));
}

TEST(ComputeNodeFingerprints_UnchangedFile_KeepsFingerprints, SourceId) {
  MockFileIdentityProvider provider;
  provider.identities["/media/a.tbc"] = FileIdentity{true, 100, 1000};

  auto build = [] {
    auto dag = std::make_shared<DAG>();
    dag->add_node(make_node(NodeID(1), std::make_shared<MockSourceStage>(),
                            {{"file", std::string("/media/a.tbc")}}));
    return dag;
  };

  const auto a = compute_node_fingerprints(*build(), provider);
  const auto b = compute_node_fingerprints(*build(), provider);
  EXPECT_EQ(a.at(NodeID(1)), b.at(NodeID(1)));
}

TEST(ComputeNodeFingerprints_MissingFile_NoThrow_DistinctIdentity, SourceId) {
  auto build = [] {
    auto dag = std::make_shared<DAG>();
    dag->add_node(make_node(NodeID(1), std::make_shared<MockSourceStage>(),
                            {{"file", std::string("/media/missing.tbc")}}));
    return dag;
  };

  NullFileIdentityProvider missing;
  NodeFingerprintMap map_missing;
  ASSERT_NO_THROW(map_missing = compute_node_fingerprints(*build(), missing));
  ASSERT_TRUE(map_missing.count(NodeID(1)));

  // A present file produces a different fingerprint than the "unknown"
  // identity.
  MockFileIdentityProvider present;
  present.identities["/media/missing.tbc"] = FileIdentity{true, 1, 1};
  const auto map_present = compute_node_fingerprints(*build(), present);
  EXPECT_NE(map_missing.at(NodeID(1)), map_present.at(NodeID(1)));
}

// ---------------------------------------------------------------------------
// Executor alignment / parity — Task 1.3
// ---------------------------------------------------------------------------

TEST(DAGExecutor_ReExecutionOfSameDag_HitsCache, Parity) {
  auto source =
      std::make_shared<MockStage>(NodeType::SOURCE, "Source", "1.0.0", 0, 1);
  auto transform = std::make_shared<MockStage>(NodeType::TRANSFORM, "Transform",
                                               "1.0.0", 1, 1);

  DAG dag;
  DAGNode source_node = make_node(NodeID(1), source);
  DAGNode transform_node =
      make_node(NodeID(2), transform, {{"gain", 1.0}}, {NodeID(1)});
  dag.add_node(std::move(source_node));
  dag.add_node(std::move(transform_node));
  dag.set_output_nodes({NodeID(2)});

  DAGExecutor executor;
  executor.execute(dag);
  EXPECT_EQ(source->execute_count, 1);
  EXPECT_EQ(transform->execute_count, 1);

  // Second run of the identical DAG must be served entirely from cache — this
  // is only true if compute_expected_artifact_id() yields stable IDs after the
  // refactor onto the shared provenance serialiser.
  executor.execute(dag);
  EXPECT_EQ(source->execute_count, 1);
  EXPECT_EQ(transform->execute_count, 1);
}

// ---------------------------------------------------------------------------
// sidecar_source_key — the name of the per-source observation sidecar
// ---------------------------------------------------------------------------

DAG single_source_dag(std::map<std::string, ParameterValue> params) {
  DAG dag;
  dag.add_node(make_node(NodeID(1), std::make_shared<MockSourceStage>(),
                         std::move(params)));
  dag.add_node(make_node(NodeID(2), make_transform(), {}, {NodeID(1)}));
  return dag;
}

TEST(SidecarSourceKey_NoSourceNodes_IsEmpty, NoSidecar) {
  DAG dag;
  dag.add_node(make_node(NodeID(1), make_transform()));
  EXPECT_TRUE(sidecar_source_key(dag).empty());
}

TEST(SidecarSourceKey_SourceFileNotChosen_IsEmpty, NoSidecar) {
  EXPECT_TRUE(sidecar_source_key(single_source_dag({})).empty());
  EXPECT_TRUE(
      sidecar_source_key(single_source_dag({{"file", std::string()}})).empty());
}

TEST(SidecarSourceKey_SourceFileChosen_IsTheSourceProvenance, CacheNameKept) {
  const std::map<std::string, ParameterValue> params{
      {"file", std::string("/caps/a.cvbs")}};
  // Byte-identical to the serialisation sidecars were named by before, so
  // existing caches stay warm.
  EXPECT_EQ(sidecar_source_key(single_source_dag(params)),
            serialize_node_provenance("MockSource", "1.0.0", {}, params));
}

TEST(SidecarSourceKey_DifferentFiles_GiveDifferentKeys, PerSource) {
  EXPECT_NE(sidecar_source_key(
                single_source_dag({{"file", std::string("/caps/a.cvbs")}})),
            sidecar_source_key(
                single_source_dag({{"file", std::string("/caps/b.cvbs")}})));
}

TEST(SidecarSourceKey_AnySourceAwaitingItsFile_IsEmpty, NoSidecar) {
  DAG dag;
  dag.add_node(make_node(NodeID(1), std::make_shared<MockSourceStage>(),
                         {{"file", std::string("/caps/a.cvbs")}}));
  dag.add_node(make_node(NodeID(2), std::make_shared<MockSourceStage>()));
  EXPECT_TRUE(sidecar_source_key(dag).empty());
}

TEST(SidecarSourceKey_SourceWithoutFileInputs_HasKey, NeverAwaitsFile) {
  DAG dag;
  dag.add_node(make_node(NodeID(1), make_source(), {{"rate", 2.0}}));
  EXPECT_FALSE(sidecar_source_key(dag).empty());
}

}  // namespace
}  // namespace orc
