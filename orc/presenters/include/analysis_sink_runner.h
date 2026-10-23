/*
 * File:        analysis_sink_runner.h
 * Module:      orc-presenters
 * Purpose:     Trigger an analysis sink and keep its per-frame results
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/common_types.h>
#include <orc/stage/node_id.h>
#include <orc/stage/params/parameter_types.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace orc {
namespace presenters {

/// Which of the analysis sink result sets a run produced.
enum class AnalysisSinkKind { None, Dropout, SNR, BurstLevel };

/// The full-resolution results of one analysis sink run.
struct AnalysisSinkRun {
  bool success = false;
  bool cancelled = false;
  /// The stage's trigger status, or why the run could not start.
  std::string status;
  AnalysisSinkKind kind = AnalysisSinkKind::None;
  int32_t total_frames = 0;
  /// One entry per analysed frame; only the set matching @c kind is filled.
  std::vector<orc::FrameDropoutStats> dropout;
  std::vector<orc::FrameSNRStats> snr;
  std::vector<orc::FrameBurstLevelStats> burst_level;
};

/**
 * @brief Trigger an analysis sink and return its per-frame results
 *
 * Runs the sink the way `--process` does, but keeps the stage instance so
 * its results can be read afterwards. The project is not modified.
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

}  // namespace presenters
}  // namespace orc
