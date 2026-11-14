/*
 * File:        stage_tool_session_test.cpp
 * Module:      orc-gui unit tests (gui-model)
 * Purpose:     A plugin view's tool session: replies in request order
 *              whatever order the transport answers in, jobs with progress
 *              and cancel, parameter write-back, the previewer link, and
 *              nothing reaching the view once the session has closed
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "presenters/include/stage_tool_session.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::presenters::IStageToolTransport;
using orc::presenters::StageToolSession;
using orc::view::ToolReply;

// Holds every request until the test answers it, in whatever order.
class HeldTransport final : public IStageToolTransport {
 public:
  struct Sent {
    orc::NodeID node;
    std::string contract_id;
    std::string verb;
    std::vector<uint8_t> payload;
    std::function<void(ToolReply)> on_done;
  };

  void send(orc::NodeID node, const std::string& contract_id,
            const std::string& verb, std::vector<uint8_t> payload,
            std::function<void(ToolReply)> on_done) override {
    sent.push_back(
        {node, contract_id, verb, std::move(payload), std::move(on_done)});
  }

  void answer(size_t index, const std::string& text) {
    ToolReply reply;
    reply.ok = true;
    reply.data.assign(text.begin(), text.end());
    sent.at(index).on_done(std::move(reply));
  }

  struct Job {
    uint64_t id;
    std::string verb;
    std::vector<uint8_t> payload;
    std::function<void(orc::view::ToolJobProgress)> on_progress;
    std::function<void(ToolReply)> on_done;
  };

  uint64_t start_job(
      orc::NodeID /*node*/, const std::string& /*contract_id*/,
      const std::string& verb, std::vector<uint8_t> payload,
      std::function<void(orc::view::ToolJobProgress)> on_progress,
      std::function<void(ToolReply)> on_done) override {
    const uint64_t id = 100 + jobs.size();
    jobs.push_back({id, verb, std::move(payload), std::move(on_progress),
                    std::move(on_done)});
    return id;
  }

  void cancel_job(uint64_t job_id) override { cancelled.push_back(job_id); }

  struct Apply {
    orc::view::ToolParameterMap values;
    std::string label;
    std::function<void(ToolReply)> on_done;
  };

  void apply_parameters(orc::NodeID /*node*/,
                        orc::view::ToolParameterMap values,
                        const std::string& label,
                        std::function<void(ToolReply)> on_done) override {
    applies.push_back({std::move(values), label, std::move(on_done)});
  }

  void show_frame(orc::NodeID node, uint64_t frame_index) override {
    shown.push_back({node, frame_index});
  }

  struct Render {
    uint64_t frame_index;
    bool from_input;
    std::function<void(orc::view::ToolFrameImage)> on_done;
  };

  void render_frame(
      orc::NodeID /*node*/, uint64_t frame_index,
      const orc::view::ToolFrameRenderOptions& options,
      std::function<void(orc::view::ToolFrameImage)> on_done) override {
    renders.push_back({frame_index, options.from_input, std::move(on_done)});
  }

  void render(size_t index) {
    orc::view::ToolFrameImage image;
    image.ok = true;
    image.frame_index = renders.at(index).frame_index;
    renders.at(index).on_done(std::move(image));
  }

  orc::view::ToolParameterMap parameters(orc::NodeID node) override {
    parameter_reads.push_back(node);
    return stored;
  }

  std::string project_directory() override { return directory; }

  struct Analysis {
    uint64_t id;
    std::string contract_id;
    orc::view::ToolParameterMap parameters;
    std::function<void(orc::view::ToolJobProgress)> on_progress;
    std::function<void(orc::view::ToolAnalysisReport)> on_done;
  };

  uint64_t start_analysis(
      orc::NodeID /*node*/, const std::string& contract_id,
      orc::view::ToolParameterMap parameters,
      std::function<void(orc::view::ToolJobProgress)> on_progress,
      std::function<void(orc::view::ToolAnalysisReport)> on_done) override {
    const uint64_t id = 500 + analyses.size();
    analyses.push_back({id, contract_id, std::move(parameters),
                        std::move(on_progress), std::move(on_done)});
    return id;
  }

  orc::view::ToolAnalysisInfo analysis_info(
      orc::NodeID /*node*/, const std::string& contract_id) override {
    info_reads.push_back(contract_id);
    return info;
  }

  orc::view::ToolRefreshState refresh_state(
      orc::NodeID /*node*/, const std::string& /*contract_id*/) override {
    return refresh;
  }

  struct RefreshWrite {
    bool use_this_tool;
    orc::view::ToolParameterMap parameters;
    std::function<void(ToolReply)> on_done;
  };

  void set_refresh(orc::NodeID /*node*/, const std::string& /*contract_id*/,
                   bool use_this_tool, orc::view::ToolParameterMap parameters,
                   std::function<void(ToolReply)> on_done) override {
    refresh_writes.push_back(
        {use_this_tool, std::move(parameters), std::move(on_done)});
  }

  std::vector<Analysis> analyses;
  std::vector<std::string> info_reads;
  orc::view::ToolAnalysisInfo info;
  orc::view::ToolRefreshState refresh;
  std::vector<RefreshWrite> refresh_writes;
  std::vector<Sent> sent;
  std::vector<Job> jobs;
  std::vector<orc::NodeID> parameter_reads;
  orc::view::ToolParameterMap stored;
  std::string directory;
  std::vector<uint64_t> cancelled;
  std::vector<Apply> applies;
  std::vector<std::pair<orc::NodeID, uint64_t>> shown;
  std::vector<Render> renders;
};

