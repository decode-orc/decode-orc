/*
 * File:        vbi_frame_index.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Resolves output frames to stored frames from the frame counter
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_FRAME_INDEX_H
#define ORC_VBI_FRAME_INDEX_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "vbi_frame_sequence.h"
#include "vbi_source_format.h"

namespace orc {

// One stored frame's counter, read on its own.
//
// Separated from the reader so the index can be exercised against a synthetic
// counter sequence without a byte stream, and so that resolving an output
// frame decodes four bytes rather than a whole stored frame.
class IVBIFrameCounterSource {
 public:
  virtual ~IVBIFrameCounterSource() = default;

  // Counter of one stored frame, or an empty optional when the format carries
  // none.  Returns false with an error message when the frame could not be
  // read at all.
  virtual bool frame_counter(uint64_t stored_frame_index,
                             std::optional<uint32_t>& out_counter,
                             std::string& error_message) const = 0;
};

// The output frame timeline of a capture that is synthesised lazily.
//
// VBIFrameTimeline is the same model built by observing every stored frame in
// order, which is what a run that reads the whole capture does.  This index is
// for the run that does not: frames are synthesised on demand and in whatever
// order they are asked for, and a four-hour capture holds 368 007 of them, so
// reading every frame's counter to find out how many output frames there are
// would decode the entire capture before the first frame could be shown.
//
// It does not have to.  The driver's counter advances once per captured frame
// whether or not the frame reached the file, so for a capture with no drops
// counter(n) - counter(0) == n at every n.  Two counter reads therefore
// establish the total number of dropped frames across the whole capture, and
// because the counter is monotonic, the stored frame carrying any given output
// frame is found by bisection over a range that the drop total bounds — one
// read for a capture that dropped nothing, which is the overwhelmingly common
// case (design §6.3).
//
// Not thread-safe: resolving a frame reads counters through the borrowed
// source, which holds a stream position.  Callers serialise access, as they
// already must for the reader.
class VBIFrameIndex {
 public:
  // An empty index over no frames.
  VBIFrameIndex() = default;

  // Probe the capture's endpoints and establish the output timeline.
  //
  // |counters| is borrowed and must outlive the index.  Returns false with an
  // error message only when a counter could not be read; a capture whose
  // counter is missing, or whose counter did something no timeline can be
  // rebuilt from, is a successfully built index that says so.
  static bool build(const VBISourceFormat& format,
                    const VBIFrameSequenceConfig& config,
                    uint64_t stored_frame_count,
                    const IVBIFrameCounterSource& counters,
                    VBIFrameIndex& out_index, std::string& error_message);

  const VBIFrameSequenceConfig& config() const { return config_; }

  uint64_t stored_frame_count() const { return stored_frame_count_; }

  // Frames the stage emits: the stored frames, plus the frames padding
  // synthesises to fill the counter's gaps.
  uint64_t output_frame_count() const { return output_frame_count_; }

  // True when the source format carries a frame counter, and so when drops
  // are detectable at all.  For every other format the absence of reported
  // drops means nothing, which the summary says in as many words.
  bool counter_available() const;

  // Frames the counter says never reached the file.
  uint64_t dropped_frame_count() const { return dropped_frame_count_; }

  // True when the counter did something the timeline cannot be rebuilt from:
  // it ran backwards, repeated, or the policy is to emit only the frames
  // present, so output frame numbering no longer follows the source's.
  bool timeline_broken() const { return timeline_broken_; }

  // Television field the first stored field of each frame carries.
  uint32_t first_tv_field() const;

  // The field-order assumption, in the form the capture notes record it.
  std::string field_order_note() const;

  // Human-readable account of the sequence: field order, what the counter
  // showed and what the policy did about it.
  std::string summary() const;

  // What an output frame is made from.  Returns false with an error message
  // for an index beyond the capture, or when a counter read failed.
  bool frame_plan(uint64_t output_frame_index, VBIOutputFramePlan& out_plan,
                  std::string& error_message) const;

  // True when frame_plan() reads no counters and touches no state, which is
  // whenever output frame n is stored frame n.  It may then be called from
  // any number of threads at once without the lock the counter reads need.
  bool plans_without_reading() const { return identity_mapping_; }

 private:
  // Counter of a stored frame, from the cache or through the source.
  bool counter_at(uint64_t stored_frame_index, uint32_t& out_counter,
                  std::string& error_message) const;

  // Output frames between the capture's first stored frame and this one, on
  // the counter's own modular arithmetic.
  uint64_t counter_offset(uint32_t counter) const;

  VBISourceFormat format_{};
  VBIFrameSequenceConfig config_{};

  const IVBIFrameCounterSource* counters_ = nullptr;

  uint64_t stored_frame_count_ = 0;
  uint64_t output_frame_count_ = 0;
  uint64_t dropped_frame_count_ = 0;

  uint32_t first_counter_ = 0;
  bool have_counters_ = false;

  // True when output frame n is stored frame n, which is the case whenever
  // nothing was dropped and whenever the policy keeps only what is present.
  // No counter is then read to resolve a frame.
  bool identity_mapping_ = true;

  bool timeline_broken_ = false;

  mutable std::map<uint64_t, uint32_t> counter_cache_;
};

}  // namespace orc

#endif  // ORC_VBI_FRAME_INDEX_H
