/*
 * File:        analysis_dialog_smoke_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Offscreen smoke tests for analysis dialog subclasses (Phase 9)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QGroupBox>
#include <QLabel>
#include <QStatusBar>
#include <algorithm>

#include "burstlevelanalysisdialog.h"
#include "dropoutanalysisdialog.h"
#include "framescopedialog.h"
#include "frametimingdialog.h"
#include "ntscobserverdialog.h"
#include "snranalysisdialog.h"
#include "vbidialog.h"
#include "videoparameterobserverdialog.h"

// A stage's analysis tools open in orc::view::AnalysisToolWindow, tested in
// analysis_tool_window_test.cpp. (The Dropout Editor is driven through a fake
// session in dropout_editor_dialog_test.cpp.)

namespace gui_unit_test {

namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-analysis-dialog-smoke-test";
  static char platform_opt[] = "-platform";
  static char platform_val[] = "offscreen";
  static char* argv[] = {app_name, platform_opt, platform_val, nullptr};
  static QApplication* app = [] {
    auto* created_app = new QApplication(argc, argv);
    created_app->setQuitOnLastWindowClosed(false);
    return created_app;
  }();
  return *app;
}

}  // namespace

// ---------------------------------------------------------------------------
// AnalysisDialogBase subclasses
// ---------------------------------------------------------------------------

TEST(AnalysisDialogSmokeTest, BurstLevelAnalysisDialog_CanShowAndClose) {
  (void)ensureApplication();

  BurstLevelAnalysisDialog dialog;

  dialog.show();
  QCoreApplication::processEvents();

  EXPECT_TRUE(dialog.isVisible());

  dialog.close();
  QCoreApplication::processEvents();

  EXPECT_FALSE(dialog.isVisible());
}

TEST(AnalysisDialogSmokeTest, DropoutAnalysisDialog_CanShowAndClose) {
  (void)ensureApplication();

  DropoutAnalysisDialog dialog;

  dialog.show();
  QCoreApplication::processEvents();

  EXPECT_TRUE(dialog.isVisible());

  dialog.close();
  QCoreApplication::processEvents();

  EXPECT_FALSE(dialog.isVisible());
}

TEST(AnalysisDialogSmokeTest, SnrAnalysisDialog_CanShowAndClose) {
  (void)ensureApplication();

  SNRAnalysisDialog dialog;

  dialog.show();
  QCoreApplication::processEvents();

  EXPECT_TRUE(dialog.isVisible());

  dialog.close();
  QCoreApplication::processEvents();

  EXPECT_FALSE(dialog.isVisible());
}

// ---------------------------------------------------------------------------
// Analysis and observation dialogs (simple QWidget* parent constructors)
// ---------------------------------------------------------------------------

TEST(AnalysisDialogSmokeTest, NtscObserverDialog_CanShowAndClose) {
  (void)ensureApplication();

  NtscObserverDialog dialog;

  dialog.show();
  QCoreApplication::processEvents();

  EXPECT_TRUE(dialog.isVisible());

  dialog.close();
  QCoreApplication::processEvents();

  EXPECT_FALSE(dialog.isVisible());
}

TEST(AnalysisDialogSmokeTest, VideoParameterObserverDialog_CanShowAndClose) {
  (void)ensureApplication();

  VideoParameterObserverDialog dialog;

  dialog.show();
  QCoreApplication::processEvents();

  EXPECT_TRUE(dialog.isVisible());

  dialog.close();
  QCoreApplication::processEvents();

  EXPECT_FALSE(dialog.isVisible());
}

// Phase 5 Task 5.2: the observer dialogs show a pending "computing" notice
// after an async request is issued and hide it once observations arrive.
TEST(AnalysisDialogSmokeTest,
     VideoParameterObserverDialog_PendingThenPopulated) {
  (void)ensureApplication();

  VideoParameterObserverDialog dialog;
  auto* status = dialog.findChild<QLabel*>("observationStatusLabel");
  ASSERT_NE(status, nullptr);
  EXPECT_FALSE(status->isVisible());  // hidden until a request is in flight

  dialog.show();
  QCoreApplication::processEvents();

  dialog.showPending();
  QCoreApplication::processEvents();
  EXPECT_TRUE(status->isVisible());
  EXPECT_FALSE(status->text().isEmpty());

  dialog.updateObservations(orc::FieldID(4),
                            orc::presenters::VideoParameterObservationView{});
  QCoreApplication::processEvents();
  EXPECT_FALSE(status->isVisible());  // populated -> notice cleared

  dialog.close();
}

// The pending notice belongs in the status bar: it must not push the value
// groups around as it comes and goes.
TEST(AnalysisDialogSmokeTest,
     VideoParameterObserverDialog_PendingNoticeDoesNotMoveValues) {
  (void)ensureApplication();

  VideoParameterObserverDialog dialog;
  auto* status = dialog.findChild<QLabel*>("observationStatusLabel");
  auto* status_bar = dialog.findChild<QStatusBar*>("observationStatusBar");
  ASSERT_NE(status, nullptr);
  ASSERT_NE(status_bar, nullptr);
  EXPECT_EQ(status->parentWidget(), status_bar);

  dialog.show();
  QCoreApplication::processEvents();

  auto* signal_group = dialog.findChild<QGroupBox*>();
  ASSERT_NE(signal_group, nullptr);
  const QRect before = signal_group->geometry();
  ASSERT_GT(before.height(), 0);  // the layout really ran

  dialog.showPending();
  QCoreApplication::processEvents();
  EXPECT_EQ(signal_group->geometry(), before);

  dialog.updateObservations(orc::FieldID(4),
                            orc::presenters::VideoParameterObservationView{});
  QCoreApplication::processEvents();
  EXPECT_EQ(signal_group->geometry(), before);

  dialog.close();
}

TEST(AnalysisDialogSmokeTest, NtscObserverDialog_PendingThenPopulated) {
  (void)ensureApplication();

  NtscObserverDialog dialog;
  auto* status = dialog.findChild<QLabel*>("observationStatusLabel");
  ASSERT_NE(status, nullptr);
  EXPECT_FALSE(status->isVisible());

  dialog.show();
  QCoreApplication::processEvents();

  dialog.showPending();
  QCoreApplication::processEvents();
  EXPECT_TRUE(status->isVisible());
  EXPECT_FALSE(status->text().isEmpty());

  dialog.updateObservations(orc::FieldID(4),
                            orc::presenters::NtscFieldObservationsView{});
  QCoreApplication::processEvents();
  EXPECT_FALSE(status->isVisible());

  dialog.close();
}

TEST(AnalysisDialogSmokeTest,
     NtscObserverDialog_PendingNoticeDoesNotMoveValues) {
  (void)ensureApplication();

  NtscObserverDialog dialog;
  auto* status = dialog.findChild<QLabel*>("observationStatusLabel");
  auto* status_bar = dialog.findChild<QStatusBar*>("observationStatusBar");
  ASSERT_NE(status, nullptr);
  ASSERT_NE(status_bar, nullptr);
  EXPECT_EQ(status->parentWidget(), status_bar);

  dialog.show();
  QCoreApplication::processEvents();

  auto* field1_group = dialog.findChild<QGroupBox*>();
  ASSERT_NE(field1_group, nullptr);
  const QRect before = field1_group->geometry();
  ASSERT_GT(before.height(), 0);  // the layout really ran

  dialog.showPending();
  QCoreApplication::processEvents();
  EXPECT_EQ(field1_group->geometry(), before);

  dialog.updateObservations(orc::FieldID(4),
                            orc::presenters::NtscFieldObservationsView{});
  QCoreApplication::processEvents();
  EXPECT_EQ(field1_group->geometry(), before);

  dialog.close();
}

namespace {

orc::presenters::VideoParameterObservationView makeBurstObservation() {
  orc::presenters::VideoParameterObservationView obs;
  orc::presenters::VideoParametersView vp;
  vp.system = orc::presenters::VideoSystem::PAL;
  vp.blanking_level = 256;
  vp.white_level = 844;
  obs.video_params = vp;
  obs.burst_level_10bit = 100.0;
  return obs;
}

// True if any label in the dialog shows the given text.
bool dialogShowsText(const QDialog& dialog, const QString& text) {
  const auto labels = dialog.findChildren<QLabel*>();
  return std::any_of(labels.cbegin(), labels.cend(),
                     [&text](const QLabel* l) { return l->text() == text; });
}

// True if any label in the dialog contains the given substring.
bool dialogHasLabelContaining(const QDialog& dialog, const QString& fragment) {
  const auto labels = dialog.findChildren<QLabel*>();
  return std::any_of(labels.cbegin(), labels.cend(),
                     [&fragment](const QLabel* l) {
                       return l->isVisible() && l->text().contains(fragment);
                     });
}

}  // namespace

// ---------------------------------------------------------------------------
// Bucket-info status line (Phase 5, Task 5.2)
// ---------------------------------------------------------------------------

TEST(AnalysisDialogSmokeTest, DropoutDialog_DecimatedSeriesShowsBucketRange) {
  (void)ensureApplication();

  DropoutAnalysisDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  // A decimated series: two buckets, each covering a range of analysed frames.
  dialog.startUpdate(/*numberOfFrames=*/5000, /*decimated=*/true);
  dialog.addDataPoint(/*frameNumber=*/2500, /*dropoutLength=*/40.0,
                      /*frameStart=*/1, /*frameEnd=*/2500, /*dropoutCount=*/8);
  dialog.addDataPoint(/*frameNumber=*/5000, /*dropoutLength=*/12.0,
                      /*frameStart=*/2501, /*frameEnd=*/5000,
                      /*dropoutCount=*/3);
  dialog.finishUpdate(/*currentFrameNumber=*/1);
  QCoreApplication::processEvents();

  // The decimated view states its bucketing so it is not read as per-frame.
  EXPECT_TRUE(dialogHasLabelContaining(dialog, "Decimated view"));
  EXPECT_FALSE(dialogHasLabelContaining(dialog, "Per-frame view"));
}

