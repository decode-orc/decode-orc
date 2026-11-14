/*
 * File:        analysis_report_statistics.h
 * Module:      decode-orc stage plugins
 * Purpose:     The mean, minimum and maximum the analysis sinks put in their
 *              results reports
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/tooling/stage_results_report.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace orc::analysis_report {

/// Add mean_<name>, min_<name> and max_<name> of @p values; nothing when
/// there are none.
inline void add_series_statistics(
    std::map<std::string, StageReportValue>* statistics,
    const std::string& name, const std::vector<double>& values) {
  if (values.empty()) {
    return;
  }
  double sum = 0.0;
  for (const double v : values) {
    sum += v;
  }
  (*statistics)["mean_" + name] =
      StageReportValue::of_real(sum / static_cast<double>(values.size()));
  (*statistics)["min_" + name] = StageReportValue::of_real(
      *std::min_element(values.begin(), values.end()));
  (*statistics)["max_" + name] = StageReportValue::of_real(
      *std::max_element(values.begin(), values.end()));
}

}  // namespace orc::analysis_report
