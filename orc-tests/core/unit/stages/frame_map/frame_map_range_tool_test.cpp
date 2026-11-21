/*
 * File:        frame_map_range_tool_test.cpp
 * Module:      orc-core-tests
 * Purpose:     The Frame Map's Range Finder: reading addresses, and runs on
 *              a fake job context over frames carrying picture numbers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../../orc/plugins/stages/frame_map/frame_map_range_tool.h"

#include <gtest/gtest.h>
#include <orc/stage/tooling/stage_analysis_tool.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "../../include/fake_tool_job_context.h"
#include "../../include/pal_picture_number_source.h"

namespace orc_unit_test {
namespace {

using Status = orc::StageAnalysisReport::Status;
using orc::frame_map_range_tool::parse_address;

std::vector<int32_t> pictures(int32_t first, int32_t last) {
  std::vector<int32_t> out;
  for (int32_t pn = first; pn <= last; ++pn) out.push_back(pn);
  return out;
}

orc::StageAnalysisReport find(std::vector<int32_t> picture_numbers,
                              const std::string& start, const std::string& end,
                              bool cancelled = false) {
  FakeToolJobContext context(
      {std::make_shared<PalPictureNumberSource>(std::move(picture_numbers))});
  context.cancelled = cancelled;
  return orc::frame_map_range_tool::run(
      {{"startAddress", orc::ParameterValue{start}},
       {"endAddress", orc::ParameterValue{end}}},
      context);
}

std::string ranges_of(const orc::StageAnalysisReport& report) {
  const auto it = report.parameter_changes.find("ranges");
  return it == report.parameter_changes.end()
             ? std::string("(none)")
             : std::get<std::string>(it->second);
}

}  // namespace

TEST(FrameMapRangeTool, ReadsPictureNumbers) {
  const auto pn = parse_address(" 12345 ", true);
  ASSERT_TRUE(pn.ok);
  EXPECT_FALSE(pn.is_timecode);
  EXPECT_EQ(pn.picture_number, 12345);
  EXPECT_EQ(pn.normalized, "12345");

  EXPECT_FALSE(parse_address("0", true).ok);
  EXPECT_EQ(parse_address("", true).error, "Address is empty");
  EXPECT_EQ(parse_address("12a", true).error, "Invalid picture number: 12a");
}

TEST(FrameMapRangeTool, ReadsTimeCodesAtTheProjectsRate) {
  EXPECT_EQ(parse_address("0:0:0.0", true).picture_number, 1);
  EXPECT_EQ(parse_address("0:0:1.0", true).picture_number, 26);
  EXPECT_EQ(parse_address("0:0:1.0", false).picture_number, 31);
  EXPECT_EQ(parse_address("0:1:0;2", true).picture_number, 25 * 60 + 3);
  const auto four_part = parse_address("1:2:3:4", false);
  ASSERT_TRUE(four_part.ok);
  EXPECT_TRUE(four_part.is_timecode);
  EXPECT_EQ(four_part.picture_number, ((3600 + 120 + 3) * 30) + 4 + 1);
  EXPECT_EQ(four_part.normalized, "1:2:3.4");

  EXPECT_EQ(parse_address("0:0:1", true).error,
            "Timecode must include picture component (e.g., 0:0:0.0)");
  EXPECT_EQ(parse_address("0:x:1.0", true).error,
            "Invalid timecode format: 0:x:1.0");
}

TEST(FrameMapRangeTool, NeedsBothAddresses) {
  const auto report = find(pictures(1, 10), "3", "");
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary, "Start and end addresses are required.");
}

TEST(FrameMapRangeTool, NeedsAVideoInput) {
  FakeToolJobContext context({std::make_shared<NotVideoArtifact>()});
  const auto report = orc::frame_map_range_tool::run(
      {{"startAddress", orc::ParameterValue{std::string("1")}},
       {"endAddress", orc::ParameterValue{std::string("2")}}},
      context);
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary,
            "Input node did not produce VideoFrameRepresentation");
}

TEST(FrameMapRangeTool, FindsTheFramesBetweenTwoPictureNumbers) {
  const auto report = find(pictures(101, 140), "105", "110");
  ASSERT_EQ(report.status, Status::Success) << report.summary;
  EXPECT_EQ(ranges_of(report), "4-9");
  EXPECT_NE(report.summary.find("Frame range: 5-10"), std::string::npos)
      << "shown 1-based:\n"
      << report.summary;
}

TEST(FrameMapRangeTool, FindsAPictureOutOfOrderByScanning) {
  const auto report = find({1, 2, 3, 8, 9, 10, 4, 5, 6, 7}, "5", "5");
  ASSERT_EQ(report.status, Status::Success) << report.summary;
  EXPECT_EQ(ranges_of(report), "7-7");
}

TEST(FrameMapRangeTool, SaysWhichAddressItCouldNotFind) {
  auto report = find(pictures(1, 20), "50", "60");
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary, "Start address not found in source.");

  report = find(pictures(1, 20), "5", "60");
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary, "End address not found after start address.");
  EXPECT_TRUE(report.parameter_changes.empty());
}

TEST(FrameMapRangeTool, ACancelledRunIsCancelled) {
  const auto report = find(pictures(1, 20), "5", "9", /*cancelled=*/true);
  EXPECT_EQ(report.status, Status::Cancelled);
}

}  // namespace orc_unit_test
