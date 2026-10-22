/*
 * File:        tool_refresh_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     Unit tests for `orc-cli tools refresh`: the run order, --only,
 *              the stop at the first failure with an all-or-nothing save,
 *              dry runs, and which tools a refresh may run
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "tool_refresh.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../../../orc/core/analysis/analysis_init.h"
#include "../../../../orc/core/include/project.h"

namespace orc::presenters::test {

namespace {

NodeInfo make_node(int32_t id, const std::string& label,
                   const std::string& stage) {
  return NodeInfo{NodeID(id), stage, label, 0.0, 0.0, true, true, "", ""};
}

EdgeInfo edge(int32_t from, int32_t to) { return {NodeID(from), NodeID(to)}; }

orc::AnalysisToolInfo make_tool(const std::string& id,
                                const std::string& kind) {
  orc::AnalysisToolInfo tool;
  tool.id = id;
  tool.name = id + " name";
  tool.priority = 1;
  tool.stage_tool_kind = kind;
  return tool;
}

/// Two discs, mapped and aligned. The ids run against the data flow (the
/// align is node 1), so only a pipeline order puts the maps first.
struct Stack {
  std::vector<NodeInfo> nodes;
  std::vector<EdgeInfo> edges;
  std::map<NodeID, ToolRefreshConfig> refresh;

  std::function<std::optional<ToolRefreshConfig>(NodeID)> lookup() const {
    return [this](NodeID id) -> std::optional<ToolRefreshConfig> {
      const auto it = refresh.find(id);
      if (it == refresh.end()) {
        return std::nullopt;
      }
      return it->second;
    };
  }
};

Stack two_disc_stack() {
  Stack stack;
  stack.nodes = {make_node(1, "align", "source_align"),
                 make_node(2, "map2", "frame_map"),
                 make_node(3, "map1", "frame_map"),
                 make_node(4, "disc1", "tbc_source"),
                 make_node(5, "disc2", "tbc_source"),
                 make_node(6, "stack", "stacker")};
  stack.edges = {edge(4, 3), edge(5, 2), edge(3, 1), edge(2, 1), edge(1, 6)};
  stack.refresh = {{NodeID(1), {"source_alignment", {}}},
                   {NodeID(2), {"field_mapping", {}}},
                   {NodeID(3), {"field_mapping", {}}}};
  return stack;
}

std::vector<std::string> labels(const std::vector<RefreshTarget>& targets) {
  std::vector<std::string> out;
  for (const auto& t : targets) {
    out.push_back(t.node.label);
  }
  return out;
}

/// Records what the refresh asked of the project, and fails where told to.
class FakeBackend : public ToolRefreshBackend {
 public:
  std::vector<std::string> calls;
  std::map<std::string, orc::AnalysisResult::Status> status_of;
  std::set<std::string> no_changes;
  std::set<std::string> rejected;
  bool save_result = true;

  orc::AnalysisResult runTool(
      const RefreshTarget& target,
      const std::function<void(int, const std::string&)>& progress,
      const std::function<bool()>& /*cancel_check*/) override {
    calls.push_back("run " + target.node.label);
    progress(100, "done");
    orc::AnalysisResult result;
    const auto it = status_of.find(target.node.label);
    result.status = it == status_of.end() ? orc::AnalysisResult::Status::Success
                                          : it->second;
    return result;
  }

  bool prepareChanges(const RefreshTarget& target,
                      const orc::AnalysisResult& /*result*/,
                      std::map<std::string, ParameterValue>* changes) override {
    if (no_changes.count(target.node.label) != 0) {
      return false;
    }
    (*changes)["ranges"] = std::string("0-9");
    return true;
  }

  bool applyChanges(const RefreshTarget& target,
                    const orc::AnalysisResult& /*result*/) override {
    calls.push_back("apply " + target.node.label);
    return rejected.count(target.node.label) == 0;
  }

  bool save() override {
    calls.push_back("save");
    return save_result;
  }
};

const auto kNoProgress = [](size_t, int, const std::string&) {};
const auto kNeverCancel = [] { return false; };

}  // namespace

