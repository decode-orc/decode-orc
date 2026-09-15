/*
 * File:        trigger_all_sinks_shared_execution_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     Verify triggerAllSinks() executes a predecessor shared by
 *              multiple sinks only once per batch
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/triggerable_stage.h>

#include <atomic>
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

// Trivial artifact so the counting source has something to hand downstream;
// its content is never inspected.
class CountingSourceArtifact : public orc::Artifact {
 public:
  CountingSourceArtifact()
      : orc::Artifact(orc::ArtifactID("counting-source-artifact"),
                      orc::Provenance{}) {}
  std::string type_name() const override { return "CountingSourceArtifact"; }
};

// A SOURCE stage that counts how many times execute() actually runs. Shared
// across every instance the registry creates (a static counter, not an
// instance member), because the mechanism under test — triggerAllSinks()
// batching every sink against one DAG/executor — is about how many times a
// *fresh* stage instance is built and executed for the batch, not about any
// one instance's own state.
class CountingSourceStage : public orc::DAGStage {
 public:
  static std::atomic<int> execute_count;

  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return orc::NodeTypeInfo{orc::NodeType::SOURCE,
                             "unit_test_counting_source",
                             "Counting Source (test-only)",
                             "Test-only stage counting execute() calls",
                             0,
                             0,
                             1,
                             UINT32_MAX,
                             orc::VideoFormatCompatibility::ALL};
  }
  std::vector<orc::ArtifactPtr> execute(
      const std::vector<orc::ArtifactPtr>& inputs,
      const std::map<std::string, orc::ParameterValue>& parameters,
      orc::ObservationContext& observation_context) override {
    (void)inputs;
    (void)parameters;
    (void)observation_context;
    execute_count.fetch_add(1, std::memory_order_relaxed);
    return {std::make_shared<CountingSourceArtifact>()};
  }
  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }
};
std::atomic<int> CountingSourceStage::execute_count{0};

// A SINK stage that just records whether it was triggered with a non-empty
// input. Two of these share the counting source above.
class CountingSinkStage : public orc::DAGStage, public orc::TriggerableStage {
 public:
  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return orc::NodeTypeInfo{orc::NodeType::SINK,
                             "unit_test_counting_sink",
                             "Counting Sink (test-only)",
                             "Test-only stage",
                             1,
                             1,
                             0,
                             0,
                             orc::VideoFormatCompatibility::ALL};
  }
  std::vector<orc::ArtifactPtr> execute(
      const std::vector<orc::ArtifactPtr>& /*inputs*/,
      const std::map<std::string, orc::ParameterValue>& /*parameters*/,
      orc::ObservationContext& /*observation_context*/) override {
    return {};
  }
  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 0; }

  bool trigger(const std::vector<orc::ArtifactPtr>& inputs,
               const std::map<std::string, orc::ParameterValue>& /*parameters*/,
               orc::IObservationContext& /*observation_context*/) override {
    triggered_with_input_ = !inputs.empty();
    return triggered_with_input_;
  }
  std::string get_trigger_status() const override {
    return triggered_with_input_ ? "ok" : "no input";
  }

 private:
  bool triggered_with_input_ = false;
};

// Registers the two test-only stages exactly once for the whole test binary.
// Additive only (never clears the shared StageRegistry singleton), so other
// test files sharing this process are unaffected.
void ensure_test_stages_registered() {
  static const bool registered = [] {
    auto& registry = orc::StageRegistry::instance();
    if (!registry.has_stage("unit_test_counting_source")) {
      registry.register_stage("unit_test_counting_source", [] {
        return std::make_shared<CountingSourceStage>();
      });
    }
    if (!registry.has_stage("unit_test_counting_sink")) {
      registry.register_stage("unit_test_counting_sink", [] {
        return std::make_shared<CountingSinkStage>();
      });
    }
    return true;
  }();
  (void)registered;
}

// One counting source feeding two counting sinks: node 1 -> nodes 2 and 3.
orc::Project make_fan_out_project() {
  const std::string yaml =
      "project:\n"
      "  name: trigger-all-sinks-shared-execution-test\n"
      "  version: \"2.0\"\n"
      "  video_format: PAL\n"
      "  source_format: Composite\n"
      "  amplitude_unit: mV\n"
      "dag:\n"
      "  nodes:\n"
      "    - id: 1\n"
      "      stage: unit_test_counting_source\n"
      "      node_type: SOURCE\n"
      "    - id: 2\n"
      "      stage: unit_test_counting_sink\n"
      "      node_type: SINK\n"
      "    - id: 3\n"
      "      stage: unit_test_counting_sink\n"
      "      node_type: SINK\n"
      "  edges:\n"
      "    - from: 1\n"
      "      to: 2\n"
      "    - from: 1\n"
      "      to: 3\n";
  return orc::project_io::load_project_from_yaml(
      yaml, "/virtual/trigger_all_sinks_shared_execution.orcprj");
}

}  // namespace

TEST(TriggerAllSinksSharedExecutionTest,
     SharedPredecessor_ExecutesOnce_AcrossTwoSinksInOneBatch) {
  ensure_test_stages_registered();
  CountingSourceStage::execute_count.store(0, std::memory_order_relaxed);

  auto project = make_fan_out_project();
  orc::presenters::ProjectPresenter presenter(static_cast<void*>(&project));

  ASSERT_TRUE(presenter.triggerAllSinks(nullptr));
  EXPECT_EQ(CountingSourceStage::execute_count.load(std::memory_order_relaxed),
            1);
}

}  // namespace orc_unit_test
