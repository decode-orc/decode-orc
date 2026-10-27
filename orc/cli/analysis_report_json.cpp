/*
 * File:        analysis_report_json.cpp
 * Module:      orc-cli
 * Purpose:     JSON form of an analysis tool's statistics and result items
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "analysis_report_json.h"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <variant>

namespace orc {
namespace cli {

namespace {

/// StatisticValue's wide integer alternative.
using StatisticInt = long long;  // NOLINT(google-runtime-int)

}  // namespace

std::string number_token(double value) {
  if (!std::isfinite(value)) {
    return {};
  }
  std::ostringstream out;
  out << std::setprecision(10) << value;
  return out.str();
}

void write_double_json(JsonWriter* json, const std::string& name,
                       double value) {
  const std::string token = number_token(value);
  if (token.empty()) {
    json->member_null(name);
  } else {
    json->member_number(name, token);
  }
}

void write_statistic_json(JsonWriter* json, const std::string& name,
                          const orc::StatisticValue& value) {
  if (const auto* v = std::get_if<bool>(&value)) {
    json->member_bool(name, *v);
  } else if (const auto* v = std::get_if<int>(&value)) {
    json->member_int(name, *v);
  } else if (const auto* v = std::get_if<StatisticInt>(&value)) {
    json->member_int(name, *v);
  } else if (const auto* v = std::get_if<double>(&value)) {
    write_double_json(json, name, *v);
  } else {
    json->member(name, std::get<std::string>(value));
  }
}

void write_analysis_item_json(JsonWriter* json,
                              const orc::AnalysisResultItem& item) {
  json->begin_object();
  json->member("type", item.type);
  json->member("message", item.message);
  if (item.startFrame >= 0) {
    json->member_int("start_frame", item.startFrame);
  } else {
    json->member_null("start_frame");
  }
  if (item.endFrame >= 0) {
    json->member_int("end_frame", item.endFrame);
  } else {
    json->member_null("end_frame");
  }
  if (!item.metadata.empty()) {
    json->key("data");
    json->begin_object();
    for (const auto& [name, value] : item.metadata) {
      write_statistic_json(json, name, value);
    }
    json->end_object();
  }
  json->end_object();
}

}  // namespace cli
}  // namespace orc
