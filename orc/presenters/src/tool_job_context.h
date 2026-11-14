/*
 * File:        tool_job_context.h
 * Module:      orc-presenters
 * Purpose:     What a stage's tool job is run with: a graph of its own, the
 *              node's stage on it, the node's inputs and the job context
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/artifact.h>
#include <orc/stage/node_id.h>
#include <orc/stage/tooling/stage_tool_jobs.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace orc {
class DAG;
class DAGExecutor;
class DAGStage;
class Project;
}  // namespace orc

namespace orc::presenters {

/**
 * @brief A node's stage and inputs on a graph built for one job
 *
 * Built from a copy of the project, so the stage object is the job's own:
 * previews, requests and triggers never see it.
 */
struct PreparedToolJob {
  /// Hold the stage and what the inputs read through; the inputs must not
  /// outlive them.
  std::shared_ptr<orc::DAG> dag;
  std::shared_ptr<orc::DAGExecutor> executor;
  /// The node's stage on @c dag; null when @c error says why.
  orc::DAGStage* stage = nullptr;
  std::vector<orc::ArtifactPtr> inputs;
  std::string error;
};

/**
 * @brief Build the job's graph from @p project and run the node's inputs
 *
 * Call on the job's thread: running the inputs can take as long as their
 * sources take to open. @p project is only read.
 *
 * @return error set (and stage null) when the project has no graph or the
 *         node is gone; exceptions from building or running the inputs pass
 *         through
 */
PreparedToolJob prepareToolJob(const orc::Project& project, NodeID node);

/**
 * @brief Open another copy of the node's inputs (open_private_inputs())
 *
 * Builds the graph from @p project again and runs it to the node's inputs;
 * the copy shares no stage or cache with any other. @p project is only read,
 * and must outlive the call.
 *
 * @return inputs empty when the graph cannot be built or run
 */
orc::ToolJobPrivateInputs openPrivateToolInputs(const orc::Project& project,
                                                NodeID node);

/// The context a job's stage is called with.
class HostToolJobContext final : public orc::IToolJobContext {
 public:
  using ProgressFn = std::function<void(double fraction, const std::string&)>;
  using CancelledFn = std::function<bool()>;
  using OpenPrivateFn = std::function<orc::ToolJobPrivateInputs()>;

  /// @p open_private may be empty: open_private_inputs() then makes none.
  HostToolJobContext(std::vector<orc::ArtifactPtr> inputs,
                     ProgressFn on_progress, CancelledFn is_cancelled,
                     OpenPrivateFn open_private = {})
      : inputs_(std::move(inputs)),
        on_progress_(std::move(on_progress)),
        is_cancelled_(std::move(is_cancelled)),
        open_private_(std::move(open_private)) {}

  const std::vector<orc::ArtifactPtr>& inputs() const override {
    return inputs_;
  }

  void report_progress(double fraction, const std::string& text) override {
    if (on_progress_) {
      on_progress_(fraction, text);
    }
  }

  bool is_cancelled() const override {
    return is_cancelled_ && is_cancelled_();
  }

  orc::ToolJobPrivateInputs open_private_inputs() override;

 private:
  std::vector<orc::ArtifactPtr> inputs_;
  ProgressFn on_progress_;
  CancelledFn is_cancelled_;
  OpenPrivateFn open_private_;
};

}  // namespace orc::presenters
