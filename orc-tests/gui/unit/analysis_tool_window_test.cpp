/*
 * File:        analysis_tool_window_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     orc-view-support's AnalysisToolWindow through a fake session:
 *              the controls a tool's parameters get, a run, Apply to Stage
 *              and the refresh option; and the core view's Frame Map Range
 *              window, which subclasses it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/view-support/analysis_tool_window.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "frame_map_range_window.h"

namespace gui_unit_test {
namespace {

using orc::view::ToolAnalysisParameter;
using orc::view::ToolAnalysisReport;
using orc::view::ToolParameterMap;
using orc::view::ToolReply;

QApplication& ensureApplication() {
  if (auto* existing =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing;
  }
  static int argc = 3;
  static char app_name[] = "orc-gui-widget-test";
  static char platform_opt[] = "-platform";
  static char platform_val[] = "offscreen";
  static char* argv[] = {app_name, platform_opt, platform_val, nullptr};
  static QApplication* app = new QApplication(argc, argv);
  return *app;
}

/// Close whatever message box the window raises, once it is up.
void dismissMessageBoxes() {
  QTimer::singleShot(0, [] {
    for (auto* widget : QApplication::topLevelWidgets()) {
      if (widget->isVisible() && widget->inherits("QMessageBox")) {
        qobject_cast<QDialog*>(widget)->reject();
      }
    }
  });
}

class FakeJob final : public orc::view::IToolJob {
 public:
  void cancel() override { cancelled = true; }
  bool is_running() const override { return running; }
  bool cancelled = false;
  bool running = true;
};

// Holds every call; the test answers them.
class FakeSession final : public orc::view::IStageToolSession {
 public:
  std::string contract_id() const override { return "test.analysis.v1"; }
  std::string title() const override { return "Measure - Node (node_3)"; }
  bool is_open() const override { return true; }
  void request(const std::string&, std::vector<uint8_t>,
               orc::view::ToolReplyHandler) override {}
  void set_results_changed_handler(std::function<void()>) override {}
  std::shared_ptr<orc::view::IToolJob> run_job(
      const std::string&, std::vector<uint8_t>, orc::view::ToolProgressHandler,
      orc::view::ToolReplyHandler) override {
    return nullptr;
  }
  void apply_parameters(ToolParameterMap values, const std::string& label,
                        orc::view::ToolReplyHandler on_done) override {
    applied.push_back(std::move(values));
    apply_labels.push_back(label);
    applies.push_back(std::move(on_done));
  }
  void show_frame(uint64_t) override {}
  void set_preview_frame_handler(std::function<void(uint64_t)>) override {}
  void render_frame(uint64_t, const orc::view::ToolFrameRenderOptions&,
                    orc::view::ToolFrameHandler) override {}
  void set_graph_rebuilt_handler(std::function<void()> handler) override {
    keeps_open = static_cast<bool>(handler);
  }
  orc::view::AmplitudeUnit amplitude_unit() const override {
    return orc::view::AmplitudeUnit::IRE;
  }
  void set_amplitude_unit_handler(
      std::function<void(orc::view::AmplitudeUnit)>) override {}
  ToolParameterMap parameters() const override { return {}; }
  std::string project_directory() const override { return {}; }

  orc::view::ToolAnalysisInfo analysis_info() const override { return info; }
  std::shared_ptr<orc::view::IToolJob> run_analysis(
      ToolParameterMap parameters, orc::view::ToolProgressHandler on_progress,
      orc::view::ToolAnalysisHandler on_report) override {
    runs.push_back(std::move(parameters));
    progress = std::move(on_progress);
    report = std::move(on_report);
    job = std::make_shared<FakeJob>();
    return job;
  }
  orc::view::ToolRefreshState refresh_state() const override { return refresh; }
  void set_refresh(bool use_this_tool, ToolParameterMap parameters,
                   orc::view::ToolReplyHandler on_done) override {
    refresh_writes.push_back({use_this_tool, std::move(parameters)});
    refresh_answers.push_back(std::move(on_done));
  }

  /// End the run with @p status, @p summary and @p changes.
  void finish(ToolAnalysisReport::Status status, const std::string& summary,
              ToolParameterMap changes = {}) {
    job->running = false;
    ToolAnalysisReport answer;
    answer.status = status;
    answer.summary = summary;
    answer.parameter_changes = std::move(changes);
    report(std::move(answer));
  }

  static ToolReply ok() {
    ToolReply reply;
    reply.ok = true;
    return reply;
  }

  orc::view::ToolAnalysisInfo info;
  orc::view::ToolRefreshState refresh;
  bool keeps_open = false;
  std::vector<ToolParameterMap> runs;
  orc::view::ToolProgressHandler progress;
  orc::view::ToolAnalysisHandler report;
  std::shared_ptr<FakeJob> job;
  std::vector<ToolParameterMap> applied;
  std::vector<std::string> apply_labels;
  std::vector<orc::view::ToolReplyHandler> applies;
  std::vector<std::pair<bool, ToolParameterMap>> refresh_writes;
  std::vector<orc::view::ToolReplyHandler> refresh_answers;
};

ToolAnalysisParameter parameter(const std::string& name,
                                ToolAnalysisParameter::Type type,
                                const std::string& default_value) {
  ToolAnalysisParameter p;
  p.name = name;
  p.display_name = name;
  p.type = type;
  p.default_value = default_value;
  return p;
}

class AnalysisToolWindowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ensureApplication();
    session_ = std::make_shared<FakeSession>();
    session_->info.description = "Measures the input";
    auto mode = parameter("mode", ToolAnalysisParameter::Type::Text, "fast");
    mode.allowed_values = {"fast", "thorough"};
    auto passes =
        parameter("passes", ToolAnalysisParameter::Type::Integer, "3");
    passes.min_value = "1";
    passes.max_value = "9";
    passes.depends_on = "mode";
    passes.depends_on_values = {"thorough"};
    session_->info.parameters = {
        parameter("lead_in", ToolAnalysisParameter::Type::Bool, "true"), mode,
        passes};
  }

  std::unique_ptr<orc::view::AnalysisToolWindow> makeWindow() {
    auto window = std::make_unique<orc::view::AnalysisToolWindow>(session_);
    window->show();
    return window;
  }

  static QPushButton* button(QWidget& window, const QString& text) {
    for (auto* b : window.findChildren<QPushButton*>()) {
      if (b->text() == text) {
        return b;
      }
    }
    return nullptr;
  }

  std::shared_ptr<FakeSession> session_;
};

TEST_F(AnalysisToolWindowTest, ANullSessionIsRefused) {
  ensureApplication();
  EXPECT_THROW(orc::view::AnalysisToolWindow(nullptr), std::invalid_argument);
}

TEST_F(AnalysisToolWindowTest, ItShowsTheToolAndKeepsOpenAcrossRebuilds) {
  auto window = makeWindow();
  EXPECT_EQ(window->windowTitle(), "Measure - Node (node_3)");
  bool described = false;
  for (auto* label : window->findChildren<QLabel*>()) {
    described |= label->text() == "Measures the input";
  }
  EXPECT_TRUE(described);
  EXPECT_TRUE(session_->keeps_open);
  // No refresh option for a tool that cannot refresh the node.
  EXPECT_EQ(window->refreshCheck(), nullptr);
}

TEST_F(AnalysisToolWindowTest, RunSendsTheParametersAsShown) {
  auto window = makeWindow();
  auto* passes = window->findChild<QSpinBox*>();
  ASSERT_NE(passes, nullptr);
  EXPECT_EQ(passes->value(), 3);
  EXPECT_EQ(passes->minimum(), 1);
  EXPECT_EQ(passes->maximum(), 9);
  // "passes" only means something in thorough mode.
  EXPECT_FALSE(passes->isEnabled());
  auto* mode = window->findChild<QComboBox*>();
  ASSERT_NE(mode, nullptr);
  mode->setCurrentText("thorough");
  EXPECT_TRUE(passes->isEnabled());
  passes->setValue(5);

  button(*window, "Run Analysis")->click();

  ASSERT_EQ(session_->runs.size(), 1u);
  EXPECT_EQ(session_->runs[0],
            (ToolParameterMap{
                {"lead_in", "true"}, {"mode", "thorough"}, {"passes", "5"}}));
  EXPECT_TRUE(window->isRunning());
  EXPECT_FALSE(button(*window, "Run Analysis")->isEnabled());
  EXPECT_TRUE(button(*window, "Cancel")->isEnabled());
  EXPECT_FALSE(button(*window, "OK")->isEnabled());

  session_->progress({0.4, "Reading the source"});
  EXPECT_EQ(window->findChild<QProgressBar*>()->value(), 40);
}

TEST_F(AnalysisToolWindowTest, ASuccessfulReportCanBeApplied) {
  auto window = makeWindow();
  button(*window, "Run Analysis")->click();
  session_->finish(ToolAnalysisReport::Status::Success, "Carrier 7.0915 MHz",
                   {{"carrier_black_mhz", "7.0915"}});

  EXPECT_FALSE(window->isRunning());
  EXPECT_EQ(window->reportText(),
            "Analysis completed successfully.\n\nCarrier 7.0915 MHz");
  auto* apply = button(*window, "Apply to Stage");
  ASSERT_TRUE(apply->isEnabled());
  apply->click();

  ASSERT_EQ(session_->applied.size(), 1u);
  EXPECT_EQ(session_->applied[0],
            (ToolParameterMap{{"carrier_black_mhz", "7.0915"}}));
  EXPECT_EQ(session_->apply_labels[0], "Measure - Node (node_3)");
  EXPECT_TRUE(window->isVisible());  // Until the host has written it.

  session_->applies[0](FakeSession::ok());
  EXPECT_FALSE(window->isVisible());
  EXPECT_EQ(window->result(), QDialog::Accepted);
  EXPECT_TRUE(session_->refresh_writes.empty());
}

TEST_F(AnalysisToolWindowTest, ARefusedApplyKeepsTheWindowOpen) {
  auto window = makeWindow();
  button(*window, "Run Analysis")->click();
  session_->finish(ToolAnalysisReport::Status::Success, "found",
                   {{"carrier_black_mhz", "7.5"}});
  button(*window, "Apply to Stage")->click();

  dismissMessageBoxes();
  ToolReply refused;
  refused.error = "'carrier_black_mhz' must be at most 7.3";
  session_->applies[0](refused);

  EXPECT_TRUE(window->isVisible());
  EXPECT_TRUE(button(*window, "Apply to Stage")->isEnabled());
}

TEST_F(AnalysisToolWindowTest, AFailedOrEmptyReportWritesNothing) {
  auto window = makeWindow();
  button(*window, "Run Analysis")->click();
  session_->finish(ToolAnalysisReport::Status::Failed, "No input");
  EXPECT_EQ(window->reportText(), "Analysis failed.\n\nNo input");
  EXPECT_FALSE(button(*window, "Apply to Stage")->isEnabled());

  button(*window, "Run Analysis")->click();
  session_->finish(ToolAnalysisReport::Status::Success, "No beat found");
  dismissMessageBoxes();
  window->applyResults();
  EXPECT_TRUE(session_->applied.empty());
  EXPECT_TRUE(window->isVisible());
}

TEST_F(AnalysisToolWindowTest, CancelAsksTheRunToStop) {
  auto window = makeWindow();
  button(*window, "Run Analysis")->click();
  button(*window, "Cancel")->click();
  EXPECT_TRUE(session_->job->cancelled);

  session_->finish(ToolAnalysisReport::Status::Cancelled, "");
  EXPECT_EQ(window->reportText(), "Analysis cancelled.\n\n");
  EXPECT_TRUE(button(*window, "Run Analysis")->isEnabled());
  EXPECT_FALSE(button(*window, "Apply to Stage")->isEnabled());
}

TEST_F(AnalysisToolWindowTest, TheStoredRefreshParametersAreShown) {
  session_->refresh.available = true;
  session_->refresh.this_tool = true;
  session_->refresh.parameters = {
      {"lead_in", "false"}, {"mode", "thorough"}, {"passes", "4"}};
  auto window = makeWindow();

  ASSERT_NE(window->refreshCheck(), nullptr);
  EXPECT_TRUE(window->refreshCheck()->isChecked());
  for (auto* box : window->findChildren<QCheckBox*>()) {
    if (box != window->refreshCheck()) {
      EXPECT_FALSE(box->isChecked());  // lead_in, stored as false
    }
  }
  EXPECT_EQ(window->findChild<QComboBox*>()->currentText(), "thorough");
  EXPECT_EQ(window->findChild<QSpinBox*>()->value(), 4);

  // Unchanged: OK stores nothing.
  window->accept();
  EXPECT_TRUE(session_->refresh_writes.empty());
  EXPECT_FALSE(window->isVisible());
}

TEST_F(AnalysisToolWindowTest, TickingTheRefreshOptionStoresTheLastRun) {
  session_->refresh.available = true;
  session_->refresh.other_tool = "field_mapping";
  auto window = makeWindow();
  ASSERT_NE(window->refreshCheck(), nullptr);
  EXPECT_FALSE(window->refreshCheck()->isChecked());
  EXPECT_TRUE(window->refreshCheck()->toolTip().contains("field_mapping"));

  button(*window, "Run Analysis")->click();
  session_->finish(ToolAnalysisReport::Status::Success, "found",
                   {{"carrier_black_mhz", "7.1"}});
  // Changed after the run: the run's parameters are what is stored.
  window->findChild<QComboBox*>()->setCurrentText("thorough");
  window->refreshCheck()->setChecked(true);
  button(*window, "Apply to Stage")->click();
  session_->applies[0](FakeSession::ok());

  ASSERT_EQ(session_->refresh_writes.size(), 1u);
  EXPECT_TRUE(session_->refresh_writes[0].first);
  EXPECT_EQ(session_->refresh_writes[0].second.at("mode"), "fast");
  EXPECT_TRUE(window->isVisible());  // Until the option is stored.
  session_->refresh_answers[0](FakeSession::ok());
  EXPECT_FALSE(window->isVisible());
}

TEST_F(AnalysisToolWindowTest, UntickingTheRefreshOptionClearsIt) {
  session_->refresh.available = true;
  session_->refresh.this_tool = true;
  auto window = makeWindow();
  window->refreshCheck()->setChecked(false);
  window->accept();

  ASSERT_EQ(session_->refresh_writes.size(), 1u);
  EXPECT_FALSE(session_->refresh_writes[0].first);
  session_->refresh_answers[0](FakeSession::ok());
  EXPECT_FALSE(window->isVisible());
}

// A window that builds its own controls: its hooks are the ones used, though
// the base class constructs first.
class OwnControlsWindow final : public orc::view::AnalysisToolWindow {
 public:
  using AnalysisToolWindow::AnalysisToolWindow;
  int built = 0;

 protected:
  void buildParameters(const orc::view::ToolAnalysisInfo&,
                       QFormLayout*) override {
    ++built;
  }
  ToolParameterMap currentParameters() const override {
    return {{"own", "yes"}};
  }
};

TEST_F(AnalysisToolWindowTest, ASubclassBuildsItsOwnControls) {
  OwnControlsWindow window(session_);
  window.show();
  EXPECT_EQ(window.built, 1);
  EXPECT_EQ(window.findChild<QSpinBox*>(), nullptr) << "no default controls";
  window.runAnalysis();
  ASSERT_EQ(session_->runs.size(), 1u);
  EXPECT_EQ(session_->runs[0], (ToolParameterMap{{"own", "yes"}}));
  EXPECT_EQ(window.built, 1) << "built once";
}

class FrameMapRangeWindowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ensureApplication();
    session_ = std::make_shared<FakeSession>();
    session_->info.description = "Find a frame range";
  }

  std::unique_ptr<FrameMapRangeWindow> makeWindow(const std::string& system) {
    session_->info.video_system = system;
    auto window = std::make_unique<FrameMapRangeWindow>(session_);
    window->show();
    return window;
  }

  std::shared_ptr<FakeSession> session_;
};

TEST_F(FrameMapRangeWindowTest, ATimeCodeFollowsThePictureNumber) {
  auto window = makeWindow("PAL");
  EXPECT_EQ(window->picturesPerSecond(), 25);
  window->pictureSpin(true)->setValue(25 * 61 + 3);  // 0:1:1.2
  EXPECT_EQ(window->timecodeSpin(true, 0)->value(), 0);
  EXPECT_EQ(window->timecodeSpin(true, 1)->value(), 1);
  EXPECT_EQ(window->timecodeSpin(true, 2)->value(), 1);
  EXPECT_EQ(window->timecodeSpin(true, 3)->value(), 2);
  EXPECT_EQ(window->timecodeSpin(true, 3)->maximum(), 24);
}

TEST_F(FrameMapRangeWindowTest, ThePictureNumberFollowsTheTimeCode) {
  auto window = makeWindow("NTSC");
  EXPECT_EQ(window->picturesPerSecond(), 30);
  window->timecodeSpin(false, 2)->setValue(2);
  window->timecodeSpin(false, 3)->setValue(5);
  EXPECT_EQ(window->pictureSpin(false)->value(), 30 * 2 + 5 + 1);
  EXPECT_EQ(window->pictureSpin(true)->value(), 1) << "the start is its own";
}

TEST_F(FrameMapRangeWindowTest, ARunTakesThePictureNumbers) {
  auto window = makeWindow("PAL");
  window->pictureSpin(true)->setValue(100);
  window->pictureSpin(false)->setValue(250);
  window->runAnalysis();
  ASSERT_EQ(session_->runs.size(), 1u);
  EXPECT_EQ(session_->runs[0],
            (ToolParameterMap{{"startAddress", "100"}, {"endAddress", "250"}}));
}

TEST_F(FrameMapRangeWindowTest, ItShowsTheStoredRefreshAddresses) {
  session_->refresh.available = true;
  session_->refresh.this_tool = true;
  session_->refresh.parameters = {{"startAddress", "26"},
                                  {"endAddress", "0:0:1.0"}};
  auto window = makeWindow("PAL");
  EXPECT_EQ(window->pictureSpin(true)->value(), 26);
  EXPECT_EQ(window->timecodeSpin(true, 2)->value(), 1);
  EXPECT_EQ(window->pictureSpin(false)->value(), 1)
      << "a time code typed at the command line leaves the window's value";
}

}  // namespace
}  // namespace gui_unit_test
