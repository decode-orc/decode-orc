/*
 * File:        plugin_view_host_test.cpp
 * Module:      orc-gui unit tests (gui-model)
 * Purpose:     The plugin view host's transport: jobs run on worker threads
 *              and answer on the GUI thread, shutdown with a job in flight
 *              cancels and waits for it, parameter writes and preview moves
 *              reach the main window later, and frames render on the host's
 *              render thread
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "plugin_view_host.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QThread>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "tool_frame_renderer.h"

namespace orc_unit_test {
namespace {

using orc::view::ToolJobProgress;
using orc::view::ToolReply;

QCoreApplication& ensureApplication() {
  if (auto* app = QCoreApplication::instance()) {
    return *app;
  }
  static int argc = 1;
  static char name[] = "plugin_view_host_test";
  static char* argv[] = {name, nullptr};
  static QCoreApplication app(argc, argv);
  return app;
}

// Pump the GUI thread's events until @p predicate holds.
template <typename Predicate>
bool pumpUntil(Predicate predicate, int timeout_ms = 5000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  QCoreApplication::processEvents();
  return predicate();
}

ToolReply okReply(const std::string& text) {
  ToolReply reply;
  reply.ok = true;
  reply.data.assign(text.begin(), text.end());
  return reply;
}

// Records the thread it renders on.
class FakeRenderer final : public IToolFrameRenderer {
 public:
  orc::view::ToolFrameImage render(const orc::NodeID& node,
                                   uint64_t frame_index) override {
    thread = QThread::currentThread();
    rendered_node = node;
    orc::view::ToolFrameImage image;
    image.ok = true;
    image.frame_index = frame_index;
    image.width = 1;
    image.height = 1;
    image.rgb = {1, 2, 3};
    return image;
  }
  std::atomic<QThread*> thread{nullptr};
  orc::NodeID rendered_node;
};

}  // namespace

TEST(PluginViewHostTest, AJobRunsOffTheGuiThreadAndAnswersOnIt) {
  ensureApplication();
  std::atomic<QThread*> job_thread{nullptr};
  PluginViewHost::Actions actions;
  actions.prepare_job = [&job_thread](const orc::NodeID&, const std::string&,
                                      const std::string& verb,
                                      std::vector<uint8_t>) {
    return PluginViewHost::JobRunner(
        [&job_thread, verb](const auto& on_progress, const auto&) {
          job_thread = QThread::currentThread();
          on_progress(ToolJobProgress{0.5, "half"});
          return okReply(verb);
        });
  };
  PluginViewHost host(nullptr, nullptr, std::move(actions));

  std::vector<std::string> events;
  QThread* reply_thread = nullptr;
  host.transport()->start_job(
      orc::NodeID(3), "test.v1", "map", {},
      [&events](ToolJobProgress p) { events.push_back(p.text); },
      [&](ToolReply reply) {
        reply_thread = QThread::currentThread();
        events.push_back(std::string(reply.data.begin(), reply.data.end()));
      });
  EXPECT_TRUE(events.empty()) << "nothing is delivered inside start_job";

  ASSERT_TRUE(pumpUntil([&] { return events.size() == 2; }));
  EXPECT_EQ(events, (std::vector<std::string>{"half", "map"}));
  EXPECT_NE(job_thread.load(), QThread::currentThread());
  EXPECT_EQ(reply_thread, QThread::currentThread());
  EXPECT_TRUE(pumpUntil([&] { return host.runningJobCount() == 0; }));
}

TEST(PluginViewHostTest, CancelReachesTheRunningJob) {
  ensureApplication();
  std::atomic<bool> started{false};
  PluginViewHost::Actions actions;
  actions.prepare_job = [&started](const orc::NodeID&, const std::string&,
                                   const std::string&, std::vector<uint8_t>) {
    return PluginViewHost::JobRunner(
        [&started](const auto&, const auto& is_cancelled) {
          started = true;
          while (!is_cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          ToolReply reply;
          reply.error = "Cancelled";
          return reply;
        });
  };
  PluginViewHost host(nullptr, nullptr, std::move(actions));

  std::optional<ToolReply> reply;
  const uint64_t id = host.transport()->start_job(
      orc::NodeID(3), "test.v1", "map", {}, nullptr,
      [&reply](ToolReply r) { reply = std::move(r); });
  ASSERT_TRUE(pumpUntil([&] { return started.load(); }));
  host.transport()->cancel_job(id);

  ASSERT_TRUE(pumpUntil([&] { return reply.has_value(); }));
  EXPECT_FALSE(reply->ok);
  EXPECT_EQ(reply->error, "Cancelled");
}

// Closing the window (or the project) while a job runs must not leave a
// thread running against a host that is gone.
TEST(PluginViewHostTest, DestroyingTheHostCancelsAndWaitsForItsJobs) {
  ensureApplication();
  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};
  bool replied = false;
  {
    PluginViewHost::Actions actions;
    actions.prepare_job = [&](const orc::NodeID&, const std::string&,
                              const std::string&, std::vector<uint8_t>) {
      return PluginViewHost::JobRunner(
          [&](const auto&, const auto& is_cancelled) {
            started = true;
            while (!is_cancelled()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            finished = true;
            return okReply("late");
          });
    };
    PluginViewHost host(nullptr, nullptr, std::move(actions));
    host.transport()->start_job(orc::NodeID(3), "test.v1", "map", {}, nullptr,
                                [&replied](ToolReply) { replied = true; });
    ASSERT_TRUE(pumpUntil([&] { return started.load(); }));
  }
  EXPECT_TRUE(finished.load());
  QCoreApplication::processEvents();
  EXPECT_FALSE(replied);
}

TEST(PluginViewHostTest, AJobThatCannotBePreparedIsRefusedLater) {
  ensureApplication();
  PluginViewHost host(nullptr, nullptr, {});

  std::optional<ToolReply> reply;
  host.transport()->start_job(orc::NodeID(3), "test.v1", "map", {}, nullptr,
                              [&reply](ToolReply r) { reply = std::move(r); });
  EXPECT_FALSE(reply.has_value());
  ASSERT_TRUE(pumpUntil([&] { return reply.has_value(); }));
  EXPECT_FALSE(reply->ok);
}

TEST(PluginViewHostTest, ApplyReachesTheMainWindowAfterTheCallReturns) {
  ensureApplication();
  struct Seen {
    orc::NodeID node;
    orc::view::ToolParameterMap values;
    std::string label;
  };
  std::optional<Seen> seen;
  PluginViewHost::Actions actions;
  actions.apply_parameters = [&seen](const orc::NodeID& node,
                                     const orc::view::ToolParameterMap& values,
                                     const std::string& label) {
    seen = Seen{node, values, label};
    return okReply("");
  };
  PluginViewHost host(nullptr, nullptr, std::move(actions));

  std::optional<ToolReply> reply;
  host.transport()->apply_parameters(
      orc::NodeID(7), {{"ranges", "0-9"}}, "Apply map",
      [&reply](ToolReply r) { reply = std::move(r); });
  EXPECT_FALSE(seen.has_value()) << "the write never runs inside the call";

  ASSERT_TRUE(pumpUntil([&] { return reply.has_value(); }));
  EXPECT_TRUE(reply->ok);
  ASSERT_TRUE(seen.has_value());
  EXPECT_EQ(seen->node, orc::NodeID(7));
  EXPECT_EQ(seen->values, (orc::view::ToolParameterMap{{"ranges", "0-9"}}));
  EXPECT_EQ(seen->label, "Apply map");
}

TEST(PluginViewHostTest, ShowFrameReachesTheMainWindow) {
  ensureApplication();
  std::vector<uint64_t> frames;
  PluginViewHost::Actions actions;
  actions.show_frame = [&frames](const orc::NodeID&, uint64_t frame) {
    frames.push_back(frame);
  };
  PluginViewHost host(nullptr, nullptr, std::move(actions));

  host.transport()->show_frame(orc::NodeID(1), 42);
  EXPECT_TRUE(frames.empty());
  ASSERT_TRUE(pumpUntil([&] { return !frames.empty(); }));
  EXPECT_EQ(frames, (std::vector<uint64_t>{42}));
}

TEST(PluginViewHostTest, FramesRenderOnTheHostsRenderThreadForTheTarget) {
  ensureApplication();
  auto renderer = std::make_shared<FakeRenderer>();
  int renderers_made = 0;
  PluginViewHost::Actions actions;
  actions.render_target = [](const orc::NodeID& node,
                             bool from_input) -> std::optional<orc::NodeID> {
    return from_input ? orc::NodeID(node.value() - 1) : node;
  };
  actions.make_frame_renderer = [&]() -> std::shared_ptr<IToolFrameRenderer> {
    ++renderers_made;
    return renderer;
  };
  PluginViewHost host(nullptr, nullptr, std::move(actions));

  std::optional<orc::view::ToolFrameImage> image;
  host.transport()->render_frame(
      orc::NodeID(5), 12, {},
      [&image](orc::view::ToolFrameImage i) { image = std::move(i); });
  ASSERT_TRUE(pumpUntil([&] { return image.has_value(); }));
  EXPECT_TRUE(image->ok);
  EXPECT_EQ(image->frame_index, 12u);
  EXPECT_EQ(renderer->rendered_node, orc::NodeID(4)) << "the node's input";
  EXPECT_NE(renderer->thread.load(), nullptr);
  EXPECT_NE(renderer->thread.load(), QThread::currentThread());

  // The renderer draws one graph; after a rebuild a new one is made.
  image.reset();
  host.onGraphRebuilt();
  host.transport()->render_frame(
      orc::NodeID(5), 13, {},
      [&image](orc::view::ToolFrameImage i) { image = std::move(i); });
  ASSERT_TRUE(pumpUntil([&] { return image.has_value(); }));
  EXPECT_EQ(renderers_made, 2);
}

TEST(PluginViewHostTest, ARenderWithNoInputToDrawFails) {
  ensureApplication();
  PluginViewHost::Actions actions;
  actions.render_target = [](const orc::NodeID&,
                             bool) -> std::optional<orc::NodeID> {
    return std::nullopt;
  };
  actions.make_frame_renderer = [] {
    return std::shared_ptr<IToolFrameRenderer>(
        std::make_shared<FakeRenderer>());
  };
  PluginViewHost host(nullptr, nullptr, std::move(actions));

  std::optional<orc::view::ToolFrameImage> image;
  host.transport()->render_frame(
      orc::NodeID(5), 1, {},
      [&image](orc::view::ToolFrameImage i) { image = std::move(i); });
  ASSERT_TRUE(pumpUntil([&] { return image.has_value(); }));
  EXPECT_FALSE(image->ok);
  EXPECT_FALSE(image->error.empty());
}

TEST(PluginViewHostTest, AnAnalysisRunsOffTheGuiThreadAndReportsOnIt) {
  ensureApplication();
  std::atomic<QThread*> run_thread{nullptr};
  orc::view::ToolParameterMap prepared_with;
  PluginViewHost::Actions actions;
  actions.prepare_analysis =
      [&](const orc::NodeID&, const std::string& contract_id,
          const orc::view::ToolParameterMap& parameters, std::string*) {
        prepared_with = parameters;
        return PluginViewHost::AnalysisRunner(
            [&run_thread, contract_id](const auto& on_progress, const auto&) {
              run_thread = QThread::currentThread();
              on_progress(ToolJobProgress{0.5, "half"});
              orc::view::ToolAnalysisReport report;
              report.status = orc::view::ToolAnalysisReport::Status::Success;
              report.summary = contract_id;
              return report;
            });
      };
  PluginViewHost host(nullptr, nullptr, std::move(actions));

  std::vector<std::string> events;
  host.transport()->start_analysis(
      orc::NodeID(3), "test.analysis.v1", {{"mode", "fast"}},
      [&events](ToolJobProgress p) { events.push_back(p.text); },
      [&events](orc::view::ToolAnalysisReport report) {
        events.push_back("report " + report.summary);
      });
  EXPECT_TRUE(events.empty()) << "nothing is delivered inside the call";
  EXPECT_EQ(prepared_with, (orc::view::ToolParameterMap{{"mode", "fast"}}));

  ASSERT_TRUE(pumpUntil([&] { return events.size() == 2; }));
  EXPECT_EQ(events,
            (std::vector<std::string>{"half", "report test.analysis.v1"}));
  EXPECT_NE(run_thread.load(), QThread::currentThread());
  EXPECT_TRUE(pumpUntil([&] { return host.runningJobCount() == 0; }));
}

TEST(PluginViewHostTest, AnAnalysisThatCannotBePreparedSaysWhyLater) {
  ensureApplication();
  PluginViewHost::Actions actions;
  actions.prepare_analysis = [](const orc::NodeID&, const std::string&,
                                const orc::view::ToolParameterMap&,
                                std::string* why) {
    *why = "'passes' must be at most 9";
    return PluginViewHost::AnalysisRunner();
  };
  PluginViewHost host(nullptr, nullptr, std::move(actions));

  std::optional<orc::view::ToolAnalysisReport> report;
  host.transport()->start_analysis(
      orc::NodeID(3), "test.analysis.v1", {{"passes", "12"}}, nullptr,
      [&report](orc::view::ToolAnalysisReport r) { report = std::move(r); });
  EXPECT_FALSE(report.has_value());
  ASSERT_TRUE(pumpUntil([&] { return report.has_value(); }));
  EXPECT_EQ(report->status, orc::view::ToolAnalysisReport::Status::Failed);
  EXPECT_EQ(report->summary, "'passes' must be at most 9");
}

TEST(PluginViewHostTest, TheRefreshOptionReachesTheMainWindow) {
  ensureApplication();
  std::optional<bool> stored;
  PluginViewHost::Actions actions;
  actions.analysis_info = [](const orc::NodeID&, const std::string&) {
    orc::view::ToolAnalysisInfo info;
    info.description = "Measures";
    return info;
  };
  actions.refresh_state = [](const orc::NodeID&, const std::string&) {
    orc::view::ToolRefreshState state;
    state.available = true;
    return state;
  };
  actions.set_refresh = [&stored](const orc::NodeID&, const std::string&,
                                  bool use_this_tool,
                                  const orc::view::ToolParameterMap&) {
    stored = use_this_tool;
    return okReply("");
  };
  PluginViewHost host(nullptr, nullptr, std::move(actions));

  EXPECT_EQ(host.transport()->analysis_info(orc::NodeID(3), "c").description,
            "Measures");
  EXPECT_TRUE(host.transport()->refresh_state(orc::NodeID(3), "c").available);

  std::optional<ToolReply> reply;
  host.transport()->set_refresh(
      orc::NodeID(3), "c", true, {},
      [&reply](ToolReply r) { reply = std::move(r); });
  EXPECT_FALSE(stored.has_value()) << "the write never runs inside the call";
  ASSERT_TRUE(pumpUntil([&] { return reply.has_value(); }));
  EXPECT_TRUE(reply->ok);
  EXPECT_EQ(stored, std::optional<bool>(true));
}

}  // namespace orc_unit_test
