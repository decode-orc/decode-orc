/*
 * File:        analysis_report_json.h
 * Module:      orc-cli
 * Purpose:     JSON form of an analysis tool's statistics and result items
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_CLI_ANALYSIS_REPORT_JSON_H
#define ORC_CLI_ANALYSIS_REPORT_JSON_H

#include <orc_analysis.h>

#include <string>

#include "json_writer.h"

namespace orc {
namespace cli {

/// A double in JSON number syntax, or empty when it is not finite.
std::string number_token(double value);

/// A double member; null when it is not finite, which JSON cannot hold.
void write_double_json(JsonWriter* json, const std::string& name, double value);

/// A statistic member in the JSON type its value holds.
void write_statistic_json(JsonWriter* json, const std::string& name,
                          const orc::StatisticValue& value);

/// One element of a report's `items` array. The item's metadata, when it has
/// any, is written as a `data` object of statistics; an item without
/// metadata keeps the shape it always had.
void write_analysis_item_json(JsonWriter* json,
                              const orc::AnalysisResultItem& item);

}  // namespace cli
}  // namespace orc

#endif  // ORC_CLI_ANALYSIS_REPORT_JSON_H
