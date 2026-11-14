/*
 * File:        stage_analysis_tools_test.cpp
 * Module:      orc-core-tests
 * Purpose:     A stage's analysis tools as the host runs them: finding one
 *              by contract, the run, what applying it writes, the refresh
 *              option, and the view-tier forms of parameters and reports
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/presenters/include/stage_analysis_tools.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <variant>
#include <vector>

#include "../../../../orc/core/include/project.h"
#include "../../../../orc/presenters/include/analysis_presenter.h"
#include "../../../../orc/presenters/include/analysis_tool_presenter.h"
#include "../../../../orc/presenters/include/tool_refresh.h"
#include "../../../../orc/presenters/include/tool_running.h"

namespace orc::presenters::test {

namespace {

constexpr char kContract[] = "decode-orc.stage-tools.fm-beat-calibration.v1";

// An FM Beat Cancel node with nothing connected to it.
orc::Project beatProject() {
  return orc::project_io::load_project_from_yaml(R"yaml(
project:
  name: beat
  version: "2.0"
  video_format: PAL
  source_format: Composite
  amplitude_unit: mV
dag:
  nodes:
    - id: 2
      stage: fm_beat_cancel
      node_type: TRANSFORM
      display_name: FM Beat Cancel
      user_label: Beat
      x: 0
      y: 0
  edges: []
)yaml",
                                                 "beat.orcprj");
}

orc::ParameterDescriptor descriptor(const std::string& name,
                                    orc::ParameterType type) {
  orc::ParameterDescriptor d;
  d.name = name;
  d.display_name = name + " shown";
  d.description = name + " described";
  d.type = type;
  return d;
}

}  // namespace

TEST(StageAnalysisToolsTest, TheStagesToolIsListedAsAnAnalysisTool) {
  auto project = beatProject();
  AnalysisPresenter analysis(&project);
  const auto tools = analysis.getToolsForStage("fm_beat_cancel");
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0].id, "fm_beat_calibration");
  EXPECT_TRUE(isStageAnalysisTool(tools[0]));
  EXPECT_EQ(tools[0].stage_tool_contract, kContract);
  EXPECT_EQ(toolRunSupport(tools[0]).mode, ToolRunMode::Analysis);
  EXPECT_TRUE(toolCanApplyToGraph(tools[0]));
  EXPECT_TRUE(toolCanRefresh(tools[0]));
  EXPECT_TRUE(analysis.getToolParameters(tools[0]).empty());
}

TEST(StageAnalysisToolsTest, ARefreshAsksTheStageWhetherAToolRunsAlongside) {
  EXPECT_TRUE(stageAnalysisToolRunsAlongside("frame_map", "field_mapping"));
  EXPECT_FALSE(stageAnalysisToolRunsAlongside("frame_map", "frame_map_range"));
  EXPECT_FALSE(
      stageAnalysisToolRunsAlongside("fm_beat_cancel", "fm_beat_calibration"));
  EXPECT_FALSE(stageAnalysisToolRunsAlongside("no_such_stage", "x"));
}

TEST(StageAnalysisToolsTest, ItIsFoundByTheContractOfItsSession) {
  auto project = beatProject();
  const auto tool = findStageAnalysisTool(&project, NodeID(2), kContract);
  ASSERT_TRUE(tool.has_value());
  EXPECT_EQ(tool->stage_name, "fm_beat_cancel");
  EXPECT_EQ(tool->tool_id, "fm_beat_calibration");
  EXPECT_EQ(tool->tool_name, "FM Beat Calibration");
  EXPECT_FALSE(stageAnalysisToolInfo(&project, NodeID(2), kContract)
                   .description.empty());

  EXPECT_FALSE(findStageAnalysisTool(&project, NodeID(2), "other.v1"));
  EXPECT_FALSE(findStageAnalysisTool(&project, NodeID(9), kContract));
  EXPECT_FALSE(findStageAnalysisTool(nullptr, NodeID(2), kContract));
}

TEST(StageAnalysisToolsTest, ARunGetsTheStagesOwnReport) {
  auto project = beatProject();
  StageAnalysisRun run(&project, NodeID(2), "fm_beat_calibration", {});
  std::vector<int> percents;
  const auto result =
      run.run([&percents](int percent,
                          const std::string&) { percents.push_back(percent); },
              nullptr);
  EXPECT_EQ(result.status, orc::AnalysisResult::Failed);
  EXPECT_EQ(result.summary,
            "Connect a source to the FM Beat Cancel node first.");
  ASSERT_FALSE(percents.empty());
  EXPECT_EQ(percents.front(), 0);
}

TEST(StageAnalysisToolsTest, ARunOfAToolTheStageLacksSaysSo) {
  auto project = beatProject();
  StageAnalysisRun run(&project, NodeID(2), "no_such_tool", {});
  const auto result = run.run(nullptr, nullptr);
  EXPECT_EQ(result.status, orc::AnalysisResult::Failed);
  EXPECT_EQ(result.summary,
            "Analysis tool 'no_such_tool' cannot run: the node's stage does "
            "not offer it");
}

TEST(StageAnalysisToolsTest, ACancelledRunIsCancelled) {
  auto project = beatProject();
  StageAnalysisRun run(&project, NodeID(2), "fm_beat_calibration", {});
  const auto result = run.run(nullptr, [] { return true; });
  EXPECT_EQ(result.status, orc::AnalysisResult::Cancelled);
}

TEST(StageAnalysisToolsTest, ApplyingWritesTheReportsChangesOverTheNode) {
  auto project = beatProject();
  AnalysisToolPresenter presenter("fm_beat_calibration", &project);

  orc::AnalysisResult result;
  result.status = orc::AnalysisResult::Success;
  std::map<std::string, orc::ParameterValue> changes;
  EXPECT_FALSE(presenter.prepareResultChanges(result, NodeID(2), &changes))
      << "a successful run with nothing to write has nothing to apply";

  result.parameterChanges["carrier_black_mhz"] = 7.0762;
  ASSERT_TRUE(presenter.prepareResultChanges(result, NodeID(2), &changes));
  EXPECT_EQ(changes, result.parameterChanges);
  ASSERT_TRUE(presenter.applyResultToGraph(result, NodeID(2)));
  const auto& node = project.get_nodes().front();
  EXPECT_EQ(std::get<double>(node.parameters.at("carrier_black_mhz")), 7.0762);

  result.status = orc::AnalysisResult::Failed;
  EXPECT_FALSE(presenter.prepareResultChanges(result, NodeID(2), nullptr));
}

TEST(StageAnalysisToolsTest, TheRefreshOptionRoundTrips) {
  auto project = beatProject();
  auto state = stageAnalysisRefreshState(&project, NodeID(2), kContract);
  EXPECT_TRUE(state.available);
  EXPECT_FALSE(state.this_tool);

  auto reply =
      setStageAnalysisRefresh(&project, NodeID(2), kContract, true, {});
  ASSERT_TRUE(reply.ok) << reply.error;
  EXPECT_EQ(reply.data, (std::vector<uint8_t>{1}));
  EXPECT_EQ(getNodeToolRefresh(&project, NodeID(2))->tool_id,
            "fm_beat_calibration");
  EXPECT_TRUE(
      stageAnalysisRefreshState(&project, NodeID(2), kContract).this_tool);

  reply = setStageAnalysisRefresh(&project, NodeID(2), kContract, true, {});
  EXPECT_EQ(reply.data, (std::vector<uint8_t>{0})) << "already stored";

  reply = setStageAnalysisRefresh(&project, NodeID(2), kContract, true,
                                  {{"no_such", "1"}});
  EXPECT_FALSE(reply.ok);

  reply = setStageAnalysisRefresh(&project, NodeID(2), kContract, false, {});
  ASSERT_TRUE(reply.ok);
  EXPECT_FALSE(getNodeToolRefresh(&project, NodeID(2)).has_value());
}

TEST(StageAnalysisToolsTest, AnotherToolsRefreshIsNamedAndLeftAlone) {
  auto project = beatProject();
  setNodeToolRefresh(&project, NodeID(2), ToolRefreshConfig{"other_tool", {}});
  const auto state = stageAnalysisRefreshState(&project, NodeID(2), kContract);
  EXPECT_FALSE(state.this_tool);
  EXPECT_EQ(state.other_tool, "other_tool");

  const auto reply =
      setStageAnalysisRefresh(&project, NodeID(2), kContract, false, {});
  ASSERT_TRUE(reply.ok);
  EXPECT_EQ(getNodeToolRefresh(&project, NodeID(2))->tool_id, "other_tool");
}

TEST(StageAnalysisToolsTest, ParametersAreOfferedAsText) {
  auto lead_in = descriptor("lead_in", orc::ParameterType::BOOL);
  lead_in.constraints.default_value = true;
  auto passes = descriptor("passes", orc::ParameterType::INT32);
  passes.constraints.min_value = int32_t{1};
  passes.constraints.max_value = int32_t{9};
  passes.constraints.default_value = int32_t{3};
  passes.constraints.depends_on =
      orc::ParameterDependency{"mode", {"thorough"}, true};
  auto gain = descriptor("gain", orc::ParameterType::DOUBLE);
  gain.constraints.default_value = 0.1;
  auto mode = descriptor("mode", orc::ParameterType::STRING);
  mode.constraints.allowed_strings = {"fast", "thorough"};
  mode.constraints.default_value = std::string("fast");

  const auto offered = toolAnalysisParameters({lead_in, passes, gain, mode});

  using Type = orc::view::ToolAnalysisParameter::Type;
  ASSERT_EQ(offered.size(), 4u);
  EXPECT_EQ(offered[0].type, Type::Bool);
  EXPECT_EQ(offered[0].default_value, "true");
  EXPECT_EQ(offered[0].display_name, "lead_in shown");
  EXPECT_EQ(offered[0].description, "lead_in described");
  EXPECT_EQ(offered[1].type, Type::Integer);
  EXPECT_EQ(offered[1].min_value, "1");
  EXPECT_EQ(offered[1].max_value, "9");
  EXPECT_EQ(offered[1].default_value, "3");
  EXPECT_EQ(offered[1].depends_on, "mode");
  EXPECT_EQ(offered[1].depends_on_values, std::vector<std::string>{"thorough"});
  EXPECT_EQ(offered[2].type, Type::Real);
  EXPECT_EQ(offered[2].default_value, "0.1");
  EXPECT_TRUE(offered[2].min_value.empty());
  EXPECT_EQ(offered[3].type, Type::Text);
  EXPECT_EQ(offered[3].allowed_values,
            (std::vector<std::string>{"fast", "thorough"}));
  EXPECT_EQ(offered[3].default_value, "fast");

  std::map<std::string, orc::ParameterValue> values;
  std::string error;
  ASSERT_TRUE(toolAnalysisParameterValues({lead_in, passes, gain, mode},
                                          {{"passes", "5"}}, &values, &error))
      << error;
  EXPECT_EQ(std::get<int32_t>(values.at("passes")), 5);
  EXPECT_EQ(std::get<bool>(values.at("lead_in")), true) << "a default";
  EXPECT_FALSE(toolAnalysisParameterValues({passes}, {{"passes", "12"}},
                                           &values, &error));
  EXPECT_FALSE(error.empty());
}

TEST(StageAnalysisToolsTest, AReportCarriesItsChangesAsText) {
  orc::AnalysisResult result;
  result.status = orc::AnalysisResult::Success;
  result.summary = "Carrier 7.0762 MHz";
  result.parameterChanges["carrier_black_mhz"] = 7.0762;
  result.parameterChanges["ranges"] = std::string("0-9");

  auto report = toolAnalysisReport(result);
  EXPECT_EQ(report.status, orc::view::ToolAnalysisReport::Status::Success);
  EXPECT_EQ(report.summary, "Carrier 7.0762 MHz");
  EXPECT_EQ(report.parameter_changes,
            (orc::view::ToolParameterMap{{"carrier_black_mhz", "7.0762"},
                                         {"ranges", "0-9"}}));

  result.status = orc::AnalysisResult::Failed;
  report = toolAnalysisReport(result);
  EXPECT_EQ(report.status, orc::view::ToolAnalysisReport::Status::Failed);
  EXPECT_TRUE(report.parameter_changes.empty()) << "a failure writes nothing";
}

}  // namespace orc::presenters::test
