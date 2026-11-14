/*
 * File:        stage_tool_jobs.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     The stage side of a view tool's long jobs: run with the node's
 *              inputs on a job worker, with progress and cancel
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_TOOLING_STAGE_TOOL_JOBS_H
#define ORC_STAGE_TOOLING_STAGE_TOOL_JOBS_H

// SDK TIER: stage/tooling — stage contract type crossing the plugin boundary.
// A layout change here bumps the host ABI version.

#include <orc/stage/artifact.h>
#include <orc/stage/tooling/stage_tool_requests.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace orc {

/**
 * @brief A copy of a job's inputs for one worker thread
 *
 * The inputs read through stages and caches of their own, shared with
 * nothing else, so one thread may read them while others read theirs.
 */
struct ToolJobPrivateInputs {
  /// Keeps what the inputs read through alive. Declared first so that it is
  /// released after them.
  std::shared_ptr<void> owner;
  /// In the order IToolJobContext::inputs() gives them; empty when the host
  /// could not make a copy.
  std::vector<ArtifactPtr> inputs;
};

/**
 * @brief What a running job is given by the host
 *
 * Valid only for the duration of the IStageToolJobHandler::run_tool_job()
 * call it was passed to. Call it from the job's own thread.
 */
class IToolJobContext {
 public:
  virtual ~IToolJobContext() = default;

  /// The node's inputs, in the order execute() receives them. Read them the
  /// way execute() would; they are shared with previews, which may be
  /// reading the same frames at the same time.
  virtual const std::vector<ArtifactPtr>& inputs() const = 0;

  /// Say how far the job has got: @p fraction from 0.0 to 1.0 (negative
  /// when unknown) and a status line for the user. Cheap enough to call per
  /// frame; the host passes on only as many reports as a view can show.
  virtual void report_progress(double fraction, const std::string& text) = 0;

  /// True once the user has cancelled the job or closed its view. Check it
  /// often (at least once per frame) and return soon after it turns true,
  /// with an ok == false reply; the host replies "Cancelled" in its place.
  virtual bool is_cancelled() const = 0;

  /**
   * @brief Open a private copy of the inputs, for one worker thread
   *
   * inputs() read through the stages and frame caches the graph holds, so
   * only one thread may read them at a time. A job that reads with several
   * threads opens one copy per extra thread: the host builds the node's
   * graph again and runs it up to the inputs, so each call costs what
   * opening the sources costs. Call it from the job's own thread, before
   * starting the worker that will read the copy; the copy may be read from
   * any one thread after that, and must be dropped before the job returns.
   *
   * @return the copy; inputs empty when the host cannot make one (then read
   *         inputs() from one thread)
   */
  virtual ToolJobPrivateInputs open_private_inputs() { return {}; }
};

/**
 * @brief Run long jobs for the plugin's view binary
 *
 * A stage whose view needs work too long for a request (a whole-capture
 * analysis, say) implements this beside IStageToolRequestHandler. The host
 * calls it once per IStageToolSession::run_job(), with the tool's contract
 * id, and hands the reply back to the view as it is. What the verbs and bytes
 * mean is up to the plugin.
 *
 * Threading: called on a job worker thread, on a stage object of its own,
 * built for the job from the project as it stood when the job started. It is
 * never the object that previews, requests and triggers use, so the job may
 * use the stage's members freely, but nothing it leaves there is seen by
 * anything else. Its parameters are the node's stored parameters. Several
 * jobs, for different nodes or views, can run at once.
 *
 * A job only computes: to change the node, return what should change and let
 * the view write it with IStageToolSession::apply_parameters().
 *
 * Exceptions thrown here are caught and become an ok == false reply.
 */
class IStageToolJobHandler {
 public:
  virtual ~IStageToolJobHandler() = default;

  virtual StageToolReply run_tool_job(const std::string& contract_id,
                                      const std::string& verb,
                                      const std::vector<uint8_t>& payload,
                                      IToolJobContext& context) = 0;
};

}  // namespace orc

#endif  // ORC_STAGE_TOOLING_STAGE_TOOL_JOBS_H
