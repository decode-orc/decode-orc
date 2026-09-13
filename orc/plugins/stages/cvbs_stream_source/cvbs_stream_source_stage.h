/*
 * File:        cvbs_stream_source_stage.h
 * Module:      orc-core
 * Purpose:     Sequential (pipe-compatible) CVBS source loading stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef CVBS_STREAM_SOURCE_STAGE_H
#define CVBS_STREAM_SOURCE_STAGE_H

#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/streaming_capability.h>
#include <orc/stage/video_frame_representation.h>

#include <condition_variable>
#include <cstdint>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace orc {

// The four sample encodings the CVBS container format can declare (file
// format spec §3.1). Exposed here (rather than kept file-local, as
// cvbs_source_stage.cpp keeps its identical copy) only so CVBSStreamReader's
// constructor has a type to take and unit tests can name it directly.
enum class SampleEncoding { kU10, kU16, kTPG21, kS16 };

// Reads CVBS frames strictly forward from an input stream, one full frame
// at a time, into a fixed-size ring buffer — see cvbs_stream_source_stage.cpp
// for the full design rationale (wire format, threading model, and the known
// limitation around cancelling a blocked read). Exposed here so it can be
// unit-tested directly against an in-memory stream (e.g. std::istringstream)
// without going through the stage/DAG/registry machinery at all.
class CVBSStreamReader {
 public:
  CVBSStreamReader(std::istream& input, size_t frame_samples,
                   size_t frame_count, size_t buffer_frames,
                   SampleEncoding encoding, int32_t blanking_10bit);
  ~CVBSStreamReader();

  CVBSStreamReader(const CVBSStreamReader&) = delete;
  CVBSStreamReader& operator=(const CVBSStreamReader&) = delete;

  // Blocks until frame `id`'s samples are available. Returned pointer is
  // valid until the frame scrolls out of the ring buffer or this object is
  // destroyed — matching VideoFrameRepresentation::get_frame()'s own
  // contract, do not retain across calls. Returns nullptr when `id` is out
  // of [0, frame_count), the stream has failed, or `id` was requested after
  // it already scrolled out of the buffer window.
  const int16_t* get_frame(FrameID id) const;

  bool failed() const;
  std::string last_error() const;

 private:
  void fail_locked(const std::string& message) const;
  void reader_loop();

  std::istream& input_;
  const size_t frame_samples_;
  const size_t frame_count_;
  const size_t buffer_frames_;
  const SampleEncoding encoding_;
  const int32_t blanking_10bit_;

  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::vector<std::vector<int16_t>> ring_;
  size_t produced_ = 0;
  mutable bool failed_ = false;
  mutable std::string error_;
  bool stop_ = false;
  std::thread thread_;

  // IDs currently being waited on inside get_frame() — a caller inserts its
  // own id before waiting and erases it before returning, from either thread.
  // read_ahead_floor_ tracks the minimum of this set, and is what actually
  // gates reader_loop(): production may run at most buffer_frames_ ahead of
  // it, never of produced_ itself. Using the MINIMUM of what is genuinely
  // outstanding right now — rather than the maximum id ever requested, which
  // an earlier version of this reader used and which is why it was removed
  // (see reader_loop()'s comment) — means one thread's fast, high-id request
  // can never let the reader race past a slower thread's still-pending
  // low-id one: that low id simply keeps read_ahead_floor_ pinned until it is
  // satisfied, and nothing lets it jump ahead early. When pending_requests_
  // is empty, read_ahead_floor_ simply keeps its last value rather than
  // resetting — there is no "next" id to be conservative about yet, but
  // resetting to 0 would let an idle stretch (nobody currently waiting, e.g.
  // between one call returning and the next one starting) throw away
  // progress already made and force the reader back to square one.
  mutable std::multiset<size_t> pending_requests_;
  mutable size_t read_ahead_floor_ = 0;
};

// A sequential-access counterpart to FixedFormatCVBSSourceStage
// (orc/plugins/stages/cvbs_source/), for a source that cannot be opened,
// seeked, or re-read — a pipe. See the "-" convention in
// orc/support/pipe_io.h.
//
// Trades away everything that convention can't carry through a single
// stdin stream:
//   - No .meta sidecar (there is nothing to derive it from): video system
//     is fixed per concrete subclass exactly like FixedFormatCVBSSourceStage,
//     and sample_encoding/frame_count are required parameters instead of
//     optional ones with a metadata-derived default.
//   - No audio, dropout sidecar, EFM, or AC3 extension data. Video only.
//
// Wire format is a flat, unframed sequence of 16-bit words identical to the
// on-disk .cvbs layout (frame N at word offset N * frame_samples_from_system
// (system)) — piping a real .cvbs file through this stage (`cat file.cvbs |
// orc-cli ... input_path=-`) reads byte-for-byte the same data reading the
// file directly would.
//
// Reads strictly forward: frames must be requested in a range no wider
// than `buffer_frames` at any time (see CVBSStreamReader), which is why
// this stage always reports IStreamingCompatibility true regardless of
// parameters — unlike FixedFormatCVBSSourceStage, there is no configuration
// of this stage that is NOT streaming-safe.
class FixedFormatCVBSStreamSourceStage : public DAGStage,
                                         public ParameterizedStage,
                                         public IStreamingCompatibility {
 public:
  explicit FixedFormatCVBSStreamSourceStage(
      const char* stage_name, const char* fixed_display_name,
      const char* description, VideoFormatCompatibility compatible_formats,
      VideoSystem system);
  ~FixedFormatCVBSStreamSourceStage() override = default;

  // DAGStage interface
  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::SOURCE,
                        stage_name_,
                        display_name_,
                        description_,
                        0,
                        0,
                        1,
                        UINT32_MAX,
                        compatible_formats_};
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }

  // ParameterizedStage interface
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;

  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // IStreamingCompatibility interface. Always true: every configuration of
  // this stage reads strictly forward within a bounded window (see the
  // class comment above) — there is no parameter combination that needs
  // random access, a full pre-pass, or the total length known some other
  // way (frame_count is always an explicit parameter here).
  bool supports_streaming_execution() const override { return true; }

 protected:
  VideoSystem system_;

 private:
  const char* stage_name_;
  std::string display_name_;
  const char* description_;
  VideoFormatCompatibility compatible_formats_;

  std::string input_path_;       // "-" (stdin) or a real named pipe path
  std::string sample_encoding_;  // required; no ".meta"-derived default
  uint32_t frame_count_ = 0;     // required; replaces .meta's frame count
  uint32_t buffer_frames_ = 32;  // ring buffer depth; see CVBSStreamReader

  // execute() is called once per DAGExecutor batch (see triggerAllSinks());
  // the underlying stream can only be read once, so the representation
  // (and the CVBSStreamReader/background thread it owns) is built on first
  // call and reused on every later call with the same effective parameters
  // — the same instance-level memoization contract every CVBS/TBC source
  // already follows (see project_to_dag.h), and specifically required by
  // IStreamingCompatibility's documented contract for a stage that answers
  // true.
  mutable std::mutex execute_mutex_;
  mutable std::string cached_config_key_;
  mutable ArtifactPtr cached_representation_;
};

class PALCVBSStreamSourceStage final : public FixedFormatCVBSStreamSourceStage {
 public:
  PALCVBSStreamSourceStage();
  ~PALCVBSStreamSourceStage() override = default;
};

class NTSCCVBSStreamSourceStage final
    : public FixedFormatCVBSStreamSourceStage {
 public:
  NTSCCVBSStreamSourceStage();
  ~NTSCCVBSStreamSourceStage() override = default;
};

class PALMCVBSStreamSourceStage final
    : public FixedFormatCVBSStreamSourceStage {
 public:
  PALMCVBSStreamSourceStage();
  ~PALMCVBSStreamSourceStage() override = default;
};

}  // namespace orc

#endif  // CVBS_STREAM_SOURCE_STAGE_H
