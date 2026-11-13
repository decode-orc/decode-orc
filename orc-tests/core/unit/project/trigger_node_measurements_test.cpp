/*
 * File:        trigger_node_measurements_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     project_io::trigger_node() gives stages that keep measurements
 *              the store it is handed, so a second run reads what the first
 *              measured
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/observation/stage_measurement_store.h>
#include <orc/stage/triggerable_stage.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../../orc/core/include/observation_store.h"
#include "../../../orc/core/include/project.h"
#include "../../../orc/core/include/stage_registry.h"

namespace orc_unit_test {
namespace {

class MeasuredArtifact : public orc::Artifact {
 public:
  MeasuredArtifact()
      : orc::Artifact(orc::ArtifactID("trigger-node-measurements-artifact"),
                      orc::Provenance{}) {}
  std::string type_name() const override { return "MeasuredArtifact"; }
};

// Measurements taken, and whether the last execute had a store.
std::atomic<int>& measurements_taken() {
  static std::atomic<int> count{0};
  return count;
}
std::atomic<bool>& had_store() {
  static std::atomic<bool> value{false};
  return value;
}

// A source that measures something once per content, as the Stacker measures
// its pressing groups.
class MeasuringSource : public orc::DAGStage,
                        public orc::IStageMeasurementConsumer {
 public:
  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return orc::NodeTypeInfo{orc::NodeType::SOURCE,
                             "unit_test_measuring_source",
                             "Measuring Source (test-only)",
                             "Test-only stage",
                             0,
                             0,
                             1,
                             UINT32_MAX,
                             orc::VideoFormatCompatibility::ALL};
  }
  void set_measurement_store(
      std::shared_ptr<orc::IStageMeasurementStore> store) override {
    store_ = std::move(store);
  }
  std::vector<orc::ArtifactPtr> execute(
      const std::vector<orc::ArtifactPtr>&,
      const std::map<std::string, orc::ParameterValue>&,
      orc::ObservationContext&) override {
    had_store() = store_ != nullptr;
    const auto measure = [] {
      ++measurements_taken();
      return orc::StageMeasurement{{"value", int32_t{7}}};
    };
    if (store_) {
      store_->get_or_measure("probe", "1", measure);
    } else {
      measure();
    }
    return {std::make_shared<MeasuredArtifact>()};
  }
  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }

 private:
  std::shared_ptr<orc::IStageMeasurementStore> store_;
};

class MeasuredSink : public orc::DAGStage, public orc::TriggerableStage {
 public:
  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return orc::NodeTypeInfo{orc::NodeType::SINK,
                             "unit_test_measured_sink",
                             "Measured Sink (test-only)",
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
  bool trigger(const std::vector<orc::ArtifactPtr>&,
               const std::map<std::string, orc::ParameterValue>&,
               orc::IObservationContext&) override {
    return true;
  }
  std::string get_trigger_status() const override { return "ok"; }
};

void ensure_stages_registered() {
  static const bool registered = [] {
    auto& registry = orc::StageRegistry::instance();
    if (!registry.has_stage("unit_test_measuring_source")) {
      registry.register_stage("unit_test_measuring_source", [] {
        return std::make_shared<MeasuringSource>();
      });
    }
    if (!registry.has_stage("unit_test_measured_sink")) {
      registry.register_stage("unit_test_measured_sink",
                              [] { return std::make_shared<MeasuredSink>(); });
    }
    return true;
  }();
  (void)registered;
}

constexpr const char* kProject = R"yaml(
project:
  name: trigger-measurements
  version: "2.0"
  video_format: PAL
  source_format: Composite
  amplitude_unit: mV
dag:
  nodes:
    - id: 1
      stage: unit_test_measuring_source
      node_type: SOURCE
      display_name: Source
      user_label: Source
      x: 0
      y: 0
    - id: 2
      stage: unit_test_measured_sink
      node_type: SINK
      display_name: Sink
      user_label: Sink
      x: 0
      y: 0
  edges:
    - from: 1
      to: 2
)yaml";

// Triggers the sink in a fresh load of the project, as each orc-cli run does.
void trigger(const std::shared_ptr<orc::ObservationStore>& measurements) {
  ensure_stages_registered();
  auto project =
      orc::project_io::load_project_from_yaml(kProject, "measured.orcprj");
  std::string status;
  ASSERT_TRUE(orc::project_io::trigger_node(project, orc::NodeID(2), status,
                                            nullptr, measurements));
}

}  // namespace

TEST(TriggerNodeMeasurementsTest, WithoutAStoreEveryRunMeasures) {
  measurements_taken() = 0;
  trigger(nullptr);
  EXPECT_FALSE(had_store());
  trigger(nullptr);
  EXPECT_EQ(measurements_taken(), 2);
}

TEST(TriggerNodeMeasurementsTest, ASecondRunReadsTheFirstRunsMeasurement) {
  measurements_taken() = 0;
  const auto store = std::make_shared<orc::ObservationStore>();
  trigger(store);
  EXPECT_TRUE(had_store());
  trigger(store);
  EXPECT_EQ(measurements_taken(), 1);
}

}  // namespace orc_unit_test
