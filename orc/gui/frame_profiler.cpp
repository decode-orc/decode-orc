/*
 * File:        frame_profiler.cpp
 * Module:      orc-gui
 * Purpose:     Per-frame timing breakdown of the preview and observer path
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_profiler.h"

#include <algorithm>

#include "logging.h"

namespace orc::gui {
namespace {

// Reporting window length. One second is short enough to follow live and long
// enough that a 25 fps session contributes a meaningful sample.
constexpr std::int64_t kSummaryWindowMs = 1000;

QString microsToMillis(std::int64_t micros) {
  return QString::number(static_cast<double>(micros) / 1000.0, 'f', 2);
}

}  // namespace

const char* frameStageName(FrameStage stage) {
  switch (stage) {
    case FrameStage::kPreviewRender:
      return "render";
    case FrameStage::kDagExecution:
      return "dag-exec";
    case FrameStage::kObservationFill:
      return "obs-fill";
    case FrameStage::kUpdateAll:
      return "update-all";
    case FrameStage::kVectorscope:
      return "vectorscope";
    case FrameStage::kHistogram:
      return "histogram";
    case FrameStage::kObservers:
      return "observers";
    case FrameStage::kVbi:
      return "vbi";
    case FrameStage::kClosedCaption:
      return "captions";
    case FrameStage::kFrameTiming:
      return "timing";
    case FrameStage::kWaveform:
      return "waveform";
    case FrameStage::kPreviewPaint:
      return "paint";
    case FrameStage::kCount:
      break;
  }
  return "unknown";
}

bool isWorkerStage(FrameStage stage) {
  switch (stage) {
    case FrameStage::kPreviewRender:
    case FrameStage::kDagExecution:
    case FrameStage::kObservationFill:
      return true;
    default:
      return false;
  }
}

std::int64_t FrameTimings::guiThreadUs() const {
  std::int64_t total = 0;
  for (std::size_t i = 0; i < kFrameStageCount; ++i) {
    if (isWorkerStage(static_cast<FrameStage>(i))) {
      continue;
    }
    total += stage_us[i];
  }
  return total;
}

FrameStage FrameTimings::dominantStage() const {
  FrameStage best = FrameStage::kCount;
  std::int64_t best_us = 0;
  for (std::size_t i = 0; i < kFrameStageCount; ++i) {
    if (isWorkerStage(static_cast<FrameStage>(i))) {
      continue;
    }
    if (stage_us[i] > best_us) {
      best_us = stage_us[i];
      best = static_cast<FrameStage>(i);
    }
  }
  return best;
}

void FrameTimingSummary::add(const FrameTimings& frame) {
  ++frames;
  total_wall_us += frame.wall_us;
  max_wall_us = std::max(max_wall_us, frame.wall_us);
  max_queue_depth = std::max(max_queue_depth, frame.queue_depth);
  for (std::size_t i = 0; i < kFrameStageCount; ++i) {
    total_us[i] += frame.stage_us[i];
    max_us[i] = std::max(max_us[i], frame.stage_us[i]);
  }
}

void FrameTimingSummary::reset() { *this = FrameTimingSummary{}; }

double FrameTimingSummary::framesPerSecond() const {
  if (frames == 0 || window_us <= 0) {
    return 0.0;
  }
  return static_cast<double>(frames) * 1e6 / static_cast<double>(window_us);
}

QString formatFrameLine(const FrameTimings& frame) {
  QString text = QStringLiteral("frame %1 wall=%2ms gui=%3ms queue=%4")
                     .arg(frame.frame_index)
                     .arg(microsToMillis(frame.wall_us),
                          microsToMillis(frame.guiThreadUs()))
                     .arg(frame.queue_depth);
  for (std::size_t i = 0; i < kFrameStageCount; ++i) {
    if (frame.stage_us[i] == 0) {
      continue;  // a consumer that is closed should not pad every line
    }
    text +=
        QStringLiteral(" %1=%2ms")
            .arg(
                QString::fromLatin1(frameStageName(static_cast<FrameStage>(i))),
                microsToMillis(frame.stage_us[i]));
  }
  const FrameStage dominant = frame.dominantStage();
  if (dominant != FrameStage::kCount) {
    text += QStringLiteral(" dominant=%1")
                .arg(QString::fromLatin1(frameStageName(dominant)));
  }
  return text;
}

QString formatSummary(const FrameTimingSummary& summary) {
  if (summary.frames == 0) {
    return QStringLiteral("summary: no frames displayed");
  }
  const auto mean = [&summary](std::int64_t total) {
    return microsToMillis(total / summary.frames);
  };

  QString text =
      QStringLiteral(
          "summary: %1 frames %2 fps wall mean=%3ms max=%4ms "
          "queue max=%5")
          .arg(summary.frames)
          .arg(QString::number(summary.framesPerSecond(), 'f', 1),
               mean(summary.total_wall_us), microsToMillis(summary.max_wall_us))
          .arg(summary.max_queue_depth);

  for (std::size_t i = 0; i < kFrameStageCount; ++i) {
    if (summary.total_us[i] == 0) {
      continue;
    }
    text +=
        QStringLiteral(" %1 mean=%2ms max=%3ms")
            .arg(
                QString::fromLatin1(frameStageName(static_cast<FrameStage>(i))),
                mean(summary.total_us[i]), microsToMillis(summary.max_us[i]));
  }
  return text;
}

FrameProfiler& FrameProfiler::instance() {
  static FrameProfiler profiler;
  return profiler;
}

void FrameProfiler::setEnabled(bool enabled) {
  const bool was_enabled =
      enabled_.exchange(enabled, std::memory_order_relaxed);
  if (was_enabled == enabled) {
    return;
  }
  // A partial frame measured under the old setting would be reported as if it
  // were whole, so it is discarded rather than emitted.
  frame_open_ = false;
  latency_running_ = false;
  current_ = FrameTimings{};
  summary_.reset();
  if (enabled) {
    window_timer_.start();
    ORC_LOG_INFO("frame timing: collection started");
  } else {
    ORC_LOG_INFO("frame timing: collection stopped");
  }
}

void FrameProfiler::beginFrame(std::uint64_t frame_index) {
  if (!isEnabled()) {
    return;
  }
  if (frame_open_) {
    // The previous frame never reached the display (superseded mid-flight).
    // Emit it so the cost it did incur is still accounted for.
    emitFrame();
  }
  current_ = FrameTimings{};
  current_.frame_index = frame_index;
  frame_open_ = true;
  latency_running_ = false;
  frame_timer_.start();
}

void FrameProfiler::markPreviewRequested(int queue_depth) {
  if (!isEnabled() || !frame_open_) {
    return;
  }
  current_.queue_depth = std::max(current_.queue_depth, queue_depth);
  latency_timer_.start();
  latency_running_ = true;
}

void FrameProfiler::markPreviewReady() {
  if (!isEnabled() || !frame_open_ || !latency_running_) {
    return;
  }
  current_.stage_us[static_cast<std::size_t>(FrameStage::kPreviewRender)] +=
      latency_timer_.nsecsElapsed() / 1000;
  latency_running_ = false;
}

void FrameProfiler::addStage(FrameStage stage, std::int64_t micros) {
  if (!isEnabled() || !frame_open_ || stage == FrameStage::kCount) {
    return;
  }
  current_.stage_us[static_cast<std::size_t>(stage)] += micros;
}

void FrameProfiler::endFrame() {
  if (!isEnabled() || !frame_open_) {
    return;
  }
  emitFrame();
}

void FrameProfiler::emitFrame() {
  current_.wall_us = frame_timer_.nsecsElapsed() / 1000;
  frame_open_ = false;
  latency_running_ = false;

  ORC_LOG_INFO("frame timing: {}", formatFrameLine(current_).toStdString());
  summary_.add(current_);

  if (window_timer_.isValid() && window_timer_.elapsed() >= kSummaryWindowMs) {
    summary_.window_us = window_timer_.nsecsElapsed() / 1000;
    ORC_LOG_INFO("frame timing: {}", formatSummary(summary_).toStdString());
    summary_.reset();
    window_timer_.start();
  }
}

}  // namespace orc::gui
