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
#include <orc/support/throttled_ring_reader.h>

#include <cstdint>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace orc {

// The four sample encodings the CVBS container format can declare (file
// format spec §3.1). Exposed here (rather than kept file-local, as
// cvbs_source_stage.cpp keeps its identical copy) only so CVBSStreamReader's
// constructor has a type to take and unit tests can name it directly.
enum class SampleEncoding { kU10, kU16, kTPG21, kS16 };

// Reads CVBS frames strictly forward from an input stream, one full frame
// at a time, into a fixed-size ring buffer — a thin wrapper around the
// generic orc::pipe_io::ThrottledRingReader (see that header for the
// threading model and the read-ahead throttle's design rationale) that
// supplies the "read frame_samples_ words and normalise them" piece specific
// to the CVBS wire format. See cvbs_stream_source_stage.cpp for the wire
// format itself and the known limitation around cancelling a blocked read.
// Exposed here so it can be unit-tested directly against an in-memory stream
// (e.g. std::istringstream) without going through the stage/DAG/registry
// machinery at all.
class CVBSStreamReader {
 public:
  CVBSStreamReader(std::istream& input, size_t frame_samples,
                   size_t frame_count, size_t buffer_frames,
                   SampleEncoding encoding, int32_t blanking_10bit);

  CVBSStreamReader(const CVBSStreamReader&) = delete;
  CVBSStreamReader& operator=(const CVBSStreamReader&) = delete;

  // Blocks until frame `id`'s samples are available. Returned pointer is
  // valid until the frame scrolls out of the ring buffer or this object is
  // destroyed — matching VideoFrameRepresentation::get_frame()'s own
  // contract, do not retain across calls. Returns nullptr when `id` is out
  // of [0, frame_count), the stream has failed, or `id` was requested after
  // it already scrolled out of the buffer window.
  const int16_t* get_frame(FrameID id) const {
    return reader_.get_frame(static_cast<size_t>(id));
  }

  bool failed() const { return reader_.failed(); }
  bool is_eof() const { return reader_.is_eof(); }
  std::string last_error() const { return reader_.last_error(); }
  void request_stop() { reader_.request_stop(); }

 private:
  orc::pipe_io::ThrottledRingReader<int16_t> reader_;
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
//     and sample_encoding is a required parameter instead of one with a
//     metadata-derived default. frame_count is optional — left at 0 (the
//     default) it means "unbounded", reading until a real end of stream
//     rather than requiring an exact count up front.
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
  uint32_t frame_count_ = 0;     // 0 = unbounded; replaces .meta's count
  uint32_t buffer_frames_ = 32;  // ring buffer depth; see CVBSStreamReader
  // Host-owned; see orc::kStreamReaderCountParameter.
  uint32_t stream_reader_count_ = 1;

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