ToolReply ok_reply(const std::string& text) {
  ToolReply reply;
  reply.ok = true;
  reply.data.assign(text.begin(), text.end());
  return reply;
}

std::string text_of(const ToolReply& reply) {
  return std::string(reply.data.begin(), reply.data.end());
}

struct Fixture {
  std::shared_ptr<HeldTransport> transport = std::make_shared<HeldTransport>();
  std::shared_ptr<StageToolSession> session =
      std::make_shared<StageToolSession>(transport, orc::NodeID(4),
                                         "test.view.v1", "Pages - Sink (4)");
  std::vector<std::string> delivered;

  void ask(const std::string& verb) {
    session->request(verb, {}, [this](ToolReply reply) {
      delivered.push_back(text_of(reply));
    });
  }
};

}  // namespace

TEST(StageToolSessionTest, SendsTheRequestToTheNodesStageUntouched) {
  Fixture f;
  f.session->request("page", {1, 2, 3}, [](ToolReply) {});

  ASSERT_EQ(f.transport->sent.size(), 1u);
  const auto& sent = f.transport->sent[0];
  EXPECT_EQ(sent.node, orc::NodeID(4));
  EXPECT_EQ(sent.contract_id, "test.view.v1");
  EXPECT_EQ(sent.verb, "page");
  EXPECT_EQ(sent.payload, (std::vector<uint8_t>{1, 2, 3}));
  EXPECT_EQ(f.session->contract_id(), "test.view.v1");
  EXPECT_EQ(f.session->title(), "Pages - Sink (4)");
  EXPECT_TRUE(f.session->is_open());
}

TEST(StageToolSessionTest, RepliesArriveInRequestOrderWhenAnsweredInOrder) {
  Fixture f;
  f.ask("a");
  f.ask("b");
  f.ask("c");
  f.transport->answer(0, "A");
  f.transport->answer(1, "B");
  f.transport->answer(2, "C");
  EXPECT_EQ(f.delivered, (std::vector<std::string>{"A", "B", "C"}));
}

// A later answer waits for the earlier ones, then they all go together.
TEST(StageToolSessionTest, AnOutOfOrderReplyIsHeldUntilItsTurn) {
  Fixture f;
  f.ask("a");
  f.ask("b");
  f.ask("c");

  f.transport->answer(2, "C");
  EXPECT_TRUE(f.delivered.empty());
  f.transport->answer(1, "B");
  EXPECT_TRUE(f.delivered.empty());
  f.transport->answer(0, "A");
  EXPECT_EQ(f.delivered, (std::vector<std::string>{"A", "B", "C"}));
}

TEST(StageToolSessionTest, AFailedReplyStillTakesItsTurn) {
  Fixture f;
  std::vector<bool> oks;
  for (int i = 0; i < 2; ++i) {
    f.session->request("x", {},
                       [&oks](ToolReply reply) { oks.push_back(reply.ok); });
  }
  ToolReply refused;
  refused.error = "The node's stage does not answer tool requests";
  f.transport->answer(1, "fine");
  f.transport->sent[0].on_done(refused);
  EXPECT_EQ(oks, (std::vector<bool>{false, true}));
}

// --- Shutdown
// -----------------------------------------------------------------

