/*
 * File:        closed_caption_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 3 offscreen tests for the closed caption preview dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QTableWidget>

#include "closedcaptiondialog.h"
#include "support/closed_caption_fixtures.h"

namespace gui_unit_test {

namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-closed-caption-dialog-test";
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

// The bytes each frame of the fixture window carries: a pop-on caption sent
// over frames 1-5 and put on screen at frame 5, then erased at frame 8.
orc::presenters::ClosedCaptionFieldDataView fixtureField(uint64_t frame) {
  switch (frame) {
    case 1:
      return makeCaptionField(kCcControlByte, kCcResumeCaptionLoading);
    case 2:
      return makeCaptionField(kCcControlByte, kCcPacRow15Col0);
    case 3:
      return makeTextField('H', 'E');
    case 4:
      return makeTextField('L', 'P');
    case 5:
      return makeCaptionField(kCcControlByte, kCcEndOfCaption);
    case 8:
      return makeCaptionField(kCcControlByte, kCcEraseDisplayedMemory);
    default:
      return makeEmptyField();
  }
}

// The same window with a TEXT2 page spliced through the caption, which is what
// line 21 really carries when a recording uses more than one service.
orc::presenters::ClosedCaptionFieldDataView interleavedField(uint64_t frame) {
  switch (frame) {
    case 6:
      return makeCaptionField(kCcControlByteChannel2, kCcTextRestart);
    case 7:
      return makeTextField('4', '2');
    default:
      return fixtureField(frame);
  }
}

// Answer every frame the dialog asks for, as MainWindow's request loop does.
void deliverWindow(ClosedCaptionDialog& dialog,
                   orc::presenters::ClosedCaptionFieldDataView (*source)(
                       uint64_t) = &fixtureField) {
  for (const uint64_t frame : dialog.framesNeedingData()) {
    dialog.deliverFrameData(true, frame * 2, source(frame), makeEmptyField());
  }
}

// Move the dialog to |frame| and answer whatever it needs there.
void showFrame(ClosedCaptionDialog& dialog, uint64_t frame) {
  dialog.setCurrentFrame(frame);
  deliverWindow(dialog);
}

}  // namespace

TEST(ClosedCaptionDialogTest, EmptyDialogReportsNoCaptionData) {
  ensureApplication();
  ClosedCaptionDialog dialog;

  EXPECT_EQ(dialog.statusText(), QStringLiteral("No caption data"));
  EXPECT_TRUE(dialog.listedCaptions().empty());
  EXPECT_EQ(dialog.currentCaptionRow(), -1);
}

TEST(ClosedCaptionDialogTest, CaptionAppearsInTheTranscriptAndIsMarked) {
  ensureApplication();
  ClosedCaptionDialog dialog;
  // Shown, because the mode readout reports itself hidden until the dialog is.
  dialog.show();
  showFrame(dialog, 6);

  EXPECT_EQ(dialog.currentCaptionText(), QStringLiteral("HELP"));
  const auto listed = dialog.listedCaptions();
  ASSERT_EQ(listed.size(), 1u);
  EXPECT_EQ(listed.front(), QStringLiteral("HELP"));
  EXPECT_EQ(dialog.currentCaptionRow(), 0);
  // Frame numbers are 1-based in the UI; the caption arrived at frame index 5.
  EXPECT_EQ(dialog.statusText(), QStringLiteral("Caption shown from frame 6"));
  EXPECT_EQ(dialog.modeText(), QStringLiteral("Pop-on"));
}

// The transcript outlives the caption: once the caption has been taken off
// screen it is still listed, but nothing is marked as showing.
TEST(ClosedCaptionDialogTest, ClearedScreenKeepsTheCaptionListed) {
  ensureApplication();
  ClosedCaptionDialog dialog;
  showFrame(dialog, 6);
  showFrame(dialog, 9);

  EXPECT_TRUE(dialog.currentCaptionText().isEmpty());
  EXPECT_EQ(dialog.listedCaptions().size(), 1u);
  EXPECT_EQ(dialog.currentCaptionRow(), -1);
  EXPECT_EQ(dialog.statusText(), QStringLiteral("No caption on screen"));
}

// The byte pair is the observation itself: a frame that carried one shows it,
// and a frame that carried none says so rather than showing nothing at all.
TEST(ClosedCaptionDialogTest, ReportsTheFramesRecoveredBytes) {
  ensureApplication();
  ClosedCaptionDialog dialog;
  showFrame(dialog, 5);

  // 0x14 and 0x2F with odd parity restored, as a caption tool writes them.
  EXPECT_EQ(dialog.dataText(), QStringLiteral("F1 94 2f"));

  showFrame(dialog, 6);
  EXPECT_EQ(dialog.dataText(), QStringLiteral("No caption data on this frame"));
}

// A recording whose first stored field is television field 2 carries its
// captions on the second stored field: once its control codes show that, the
// readout numbers the fields as television fields.
TEST(ClosedCaptionDialogTest, NumbersTheFieldsAsTelevisionFields) {
  ensureApplication();
  ClosedCaptionDialog dialog;
  dialog.setCurrentFrame(12);
  for (const uint64_t frame : dialog.framesNeedingData()) {
    dialog.deliverFrameData(
        true, frame * 2, makeEmptyField(),
        makeCaptionField(kCcControlByte, kCcEraseDisplayedMemory));
  }

  // 0x14 and 0x2C with odd parity restored.
  EXPECT_EQ(dialog.dataText(), QStringLiteral("F1 94 2c"));
}

// Stepping back through the recording marks the caption each frame was
// showing, from the history rather than from a re-read.
TEST(ClosedCaptionDialogTest, SteppingBackMarksTheEarlierCaption) {
  ensureApplication();
  ClosedCaptionDialog dialog;
  showFrame(dialog, 9);
  ASSERT_TRUE(dialog.currentCaptionText().isEmpty());

  dialog.setCurrentFrame(6);

  EXPECT_EQ(dialog.currentCaptionText(), QStringLiteral("HELP"));
  EXPECT_EQ(dialog.currentCaptionRow(), 0);
}

// A frame whose observations could not be produced still has to advance the
// decoder, or the window would never converge and the dialog would ask for it
// again on every frame change.
TEST(ClosedCaptionDialogTest, UnavailableFramesStillConvergeTheWindow) {
  ensureApplication();
  ClosedCaptionDialog dialog;
  dialog.setCurrentFrame(4);
  for (const uint64_t frame : dialog.framesNeedingData()) {
    dialog.deliverFrameData(false, frame * 2, {}, {});
  }

  EXPECT_TRUE(dialog.framesNeedingData().empty());
  EXPECT_TRUE(dialog.listedCaptions().empty());
}

// A node or DAG change means different observations entirely.
TEST(ClosedCaptionDialogTest, ClearCacheEmptiesTheTranscript) {
  ensureApplication();
  ClosedCaptionDialog dialog;
  showFrame(dialog, 6);
  ASSERT_FALSE(dialog.listedCaptions().empty());

  dialog.clearCache();

  EXPECT_TRUE(dialog.listedCaptions().empty());
  EXPECT_TRUE(dialog.currentCaptionText().isEmpty());
}

// Issue #273: the four services multiplexed onto line 21 have to be read one
// at a time, so the dialog offers a choice of which.
TEST(ClosedCaptionDialogTest, DefaultsToTheCaptionServiceAndShowsOnlyIt) {
  ensureApplication();
  ClosedCaptionDialog dialog;
  EXPECT_EQ(dialog.service(), orc::EIA608Service::CC1);

  // Frame 7: the caption is on screen and the text page has arrived too.
  dialog.setCurrentFrame(7);
  deliverWindow(dialog, &interleavedField);

  // "42" belongs to the text service and must not be in the caption.
  EXPECT_EQ(dialog.currentCaptionText(), QStringLiteral("HELP"));
}

TEST(ClosedCaptionDialogTest, ChoosingATextServiceAsksForTheWindowAgain) {
  ensureApplication();
  ClosedCaptionDialog dialog;
  dialog.setCurrentFrame(7);
  deliverWindow(dialog, &interleavedField);
  ASSERT_TRUE(dialog.framesNeedingData().empty());

  int requests = 0;
  QObject::connect(&dialog, &ClosedCaptionDialog::dataRequestNeeded,
                   [&requests] { ++requests; });

  dialog.selectService(orc::EIA608Service::T2);

  EXPECT_EQ(dialog.service(), orc::EIA608Service::T2);
  EXPECT_EQ(requests, 1);
  // Nothing decoded under CC1 describes TEXT2, so the transcript is emptied
  // and the frames are needed again.
  EXPECT_TRUE(dialog.listedCaptions().empty());
  EXPECT_FALSE(dialog.framesNeedingData().empty());

  deliverWindow(dialog, &interleavedField);
  EXPECT_EQ(dialog.currentCaptionText(), QStringLiteral("42"));
}

TEST(ClosedCaptionDialogTest, ReselectingTheSameServiceAsksForNothing) {
  ensureApplication();
  ClosedCaptionDialog dialog;
  dialog.setCurrentFrame(7);
  deliverWindow(dialog, &interleavedField);

  int requests = 0;
  QObject::connect(&dialog, &ClosedCaptionDialog::dataRequestNeeded,
                   [&requests] { ++requests; });

  dialog.selectService(orc::EIA608Service::CC1);

  EXPECT_EQ(requests, 0);
  EXPECT_EQ(dialog.currentCaptionText(), QStringLiteral("HELP"));
}

}  // namespace gui_unit_test
