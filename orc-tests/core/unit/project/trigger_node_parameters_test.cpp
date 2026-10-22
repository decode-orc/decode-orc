/*
 * File:        trigger_node_parameters_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     project_io::trigger_node() hands the stage its parameters as
 *              the DAG resolves them: format-specific defaults for values
 *              the project never stored, with stored values winning
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/triggerable_stage.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../../orc/core/include/project.h"
#include "../../../orc/core/include/stage_registry.h"

namespace orc_unit_test {
namespace {

class TriggerTestArtifact : public orc::Artifact {
 public:
  TriggerTestArtifact()
      : orc::Artifact(orc::ArtifactID("trigger-node-parameters-artifact"),
                      orc::Provenance{}) {}
  std::string type_name() const override { return "TriggerTestArtifact"; }
};

class TriggerTestSource : public orc::DAGStage {
 public:
  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return orc::NodeTypeInfo{orc::NodeType::SOURCE,
                             "unit_test_trigger_source",
                             "Trigger Source (test-only)",
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
    return {std::make_shared<TriggerTestArtifact>()};
  }
  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }
};

/// The parameters the sink was last triggered with.
std::map<std::string, orc::ParameterValue>& triggered_with() {
  static std::map<std::string, orc::ParameterValue> parameters;
  return parameters;
}

/// A sink whose decoder default depends on the video format, as the video
/// sink's does.
class TriggerTestSink : public orc::DAGStage,
                        public orc::ParameterizedStage,
                        public orc::TriggerableStage {
 public:
  std::string version() const override { return "1.0"; }
  orc::NodeTypeInfo get_node_type_info() const override {
    return orc::NodeTypeInfo{orc::NodeType::SINK,
                             "unit_test_trigger_sink",
                             "Trigger Sink (test-only)",
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
      orc::VideoSystem system, orc::SourceType) const override {
    orc::ParameterDescriptor decoder;
    decoder.name = "decoder";
    decoder.display_name = "Decoder";
    decoder.type = orc::ParameterType::STRING;
    decoder.constraints.default_value = std::string(
        system == orc::VideoSystem::PAL ? "pal_decoder" : "ntsc_decoder");
    return {decoder};
  }
  std::map<std::string, orc::ParameterValue> get_parameters() const override {
    return {};
  }
  bool set_parameters(
      const std::map<std::string, orc::ParameterValue>&) override {
    return true;
  }

  bool trigger(const std::vector<orc::ArtifactPtr>&,
               const std::map<std::string, orc::ParameterValue>& parameters,
               orc::IObservationContext&) override {
    triggered_with() = parameters;
    return true;
  }
  std::string get_trigger_status() const override { return "ok"; }
};

void ensure_trigger_test_stages_registered() {
  static const bool registered = [] {
    auto& registry = orc::StageRegistry::instance();
    if (!registry.has_stage("unit_test_trigger_source")) {
      registry.register_stage("unit_test_trigger_source", [] {
        return std::make_shared<TriggerTestSource>();
      });
    }
    if (!registry.has_stage("unit_test_trigger_sink")) {
      registry.register_stage("unit_test_trigger_sink", [] {
        return std::make_shared<TriggerTestSink>();
      });
    }
    return true;
  }();
  (void)registered;
}

std::string project_yaml(const std::string& sink_parameters) {
  return R"yaml(
project:
  name: trigger-parameters
  version: "2.0"
  video_format: PAL
  source_format: Composite
  amplitude_unit: mV
dag:
  nodes:
    - id: 1
      stage: unit_test_trigger_source
      node_type: SOURCE
      display_name: Source
      user_label: Source
      x: 0
      y: 0
    - id: 2
      stage: unit_test_trigger_sink
      node_type: SINK
      display_name: Sink
      user_label: Sink
      x: 0
      y: 0
)yaml" + sink_parameters +
         R"yaml(
  edges:
    - from: 1
      to: 2
)yaml";
}

std::string trigger_sink(const std::string& sink_parameters) {
  ensure_trigger_test_stages_registered();
  auto project = orc::project_io::load_project_from_yaml(
      project_yaml(sink_parameters), "trigger.orcprj");
  triggered_with().clear();
  std::string status;
  EXPECT_TRUE(orc::project_io::trigger_node(project, orc::NodeID(2), status));
  const auto it = triggered_with().find("decoder");
  return it == triggered_with().end() ? "(not passed)"
                                      : std::get<std::string>(it->second);
}

}  // namespace

// `orc-cli <project> --process` triggers through trigger_node(); a video sink
// in a PAL project that never stored its decoder used to get the NTSC one.
TEST(TriggerNodeParametersTest, AnUnsetParameterGetsTheProjectFormatsDefault) {
  EXPECT_EQ(trigger_sink(""), "pal_decoder");
}

TEST(TriggerNodeParametersTest, AStoredParameterWinsOverTheDefault) {
  EXPECT_EQ(trigger_sink(R"yaml(      parameters:
        decoder:
          type: string
          value: chosen_decoder)yaml"),
            "chosen_decoder");
}

}  // namespace orc_unit_test
