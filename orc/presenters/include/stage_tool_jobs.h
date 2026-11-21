/*
 * File:        stage_tool_jobs.h
 * Module:      orc-presenters
 * Purpose:     One plugin view's job: a snapshot of the project taken on the
 *              GUI thread, run against the node's stage on a job worker
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc/view/stage_tool_session.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace orc {
class Project;
}  // namespace orc

namespace orc::presenters {

/**
 * @brief A stage tool job (orc::IStageToolJobHandler) ready to run
 *
 * Construct it on the GUI thread: it copies the project there, so the job
 * sees the project as it stood when the user started it, whatever is edited
 * while it runs. run() builds a graph of its own from that copy, executes the
 * node's inputs and calls the node's stage, on whichever thread calls it. The
 * stage object it calls is the job's own, never the one previews use.
 *
 * Thread safety: construct on the GUI thread; then run() once, from one
 * other thread. The object may be destroyed on either thread after run()
 * returns.
 */
class StageToolJob {
 public:
  using ProgressFn = std::function<void(orc::view::ToolJobProgress)>;
  using CancelledFn = std::function<bool()>;

  /// @param project_handle ProjectPresenter::getCoreProjectHandle()
  StageToolJob(void* project_handle, NodeID node, std::string contract_id,
               std::string verb, std::vector<uint8_t> payload);
  ~StageToolJob();

  StageToolJob(const StageToolJob&) = delete;
  StageToolJob& operator=(const StageToolJob&) = delete;

  /**
   * @brief Run the job to the end
   *
   * @param on_progress Called on this thread for each progress report
   * @param is_cancelled Polled on this thread; true stops the job
   * @return The stage's reply, or ok == false with the reason: "Cancelled",
   *         the node is gone, its stage runs no jobs, or it failed
   */
  orc::view::ToolReply run(const ProgressFn& on_progress,
                           const CancelledFn& is_cancelled);

 private:
  std::unique_ptr<orc::Project> project_;
  NodeID node_;
  std::string contract_id_;
  std::string verb_;
  std::vector<uint8_t> payload_;
};

/// The error a cancelled job replies with.
inline constexpr const char kToolJobCancelledError[] = "Cancelled";

}  // namespace orc::presenters
