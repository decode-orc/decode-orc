/*
 * File:        dropout_analysis_sink_report_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the per-dropout detail report (issue #214,
 *              plan Phase 4): line/sample derivation and stream-based report
 *              writers (CSV and text), all filesystem-free.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <sstream>
#include <string>
#include <vector>

#include "../../../../orc/plugins/stages/dropout_analysis_sink/dropout_analysis_sink_deps.h"
#include "../../mocks/mock_video_frame_representation.h"

using ::testing::NiceMock;
using ::testing::Return;

namespace orc_unit_test {
namespace {

// Single-frame source mock with the given nominal samples-per-line and system,
// returning `runs` as its dropout hints. No active-video range is set, so all
// runs are counted in FULL_FIELD mode.
std::shared_ptr<NiceMock<MockVideoFrameRepresentation>>
make_single_frame_source(orc::VideoSystem system, int32_t nominal_spl,
                         int32_t height, std::vector<orc::DropoutRun> runs) {
  orc::SourceParameters params;
  params.system = system;
  params.frame_width_nominal = nominal_spl;
  params.frame_height = height;

  orc::FrameDescriptor desc;
  desc.frame_id = 0;
  desc.system = system;
  desc.height = height;
  desc.samples_total =
      static_cast<uint64_t>(height) * static_cast<uint64_t>(nominal_spl);
  desc.samples_per_line_nominal = nominal_spl;

  auto source = std::make_shared<NiceMock<MockVideoFrameRepresentation>>();
  ON_CALL(*source, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0u, 0u}));
  ON_CALL(*source, frame_count()).WillByDefault(Return(1u));
  ON_CALL(*source, get_frame_descriptor(orc::FrameID{0}))
      .WillByDefault(Return(desc));
  ON_CALL(*source, get_video_parameters()).WillByDefault(Return(params));
  ON_CALL(*source, get_dropout_hints(orc::FrameID{0}))
      .WillByDefault(Return(std::move(runs)));
  return source;
}

orc::DropoutAnalysisComputeOptions detail_options() {
  orc::DropoutAnalysisComputeOptions opts;
  opts.collect_detail = true;
  return opts;
}

}  // namespace

// NTSC nominal geometry is 910 samples/line. A run starting at flat offset
// (line 10 * 910) + 100 with 40 samples must resolve to line 10, samples
// 100-139 inclusive.
TEST(DropoutAnalysisSinkReportTest, DetailDerivation_Ntsc910) {
  const uint64_t start = 10u * 910u + 100u;
  auto rep = make_single_frame_source(orc::VideoSystem::NTSC, 910, 525,
                                      {orc::DropoutRun{0u, start, 40u, 128}});

  orc::ObservationContext ctx;
  orc::DropoutAnalysisSinkStageDeps deps;
  deps.init({}, nullptr);
  const auto result =
      deps.compute_and_analyze(rep.get(), ctx, detail_options());

  ASSERT_EQ(result.detail_records.size(), 1u);
  const auto& rec = result.detail_records[0];
  EXPECT_EQ(rec.frame_number, 1);
  EXPECT_EQ(rec.line_number, 10);
  EXPECT_EQ(rec.sample_start, 100);
  EXPECT_EQ(rec.sample_end, 139);
  EXPECT_EQ(rec.length_samples, 40);
}

// PAL nominal geometry is 1135 samples/line. A run at (line 100 * 1135) + 50
// with 25 samples resolves to line 100, samples 50-74 inclusive.
TEST(DropoutAnalysisSinkReportTest, DetailDerivation_Pal1135) {
  const uint64_t start = 100u * 1135u + 50u;
  auto rep = make_single_frame_source(orc::VideoSystem::PAL, 1135, 625,
                                      {orc::DropoutRun{0u, start, 25u, 128}});

  orc::ObservationContext ctx;
  orc::DropoutAnalysisSinkStageDeps deps;
  deps.init({}, nullptr);
  const auto result =
      deps.compute_and_analyze(rep.get(), ctx, detail_options());

  ASSERT_EQ(result.detail_records.size(), 1u);
  const auto& rec = result.detail_records[0];
  EXPECT_EQ(rec.line_number, 100);
  EXPECT_EQ(rec.sample_start, 50);
  EXPECT_EQ(rec.sample_end, 74);
  EXPECT_EQ(rec.length_samples, 25);
}

// PAL frame lines 312 and 624 carry 2 extra samples, so frame line k ≥ 313
// starts at k × 1135 + 2 (issue #370). Each case is {flat offset, line,
// sample}.
TEST(DropoutAnalysisSinkReportTest, DetailDerivation_PalExtraLineSamples) {
  struct Case {
    uint64_t start;
    int32_t line;
    int32_t sample;
  };
  const Case cases[] = {
      {400u * 1135u + 2u + 500u, 400, 500},    // field 2 mid-line
      {313u * 1135u + 2u, 313, 0},             // first sample of field 2
      {313u * 1135u + 1u, 312, 1136},          // line 312's second extra sample
      {624u * 1135u + 2u + 1136u, 624, 1136},  // line 624's last extra sample
  };
  for (const auto& c : cases) {
    auto rep =
        make_single_frame_source(orc::VideoSystem::PAL, 1135, 625,
                                 {orc::DropoutRun{0u, c.start, 1u, 128}});

    orc::ObservationContext ctx;
    orc::DropoutAnalysisSinkStageDeps deps;
    deps.init({}, nullptr);
    const auto result =
        deps.compute_and_analyze(rep.get(), ctx, detail_options());

    ASSERT_EQ(result.detail_records.size(), 1u) << "offset " << c.start;
    EXPECT_EQ(result.detail_records[0].line_number, c.line)
        << "offset " << c.start;
    EXPECT_EQ(result.detail_records[0].sample_start, c.sample)
        << "offset " << c.start;
  }
}

// VISIBLE_AREA filters on the corrected PAL sample: a field 2 run that ends
// just before active video starts is excluded, where the old k × 1135 maths
// put it 2 samples later and let it overlap active video.
TEST(DropoutAnalysisSinkReportTest, VisibleArea_PalField2UsesCorrectedSample) {
  orc::SourceParameters params;
  params.system = orc::VideoSystem::PAL;
  params.frame_width_nominal = 1135;
  params.frame_height = 625;
  params.first_active_frame_line = 22;
  params.last_active_frame_line = 620;
  params.active_video_start = 185;
  params.active_video_end = 1107;

  // Line 400, samples 183-184: entirely before active video.
  const uint64_t start = 400u * 1135u + 2u + 183u;
  auto rep = make_single_frame_source(orc::VideoSystem::PAL, 1135, 625,
                                      {orc::DropoutRun{0u, start, 2u, 128}});
  ON_CALL(*rep, get_video_parameters()).WillByDefault(Return(params));

  orc::DropoutAnalysisComputeOptions opts = detail_options();
  opts.mode = orc::DropoutAnalysisMode::VISIBLE_AREA;

  orc::ObservationContext ctx;
  orc::DropoutAnalysisSinkStageDeps deps;
  deps.init({}, nullptr);
  const auto result = deps.compute_and_analyze(rep.get(), ctx, opts);

  ASSERT_EQ(result.frame_stats.size(), 1u);
  EXPECT_EQ(result.frame_stats[0].dropout_count, 0);
  EXPECT_TRUE(result.detail_records.empty());
}

// A run whose start sample plus length crosses the nominal line width is
// reported against its start line with a sample_end that extends past the line
// boundary — the run is not split. NTSC line 5, sample 900, length 30 →
// samples 900-929 (nominal line width is 910).
TEST(DropoutAnalysisSinkReportTest, DetailDerivation_RunSpanningLineBoundary) {
  const uint64_t start = 5u * 910u + 900u;
  auto rep = make_single_frame_source(orc::VideoSystem::NTSC, 910, 525,
                                      {orc::DropoutRun{0u, start, 30u, 128}});

  orc::ObservationContext ctx;
  orc::DropoutAnalysisSinkStageDeps deps;
  deps.init({}, nullptr);
  const auto result =
      deps.compute_and_analyze(rep.get(), ctx, detail_options());

  ASSERT_EQ(result.detail_records.size(), 1u);
  const auto& rec = result.detail_records[0];
  EXPECT_EQ(rec.line_number, 5);
  EXPECT_EQ(rec.sample_start, 900);
  EXPECT_EQ(rec.sample_end, 929);
  EXPECT_EQ(rec.length_samples, 30);
}

// Detail collection is gated: with collect_detail unset, no records accrue even
// though per-frame stats are still produced.
TEST(DropoutAnalysisSinkReportTest, DetailNotCollectedWhenDisabled) {
  auto rep = make_single_frame_source(orc::VideoSystem::NTSC, 910, 525,
                                      {orc::DropoutRun{0u, 100u, 40u, 128}});

  orc::ObservationContext ctx;
  orc::DropoutAnalysisSinkStageDeps deps;
  deps.init({}, nullptr);
  const auto result = deps.compute_and_analyze(rep.get(), ctx, {});

  EXPECT_TRUE(result.detail_records.empty());
  ASSERT_EQ(result.frame_stats.size(), 1u);
  EXPECT_EQ(result.frame_stats[0].dropout_count, 1);
}

// CSV report: self-describing header plus one row per dropout run.
TEST(DropoutAnalysisSinkReportTest, CsvReportHeaderAndRows) {
  orc::DropoutAnalysisSinkStageDeps deps;
  std::vector<orc::DropoutDetailRecord> records{
      {1, 10, 100, 139, 40},
      {1, 20, 5, 14, 10},
      {3, 0, 50, 69, 20},
  };

  std::ostringstream os;
  deps.write_report_csv(os, records);
  EXPECT_EQ(os.str(),
            "frame_number,line_number,sample_start,sample_end,length_samples\n"
            "1,10,100,139,40\n"
            "1,20,5,14,10\n"
            "3,0,50,69,20\n");
}

TEST(DropoutAnalysisSinkReportTest, CsvReportEmptyIsHeaderOnly) {
  orc::DropoutAnalysisSinkStageDeps deps;
  std::ostringstream os;
  deps.write_report_csv(os, {});
  EXPECT_EQ(
      os.str(),
      "frame_number,line_number,sample_start,sample_end,length_samples\n");
}

// Text report: grouped by frame with a heading (count + total length) then one
// line per run. Frames without dropouts never appear (no records exist for
// them). Singular/plural of "dropout" is handled.
TEST(DropoutAnalysisSinkReportTest, TextReportGroupsByFrame) {
  orc::DropoutAnalysisSinkStageDeps deps;
  std::vector<orc::DropoutDetailRecord> records{
      {1, 10, 100, 139, 40},
      {1, 20, 5, 14, 10},
      {3, 0, 50, 69, 20},
  };

  std::ostringstream os;
  deps.write_report_text(os, records);
  EXPECT_EQ(os.str(),
            "Frame 1: 2 dropouts, 50 samples total\n"
            "  line 10, samples 100-139 (40 samples)\n"
            "  line 20, samples 5-14 (10 samples)\n"
            "Frame 3: 1 dropout, 20 samples total\n"
            "  line 0, samples 50-69 (20 samples)\n");
}

}  // namespace orc_unit_test
