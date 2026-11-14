/*
 * File:        stage_results_report.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     A sink's results in a form the host can print without knowing
 *              the stage: summary figures and one row per analysed frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_TOOLING_STAGE_RESULTS_REPORT_H
#define ORC_STAGE_TOOLING_STAGE_RESULTS_REPORT_H

// SDK TIER: stage/tooling — stage contract type crossing the plugin boundary.
// A layout change here bumps the host ABI version.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace orc {

/// One figure of a results report: a whole number, a real number, or nothing
/// (a frame the measurement did not reach, say).
struct StageReportValue {
  enum class Type { Absent, Integer, Real };

  Type type = Type::Absent;
  int64_t integer = 0;
  double real = 0.0;

  static StageReportValue of_integer(int64_t value) {
    StageReportValue v;
    v.type = Type::Integer;
    v.integer = value;
    return v;
  }
  static StageReportValue of_real(double value) {
    StageReportValue v;
    v.type = Type::Real;
    v.real = value;
    return v;
  }
  static StageReportValue absent() { return StageReportValue{}; }
};

/// What a stage's last trigger found. orc-cli prints it, unchanged, as the
/// `statistics` and `results` of `tools run --json`.
struct StageResultsReport {
  /// A short name for what the rows measure, e.g. "dropout".
  std::string kind;
  /// Frames in the analysed source.
  int64_t total_frames = 0;
  /// Summary figures by name, e.g. "mean_white_snr".
  std::map<std::string, StageReportValue> statistics;
  /// The name of each column of @ref rows, e.g. {"frame", "white_snr"}.
  std::vector<std::string> columns;
  /// One row per analysed frame, one value per column.
  std::vector<std::vector<StageReportValue>> rows;
};

/**
 * @brief Report what the stage's last trigger found
 *
 * A sink whose tool is a StageToolKind::BatchAnalysis implements this, so
 * that `orc-cli tools run` can trigger it and print its results without
 * knowing the stage. The host reaches it by dynamic_cast on the stage.
 *
 * Threading: called after the trigger has returned, never during one.
 */
class IStageResultsReportProvider {
 public:
  virtual ~IStageResultsReportProvider() = default;

  /// Fill @p report from the last trigger. Return false when there are no
  /// results (the stage has not been triggered, or its trigger failed).
  virtual bool results_report(StageResultsReport* report) const = 0;
};

}  // namespace orc

#endif  // ORC_STAGE_TOOLING_STAGE_RESULTS_REPORT_H