TEST(StageToolSessionTest, CloseDropsEveryReplyStillInFlight) {
  Fixture f;
  f.ask("a");
  f.ask("b");
  f.session->close();
  f.transport->answer(0, "A");
  f.transport->answer(1, "B");
  EXPECT_TRUE(f.delivered.empty());
  EXPECT_FALSE(f.session->is_open());
}

TEST(StageToolSessionTest, ARequestOnAClosedSessionIsNeverSent) {
  Fixture f;
  f.session->close();
  f.ask("a");
  EXPECT_TRUE(f.transport->sent.empty());
  EXPECT_TRUE(f.delivered.empty());
}

// The view's widget can be destroyed (and the session with it) while its
// requests are still being answered on the render worker.
TEST(StageToolSessionTest, AReplyAfterTheSessionIsGoneFindsNothingToCall) {
  Fixture f;
  bool called = false;
  f.session->request("a", {}, [&called](ToolReply) { called = true; });
  f.session.reset();
  f.transport->answer(0, "A");
  EXPECT_FALSE(called);
}

// A view may close itself from a reply handler, as a dialog does when it is
// told the node is gone. The replies after it are not delivered.
TEST(StageToolSessionTest, AHandlerThatClosesTheSessionStopsDelivery) {
  Fixture f;
  std::vector<std::string> seen;
  f.session->request("a", {}, [&](ToolReply reply) {
    seen.push_back(text_of(reply));
    f.session->close();
  });
  f.session->request("b", {},
                     [&](ToolReply reply) { seen.push_back(text_of(reply)); });
  f.transport->answer(1, "B");
  f.transport->answer(0, "A");
  EXPECT_EQ(seen, (std::vector<std::string>{"A"}));
}

// --- Results changed
// ------------------------------------------------------------

TEST(StageToolSessionTest, ResultsChangedReachesTheViewUntilClosed) {
  Fixture f;
  int changes = 0;
  f.session->set_results_changed_handler([&changes] { ++changes; });
  f.session->notifyResultsChanged();
  EXPECT_EQ(changes, 1);

  f.session->close();
  f.session->notifyResultsChanged();
  f.session->set_results_changed_handler([&changes] { changes += 10; });
  f.session->notifyResultsChanged();
  EXPECT_EQ(changes, 1);
}

// --- Jobs
// ----------------------------------------------------------------------

TEST(StageToolSessionTest, AJobReportsProgressThenItsReply) {
  Fixture f;
  std::vector<std::string> events;
  auto job = f.session->run_job(
      "map", {9},
      [&events](orc::view::ToolJobProgress p) { events.push_back(p.text); },
      [&events](ToolReply reply) {
        events.push_back("reply " + text_of(reply));
      });

  ASSERT_NE(job, nullptr);
  ASSERT_EQ(f.transport->jobs.size(), 1u);
  EXPECT_EQ(f.transport->jobs[0].verb, "map");
  EXPECT_EQ(f.transport->jobs[0].payload, (std::vector<uint8_t>{9}));
  EXPECT_TRUE(job->is_running());

  f.transport->jobs[0].on_progress({0.25, "quarter"});
  f.transport->jobs[0].on_progress({0.5, "half"});
  f.transport->jobs[0].on_done(ok_reply("map"));

  EXPECT_EQ(events, (std::vector<std::string>{"quarter", "half", "reply map"}));
  EXPECT_FALSE(job->is_running());
}

TEST(StageToolSessionTest, OnlyOneJobRunsAtATime) {
  Fixture f;
  auto first = f.session->run_job("a", {}, nullptr, [](ToolReply) {});
  auto second = f.session->run_job("b", {}, nullptr, [](ToolReply) {});

  EXPECT_NE(first, nullptr);
  EXPECT_EQ(second, nullptr);
  EXPECT_EQ(f.transport->jobs.size(), 1u);

  f.transport->jobs[0].on_done(ok_reply("a"));
  EXPECT_NE(f.session->run_job("c", {}, nullptr, [](ToolReply) {}), nullptr);
}

// Cancel asks the transport to stop the job; the reply it then sends (here
// "Cancelled") still reaches the view, which is how it learns the job ended.
TEST(StageToolSessionTest, CancelStopsTheJobAndItsReplyStillArrives) {
  Fixture f;
  std::string error;
  auto job = f.session->run_job(
      "a", {}, nullptr, [&error](ToolReply reply) { error = reply.error; });
  job->cancel();
  ASSERT_EQ(f.transport->cancelled, (std::vector<uint64_t>{100}));

  ToolReply cancelled;
  cancelled.error = "Cancelled";
  f.transport->jobs[0].on_done(cancelled);
  EXPECT_EQ(error, "Cancelled");

  job->cancel();  // Finished: nothing more to stop.
  EXPECT_EQ(f.transport->cancelled.size(), 1u);
}

