/*
 * File:        sink_read_pacer.h
 * Module:      orc-core
 * Purpose:     Keeps sinks that read one input near the same frames, so they
 *              share its frame caches
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/artifact.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/video_frame_representation.h>

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace orc {

/**
 * @brief Keeps several sinks reading one input within a few frames of each
 * other.
 *
 * Sinks that run side by side on one node's output share the frame caches
 * upstream of it only while they read near the same frames: a frame one sink
 * builds is then still cached when the other asks for it. Left alone, the
 * faster sink runs ahead, the caches evict what it read, and the slower sink
 * builds every frame again.
 *
 * A reader's position is the furthest frame it has read. A read more than
 * window() frames past the slowest reader's position waits until that reader
 * catches up. The slowest reader never waits, so the readers as a whole always
 * make progress. A reader that finishes, fails or is cancelled detaches, which
 * releases any read waiting on it.
 *
 * Every reader starts at the input's first frame, so a sink still setting up
 * holds the others near the start rather than letting them run past the
 * caches. A read further behind a reader's position than the rewind distance
 * starts a new pass over the input, and moves the position back to it.
 *
 * Thread safety: every method may be called from any thread.
 */
class SinkReadPacer {
 public:
  using Reader = size_t;

  /// @p window: how far past the slowest reader a read may go without
  /// waiting. @p rewind: how far behind its position a read must be to start
  /// a new pass.
  SinkReadPacer(FrameID start, size_t window, size_t rewind);

  /// A new reader, positioned at the start.
  Reader add_reader();

  /// Waits until @p reader may read frame @p id, then records the read.
  void before_read(Reader reader, FrameID id);

  /// Removes @p reader: it no longer holds back the others. Idempotent.
  void detach(Reader reader);

  /// Stops pacing: every waiting read proceeds, and later reads never wait.
  void release();

  size_t window() const { return window_; }

 private:
  struct ReaderState {
    FrameID position = 0;
    bool attached = true;
  };

  // Whether @p reader may read @p id now. Lock held.
  bool may_read(Reader reader, FrameID id) const;

  const FrameID start_;
  const size_t window_;
  const size_t rewind_;

  mutable std::mutex mutex_;
  std::condition_variable moved_;
  std::vector<ReaderState> readers_;
  bool released_ = false;
};

/**
 * @brief One sink's view of an input it shares with other sinks: the input
 * unchanged, with every sample read paced by a SinkReadPacer.
 *
 * Every accessor gives exactly what the input gives. Reads of a frame's
 * samples (frames, lines, luma and chroma) first pass the pacer; metadata,
 * hints, audio, EFM and AC3 do not, since they do not go through the frame
 * caches the pacing is for.
 *
 * max_concurrent_frame_requests() reports the input's bound less the pacer's
 * window: the sinks read within that window of each other, so the frames they
 * want at once span one sink's frames plus the window, and the caches must
 * hold that span.
 *
 * Detaches its reader when destroyed.
 */
class PacedFrameRepresentation final : public VideoFrameRepresentationWrapper,
                                       public Artifact {
 public:
  PacedFrameRepresentation(
      std::shared_ptr<const VideoFrameRepresentation> source,
      const Artifact& source_artifact, std::shared_ptr<SinkReadPacer> pacer);
  ~PacedFrameRepresentation() override;

  std::string type_name() const override { return type_name_; }

  /// Stops this view holding back the other readers.
  void detach() const { pacer_->detach(reader_); }

  const sample_type* get_frame(FrameID id) const override;
  const sample_type* get_line(FrameID id, size_t line) const override;
  std::vector<sample_type> get_frame_copy(FrameID id) const override;
  std::vector<sample_type> get_line_samples(FrameID id,
                                            size_t line) const override;
  const sample_type* get_frame_luma(FrameID id) const override;
  const sample_type* get_frame_chroma(FrameID id) const override;
  const sample_type* get_line_luma(FrameID id, size_t line) const override;
  const sample_type* get_line_chroma(FrameID id, size_t line) const override;

  std::shared_ptr<const VideoFrameRepresentation> video_passthrough_source(
      FrameID id) const override;
  size_t max_concurrent_frame_requests() const override;

 private:
  std::shared_ptr<SinkReadPacer> pacer_;
  SinkReadPacer::Reader reader_;
  std::string type_name_;
};

}  // namespace orc
