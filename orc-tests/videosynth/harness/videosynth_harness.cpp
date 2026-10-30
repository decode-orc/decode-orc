/*
 * File:        videosynth_harness.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     What every videosynth suite test shares: where its cases were
 *              generated, and recording metrics against baselines.json
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "videosynth_harness.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace orc_videosynth {

namespace {

namespace fs = std::filesystem;

std::optional<std::string> env(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return std::nullopt;
  return std::string(value);
}

std::string current_test_name() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  if (info == nullptr) return {};
  return std::string(info->test_suite_name()) + "." + info->name();
}

// The notes the running test has added.
std::vector<std::string>& notes() {
  static std::vector<std::string> lines;
  return lines;
}

// A metric as judged, kept for the result file.
struct JudgedMetric {
  Metric metric;
  std::optional<Baseline> baseline;
  Verdict verdict = Verdict::Missing;
};

// The metrics the running test has recorded.
std::vector<JudgedMetric>& recorded() {
  static std::vector<JudgedMetric> metrics;
  return metrics;
}

std::string json_string(const std::string& text) {
  std::ostringstream out;
  out << '"';
  for (const char c : text) {
    switch (c) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(c) << std::dec;
        } else {
          out << c;
        }
    }
  }
  out << '"';
  return out.str();
}

// Shortest text that reads back as the same double, as Python's repr.
std::string json_number(double value) {
  if (!std::isfinite(value)) return "null";
  for (int precision = 6; precision <= 17; ++precision) {
    std::ostringstream out;
    out << std::setprecision(precision) << value;
    if (std::stod(out.str()) == value) return out.str();
  }
  std::ostringstream out;
  out << std::setprecision(17) << value;
  return out.str();
}

// The first line of a failure or skip message that says something:
// ADD_FAILURE() opens its message with a bare "Failed".
std::string first_line(const std::string& text) {
  std::string rest = text;
  if (rest.rfind("Failed\n", 0) == 0) rest.erase(0, 7);
  const auto end = rest.find('\n');
  return end == std::string::npos ? rest : rest.substr(0, end);
}

class ResultWriter : public ::testing::EmptyTestEventListener {
 public:
  explicit ResultWriter(fs::path directory)
      : directory_(std::move(directory)) {}

  void OnTestStart(const ::testing::TestInfo& /*info*/) override {
    recorded().clear();
    notes().clear();
  }

  void OnTestEnd(const ::testing::TestInfo& info) override {
    const auto* result = info.result();
    std::string status = "passed";
    std::string reason;
    if (result->Skipped()) {
      status = "skipped";
    } else if (result->Failed()) {
      status = "failed";
    }
    for (int i = 0; i < result->total_part_count(); ++i) {
      const auto& part = result->GetTestPartResult(i);
      if ((status == "skipped" && part.skipped()) ||
          (status == "failed" && part.failed())) {
        reason = first_line(part.message());
        break;
      }
    }

    const std::string name =
        std::string(info.test_suite_name()) + "." + info.name();
    std::ostringstream out;
    out << "{\n  \"test\": " << json_string(name)
        << ",\n  \"status\": " << json_string(status)
        << ",\n  \"reason\": " << json_string(reason)
        << ",\n  \"elapsed_ms\": " << result->elapsed_time()
        << ",\n  \"cases\": "
        << json_string(env("ORC_VIDEOSYNTH_CASES").value_or(""))
        << ",\n  \"notes\": [";
    for (size_t i = 0; i < notes().size(); ++i) {
      out << (i == 0 ? "" : ", ") << json_string(notes()[i]);
    }
    out << "],\n  \"metrics\": [";
    const auto& metrics = recorded();
    for (size_t i = 0; i < metrics.size(); ++i) {
      const auto& m = metrics[i];
      out << (i == 0 ? "\n" : ",\n")
          << "    {\"name\": " << json_string(m.metric.name)
          << ", \"value\": " << json_number(m.metric.value)
          << ", \"better\": " << json_string(to_string(m.metric.better))
          << ", \"new_tolerance\": " << json_number(m.metric.new_tolerance)
          << ", \"verdict\": " << json_string(to_string(m.verdict));
      if (m.baseline) {
        out << ", \"baseline\": " << json_number(m.baseline->value)
            << ", \"tolerance\": " << json_number(m.baseline->tolerance);
      }
      out << "}";
    }
    out << (metrics.empty() ? "]\n}\n" : "\n  ]\n}\n");

    std::error_code ec;
    fs::create_directories(directory_, ec);
    std::ofstream file(directory_ / (name + ".json"));
    file << out.str();
    recorded().clear();
    notes().clear();
  }

 private:
  fs::path directory_;
};

}  // namespace

std::optional<fs::path> case_dir(const std::string& case_name) {
  const auto root = env("ORC_VIDEOSYNTH_OUTPUT_DIR");
  if (!root) return std::nullopt;
  return fs::path(*root) / case_name;
}

double new_tolerance(double value, double relative, double absolute) {
  return std::max(std::abs(value) * relative, absolute);
}

bool updating_baselines() {
  return env("ORC_VIDEOSYNTH_UPDATE_BASELINES").value_or("") == "1";
}

const BaselineFile* baselines() {
  static std::unique_ptr<BaselineFile> file;
  static std::string error;
  if (!file && error.empty()) {
    const auto path = env("ORC_VIDEOSYNTH_BASELINES");
    if (!path) {
      error = "ORC_VIDEOSYNTH_BASELINES is not set; run through ctest";
    } else {
      try {
        file = std::make_unique<BaselineFile>(BaselineFile::load(*path));
      } catch (const std::exception& e) {
        error = e.what();
      }
    }
  }
  if (!file) ADD_FAILURE() << error;
  return file.get();
}

void expect_metrics(const std::vector<Metric>& metrics) {
  const BaselineFile* file = baselines();
  if (file == nullptr) return;
  const std::string test = current_test_name();
  const bool updating = updating_baselines();
  for (const auto& metric : metrics) {
    JudgedMetric judged{metric, file->find(test, metric.name),
                        Verdict::Missing};
    judged.verdict = judge(judged.baseline, metric.value);
    recorded().push_back(judged);
    if (updating) continue;
    if (judged.verdict == Verdict::Missing) {
      ADD_FAILURE() << test << ": " << metric.name << " = " << metric.value
                    << " has no baseline; record it with "
                       "ORC_VIDEOSYNTH_UPDATE_BASELINES=1 (TESTING.md)";
    } else if (judged.verdict == Verdict::Regressed) {
      ADD_FAILURE() << test << ": " << metric.name
                    << " regressed: " << metric.value
                    << " against a baseline of " << judged.baseline->value
                    << " +/- " << judged.baseline->tolerance << " ("
                    << to_string(metric.better) << " is better)";
    }
  }
}

void note(const std::string& text) { notes().push_back(text); }

void install_result_writer() {
  const auto root = env("ORC_VIDEOSYNTH_OUTPUT_DIR");
  if (!root) return;
  ::testing::UnitTest::GetInstance()->listeners().Append(
      new ResultWriter(fs::path(*root) / "results"));
}

}  // namespace orc_videosynth
