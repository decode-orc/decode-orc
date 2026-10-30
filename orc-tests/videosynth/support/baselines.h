/*
 * File:        baselines.h
 * Module:      orc-videosynth-support
 * Purpose:     The videosynth suite's recorded metric baselines and the
 *              verdict on a measured value against one
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <map>
#include <optional>
#include <string>

namespace orc_videosynth {

// Which way a metric improves.
enum class Better { Lower, Higher };

const char* to_string(Better better);
std::optional<Better> better_from_string(const std::string& text);

// One recorded metric: the value, how far a run may fall short of it before
// that counts as a regression (in the metric's own units), and which way is
// better.
struct Baseline {
  double value = 0.0;
  double tolerance = 0.0;
  Better better = Better::Lower;
};

enum class Verdict {
  Pass,       // within tolerance of the baseline
  Improved,   // better than the baseline by more than the tolerance
  Regressed,  // worse than the baseline by more than the tolerance
  Missing,    // no baseline recorded
};

const char* to_string(Verdict verdict);

Verdict judge(const std::optional<Baseline>& baseline, double value);

// orc-tests/videosynth/baselines.json: per test, per metric, a Baseline.
//
//   { "tests": { "<Suite.Test>": { "<metric>":
//       { "value": 1.5, "tolerance": 0.05, "better": "lower" } } } }
class BaselineFile {
 public:
  // Throws std::runtime_error when the file cannot be read or does not have
  // that shape.
  static BaselineFile load(const std::string& path);

  // The same, from the file's text; |path| names it in errors.
  static BaselineFile parse(const std::string& text, const std::string& path);

  std::optional<Baseline> find(const std::string& test,
                               const std::string& metric) const;

 private:
  std::map<std::string, std::map<std::string, Baseline>> tests_;
};

}  // namespace orc_videosynth
