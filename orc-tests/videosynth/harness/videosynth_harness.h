/*
 * File:        videosynth_harness.h
 * Module:      orc-videosynth-tests
 * Purpose:     What every videosynth suite test shares: where its cases were
 *              generated, and recording metrics against baselines.json
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <baselines.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace orc_videosynth {

// The directory a case was generated into, from ORC_VIDEOSYNTH_OUTPUT_DIR,
// which orc_videosynth_tests() sets. nullopt outside ctest.
std::optional<std::filesystem::path> case_dir(const std::string& case_name);

// One measured value of a test.
struct Metric {
  std::string name;
  double value = 0.0;
  Better better = Better::Lower;
  // The tolerance a new baseline is given when
  // ORC_VIDEOSYNTH_UPDATE_BASELINES=1 records it. A recorded tolerance is
  // kept: it is the reviewed one.
  double new_tolerance = 0.0;
};

// A tolerance for a new baseline: |relative| of the value, but at least
// |absolute|.
double new_tolerance(double value, double relative, double absolute);

// True when ORC_VIDEOSYNTH_UPDATE_BASELINES=1: metrics are recorded for the
// report to write into baselines.json, and do not fail the test.
bool updating_baselines();

// The committed baselines (ORC_VIDEOSYNTH_BASELINES); fails the test and
// returns nullptr when they cannot be read.
const BaselineFile* baselines();

// Judges each metric against the running test's baselines and records it for
// the run report. A regression, or a metric with no baseline, fails the test
// unless the baselines are being updated.
void expect_metrics(const std::vector<Metric>& metrics);

// Adds a line to the running test's entry in the run report: a known
// deviation, or anything else a reader of the report needs to know.
void note(const std::string& text);

// Installs the listener that writes each test's result into
// <ORC_VIDEOSYNTH_OUTPUT_DIR>/results/ for the run report.
void install_result_writer();

}  // namespace orc_videosynth
