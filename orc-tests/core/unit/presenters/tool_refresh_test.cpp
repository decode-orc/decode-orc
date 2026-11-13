/*
 * File:        tool_refresh_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     Unit tests for `orc-cli tools refresh`: the run order, --only,
 *              the stop at the first failure with an all-or-nothing save,
 *              dry runs, which tools a refresh may run, and maps run
 *              side by side
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "tool_refresh.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
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

/// Maps run alongside one another, as the project backend runs Disc Mapper
/// tools. Each map waits, up to a time limit, until as many maps are running
/// as the refresh allows, so a refresh that ran them one at a time shows as
/// a map that never saw a partner.
class AlongsideBackend : public ToolRefreshBackend {
 public:
  std::set<std::string> fail;
  /// Maps that run until the refresh cancels them.
  std::set<std::string> run_until_cancelled;
  /// Maps that need no partner: the last of an odd number.
  std::set<std::string> lone;

  bool canRunAlongside(const RefreshTarget& target) const override {
    return target.config.tool_id == "field_mapping";
  }

  void prepareTool(const RefreshTarget& target) override {
    record("prepare " + target.node.label);
  }

  orc::AnalysisResult runTool(
      const RefreshTarget& target,
      const std::function<void(int, const std::string&)>& progress,
      const std::function<bool()>& cancel_check) override {
    record("run " + target.node.label);
    orc::AnalysisResult result;
    result.status = orc::AnalysisResult::Status::Success;
    if (canRunAlongside(target)) {
      std::unique_lock<std::mutex> lock(mutex_);
      ++running_;
      max_running_ = std::max(max_running_, running_);
      // Maps arrive in groups of kRefreshToolsAtOnce; each waits for the
      // rest of its group to arrive.
      const size_t group_end =
          (entered_ / kRefreshToolsAtOnce + 1) * kRefreshToolsAtOnce;
      ++entered_;
      changed_.notify_all();
      const auto limit =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      if (fail.count(target.node.label) == 0 &&
          lone.count(target.node.label) == 0) {
        changed_.wait_until(
            lock, limit, [this, group_end] { return entered_ >= group_end; });
      }
      if (run_until_cancelled.count(target.node.label) != 0) {
        lock.unlock();
        while (!cancel_check() && std::chrono::steady_clock::now() < limit) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        lock.lock();
        if (cancel_check()) {
          result.status = orc::AnalysisResult::Status::Cancelled;
        }
      }
      --running_;
      changed_.notify_all();
    }
    progress(50, target.node.label);
    progress(100, target.node.label);
    if (fail.count(target.node.label) != 0) {
      result.status = orc::AnalysisResult::Status::Failed;
    }
    return result;
  }

  bool prepareChanges(const RefreshTarget& /*target*/,
                      const orc::AnalysisResult& /*result*/,
                      std::map<std::string, ParameterValue>* changes) override {
    (*changes)["ranges"] = std::string("0-9");
    return true;
  }

  bool applyChanges(const RefreshTarget& target,
                    const orc::AnalysisResult& /*result*/) override {
    record("apply " + target.node.label);
    return true;
  }

  bool save() override {
    record("save");
    return true;
  }

  std::vector<std::string> calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
  }
  size_t max_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_running_;
  }

  /// Where @p call came in the calls, or the call count when it never did.
  size_t position(const std::string& call) const {
    const auto all = calls();
    return static_cast<size_t>(std::find(all.begin(), all.end(), call) -
                               all.begin());
  }

 private:
  void record(const std::string& call) {
    std::lock_guard<std::mutex> lock(mutex_);
    calls_.push_back(call);
  }

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<std::string> calls_;
  size_t running_ = 0;
  size_t max_running_ = 0;
  size_t entered_ = 0;
};

/// @p discs captures, each mapped, all aligned into one stack.
Stack disc_stack(int discs) {
  Stack stack;
  const int align = 1;
  stack.nodes.push_back(make_node(align, "align", "source_align"));
  stack.refresh[NodeID(align)] = {"source_alignment", {}};
  for (int i = 0; i < discs; ++i) {
    const int source = 100 + i;
    const int map = 10 + i;
    stack.nodes.push_back(
        make_node(source, "disc" + std::to_string(i + 1), "tbc_source"));
    stack.nodes.push_back(
        make_node(map, "map" + std::to_string(i + 1), "frame_map"));
    stack.edges.push_back(edge(source, map));
    stack.edges.push_back(edge(map, align));
    stack.refresh[NodeID(map)] = {"field_mapping", {}};
  }
  return stack;
}

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

