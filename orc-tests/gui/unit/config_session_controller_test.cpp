/*
 * File:        config_session_controller_test.cpp
 * Module:      orc-gui unit tests
 * Purpose:     The Mask Line and FFmpeg preset dialogs (orc-view_core) driven
 *              through a fake tool session: stored values in, the stage's
 *              answers, and what OK writes back
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "config_session_controller.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../../../orc/plugins/stages/common/config-tools/stage_config_wire.h"
#include "ffmpegpresetdialog.h"
#include "masklineconfigdialog.h"
#include "mocks/no_analysis_session.h"

namespace gui_unit_test {
namespace {

namespace wire = orc::config_wire;

QApplication& ensureApplication() {
  if (auto* existing =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing;
  }
  static int argc = 3;
  static char app_name[] = "orc-gui-config-session-controller-test";
  static char platform_opt[] = "-platform";
  static char platform_val[] = "offscreen";
  static char* argv[] = {app_name, platform_opt, platform_val, nullptr};
  static QApplication* app = [] {
    auto* created = new QApplication(argc, argv);
    created->setQuitOnLastWindowClosed(false);
    return created;
  }();
  return *app;
}

// A session the test answers by hand.
class FakeSession final : public gui_unit_test::NoAnalysisSession {
 public:
  std::string contract_id() const override { return "test"; }
  std::string title() const override { return "Test - Node (node_1)"; }
  bool is_open() const override { return true; }

  void request(const std::string& verb, std::vector<uint8_t>,
               orc::view::ToolReplyHandler on_reply) override {
    request_verbs.push_back(verb);
    requests.push_back(std::move(on_reply));
  }
  void set_results_changed_handler(std::function<void()>) override {}
  std::shared_ptr<orc::view::IToolJob> run_job(
      const std::string& verb, std::vector<uint8_t>,
      orc::view::ToolProgressHandler,
      orc::view::ToolReplyHandler on_reply) override {
    if (refuse_jobs) return nullptr;
    job_verbs.push_back(verb);
    jobs.push_back(std::move(on_reply));
    return std::make_shared<Job>();
  }
  void apply_parameters(orc::view::ToolParameterMap values,
                        const std::string& label,
                        orc::view::ToolReplyHandler on_done) override {
    applied.push_back(std::move(values));
    apply_labels.push_back(label);
    applies.push_back(std::move(on_done));
  }
  void show_frame(uint64_t) override {}
  void set_preview_frame_handler(std::function<void(uint64_t)>) override {}
  void render_frame(uint64_t, const orc::view::ToolFrameRenderOptions&,
                    orc::view::ToolFrameHandler) override {}
  void set_graph_rebuilt_handler(std::function<void()>) override {}
  orc::view::AmplitudeUnit amplitude_unit() const override {
    return orc::view::AmplitudeUnit::IRE;
  }
  void set_amplitude_unit_handler(
      std::function<void(orc::view::AmplitudeUnit)>) override {}
  orc::view::ToolParameterMap parameters() const override { return stored; }
  std::string project_directory() const override { return {}; }

  static orc::view::ToolReply okWith(std::vector<uint8_t> data) {
    orc::view::ToolReply reply;
    reply.ok = true;
    reply.data = std::move(data);
    return reply;
  }
  static orc::view::ToolReply refusal(const std::string& why) {
    orc::view::ToolReply reply;
    reply.error = why;
    return reply;
  }

  orc::view::ToolParameterMap stored;
  bool refuse_jobs = false;
  std::vector<std::string> request_verbs;
  std::vector<orc::view::ToolReplyHandler> requests;
  std::vector<std::string> job_verbs;
  std::vector<orc::view::ToolReplyHandler> jobs;
  std::vector<orc::view::ToolParameterMap> applied;
  std::vector<std::string> apply_labels;
  std::vector<orc::view::ToolReplyHandler> applies;

 private:
  class Job final : public orc::view::IToolJob {
   public:
    void cancel() override {}
    bool is_running() const override { return true; }
  };
};

QPushButton* okButton(QDialog& dialog) {
  auto* box = dialog.findChild<QDialogButtonBox*>();
  return box ? box->button(QDialogButtonBox::Ok) : nullptr;
}

std::vector<QString> rangeLabels(QDialog& dialog) {
  std::vector<QString> labels;
  auto* list = dialog.findChild<QListWidget*>();
  for (int i = 0; list && i < list->count(); ++i) {
    labels.push_back(list->item(i)->text());
  }
  return labels;
}

// A message box the controller opens is closed again at once.
void closeMessageBoxesSoon() {
  QTimer::singleShot(0, [] {
    for (auto* widget : QApplication::topLevelWidgets()) {
      if (auto* box = qobject_cast<QDialog*>(widget);
          box && box->isVisible() && box->inherits("QMessageBox")) {
        box->reject();
      }
    }
  });
}

}  // namespace

TEST(ConfigSessionControllerTest, MaskLineWaitsForTheSignalBeforeShowing) {
  (void)ensureApplication();
  auto session = std::make_shared<FakeSession>();
  // NTSC field 1 line 21 is frame-flat 20, mirrored to 283 in field 2.
  session->stored = {{"lineSpec", "20,283"}, {"maskSampleLevel", "240"}};
  MaskLineConfigDialog dialog;
  new MaskLineSessionController(&dialog, session);

  ASSERT_EQ(session->job_verbs.size(), 1u);
  EXPECT_EQ(session->job_verbs[0], wire::kSignalVerb);
  ASSERT_NE(okButton(dialog), nullptr);
  EXPECT_FALSE(okButton(dialog)->isEnabled());
  EXPECT_TRUE(rangeLabels(dialog).empty());

  session->jobs[0](FakeSession::okWith(
      wire::encode(wire::Signal{wire::System::NTSC, 240, 800})));

  EXPECT_TRUE(okButton(dialog)->isEnabled());
  EXPECT_EQ(rangeLabels(dialog), std::vector<QString>{"21"});
  auto* level = dialog.findChild<QComboBox*>();
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->itemText(0), "Blanking (240)");
  EXPECT_EQ(level->currentIndex(), 0);
}

TEST(ConfigSessionControllerTest, MaskLineNumbersAsPalWithoutASignal) {
  (void)ensureApplication();
  auto session = std::make_shared<FakeSession>();
  session->stored = {{"lineSpec", "5-21,318-334"}};
  MaskLineConfigDialog dialog;
  new MaskLineSessionController(&dialog, session);

  session->jobs[0](FakeSession::refusal("Mask Line has no video input"));

  EXPECT_TRUE(okButton(dialog)->isEnabled());
  EXPECT_EQ(rangeLabels(dialog), std::vector<QString>{"6–22"});
}

TEST(ConfigSessionControllerTest, MaskLineShowsAtOnceWhenNoJobCanRun) {
  (void)ensureApplication();
  auto session = std::make_shared<FakeSession>();
  session->refuse_jobs = true;
  session->stored = {{"lineSpec", "5-21,318-334"}};
  MaskLineConfigDialog dialog;
  new MaskLineSessionController(&dialog, session);

  EXPECT_TRUE(okButton(dialog)->isEnabled());
  EXPECT_EQ(rangeLabels(dialog).size(), 1u);
}

TEST(ConfigSessionControllerTest, OkWritesOnlyWhatTheDialogConfigures) {
  (void)ensureApplication();
  auto session = std::make_shared<FakeSession>();
  session->stored = {{"lineSpec", "5-21,318-334"},
                     {"maskSampleLevel", "256"},
                     {"some_other_parameter", "kept"}};
  MaskLineConfigDialog dialog;
  new MaskLineSessionController(&dialog, session);
  session->jobs[0](FakeSession::okWith(
      wire::encode(wire::Signal{wire::System::PAL, 256, 844})));

  okButton(dialog)->click();

  ASSERT_EQ(session->applied.size(), 1u);
  EXPECT_EQ(session->applied[0],
            (orc::view::ToolParameterMap{{"lineSpec", "5-21,318-334"},
                                         {"maskSampleLevel", "256"}}));
  EXPECT_EQ(session->apply_labels[0], "Applied mask line configuration");
  // Waiting for the host: OK stays off until it answers.
  EXPECT_FALSE(okButton(dialog)->isEnabled());
}

TEST(ConfigSessionControllerTest, ARefusedApplyKeepsTheDialogOpen) {
  (void)ensureApplication();
  auto session = std::make_shared<FakeSession>();
  MaskLineConfigDialog dialog;
  new MaskLineSessionController(&dialog, session);
  session->jobs[0](FakeSession::refusal("none"));
  dialog.show();
  okButton(dialog)->click();
  ASSERT_EQ(session->applies.size(), 1u);

  closeMessageBoxesSoon();
  session->applies[0](FakeSession::refusal("'x' is not a valid int32"));

  EXPECT_TRUE(dialog.isVisible());
  EXPECT_EQ(dialog.result(), 0);
  EXPECT_TRUE(okButton(dialog)->isEnabled());
}

TEST(ConfigSessionControllerTest, FfmpegLoadsStoredValuesAndAsksForEncoders) {
  (void)ensureApplication();
  auto session = std::make_shared<FakeSession>();
  session->stored = {{"ffmpeg_format", "mp4-h264"},
                     {"output_path", "exports/side1.mp4"}};
  FFmpegPresetDialog dialog;
  new FfmpegPresetSessionController(&dialog, session);
  dialog.show();
  QCoreApplication::processEvents();

  ASSERT_EQ(session->request_verbs.size(), 1u);
  EXPECT_EQ(session->request_verbs[0], wire::kHardwareEncodersVerb);
  QGroupBox* hardware = nullptr;
  for (auto* group : dialog.findChildren<QGroupBox*>()) {
    if (group->title() == "Hardware Acceleration") hardware = group;
  }
  ASSERT_NE(hardware, nullptr);
  EXPECT_FALSE(hardware->isVisible());

  session->requests[0](
      FakeSession::okWith(wire::encode(wire::HardwareEncoders{{"vaapi"}})));
  QCoreApplication::processEvents();
  EXPECT_TRUE(hardware->isVisible());

  okButton(dialog)->click();
  ASSERT_EQ(session->applied.size(), 1u);
  EXPECT_EQ(session->applied[0].at("ffmpeg_format"), "mp4-h264");
  EXPECT_EQ(session->applied[0].at("output_path"), "exports/side1.mp4");
  EXPECT_EQ(session->applied[0].at("output_mode"), "ffmpeg");
  EXPECT_EQ(session->apply_labels[0], "Applied FFmpeg export preset");
}

}  // namespace gui_unit_test