TEST(StageToolSessionTest, ClosingWithAJobInFlightCancelsItAndDropsItsReply) {
  Fixture f;
  int calls = 0;
  auto job = f.session->run_job(
      "a", {}, [&calls](orc::view::ToolJobProgress) { ++calls; },
      [&calls](ToolReply) { ++calls; });
  f.session->close();

  EXPECT_EQ(f.transport->cancelled, (std::vector<uint64_t>{100}));
  EXPECT_FALSE(job->is_running());
  f.transport->jobs[0].on_progress({0.9, "late"});
  f.transport->jobs[0].on_done(ok_reply("late"));
  EXPECT_EQ(calls, 0);
}

TEST(StageToolSessionTest, AJobHandleOutlivingItsSessionDoesNothing) {
  Fixture f;
  auto job = f.session->run_job("a", {}, nullptr, [](ToolReply) {});
  f.session.reset();

  EXPECT_FALSE(job->is_running());
  job->cancel();
  // The session's destructor cancelled it once; the handle adds nothing.
  EXPECT_EQ(f.transport->cancelled.size(), 1u);
}

TEST(StageToolSessionTest, AJobOnAClosedSessionIsRefused) {
  Fixture f;
  f.session->close();
  EXPECT_EQ(f.session->run_job("a", {}, nullptr, [](ToolReply) {}), nullptr);
  EXPECT_TRUE(f.transport->jobs.empty());
}

// --- Parameter write-back
// --------------------------------------------------------

TEST(StageToolSessionTest, ApplyPassesTheValuesAndLabelAndRepliesOnce) {
  Fixture f;
  std::vector<bool> oks;
  f.session->apply_parameters(
      {{"ranges", "0-10"}}, "Apply disc map",
      [&oks](ToolReply reply) { oks.push_back(reply.ok); });

  ASSERT_EQ(f.transport->applies.size(), 1u);
  EXPECT_EQ(f.transport->applies[0].values,
            (orc::view::ToolParameterMap{{"ranges", "0-10"}}));
  EXPECT_EQ(f.transport->applies[0].label, "Apply disc map");
  f.transport->applies[0].on_done(ok_reply(""));
  f.transport->applies[0].on_done(ok_reply(""));
  EXPECT_EQ(oks, (std::vector<bool>{true}));
}

// The rebuild an apply causes can close the view that asked: its answer
// must then find nothing to call.
TEST(StageToolSessionTest, AnApplyAnsweredAfterTheViewClosedIsDropped) {
  Fixture f;
  bool called = false;
  f.session->apply_parameters({{"a", "1"}}, "x",
                              [&called](ToolReply) { called = true; });
  f.session->close();
  f.transport->applies[0].on_done(ok_reply(""));
  EXPECT_FALSE(called);

  f.session->apply_parameters({{"a", "2"}}, "x", [](ToolReply) {});
  EXPECT_EQ(f.transport->applies.size(), 1u);
}

// --- Previewer link
// --------------------------------------------------------------

TEST(StageToolSessionTest, ShowFrameMovesThePreviewForTheSessionsNode) {
  Fixture f;
  f.session->show_frame(1234);
  f.session->close();
  f.session->show_frame(99);

  ASSERT_EQ(f.transport->shown.size(), 1u);
  EXPECT_EQ(f.transport->shown[0].first, orc::NodeID(4));
  EXPECT_EQ(f.transport->shown[0].second, 1234u);
}

TEST(StageToolSessionTest, PreviewFrameMovesReachTheViewUntilClosed) {
  Fixture f;
  std::vector<uint64_t> frames;
  f.session->set_preview_frame_handler(
      [&frames](uint64_t frame) { frames.push_back(frame); });
  f.session->notifyPreviewFrame(5);
  f.session->notifyPreviewFrame(6);
  f.session->close();
  f.session->notifyPreviewFrame(7);
  EXPECT_EQ(frames, (std::vector<uint64_t>{5, 6}));
}