TEST(ToolRefreshTest, FrameMapsRunBeforeTheAlignThatReadsThem) {
  const auto stack = two_disc_stack();
  const auto targets =
      planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {});
  EXPECT_EQ(labels(targets),
            (std::vector<std::string>{"map1", "map2", "align"}));
  EXPECT_EQ(targets.back().config.tool_id, "source_alignment");
}

TEST(ToolRefreshTest, NodesWithoutARefreshToolAreLeftOut) {
  auto stack = two_disc_stack();
  stack.refresh.erase(NodeID(2));
  EXPECT_EQ(
      labels(planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {})),
      (std::vector<std::string>{"map1", "align"}));
}

TEST(ToolRefreshTest, OnlyKeepsTheNamedTools) {
  const auto stack = two_disc_stack();
  EXPECT_EQ(labels(planToolRefresh(stack.nodes, stack.edges, stack.lookup(),
                                   {"field_mapping"})),
            (std::vector<std::string>{"map1", "map2"}));
  EXPECT_EQ(labels(planToolRefresh(stack.nodes, stack.edges, stack.lookup(),
                                   {"source_alignment"})),
            (std::vector<std::string>{"align"}));
}

TEST(ToolRefreshTest, AppliesEachResultBeforeTheNextToolAndSavesOnce) {
  const auto stack = two_disc_stack();
  const auto targets =
      planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {});
  FakeBackend backend;
  std::vector<size_t> progress_steps;
  const auto outcome = runToolRefresh(
      targets, &backend, false,
      [&progress_steps](size_t step, int, const std::string&) {
        progress_steps.push_back(step);
      },
      kNeverCancel);

  EXPECT_TRUE(outcome.completed);
  EXPECT_TRUE(outcome.saved);
  EXPECT_EQ(backend.calls,
            (std::vector<std::string>{"run map1", "apply map1", "run map2",
                                      "apply map2", "run align", "apply align",
                                      "save"}));
  EXPECT_EQ(progress_steps, (std::vector<size_t>{0, 1, 2}));
  for (const auto& step : outcome.steps) {
    EXPECT_EQ(step.status, RefreshStepStatus::Success);
    EXPECT_EQ(step.changes.count("ranges"), 1u);
  }
}

TEST(ToolRefreshTest, StopsAtTheFirstFailureAndSavesNothing) {
  const auto stack = two_disc_stack();
  const auto targets =
      planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {});
  FakeBackend backend;
  backend.status_of["map2"] = orc::AnalysisResult::Status::Failed;
  const auto outcome =
      runToolRefresh(targets, &backend, false, kNoProgress, kNeverCancel);

  EXPECT_FALSE(outcome.completed);
  EXPECT_FALSE(outcome.saved);
  EXPECT_FALSE(outcome.save_failed);
  EXPECT_EQ(backend.calls,
            (std::vector<std::string>{"run map1", "apply map1", "run map2"}));
  ASSERT_EQ(outcome.steps.size(), 3u);
  EXPECT_EQ(outcome.steps[0].status, RefreshStepStatus::Success);
  EXPECT_EQ(outcome.steps[1].status, RefreshStepStatus::Failed);
  EXPECT_EQ(outcome.steps[2].status, RefreshStepStatus::Skipped);
}

TEST(ToolRefreshTest, NoChangesCountsAsAFailure) {
  const auto stack = two_disc_stack();
  const auto targets =
      planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {});
  FakeBackend backend;
  backend.no_changes.insert("map1");
  const auto outcome =
      runToolRefresh(targets, &backend, false, kNoProgress, kNeverCancel);

  EXPECT_FALSE(outcome.completed);
  EXPECT_EQ(outcome.steps[0].status, RefreshStepStatus::NoChanges);
  EXPECT_EQ(backend.calls, (std::vector<std::string>{"run map1"}));
}

TEST(ToolRefreshTest, ARefusedApplyStopsTheRun) {
  const auto stack = two_disc_stack();
  const auto targets =
      planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {});
  FakeBackend backend;
  backend.rejected.insert("map1");
  const auto outcome =
      runToolRefresh(targets, &backend, false, kNoProgress, kNeverCancel);

  EXPECT_FALSE(outcome.completed);
  EXPECT_EQ(outcome.steps[0].status, RefreshStepStatus::Rejected);
  EXPECT_EQ(outcome.steps[1].status, RefreshStepStatus::Skipped);
}

