/*
 * File:        fixture_passthrough_stage.h
 * Module:      external-stage-plugin fixture
 * Purpose:     Minimal passthrough transform validating the installed SDK
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/stage.h>
#include <orc/support/logging.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace orc_fixture {

/**
 * @brief Passthrough transform stage built only from the installed SDK.
 *
 * Exists solely to prove that the exported decode-orc-plugin-sdk package is
 * sufficient to compile, link, and load a stage plugin out-of-tree. It
 * forwards its single input artifact unchanged.
 */
class FixturePassthroughStage : public orc::DAGStage {
 public:
  // The plugin carries a private spdlog (the installed SDK package builds it
  // that way), so this record reaches the host's log only by being forwarded
  // through OrcPluginServices::log. CI looks for it in orc-cli's output.
  FixturePassthroughStage() {
    ORC_LOG_INFO("External fixture stage logging through the host");
  }

  std::string version() const override { return "0.1.0"; }

  orc::NodeTypeInfo get_node_type_info() const override {
    return orc::NodeTypeInfo{orc::NodeType::TRANSFORM,
                             "external_fixture_passthrough",
                             "External Fixture Passthrough",
                             "SDK package validation fixture (passthrough)",
                             1,
                             1,
                             1,
                             1,
                             orc::VideoFormatCompatibility::ALL};
  }

  std::vector<orc::ArtifactPtr> execute(
      const std::vector<orc::ArtifactPtr>& inputs,
      const std::map<std::string, orc::ParameterValue>& parameters,
      orc::ObservationContext& observation_context) override {
    (void)parameters;
    (void)observation_context;
    if (inputs.size() != 1) {
      throw orc::DAGExecutionError(
          "FixturePassthroughStage expects exactly one input");
    }
    return {inputs.front()};
  }

  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 1; }
};

}  // namespace orc_fixture