TEST(AnalysisDialogSmokeTest, SnrDialog_PerFrameSeriesReadsAsPerFrame) {
  (void)ensureApplication();

  SNRAnalysisDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  // A per-frame series: one bucket per frame (frameStart == frameEnd).
  dialog.startUpdate(/*numberOfFrames=*/3, /*decimated=*/false);
  dialog.addDataPoint(/*frameNumber=*/1, /*whiteSNR=*/42.0, /*blackPSNR=*/38.0,
                      /*frameStart=*/1, /*frameEnd=*/1);
  dialog.addDataPoint(/*frameNumber=*/2, /*whiteSNR=*/41.0, /*blackPSNR=*/37.0,
                      /*frameStart=*/2, /*frameEnd=*/2);
  dialog.finishUpdate(/*currentFrameNumber=*/1);
  QCoreApplication::processEvents();

  EXPECT_TRUE(dialogHasLabelContaining(dialog, "Per-frame view"));
  EXPECT_FALSE(dialogHasLabelContaining(dialog, "Decimated view"));
}

TEST(AnalysisDialogSmokeTest, BurstDialog_DecimatedSeriesShowsBucketRange) {
  (void)ensureApplication();

  BurstLevelAnalysisDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  dialog.startUpdate(/*numberOfFrames=*/8000, /*decimated=*/true);
  dialog.addDataPoint(/*frameNumber=*/4000, /*burstLevel10bit=*/120.0,
                      /*frameStart=*/1, /*frameEnd=*/4000);
  dialog.addDataPoint(/*frameNumber=*/8000, /*burstLevel10bit=*/118.0,
                      /*frameStart=*/4001, /*frameEnd=*/8000);
  dialog.finishUpdate(/*currentFrameNumber=*/1);
  QCoreApplication::processEvents();

  EXPECT_TRUE(dialogHasLabelContaining(dialog, "Decimated view"));
}