TEST(StageToolSessionTest, StartsWithTheProjectsAmplitudeUnit) {
  auto transport = std::make_shared<HeldTransport>();
  StageToolSession session(transport, orc::NodeID(4), "test.contract", "Title",
                           orc::view::AmplitudeUnit::Millivolts);
  EXPECT_EQ(session.amplitude_unit(), orc::view::AmplitudeUnit::Millivolts);
}

TEST(StageToolSessionTest, AmplitudeUnitChangesReachTheViewUntilClosed) {
  Fixture f;
  std::vector<orc::view::AmplitudeUnit> units;
  f.session->set_amplitude_unit_handler(
      [&units](orc::view::AmplitudeUnit unit) { units.push_back(unit); });
  f.session->notifyAmplitudeUnit(orc::view::AmplitudeUnit::Millivolts);
  // The same unit again is not a change.
  f.session->notifyAmplitudeUnit(orc::view::AmplitudeUnit::Millivolts);
  EXPECT_EQ(f.session->amplitude_unit(), orc::view::AmplitudeUnit::Millivolts);
  f.session->close();
  f.session->notifyAmplitudeUnit(orc::view::AmplitudeUnit::Samples10Bit);
  EXPECT_EQ(units, (std::vector<orc::view::AmplitudeUnit>{
                       orc::view::AmplitudeUnit::Millivolts}));
}

TEST(StageToolSessionTest, RenderedFramesArriveInRequestOrder) {
  Fixture f;
  std::vector<uint64_t> frames;
  const auto keep = [&frames](orc::view::ToolFrameImage image) {
    frames.push_back(image.frame_index);
  };
  f.session->render_frame(10, {}, keep);
  orc::view::ToolFrameRenderOptions node_itself;
  node_itself.from_input = false;
  f.session->render_frame(11, node_itself, keep);

  EXPECT_TRUE(f.transport->renders[0].from_input);
  EXPECT_FALSE(f.transport->renders[1].from_input);
  f.transport->render(1);
  EXPECT_TRUE(frames.empty());
  f.transport->render(0);
  EXPECT_EQ(frames, (std::vector<uint64_t>{10, 11}));
}

TEST(StageToolSessionTest, CloseDropsFramesStillBeingRendered) {
  Fixture f;
  bool called = false;
  f.session->render_frame(
      1, {}, [&called](orc::view::ToolFrameImage) { called = true; });
  f.session->close();
  f.transport->render(0);
  EXPECT_FALSE(called);
}

// --- Graph rebuilds
// --------------------------------------------------------------

TEST(StageToolSessionTest, AViewStaysOpenAcrossRebuildsOnlyWhenItAsks) {
  Fixture f;
  EXPECT_FALSE(f.session->keepsOpenAcrossRebuild());

  int rebuilds = 0;
  f.session->set_graph_rebuilt_handler([&rebuilds] { ++rebuilds; });
  EXPECT_TRUE(f.session->keepsOpenAcrossRebuild());
  f.session->notifyGraphRebuilt();
  EXPECT_EQ(rebuilds, 1);

  f.session->set_graph_rebuilt_handler(nullptr);
  EXPECT_FALSE(f.session->keepsOpenAcrossRebuild());

  f.session->set_graph_rebuilt_handler([&rebuilds] { ++rebuilds; });
  f.session->close();
  EXPECT_FALSE(f.session->keepsOpenAcrossRebuild());
  f.session->notifyGraphRebuilt();
  EXPECT_EQ(rebuilds, 1);
}

// --- The node's parameters and the project folder
// ----------------------------------------------

TEST(StageToolSessionTest, ReadsTheNodesParametersAndTheProjectFolder) {
  Fixture f;
  f.transport->stored = {{"lineSpec", "5-21"}, {"maskSampleLevel", "256"}};
  f.transport->directory = "/captures/project";

  EXPECT_EQ(f.session->parameters(), f.transport->stored);
  ASSERT_EQ(f.transport->parameter_reads.size(), 1u);
  EXPECT_EQ(f.transport->parameter_reads[0], orc::NodeID(4));
  EXPECT_EQ(f.session->project_directory(), "/captures/project");
}

TEST(StageToolSessionTest, AClosedSessionReadsNothing) {
  Fixture f;
  f.transport->stored = {{"lineSpec", "5-21"}};
  f.transport->directory = "/captures/project";
  f.session->close();

  EXPECT_TRUE(f.session->parameters().empty());
  EXPECT_TRUE(f.session->project_directory().empty());
  EXPECT_TRUE(f.transport->parameter_reads.empty());
}

