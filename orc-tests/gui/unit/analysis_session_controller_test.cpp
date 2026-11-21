/*
 * File:        analysis_session_controller_test.cpp
 * Module:      orc-gui unit tests
 * Purpose:     The analysis sinks' graph windows (orc-view_core) driven
 *              through a fake tool session: results, no results, the
 *              preview's frame, frame clicks and the amplitude unit
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "analysis_session_controller.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../../../orc/plugins/stages/common/analysis-results/analysis_series_wire.h"
#include "burstlevelanalysisdialog.h"
#include "dropoutanalysisdialog.h"
#include "mocks/no_analysis_session.h"
#include "snranalysisdialog.h"

namespace gui_unit_test {
namespace {

namespace wire = orc::analysis_wire;

QApplication& ensureApplication() {
  if (auto* existing =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing;
  }
  static int argc = 3;
  static char app_name[] = "orc-gui-analysis-session-controller-test";
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

// A session the test answers by hand: requests wait until reply() is called.
class FakeSession final : public gui_unit_test::NoAnalysisSession {
 public:
  std::string contract_id() const override { return "test"; }
  std::string title() const override { return "Test - Node (node_1)"; }
  bool is_open() const override { return true; }

  void request(const std::string& verb, std::vector<uint8_t>,
               orc::view::ToolReplyHandler on_reply) override {
    verbs.push_back(verb);
    pending.push_back(std::move(on_reply));
  }
  void set_results_changed_handler(std::function<void()> handler) override {
    results_changed = std::move(handler);
  }
  std::shared_ptr<orc::view::IToolJob> run_job(
      const std::string&, std::vector<uint8_t>, orc::view::ToolProgressHandler,
      orc::view::ToolReplyHandler) override {
    return nullptr;
  }
  void apply_parameters(orc::view::ToolParameterMap, const std::string&,
                        orc::view::ToolReplyHandler) override {}
  void show_frame(uint64_t frame_index) override {
    shown_frames.push_back(frame_index);
  }
  void set_preview_frame_handler(
      std::function<void(uint64_t)> handler) override {
    preview_frame = std::move(handler);
  }
  void render_frame(uint64_t, const orc::view::ToolFrameRenderOptions&,
                    orc::view::ToolFrameHandler) override {}
  void set_graph_rebuilt_handler(std::function<void()>) override {}
  orc::view::AmplitudeUnit amplitude_unit() const override { return unit; }
  void set_amplitude_unit_handler(
      std::function<void(orc::view::AmplitudeUnit)> handler) override {
    unit_changed = std::move(handler);
  }
  orc::view::ToolParameterMap parameters() const override { return {}; }
  std::string project_directory() const override { return {}; }

  /// Answer the oldest waiting request.
  void reply(orc::view::ToolReply reply) {
    ASSERT_FALSE(pending.empty());
    auto handler = std::move(pending.front());
    pending.erase(pending.begin());
    handler(std::move(reply));
  }
  void replyWith(std::vector<uint8_t> data) {
    orc::view::ToolReply r;
    r.ok = true;
    r.data = std::move(data);
    reply(std::move(r));
  }

  std::vector<std::string> verbs;
  std::vector<orc::view::ToolReplyHandler> pending;
  std::vector<uint64_t> shown_frames;
  std::function<void()> results_changed;
  std::function<void(uint64_t)> preview_frame;
  std::function<void(orc::view::AmplitudeUnit)> unit_changed;
  orc::view::AmplitudeUnit unit = orc::view::AmplitudeUnit::IRE;
};

bool hasLabelContaining(const QWidget& widget, const QString& fragment) {
  for (const auto* label : widget.findChildren<QLabel*>()) {
    if (label->isVisibleTo(&widget) && label->text().contains(fragment)) {
      return true;
    }
  }
  return false;
}

wire::DropoutFrame dropout(int32_t frame, int32_t count, int64_t length) {
  wire::DropoutFrame f;
  f.frame_number = frame;
  f.dropout_count = count;
  f.dropout_length_samples = length;
  f.has_data = count > 0;
  return f;
}

// Owns the dialog for the test; the controller is the dialog's child.
template <typename Dialog, typename Controller>
struct Window {
  std::shared_ptr<FakeSession> session = std::make_shared<FakeSession>();
  std::unique_ptr<Dialog> dialog;
  Controller* controller = nullptr;

  Window() {
    (void)ensureApplication();
    dialog = std::make_unique<Dialog>();
    controller = new Controller(dialog.get(), session);
    dialog->show();
    QCoreApplication::processEvents();
  }
};

}  // namespace

TEST(AnalysisSessionControllerTest, AsksForResultsAsItOpens) {
  Window<DropoutAnalysisDialog, DropoutSessionController> w;
  ASSERT_EQ(w.session->verbs.size(), 1u);
  EXPECT_EQ(w.session->verbs[0], wire::kResultsVerb);
}

TEST(AnalysisSessionControllerTest, ShowsWhyThereAreNoResults) {
  Window<DropoutAnalysisDialog, DropoutSessionController> w;
  orc::view::ToolReply refusal;
  refusal.error = wire::kNotTriggeredMessage;
  w.session->reply(refusal);
  QCoreApplication::processEvents();

  EXPECT_TRUE(hasLabelContaining(*w.dialog, "not been triggered yet"));
}

TEST(AnalysisSessionControllerTest, ReportsBytesItCannotRead) {
  Window<SNRAnalysisDialog, SnrSessionController> w;
  // Dropout results sent to the SNR window.
  w.session->replyWith(
      wire::encode(wire::DropoutResults{1, {dropout(1, 1, 2)}}));
  QCoreApplication::processEvents();

  EXPECT_TRUE(hasLabelContaining(*w.dialog, "another analysis"));
}

TEST(AnalysisSessionControllerTest, DrawsResultsAndAsksAgainAfterATrigger) {
  Window<DropoutAnalysisDialog, DropoutSessionController> w;
  w.session->replyWith(wire::encode(wire::DropoutResults{
      3, {dropout(1, 0, 0), dropout(2, 2, 40), dropout(3, 1, 9)}}));
  QCoreApplication::processEvents();
  EXPECT_TRUE(hasLabelContaining(*w.dialog, "Per-frame view: 3 frames"));

  ASSERT_TRUE(w.session->results_changed);
  w.session->results_changed();
  EXPECT_EQ(w.session->verbs.size(), 2u);
}

TEST(AnalysisSessionControllerTest, EmptyResultsSayNoData) {
  Window<BurstLevelAnalysisDialog, BurstLevelSessionController> w;
  w.session->replyWith(wire::encode(wire::BurstLevelResults{}));
  QCoreApplication::processEvents();

  EXPECT_TRUE(hasLabelContaining(*w.dialog, "No burst level data available"));
}

// The graph's frames are 1-based and the preview's 0-based.
TEST(AnalysisSessionControllerTest, AClickMovesThePreviewToThatFrame) {
  Window<DropoutAnalysisDialog, DropoutSessionController> w;
  emit w.dialog->frameActivated(12);
  emit w.dialog->frameActivated(0);  // no frame 0: ignored

  ASSERT_EQ(w.session->shown_frames.size(), 1u);
  EXPECT_EQ(w.session->shown_frames[0], 11u);
}

TEST(AnalysisSessionControllerTest, FollowsThePreviewFrame) {
  Window<SNRAnalysisDialog, SnrSessionController> w;
  ASSERT_TRUE(w.session->preview_frame);
  // Nothing to check on the marker from outside; the handler must accept a
  // frame before and after results arrive without trouble.
  w.session->preview_frame(4);
  wire::SnrFrame f;
  f.frame_number = 1;
  f.has_white_snr = true;
  f.white_snr = 30.0;
  f.has_data = true;
  w.session->replyWith(wire::encode(wire::SnrResults{1, {f}}));
  w.session->preview_frame(0);
  QCoreApplication::processEvents();
  EXPECT_TRUE(hasLabelContaining(*w.dialog, "Per-frame view: 1 frame."));
}

TEST(AnalysisSessionControllerTest, BurstWindowFollowsTheAmplitudeUnit) {
  Window<BurstLevelAnalysisDialog, BurstLevelSessionController> w;
  wire::BurstLevelFrame f;
  f.frame_number = 1;
  f.has_data = true;
  f.median_burst_10bit = 147.0;
  wire::BurstLevelResults results;
  results.total_frames = 1;
  results.levels = {wire::System::PAL, 256, 844};
  results.frames = {f};
  w.session->replyWith(wire::encode(results));
  QCoreApplication::processEvents();

  ASSERT_TRUE(w.session->unit_changed);
  w.session->unit_changed(orc::view::AmplitudeUnit::Samples10Bit);
  QCoreApplication::processEvents();
  // Switching unit redraws in the new unit without asking the stage again.
  EXPECT_EQ(w.session->verbs.size(), 1u);
}

}  // namespace gui_unit_test