TEST(AnalysisDialogSmokeTest,
     VideoParameterObserverDialog_BurstLevelFollowsAmplitudeUnit) {
  (void)ensureApplication();

  VideoParameterObserverDialog dialog;
  const orc::FieldID field_id(0);
  const auto obs = makeBurstObservation();

  // 10-bit: raw ADU value.
  dialog.setAmplitudeUnit(orc::AmplitudeDisplayUnit::Samples10Bit);
  dialog.updateObservations(field_id, obs);
  EXPECT_TRUE(dialogShowsText(dialog, "100.0 ADU"));

  // IRE: AC amplitude scaled by blanking-to-white range (588 ADU = 100 IRE).
  dialog.setAmplitudeUnit(orc::AmplitudeDisplayUnit::IRE);
  dialog.updateObservations(field_id, obs);
  EXPECT_TRUE(dialogShowsText(dialog, "17.0 IRE"));

  // mV: PAL active video is 700 mV.
  dialog.setAmplitudeUnit(orc::AmplitudeDisplayUnit::Millivolts);
  dialog.updateObservations(field_id, obs);
  EXPECT_TRUE(dialogShowsText(dialog, "119.0 mV"));
}

TEST(AnalysisDialogSmokeTest,
     VideoParameterObserverDialog_BurstLevelFallsBackToAduWithoutLevels) {
  (void)ensureApplication();

  VideoParameterObserverDialog dialog;
  auto obs = makeBurstObservation();
  obs.video_params->blanking_level = -1;  // Unmigrated source: no levels.

  dialog.setAmplitudeUnit(orc::AmplitudeDisplayUnit::IRE);
  dialog.updateObservations(orc::FieldID(0), obs);
  EXPECT_TRUE(dialogShowsText(dialog, "100.0 ADU"));
}