// --- The session's analysis tool
// -----------------------------------------------------------

TEST(StageToolSessionTest, AnAnalysisRunsWithItsParametersAndReportsOnce) {
  Fixture f;
  std::vector<std::string> events;
  auto job = f.session->run_analysis(
      {{"pattern", "skip"}},
      [&events](orc::view::ToolJobProgress p) { events.push_back(p.text); },
      [&events](orc::view::ToolAnalysisReport report) {
        events.push_back("report " + report.summary);
      });

  ASSERT_NE(job, nullptr);
  ASSERT_EQ(f.transport->analyses.size(), 1u);
  EXPECT_EQ(f.transport->analyses[0].contract_id, "test.view.v1");
  EXPECT_EQ(f.transport->analyses[0].parameters,
            (orc::view::ToolParameterMap{{"pattern", "skip"}}));
  EXPECT_TRUE(job->is_running());

  f.transport->analyses[0].on_progress({0.5, "half"});
  orc::view::ToolAnalysisReport report;
  report.status = orc::view::ToolAnalysisReport::Status::Success;
  report.summary = "found";
  f.transport->analyses[0].on_done(report);
  f.transport->analyses[0].on_done(report);  // A second answer is dropped.

  EXPECT_EQ(events, (std::vector<std::string>{"half", "report found"}));
  EXPECT_FALSE(job->is_running());
}

TEST(StageToolSessionTest, AnAnalysisAndAJobShareTheOneJobSlot) {
  Fixture f;
  auto analysis = f.session->run_analysis({}, nullptr, [](auto) {});
  EXPECT_NE(analysis, nullptr);
  EXPECT_EQ(f.session->run_job("a", {}, nullptr, [](ToolReply) {}), nullptr);
  EXPECT_EQ(f.session->run_analysis({}, nullptr, [](auto) {}), nullptr);

  analysis->cancel();
  EXPECT_EQ(f.transport->cancelled, (std::vector<uint64_t>{500}));
  f.transport->analyses[0].on_done({});
  EXPECT_NE(f.session->run_job("a", {}, nullptr, [](ToolReply) {}), nullptr);
}

TEST(StageToolSessionTest, ClosingDropsAnAnalysisReport) {
  Fixture f;
  int reports = 0;
  f.session->run_analysis({}, nullptr, [&reports](auto) { ++reports; });
  f.session->close();
  EXPECT_EQ(f.transport->cancelled, (std::vector<uint64_t>{500}));
  f.transport->analyses[0].on_done({});
  EXPECT_EQ(reports, 0);
}

TEST(StageToolSessionTest, AnalysisInfoAndRefreshStateComeFromTheTransport) {
  Fixture f;
  f.transport->info.description = "Measures things";
  f.transport->refresh.available = true;
  f.transport->refresh.other_tool = "field_mapping";

  EXPECT_EQ(f.session->analysis_info().description, "Measures things");
  EXPECT_EQ(f.transport->info_reads,
            (std::vector<std::string>{"test.view.v1"}));
  EXPECT_TRUE(f.session->refresh_state().available);
  EXPECT_EQ(f.session->refresh_state().other_tool, "field_mapping");

  f.session->close();
  EXPECT_TRUE(f.session->analysis_info().description.empty());
  EXPECT_FALSE(f.session->refresh_state().available);
  EXPECT_EQ(f.session->run_analysis({}, nullptr, [](auto) {}), nullptr);
}

TEST(StageToolSessionTest, SetRefreshAnswersOnlyAnOpenSession) {
  Fixture f;
  int answers = 0;
  f.session->set_refresh(true, {{"a", "1"}},
                         [&answers](ToolReply) { ++answers; });
  ASSERT_EQ(f.transport->refresh_writes.size(), 1u);
  EXPECT_TRUE(f.transport->refresh_writes[0].use_this_tool);
  EXPECT_EQ(f.transport->refresh_writes[0].parameters,
            (orc::view::ToolParameterMap{{"a", "1"}}));
  f.transport->refresh_writes[0].on_done(ok_reply(""));
  EXPECT_EQ(answers, 1);

  f.session->set_refresh(false, {}, [&answers](ToolReply) { ++answers; });
  f.session->close();
  f.transport->refresh_writes[1].on_done(ok_reply(""));
  EXPECT_EQ(answers, 1);
}

}  // namespace orc_unit_test
