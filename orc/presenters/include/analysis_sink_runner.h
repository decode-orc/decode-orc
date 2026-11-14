/*
 * File:        analysis_sink_runner.h
 * Module:      orc-presenters
 * Purpose:     Trigger an analysis sink and keep its per-frame results
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/tooling/stage_results_report.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace orc {
namespace presenters {

/// The results of one analysis sink run.
struct AnalysisSinkRun {
  bool success = false;
  bool cancelled = false;
  /// The stage's trigger status, or why the run could not start.
  std::string status;
  /// What the stage reported (orc::IStageResultsReportProvider); empty when
  /// the run failed or the stage reports nothing.
  std::optional<orc::StageResultsReport> report;
};

/**
 * @brief Trigger an analysis sink and return its per-frame results
 *
 * Runs the sink the way `--process` does, but keeps the stage instance so
 * its results report can be read afterwards. The project is not modified.
 *
 * @param project_handle Opaque project handle
 *        (ProjectPresenter::getCoreProjectHandle())
 * @param node_id The sink node
 * @param parameter_overrides Values used in place of the stored ones for this
 *        run only (for example a CSV path)
 * @param progress_callback Receives (percentage, status) updates
 * @param cancel_check Polled while the sink runs; true asks it to stop
 */
AnalysisSinkRun runAnalysisSink(
    void* project_handle, NodeID node_id,
    const std::map<std::string, ParameterValue>& parameter_overrides,
    const std::function<void(int, const std::string&)>& progress_callback,
    const std::function<bool()>& cancel_check);

/// One sink of a runAnalysisSinks() call.
struct AnalysisSinkRequest {
  NodeID node_id;
  /// Values used in place of the stored ones for this run only.
  std::map<std::string, ParameterValue> parameter_overrides;
};

/**
 * @brief Trigger several analysis sinks on one read of their inputs
 *
 * As runAnalysisSink() for each sink, except that the pipeline is built and
 * executed once: sinks reading the same capture run side by side and share
 * its reads (see orc::run_sinks()).
 *
 * @param progress_callback Receives (sink index, percentage, status); may be
 *        called from several sinks' threads, one call at a time
 * @param cancel_check Polled while the sinks run; true asks them all to stop
 * @return One run per request, in order
 */
std::vector<AnalysisSinkRun> runAnalysisSinks(
    void* project_handle, const std::vector<AnalysisSinkRequest>& requests,
    const std::function<void(size_t, int, const std::string&)>&
        progress_callback,
    const std::function<bool()>& cancel_check);

}  // namespace presenters
}  // namespace orc