TEST(ToolRefreshTest, ANodeWaitsForTheRefreshedNodesUpstreamOfIt) {
  const auto stack = two_disc_stack();
  const auto targets =
      planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {});
  ASSERT_EQ(labels(targets),
            (std::vector<std::string>{"map1", "map2", "align"}));
  EXPECT_TRUE(targets[0].waits_for.empty());
  EXPECT_TRUE(targets[1].waits_for.empty());
  EXPECT_EQ(targets[2].waits_for, (std::vector<size_t>{0, 1}));
}

// The align reads every map's ranges, so it starts only once both are
// applied; the maps themselves run side by side, and their results are
// applied in plan order.
TEST(ToolRefreshTest, MapsRunSideBySideAndTheAlignWaitsForBoth) {
  const auto stack = two_disc_stack();
  const auto targets =
      planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {});
  AlongsideBackend backend;
  const auto outcome =
      runToolRefresh(targets, &backend, false, kNoProgress, kNeverCancel);

  ASSERT_TRUE(outcome.completed);
  EXPECT_TRUE(outcome.saved);
  EXPECT_EQ(backend.max_running(), 2u);
  EXPECT_LT(backend.position("apply map1"), backend.position("apply map2"));
  EXPECT_LT(backend.position("apply map2"), backend.position("prepare align"));
  EXPECT_LT(backend.position("prepare align"), backend.position("run align"));
  EXPECT_EQ(backend.calls().back(), "save");
}

TEST(ToolRefreshTest, NoMoreToolsRunAtOnceThanTheLimit) {
  const auto stack = disc_stack(5);
  const auto targets =
      planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {});
  AlongsideBackend backend;
  backend.lone.insert("map5");
  const auto outcome =
      runToolRefresh(targets, &backend, false, kNoProgress, kNeverCancel);

  ASSERT_TRUE(outcome.completed);
  EXPECT_EQ(backend.max_running(), kRefreshToolsAtOnce);
  for (int i = 1; i < 5; ++i) {
    EXPECT_LT(backend.position("apply map" + std::to_string(i)),
              backend.position("apply map" + std::to_string(i + 1)));
  }
  EXPECT_LT(backend.position("apply map5"), backend.position("run align"));
}

// Progress reads as if the steps ran one after another: a step running
// ahead is reported once the steps before it are done.
TEST(ToolRefreshTest, ProgressComesOneStepAtATimeInPlanOrder) {
  const auto stack = disc_stack(4);
  const auto targets =
      planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {});
  AlongsideBackend backend;
  std::vector<size_t> steps;
  const auto outcome = runToolRefresh(
      targets, &backend, false,
      [&steps](size_t step, int, const std::string&) { steps.push_back(step); },
      kNeverCancel);

  ASSERT_TRUE(outcome.completed);
  ASSERT_FALSE(steps.empty());
  EXPECT_TRUE(std::is_sorted(steps.begin(), steps.end()));
  EXPECT_EQ(std::set<size_t>(steps.begin(), steps.end()).size(),
            targets.size());
}

// A map that fails stops the refresh: the map running alongside it is
// cancelled and reported as skipped, and nothing is applied after the
// failure or saved.
TEST(ToolRefreshTest, AFailedMapCancelsTheMapRunningAlongsideIt) {
  const auto stack = two_disc_stack();
  const auto targets =
      planToolRefresh(stack.nodes, stack.edges, stack.lookup(), {});
  AlongsideBackend backend;
  backend.fail.insert("map1");
  backend.run_until_cancelled.insert("map2");
  const auto outcome =
      runToolRefresh(targets, &backend, false, kNoProgress, kNeverCancel);

  EXPECT_FALSE(outcome.completed);
  EXPECT_FALSE(outcome.saved);
  EXPECT_EQ(outcome.steps[0].status, RefreshStepStatus::Failed);
  EXPECT_EQ(outcome.steps[1].status, RefreshStepStatus::Skipped);
  EXPECT_EQ(outcome.steps[2].status, RefreshStepStatus::Skipped);
  const auto calls = backend.calls();
  EXPECT_EQ(std::count_if(
                calls.begin(), calls.end(),
                [](const std::string& c) { return c.rfind("apply", 0) == 0; }),
            0);
  EXPECT_EQ(backend.position("run align"), calls.size());
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
