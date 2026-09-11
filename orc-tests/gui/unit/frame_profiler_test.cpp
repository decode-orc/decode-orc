/*
 * File:        frame_profiler_test.cpp
 * Module:      orc-gui tests
 * Purpose:     Per-frame timing accumulation and log formatting
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_profiler.h"

#include <gtest/gtest.h>

namespace orc::gui {
namespace {

std::size_t idx(FrameStage stage) { return static_cast<std::size_t>(stage); }

FrameTimings makeFrame() {
  FrameTimings frame;
  frame.frame_index = 42;
  frame.queue_depth = 3;
  frame.wall_us = 40000;
  frame.stage_us[idx(FrameStage::kPreviewRender)] = 25000;
  frame.stage_us[idx(FrameStage::kVectorscope)] = 12000;
  frame.stage_us[idx(FrameStage::kHistogram)] = 4000;
  return frame;
}

// ---------------------------------------------------------------------------
// FrameTimings
// ---------------------------------------------------------------------------

TEST(FrameTimings, GuiThreadTotalExcludesRenderLatency) {
  // Render latency is time the GUI thread waits, not time it works, and the
  // segments measured during that wait would otherwise be counted twice.
  EXPECT_EQ(makeFrame().guiThreadUs(), 16000);
}

TEST(FrameTimings, DominantStageIgnoresRenderLatency) {
  const FrameTimings frame = makeFrame();
  ASSERT_GT(frame.stage_us[idx(FrameStage::kPreviewRender)],
            frame.stage_us[idx(FrameStage::kVectorscope)]);
  EXPECT_EQ(frame.dominantStage(), FrameStage::kVectorscope);
}

TEST(FrameTimings, DominantStageIsCountWhenNothingMeasured) {
  EXPECT_EQ(FrameTimings{}.dominantStage(), FrameStage::kCount);
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

TEST(FormatFrameLine, ReportsIndexWallGuiQueueAndDominant) {
  const QString line = formatFrameLine(makeFrame());
  EXPECT_TRUE(line.contains(QStringLiteral("frame 42"))) << line.toStdString();
  EXPECT_TRUE(line.contains(QStringLiteral("wall=40.00ms")))
      << line.toStdString();
  EXPECT_TRUE(line.contains(QStringLiteral("gui=16.00ms")))
      << line.toStdString();
  EXPECT_TRUE(line.contains(QStringLiteral("queue=3"))) << line.toStdString();
  EXPECT_TRUE(line.contains(QStringLiteral("dominant=vectorscope")))
      << line.toStdString();
}

TEST(FormatFrameLine, OmitsSegmentsThatConsumedNoTime) {
  // A closed dialog must not pad every line with a zero.
  const QString line = formatFrameLine(makeFrame());
  EXPECT_FALSE(line.contains(QStringLiteral("waveform"))) << line.toStdString();
  EXPECT_TRUE(line.contains(QStringLiteral("histogram=4.00ms")))
      << line.toStdString();
}

TEST(FormatSummary, EmptyWindowSaysSo) {
  EXPECT_EQ(formatSummary(FrameTimingSummary{}),
            QStringLiteral("summary: no frames displayed"));
}

TEST(FormatSummary, ReportsMeanAndMaxPerSegment) {
  FrameTimingSummary summary;
  FrameTimings a = makeFrame();
  FrameTimings b = makeFrame();
  b.stage_us[idx(FrameStage::kVectorscope)] = 20000;
  b.wall_us = 60000;
  summary.add(a);
  summary.add(b);
  summary.window_us = 2000000;

  const QString text = formatSummary(summary);
  EXPECT_TRUE(text.contains(QStringLiteral("2 frames"))) << text.toStdString();
  EXPECT_TRUE(text.contains(QStringLiteral("1.0 fps"))) << text.toStdString();
  // Vectorscope mean of 12 ms and 20 ms is 16 ms; the max is the larger.
  EXPECT_TRUE(text.contains(QStringLiteral("vectorscope mean=16.00ms")))
      << text.toStdString();
  EXPECT_TRUE(text.contains(QStringLiteral("max=20.00ms")))
      << text.toStdString();
}

// ---------------------------------------------------------------------------
// FrameTimingSummary
// ---------------------------------------------------------------------------

TEST(FrameTimingSummary, AccumulatesTotalsMaximaAndQueueDepth) {
  FrameTimingSummary summary;
  FrameTimings a = makeFrame();
  FrameTimings b = makeFrame();
  b.queue_depth = 9;
  b.stage_us[idx(FrameStage::kHistogram)] = 1000;
  summary.add(a);
  summary.add(b);

  EXPECT_EQ(summary.frames, 2);
  EXPECT_EQ(summary.total_us[idx(FrameStage::kHistogram)], 5000);
  EXPECT_EQ(summary.max_us[idx(FrameStage::kHistogram)], 4000);
  EXPECT_EQ(summary.max_queue_depth, 9);
}

TEST(FrameTimingSummary, FramesPerSecondIsZeroForAnEmptyWindow) {
  EXPECT_DOUBLE_EQ(FrameTimingSummary{}.framesPerSecond(), 0.0);

  FrameTimingSummary no_window;
  no_window.add(makeFrame());
  EXPECT_DOUBLE_EQ(no_window.framesPerSecond(), 0.0)
      << "a window of unknown length cannot imply a rate";
}

TEST(FrameTimingSummary, ResetClearsEverything) {
  FrameTimingSummary summary;
  summary.add(makeFrame());
  summary.window_us = 1000;
  summary.reset();

  EXPECT_EQ(summary.frames, 0);
  EXPECT_EQ(summary.window_us, 0);
  EXPECT_EQ(summary.max_queue_depth, 0);
  EXPECT_EQ(summary.total_us[idx(FrameStage::kVectorscope)], 0);
}

// ---------------------------------------------------------------------------
// Collection gate
// ---------------------------------------------------------------------------

TEST(FrameProfiler, CollectsNothingWhileDisabled) {
  FrameProfiler& profiler = FrameProfiler::instance();
  profiler.setEnabled(false);
  ASSERT_FALSE(profiler.isEnabled());

  profiler.beginFrame(1);
  {
    ORC_FRAME_STAGE(FrameStage::kVectorscope);
  }
  profiler.endFrame();

  EXPECT_EQ(profiler.summary().frames, 0);
}

TEST(FrameProfiler, EnablingDiscardsWhateverWasHalfMeasured) {
  FrameProfiler& profiler = FrameProfiler::instance();
  profiler.setEnabled(true);
  profiler.beginFrame(1);
  profiler.endFrame();
  ASSERT_EQ(profiler.summary().frames, 1);

  // Re-enabling is a no-op; only a real transition clears the window.
  profiler.setEnabled(true);
  EXPECT_EQ(profiler.summary().frames, 1);

  profiler.setEnabled(false);
  profiler.setEnabled(true);
  EXPECT_EQ(profiler.summary().frames, 0);
  profiler.setEnabled(false);
}

TEST(FrameProfiler, AnAbandonedFrameIsStillAccountedFor) {
  FrameProfiler& profiler = FrameProfiler::instance();
  profiler.setEnabled(true);

  // Two begins with no end between them: the first frame was superseded
  // before it reached the display, but its cost was still paid.
  profiler.beginFrame(1);
  profiler.beginFrame(2);
  profiler.endFrame();

  EXPECT_EQ(profiler.summary().frames, 2);
  profiler.setEnabled(false);
}

TEST(FrameProfiler, StageTimeLandsInTheOpenFrame) {
  FrameProfiler& profiler = FrameProfiler::instance();
  profiler.setEnabled(true);

  profiler.beginFrame(7);
  profiler.addStage(FrameStage::kHistogram, 1234);
  profiler.endFrame();

  EXPECT_EQ(profiler.summary().total_us[idx(FrameStage::kHistogram)], 1234);
  profiler.setEnabled(false);
}

TEST(FrameProfiler, StageTimeOutsideAFrameIsDropped) {
  FrameProfiler& profiler = FrameProfiler::instance();
  profiler.setEnabled(true);

  profiler.addStage(FrameStage::kHistogram, 999);
  profiler.beginFrame(1);
  profiler.endFrame();

  EXPECT_EQ(profiler.summary().total_us[idx(FrameStage::kHistogram)], 0);
  profiler.setEnabled(false);
}

}  // namespace
}  // namespace orc::gui
