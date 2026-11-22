/*
 * File:        preview_render_progress_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 (gui-logic) tests for the preview window's predicted
 *              render progress text
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_render_progress.h"

#include <gtest/gtest.h>

namespace orc::gui {
namespace {

constexpr const char* kView = "node_3|1|";

TEST(PreviewRenderProgress, SaysNothing_BeforeTheRenderHasBeenSlow) {
  PreviewRenderProgress progress;
  progress.begin(kView, 0);
  EXPECT_EQ(progress.statusText(1999), "");
}

TEST(PreviewRenderProgress, SaysNothing_WhenNoRenderIsInFlight) {
  PreviewRenderProgress progress;
  EXPECT_EQ(progress.statusText(10000), "");
  progress.begin(kView, 0);
  progress.finish(true, 3000);
  EXPECT_EQ(progress.statusText(10000), "");
}

TEST(PreviewRenderProgress, ShowsElapsedSeconds_WithoutHistory) {
  PreviewRenderProgress progress;
  progress.begin(kView, 0);
  EXPECT_EQ(progress.statusText(2000), "Rendering... 2 s");
  EXPECT_EQ(progress.statusText(12900), "Rendering... 12 s");
}

TEST(PreviewRenderProgress, PredictsPercent_FromThePreviousRenderOfTheView) {
  PreviewRenderProgress progress;
  progress.begin(kView, 0);
  progress.finish(true, 10000);

  progress.begin(kView, 20000);
  EXPECT_EQ(progress.statusText(24000), "Rendering... ~40%");
}

TEST(PreviewRenderProgress, CapsThePrediction_BelowFinished) {
  PreviewRenderProgress progress;
  progress.begin(kView, 0);
  progress.finish(true, 10000);

  progress.begin(kView, 20000);
  EXPECT_EQ(progress.statusText(29990), "Rendering... ~95%");
}

TEST(PreviewRenderProgress, FallsBackToElapsed_OnceThePredictionIsOutlived) {
  PreviewRenderProgress progress;
  progress.begin(kView, 0);
  progress.finish(true, 4000);

  progress.begin(kView, 10000);
  EXPECT_EQ(progress.statusText(16000), "Rendering... 6 s");
}

TEST(PreviewRenderProgress, KeepsPredictionsApart_PerView) {
  PreviewRenderProgress progress;
  progress.begin(kView, 0);
  progress.finish(true, 10000);

  progress.begin("node_4|1|", 20000);
  EXPECT_EQ(progress.statusText(24000), "Rendering... 4 s");
}

TEST(PreviewRenderProgress, AveragesTheOldAndNewDurations) {
  PreviewRenderProgress progress;
  progress.begin(kView, 0);
  progress.finish(true, 4000);
  progress.begin(kView, 10000);
  progress.finish(true, 26000);  // 16 s; expectation now 10 s

  progress.begin(kView, 30000);
  EXPECT_EQ(progress.statusText(35000), "Rendering... ~50%");
}

TEST(PreviewRenderProgress, DoesNotLearn_FromCacheHits) {
  PreviewRenderProgress progress;
  progress.begin(kView, 0);
  progress.finish(true, 10000);
  progress.begin(kView, 20000);
  progress.finish(true, 20050);  // cache hit

  progress.begin(kView, 30000);
  EXPECT_EQ(progress.statusText(34000), "Rendering... ~40%");
}

TEST(PreviewRenderProgress, DoesNotLearn_FromFailedOrAbandonedRenders) {
  PreviewRenderProgress progress;
  progress.begin(kView, 0);
  progress.finish(false, 10000);
  progress.begin(kView, 20000);
  progress.abandon();

  progress.begin(kView, 30000);
  EXPECT_EQ(progress.statusText(34000), "Rendering... 4 s");
}

TEST(PreviewRenderProgress, UnkeyedRequest_ShowsElapsedOnly) {
  PreviewRenderProgress progress;
  progress.begin("", 0);
  progress.finish(true, 10000);

  progress.begin("", 20000);
  EXPECT_EQ(progress.statusText(24000), "Rendering... 4 s");
}

TEST(PreviewRenderProgress, BackToBackRequests_KeepTheVisibleWaitGoing) {
  // Stage selection: the available-outputs query, then its first preview.
  PreviewRenderProgress progress;
  progress.begin("", 0);
  EXPECT_EQ(progress.statusText(3000), "Rendering... 3 s");
  progress.begin(kView, 3000);
  EXPECT_EQ(progress.statusText(3500), "Rendering... 3 s");
}

TEST(PreviewRenderProgress, BackToBackRequests_PredictFromTheirOwnStart) {
  PreviewRenderProgress progress;
  progress.begin(kView, 0);
  progress.finish(true, 10000);

  progress.begin("", 20000);
  progress.begin(kView, 23000);
  EXPECT_EQ(progress.statusText(25000), "Rendering... ~20%");
}

TEST(PreviewRenderProgress, ForgetPredictions_DropsHistoryOnly) {
  PreviewRenderProgress progress;
  progress.begin(kView, 0);
  progress.finish(true, 10000);

  progress.begin(kView, 20000);
  progress.forgetPredictions();
  EXPECT_TRUE(progress.inFlight());
  EXPECT_EQ(progress.statusText(24000), "Rendering... 4 s");
}

}  // namespace
}  // namespace orc::gui
