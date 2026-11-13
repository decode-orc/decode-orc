/*
 * File:        sink_read_pacer.cpp
 * Module:      orc-core
 * Purpose:     Keeps sinks that read one input near the same frames, so they
 *              share its frame caches
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "sink_read_pacer.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace orc {

SinkReadPacer::SinkReadPacer(FrameID start, size_t window, size_t rewind)
    : start_(start), window_(window), rewind_(rewind) {}

SinkReadPacer::Reader SinkReadPacer::add_reader() {
  std::lock_guard<std::mutex> lock(mutex_);
  readers_.push_back(ReaderState{start_, true});
  return readers_.size() - 1;
}

bool SinkReadPacer::may_read(Reader reader, FrameID id) const {
  if (released_ || !readers_[reader].attached) return true;
  FrameID slowest = std::numeric_limits<FrameID>::max();
  for (const auto& state : readers_) {
    if (state.attached) slowest = std::min(slowest, state.position);
  }
  // A detached reader, or no reader at all, holds back nobody.
  if (slowest == std::numeric_limits<FrameID>::max()) return true;
  if (id <= slowest || id - slowest <= window_) return true;
  // The slowest reader never waits, so the readers always make progress.
  return readers_[reader].position == slowest;
}

void SinkReadPacer::before_read(Reader reader, FrameID id) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (reader >= readers_.size()) return;
  moved_.wait(lock, [&] { return may_read(reader, id); });

  auto& state = readers_[reader];
  const bool ahead = id > state.position;
  const bool new_pass = !ahead && state.position - id > rewind_;
  if (ahead || new_pass) {
    state.position = id;
    lock.unlock();
    moved_.notify_all();
  }
}

void SinkReadPacer::detach(Reader reader) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reader >= readers_.size() || !readers_[reader].attached) return;
    readers_[reader].attached = false;
  }
  moved_.notify_all();
}

void SinkReadPacer::release() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
  }
  moved_.notify_all();
}

// ============================================================================
// PacedFrameRepresentation
// ============================================================================

PacedFrameRepresentation::PacedFrameRepresentation(
    std::shared_ptr<const VideoFrameRepresentation> source,
    const Artifact& source_artifact, std::shared_ptr<SinkReadPacer> pacer)
    : VideoFrameRepresentationWrapper(std::move(source)),
      Artifact(source_artifact.id(), source_artifact.provenance()),
      pacer_(std::move(pacer)),
      reader_(pacer_->add_reader()),
      type_name_(source_artifact.type_name()) {}

PacedFrameRepresentation::~PacedFrameRepresentation() { detach(); }

const VideoFrameRepresentation::sample_type*
PacedFrameRepresentation::get_frame(FrameID id) const {
  pacer_->before_read(reader_, id);
  return source_->get_frame(id);
}

const VideoFrameRepresentation::sample_type* PacedFrameRepresentation::get_line(
    FrameID id, size_t line) const {
  pacer_->before_read(reader_, id);
  return source_->get_line(id, line);
}

std::vector<VideoFrameRepresentation::sample_type>
PacedFrameRepresentation::get_frame_copy(FrameID id) const {
  pacer_->before_read(reader_, id);
  return source_->get_frame_copy(id);
}

std::vector<VideoFrameRepresentation::sample_type>
PacedFrameRepresentation::get_line_samples(FrameID id, size_t line) const {
  pacer_->before_read(reader_, id);
  return source_->get_line_samples(id, line);
}

const VideoFrameRepresentation::sample_type*
PacedFrameRepresentation::get_frame_luma(FrameID id) const {
  pacer_->before_read(reader_, id);
  return source_->get_frame_luma(id);
}

const VideoFrameRepresentation::sample_type*
PacedFrameRepresentation::get_frame_chroma(FrameID id) const {
  pacer_->before_read(reader_, id);
  return source_->get_frame_chroma(id);
}

const VideoFrameRepresentation::sample_type*
PacedFrameRepresentation::get_line_luma(FrameID id, size_t line) const {
  pacer_->before_read(reader_, id);
  return source_->get_line_luma(id, line);
}

const VideoFrameRepresentation::sample_type*
PacedFrameRepresentation::get_line_chroma(FrameID id, size_t line) const {
  pacer_->before_read(reader_, id);
  return source_->get_line_chroma(id, line);
}

std::shared_ptr<const VideoFrameRepresentation>
PacedFrameRepresentation::video_passthrough_source(FrameID /*id*/) const {
  return source_;  // every frame is the input's, unchanged
}

size_t PacedFrameRepresentation::max_concurrent_frame_requests() const {
  const size_t bound = source_->max_concurrent_frame_requests();
  if (bound == std::numeric_limits<size_t>::max()) return bound;
  return bound > pacer_->window() ? bound - pacer_->window() : 1;
}

}  // namespace orc
