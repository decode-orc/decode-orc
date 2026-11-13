/*
 * File:        dag_sink_runner.h
 * Module:      orc-core
 * Purpose:     Trigger several sinks of one DAG, sharing everything upstream
 *              of them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/triggerable_stage.h>

#include <map>
#include <string>
#include <vector>

#include "dag_executor.h"

namespace orc {

struct SinkRunRequest {
  NodeID node_id;
  /// Values used in place of the node's own for this run only.
  std::map<std::string, ParameterValue> parameter_overrides;
  /// Optional; may be called from several sinks' threads at once.
  TriggerProgressCallback progress;
  /// Start the sink's observation context from a copy of the executor's,
  /// rather than empty.
  bool with_executor_observations = false;
};

struct SinkRunResult {
  NodeID node_id;
  bool success = false;
  /// The stage's trigger status, or why the sink could not run.
  std::string status;
};

/**
 * @brief Trigger the sinks of @p requests on @p dag.
 *
 * Every sink's inputs come from @p executor, whose cache gives each later
 * sink what an earlier one already executed, so every node upstream of the
 * sinks executes once. Sinks that share any upstream node run side by side,
 * each on its own thread; sinks with nothing upstream in common run one group
 * after another, in the order requested. Sinks that read the same node's
 * output read it through a SinkReadPacer (sink_read_pacer.h), which keeps
 * them close enough together that a frame one builds is still cached when
 * the next reads it.
 *
 * A sink that fails, throws or is cancelled stops holding back the others,
 * which carry on; its result says why. Each sink is triggered on its node's
 * own stage instance, so its results can be read from the DAG afterwards.
 *
 * @p executor must outlive every use of what the sinks read, which ends when
 * this returns. Not to be called twice at once on one @p executor.
 *
 * @return One result per request, in the order of @p requests
 */
std::vector<SinkRunResult> run_sinks(
    const DAG& dag, DAGExecutor& executor,
    const std::vector<SinkRunRequest>& requests);

}  // namespace orc
