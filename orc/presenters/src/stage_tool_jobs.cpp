/*
 * File:        stage_tool_jobs.cpp
 * Module:      orc-presenters
 * Purpose:     One plugin view's job, run against the node's stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../include/stage_tool_jobs.h"

#include <orc/stage/tooling/stage_tool_jobs.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <exception>
#include <utility>

#include "../../core/include/dag_executor.h"
#include "../../core/include/project.h"
#include "tool_job_context.h"

namespace orc::presenters {

namespace {

orc::view::ToolReply failure(std::string error) {
  orc::view::ToolReply reply;
  reply.error = std::move(error);
  return reply;
}

}  // namespace

StageToolJob::StageToolJob(void* project_handle, NodeID node,
                           std::string contract_id, std::string verb,
                           std::vector<uint8_t> payload)
    : project_(project_handle ? std::make_unique<orc::Project>(
                                    *static_cast<orc::Project*>(project_handle))
                              : nullptr),
      node_(node),
      contract_id_(std::move(contract_id)),
      verb_(std::move(verb)),
      payload_(std::move(payload)) {}

StageToolJob::~StageToolJob() = default;

orc::view::ToolReply StageToolJob::run(const ProgressFn& on_progress,
                                       const CancelledFn& is_cancelled) {
  const auto cancelled = [&is_cancelled] {
    return is_cancelled && is_cancelled();
  };
  if (!project_) {
    return failure("No project is open");
  }
  if (on_progress) {
    on_progress({-1.0, "Preparing the node's inputs..."});
  }

  try {
    auto prepared = prepareToolJob(*project_, node_);
    if (!prepared.stage) {
      return failure(prepared.error);
    }
    auto* handler = dynamic_cast<orc::IStageToolJobHandler*>(prepared.stage);
    if (!handler) {
      return failure("The node's stage does not run tool jobs");
    }
    if (cancelled()) {
      return failure(kToolJobCancelledError);
    }

    HostToolJobContext context(
        std::move(prepared.inputs),
        [&on_progress](double fraction, const std::string& text) {
          if (on_progress) {
            on_progress({fraction, text});
          }
        },
        is_cancelled,
        [this] { return openPrivateToolInputs(*project_, node_); });
    orc::StageToolReply answer =
        handler->run_tool_job(contract_id_, verb_, payload_, context);
    orc::view::ToolReply reply;
    reply.ok = answer.ok;
    reply.data = std::move(answer.data);
    reply.error = std::move(answer.error);
    if (!reply.ok && cancelled()) {
      reply.data.clear();
      reply.error = kToolJobCancelledError;
    }
    return reply;
  } catch (const std::exception& e) {
    ORC_LOG_WARN("Tool job '{}' on node {} failed: {}", verb_,
                 node_.to_string(), e.what());
    if (cancelled()) {
      return failure(kToolJobCancelledError);
    }
    return failure(std::string("The job failed: ") + e.what());
  } catch (...) {
    if (cancelled()) {
      return failure(kToolJobCancelledError);
    }
    return failure("The job failed");
  }
}

}  // namespace orc::presenters
