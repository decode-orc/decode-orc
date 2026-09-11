/*
 * File:        frame_profiler.h
 * Module:      orc-gui
 * Purpose:     Per-frame timing breakdown of the preview and observer path
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_FRAME_PROFILER_H
#define ORC_GUI_FRAME_PROFILER_H

#include <QElapsedTimer>
#include <QString>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace orc::gui {

/**
 * @brief One measured segment of the work a displayed frame costs.
 *
 * The segments are the places the GUI thread (or the render worker on the
 * GUI's behalf) spends time between one displayed frame and the next. They are
 * deliberately coarse: the point is to say which consumer dominates, not to
 * profile inside it.
 */
enum class FrameStage {
  kPreviewRender,  ///< requestPreview() to previewReady() — worker latency.
  kUpdateAll,      ///< updateAllPreviewComponents() on the GUI thread.
  kVectorscope,    ///< Vectorscope refresh, including any decode it forces.
  kHistogram,      ///< Histogram refresh, including any decode it forces.
  kObservers,      ///< Observer dialog request issue.
  kVbi,            ///< VBI dialog request issue.
  kClosedCaption,  ///< Closed caption request issue.
  kFrameTiming,    ///< Frame timing dialog update.
  kWaveform,       ///< Waveform monitor update.
  kPreviewPaint,   ///< paintEvent() of the preview widgets.
  kCount
};

/// Number of measured segments.
inline constexpr std::size_t kFrameStageCount =
    static_cast<std::size_t>(FrameStage::kCount);

/// Short, stable name of a segment as it appears in the log.
const char* frameStageName(FrameStage stage);

/// Microsecond totals for one displayed frame.
struct FrameTimings {
  /// Preview item index the frame was displayed at.
  std::uint64_t frame_index = 0;
  /// Coordinator queue depth observed when the render was requested.
  int queue_depth = 0;
  /// Wall time from the start of the frame to the moment it was displayed.
  std::int64_t wall_us = 0;
  /// Per-segment totals. A segment may be entered more than once per frame;
  /// the entries add up.
  std::array<std::int64_t, kFrameStageCount> stage_us{};

  /// Time accounted for on the GUI thread. Excludes kPreviewRender, which is
  /// worker latency the GUI thread spends waiting rather than working, and
  /// would otherwise be double-counted against the segments that run inside
  /// that wait.
  std::int64_t guiThreadUs() const;

  /// The segment with the largest total, ignoring kPreviewRender. Returns
  /// kCount when nothing was measured.
  FrameStage dominantStage() const;
};

/// Totals across the frames displayed in one reporting window.
struct FrameTimingSummary {
  int frames = 0;
  std::int64_t window_us = 0;
  std::array<std::int64_t, kFrameStageCount> total_us{};
  std::array<std::int64_t, kFrameStageCount> max_us{};
  std::int64_t total_wall_us = 0;
  std::int64_t max_wall_us = 0;
  int max_queue_depth = 0;

  /// Fold one frame into the window.
  void add(const FrameTimings& frame);
  /// Drop every accumulated frame, keeping the window ready for reuse.
  void reset();
  /// Frames per second implied by the window, or 0 when it is empty.
  double framesPerSecond() const;
};

/// One-line record of a single displayed frame, for the log.
QString formatFrameLine(const FrameTimings& frame);

/// Multi-field record of a reporting window, for the log. Segments that
/// consumed no time are omitted so the line stays readable.
QString formatSummary(const FrameTimingSummary& summary);

/**
 * @brief Collects per-frame timings and writes them to the GUI log.
 *
 * Off by default and controlled by the Tools > Logging dialogue. While off,
 * every entry point is a relaxed atomic load and an immediate return: no
 * allocation, no formatting, no timer reads. That is what lets the scopes stay
 * permanently compiled into the preview path.
 *
 * Records are written at info level rather than debug, because the feature is
 * already opt-in through its own setting; requiring a second, unrelated
 * setting (the detail level) to see the output would be a trap.
 *
 * Thread-safety: GUI thread only. Every call site is a GUI-thread callback or
 * paint handler. The enabled flag alone is atomic, so the setting may be
 * applied from anywhere.
 */
class FrameProfiler {
 public:
  /// The process-wide profiler.
  static FrameProfiler& instance();

  /// Turn collection on or off. Turning it off discards any partial frame.
  void setEnabled(bool enabled);

  /// True while collection is on. Cheap enough for a hot path.
  bool isEnabled() const { return enabled_.load(std::memory_order_relaxed); }

  /// Start accounting for the frame at @p frame_index. An unfinished previous
  /// frame is emitted first, so a frame abandoned mid-flight is still visible.
  void beginFrame(std::uint64_t frame_index);

  /// Note that the render for the current frame was requested, with
  /// @p queue_depth requests already waiting on the worker.
  void markPreviewRequested(int queue_depth);

  /// Note that the render for the current frame arrived.
  void markPreviewReady();

  /// Add @p micros to @p stage of the current frame.
  void addStage(FrameStage stage, std::int64_t micros);

  /// Emit the current frame's line and fold it into the reporting window,
  /// emitting the window's summary when it is at least a second old.
  void endFrame();

  /// Reporting window as it stands. Exposed for tests.
  const FrameTimingSummary& summary() const { return summary_; }

  /// RAII timer for one segment. Reads the clock only while collection is on.
  class ScopedStage {
   public:
    explicit ScopedStage(FrameStage stage)
        : stage_(stage), active_(FrameProfiler::instance().isEnabled()) {
      if (active_) {
        timer_.start();
      }
    }
    ~ScopedStage() {
      if (active_) {
        FrameProfiler::instance().addStage(stage_,
                                           timer_.nsecsElapsed() / 1000);
      }
    }
    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;
    ScopedStage(ScopedStage&&) = delete;
    ScopedStage& operator=(ScopedStage&&) = delete;

   private:
    QElapsedTimer timer_;
    FrameStage stage_;
    bool active_;
  };

 private:
  FrameProfiler() = default;

  void emitFrame();

  std::atomic<bool> enabled_{false};
  bool frame_open_ = false;
  FrameTimings current_{};
  QElapsedTimer frame_timer_;
  QElapsedTimer latency_timer_;
  bool latency_running_ = false;
  FrameTimingSummary summary_{};
  QElapsedTimer window_timer_;
};

}  // namespace orc::gui

/// Time the enclosing scope into @p stage. Expands to a no-op read of one
/// atomic when frame profiling is off.
#define ORC_FRAME_STAGE_CONCAT_INNER(a, b) a##b
#define ORC_FRAME_STAGE_CONCAT(a, b) ORC_FRAME_STAGE_CONCAT_INNER(a, b)
#define ORC_FRAME_STAGE(stage)                                         \
  const ::orc::gui::FrameProfiler::ScopedStage ORC_FRAME_STAGE_CONCAT( \
      orc_frame_stage_scope_, __LINE__)(stage)

#endif  // ORC_GUI_FRAME_PROFILER_H
