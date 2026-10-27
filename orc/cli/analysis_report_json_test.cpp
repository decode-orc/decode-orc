/*
 * File:        analysis_report_json_test.cpp
 * Module:      orc-cli
 * Purpose:     Unit tests for the JSON form of analysis result items
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "analysis_report_json.h"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <string>

namespace {

using orc::AnalysisResultItem;
using orc::cli::JsonWriter;

std::string written(const AnalysisResultItem& item) {
  std::ostringstream out;
  JsonWriter json(out);
  orc::cli::write_analysis_item_json(&json, item);
  json.finish();
  return out.str();
}

// An item with no metadata keeps the shape every report had before items
// carried data, so a script that reads them is undisturbed.
TEST(AnalysisReportJsonTest, AnItemWithoutMetadataHasNoData) {
  AnalysisResultItem item;
  item.type = "warning";
  item.message = "Source 3 is soft";
  item.startFrame = 4;
  EXPECT_EQ(written(item),
            "{\n"
            "  \"type\": \"warning\",\n"
            "  \"message\": \"Source 3 is soft\",\n"
            "  \"start_frame\": 4,\n"
            "  \"end_frame\": null\n"
            "}\n");
}

// Metadata is written as `data`, each value in the JSON type it holds.
TEST(AnalysisReportJsonTest, AnItemsMetadataIsWrittenAsData) {
  AnalysisResultItem item;
  item.type = "info";
  item.message = "Source 1: noise σ 8.6 codes";
  item.metadata["source"] = 1;
  item.metadata["compared"] = true;
  item.metadata["noise_sigma"] = 8.625;
  item.metadata["stamper_group"] = std::string("A");
  item.metadata["stack_sigma_without"] = std::nan("");
  EXPECT_EQ(written(item),
            "{\n"
            "  \"type\": \"info\",\n"
            "  \"message\": \"Source 1: noise σ 8.6 codes\",\n"
            "  \"start_frame\": null,\n"
            "  \"end_frame\": null,\n"
            "  \"data\": {\n"
            "    \"compared\": true,\n"
            "    \"noise_sigma\": 8.625,\n"
            "    \"source\": 1,\n"
            "    \"stack_sigma_without\": null,\n"
            "    \"stamper_group\": \"A\"\n"
            "  }\n"
            "}\n");
}

}  // namespace
