/*
 * File:        baselines.cpp
 * Module:      orc-videosynth-support
 * Purpose:     The videosynth suite's recorded metric baselines and the
 *              verdict on a measured value against one
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "baselines.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace orc_videosynth {

const char* to_string(Better better) {
  return better == Better::Lower ? "lower" : "higher";
}

std::optional<Better> better_from_string(const std::string& text) {
  if (text == "lower") return Better::Lower;
  if (text == "higher") return Better::Higher;
  return std::nullopt;
}

const char* to_string(Verdict verdict) {
  switch (verdict) {
    case Verdict::Pass:
      return "pass";
    case Verdict::Improved:
      return "improved";
    case Verdict::Regressed:
      return "regressed";
    case Verdict::Missing:
    default:
      return "missing";
  }
}

Verdict judge(const std::optional<Baseline>& baseline, double value) {
  if (!baseline) return Verdict::Missing;
  // How far the value is better than the baseline; negative when worse.
  const double gain = baseline->better == Better::Lower
                          ? baseline->value - value
                          : value - baseline->value;
  if (gain < -baseline->tolerance) return Verdict::Regressed;
  if (gain > baseline->tolerance) return Verdict::Improved;
  return Verdict::Pass;
}

BaselineFile BaselineFile::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::ostringstream text;
  text << in.rdbuf();
  return parse(text.str(), path);
}

// JSON is read through yaml-cpp, which parses it as YAML flow style.
BaselineFile BaselineFile::parse(const std::string& text,
                                 const std::string& path) {
  YAML::Node root;
  try {
    root = YAML::Load(text);
  } catch (const YAML::Exception& e) {
    throw std::runtime_error("cannot parse " + path + ": " + e.what());
  }
  BaselineFile file;
  const auto tests = root["tests"];
  if (!tests || !tests.IsMap()) {
    throw std::runtime_error(path + " has no \"tests\" object");
  }
  for (const auto& test : tests) {
    const auto name = test.first.as<std::string>();
    if (!test.second.IsMap()) {
      throw std::runtime_error(path + ": test " + name + " is not an object");
    }
    auto& metrics = file.tests_[name];
    for (const auto& metric : test.second) {
      const auto metric_name = metric.first.as<std::string>();
      const auto& node = metric.second;
      const auto better =
          node["better"] ? better_from_string(node["better"].as<std::string>())
                         : std::nullopt;
      if (!node["value"] || !node["tolerance"] || !better) {
        throw std::runtime_error(path + ": " + name + "." + metric_name +
                                 " needs value, tolerance and better "
                                 "(\"lower\" or \"higher\")");
      }
      metrics[metric_name] = Baseline{node["value"].as<double>(),
                                      node["tolerance"].as<double>(), *better};
    }
  }
  return file;
}

std::optional<Baseline> BaselineFile::find(const std::string& test,
                                           const std::string& metric) const {
  const auto t = tests_.find(test);
  if (t == tests_.end()) return std::nullopt;
  const auto m = t->second.find(metric);
  if (m == t->second.end()) return std::nullopt;
  return m->second;
}

}  // namespace orc_videosynth
