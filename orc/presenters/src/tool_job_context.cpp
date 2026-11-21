/*
 * File:        tool_job_context.cpp
 * Module:      orc-presenters
 * Purpose:     What a stage's tool job is run with: a graph of its own, the
 *              node's stage on it, the node's inputs and the job context
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "tool_job_context.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <utility>

#include "../../core/include/dag_executor.h"
#include "../../core/include/project.h"
#include "../../core/include/project_to_dag.h"

namespace orc::presenters {

PreparedToolJob prepareToolJob(const orc::Project& project, NodeID node) {
  PreparedToolJob job;
  job.dag = orc::project_to_dag(project);
  if (!job.dag) {
    job.error = "The project has no processing graph";
    return job;
  }
  const auto& nodes = job.dag->nodes();
  const auto node_it =
      std::find_if(nodes.begin(), nodes.end(),
                   [node](const orc::DAGNode& n) { return n.node_id == node; });
  if (node_it == nodes.end() || !node_it->stage) {
    job.error = "The node no longer exists";
    return job;
  }
  job.executor = std::make_shared<orc::DAGExecutor>();
  job.inputs = job.executor->execute_inputs_of(*job.dag, node);
  job.stage = node_it->stage.get();
  return job;
}

orc::ToolJobPrivateInputs openPrivateToolInputs(const orc::Project& project,
                                                NodeID node) {
  orc::ToolJobPrivateInputs copy;
  try {
    auto job = std::make_shared<PreparedToolJob>(prepareToolJob(project, node));
    if (!job->stage) {
      ORC_LOG_WARN("Could not copy the inputs of node {}: {}", node.to_string(),
                   job->error);
      return copy;
    }
    copy.inputs = job->inputs;
    copy.owner = std::move(job);
  } catch (const std::exception& e) {
    ORC_LOG_WARN("Could not copy the inputs of node {}: {}", node.to_string(),
                 e.what());
    copy = {};
  }
  return copy;
}

orc::ToolJobPrivateInputs HostToolJobContext::open_private_inputs() {
  if (!open_private_) {
    return {};
  }
  return open_private_();
}

}  // namespace orc::presenters