TEST(ToolRefreshTest, ACancelledToolStopsTheRun) {
  const auto stack = two_disc_stack();
  const auto targets =
      planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {});
  FakeBackend backend;
  backend.status_of["map1"] = orc::AnalysisResult::Status::Cancelled;
  const auto outcome =
      runToolRefresh(targets, &backend, false, kNoProgress, kNeverCancel);

  EXPECT_EQ(outcome.steps[0].status, RefreshStepStatus::Cancelled);
  EXPECT_FALSE(outcome.saved);
}

TEST(ToolRefreshTest, ADryRunAppliesInMemoryButNeverSaves) {
  const auto stack = two_disc_stack();
  const auto targets =
      planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {});
  FakeBackend backend;
  const auto outcome =
      runToolRefresh(targets, &backend, true, kNoProgress, kNeverCancel);

  EXPECT_TRUE(outcome.completed);
  EXPECT_FALSE(outcome.saved);
  EXPECT_FALSE(outcome.save_failed);
  EXPECT_EQ(std::count(backend.calls.begin(), backend.calls.end(), "save"), 0);
  EXPECT_EQ(
      std::count(backend.calls.begin(), backend.calls.end(), "apply align"), 1);
}

TEST(ToolRefreshTest, AFailedSaveIsReported) {
  const auto stack = two_disc_stack();
  const auto targets =
      planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {});
  FakeBackend backend;
  backend.save_result = false;
  const auto outcome =
      runToolRefresh(targets, &backend, false, kNoProgress, kNeverCancel);

  EXPECT_TRUE(outcome.completed);
  EXPECT_FALSE(outcome.saved);
  EXPECT_TRUE(outcome.save_failed);
}

TEST(ToolRefreshTest, OnlyAToolThatWritesToTheStageCanRefreshIt) {
  orc::force_analysis_tool_linking();
  const std::vector<orc::AnalysisToolInfo> tools = {
      make_tool("field_mapping", ""),
      make_tool("dropout_analysis", "batch_analysis"),
      make_tool("dropout_editor", "non_modal_editor")};

  EXPECT_FALSE(refreshToolProblem(tools, "field_mapping").has_value());
  EXPECT_TRUE(refreshToolProblem(tools, "dropout_analysis").has_value());
  EXPECT_TRUE(refreshToolProblem(tools, "dropout_editor").has_value());
  const auto missing = refreshToolProblem(tools, "source_alignment");
  ASSERT_TRUE(missing.has_value());
  EXPECT_NE(missing->find("not one of this stage's tools"), std::string::npos);
}

TEST(ToolRefreshTest, TheConfigIsStoredOnTheNode) {
  auto project = orc::project_io::load_project_from_yaml(R"yaml(
project:
  name: store
  version: "2.0"
  video_format: PAL
  source_format: Composite
  amplitude_unit: mV
dag:
  nodes:
    - id: 7
      stage: frame_map
      node_type: TRANSFORM
      display_name: Frame Map
      user_label: Frame Map
      x: 0
      y: 0
  edges: []
)yaml",
                                                         "store.orcprj");
  EXPECT_FALSE(getNodeToolRefresh(&project, NodeID(7)).has_value());

  const ToolRefreshConfig config{"field_mapping",
                                 {{"include_lead_in_out", true}}};
  setNodeToolRefresh(&project, NodeID(7), config);
  EXPECT_EQ(getNodeToolRefresh(&project, NodeID(7)), config);
  EXPECT_TRUE(project.has_unsaved_changes());

  setNodeToolRefresh(&project, NodeID(7), std::nullopt);
  EXPECT_FALSE(getNodeToolRefresh(&project, NodeID(7)).has_value());
  EXPECT_THROW(setNodeToolRefresh(&project, NodeID(8), config),
               std::runtime_error);
}

}  // namespace orc::presenters::test