TEST(AnalysisDialogSmokeTest,
     VideoParameterObserverDialog_SignalLevelsFollowAmplitudeUnit) {
  (void)ensureApplication();

  VideoParameterObserverDialog dialog;
  const orc::FieldID field_id(0);
  auto obs = makeBurstObservation();
  obs.video_params->sync_tip_level = 16;
  obs.video_params->black_level = 256;

  // 10-bit: raw ADU values, no suffix.
  dialog.setAmplitudeUnit(orc::AmplitudeDisplayUnit::Samples10Bit);
  dialog.updateObservations(field_id, obs);
  EXPECT_TRUE(dialogShowsText(dialog, "16 / 256 / 844"));

  // IRE: blanking maps to 0 IRE, white to 100 IRE (range 588 ADU).
  dialog.setAmplitudeUnit(orc::AmplitudeDisplayUnit::IRE);
  dialog.updateObservations(field_id, obs);
  EXPECT_TRUE(dialogShowsText(dialog, "-40.8 IRE / 0.0 IRE / 100.0 IRE"));

  // mV: PAL active video is 700 mV.
  dialog.setAmplitudeUnit(orc::AmplitudeDisplayUnit::Millivolts);
  dialog.updateObservations(field_id, obs);
  EXPECT_TRUE(dialogShowsText(dialog, "-285.7 mV / 0.0 mV / 700.0 mV"));
}

TEST(AnalysisDialogSmokeTest,
     VideoParameterObserverDialog_SignalLevelsFallBackToAduWithoutBlanking) {
  (void)ensureApplication();

  VideoParameterObserverDialog dialog;
  auto obs = makeBurstObservation();
  obs.video_params->sync_tip_level = 16;
  obs.video_params->black_level = 256;
  obs.video_params->blanking_level = -1;  // Unmigrated source: no blanking.

  dialog.setAmplitudeUnit(orc::AmplitudeDisplayUnit::IRE);
  dialog.updateObservations(orc::FieldID(0), obs);
  EXPECT_TRUE(dialogShowsText(dialog, "16 / 256 / 844"));
}

TEST(AnalysisDialogSmokeTest, VbiDialog_CanShowAndClose) {
  (void)ensureApplication();

  VBIDialog dialog;

  dialog.show();
  QCoreApplication::processEvents();

  EXPECT_TRUE(dialog.isVisible());

  dialog.close();
  QCoreApplication::processEvents();

  EXPECT_FALSE(dialog.isVisible());
}

TEST(AnalysisDialogSmokeTest, FrameScopeDialog_CanShowAndClose) {
  (void)ensureApplication();

  FrameScopeDialog dialog;

  dialog.show();
  QCoreApplication::processEvents();

  EXPECT_TRUE(dialog.isVisible());

  dialog.close();
  QCoreApplication::processEvents();

  EXPECT_FALSE(dialog.isVisible());
}

TEST(AnalysisDialogSmokeTest, FrameTimingDialog_CanShowAndClose) {
  (void)ensureApplication();

  FrameTimingDialog dialog;

  dialog.show();
  QCoreApplication::processEvents();

  EXPECT_TRUE(dialog.isVisible());

  dialog.close();
  QCoreApplication::processEvents();

  EXPECT_FALSE(dialog.isVisible());
}

}  // namespace gui_unit_test
