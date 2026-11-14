/*
 * File:        fake_tool_job_context.h
 * Module:      orc-core-tests
 * Purpose:     An IToolJobContext for running a stage's tool in a test: the
 *              inputs given, progress recorded, cancel on request, and
 *              private copies of the inputs made by a factory
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/artifact.h>
#include <orc/stage/tooling/stage_tool_jobs.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace orc_unit_test {

class FakeToolJobContext final : public orc::IToolJobContext {
 public:
  explicit FakeToolJobContext(std::vector<orc::ArtifactPtr> inputs)
      : inputs_(std::move(inputs)) {}

  const std::vector<orc::ArtifactPtr>& inputs() const override {
    return inputs_;
  }
  void report_progress(double fraction, const std::string& text) override {
    fractions.push_back(fraction);
    texts.push_back(text);
  }
  bool is_cancelled() const override { return cancelled; }
  orc::ToolJobPrivateInputs open_private_inputs() override {
    ++private_copies;
    orc::ToolJobPrivateInputs copy;
    if (make_private) {
      copy.inputs = make_private();
    }
    return copy;
  }

  bool cancelled = false;
  /// Makes each private copy's inputs; empty makes none.
  std::function<std::vector<orc::ArtifactPtr>()> make_private;
  int private_copies = 0;
  std::vector<double> fractions;
  std::vector<std::string> texts;

 private:
  std::vector<orc::ArtifactPtr> inputs_;
};

// An artifact that is not video.
class NotVideoArtifact : public orc::Artifact {
 public:
  NotVideoArtifact()
      : orc::Artifact(orc::ArtifactID("not_video"), orc::Provenance{}) {}
  std::string type_name() const override { return "not_video"; }
};

}  // namespace orc_unit_test
