/*
 * File:        tbc_stream_source_stage.h
 * Module:      orc-core
 * Purpose:     Sequential (pipe-compatible) TBC source loading stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef TBC_STREAM_SOURCE_STAGE_H
#define TBC_STREAM_SOURCE_STAGE_H

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

// Reads TBC frames strictly forward from an input stream, one frame (a pair
// of fields) at a time — a thin wrapper around the generic
// orc::pipe_io::ThrottledRingReader (see that header for the threading model
// and read-ahead throttle) that supplies the "read two fields and assemble
// them into a CVBS_U10_4FSC frame" piece specific to the TBC wire format and
// the given video system. See tbc_stream_source_stage.cpp for the wire
// format itself. Exposed here so it can be unit-tested directly against an
// in-memory stream (e.g. std::istringstream) without going through the
// stage/DAG/registry machinery at all.
class TBCStreamReader {
 public:
  // `system` fixes the field geometry (line counts, samples per line) and
  // which converter (Pal/Ntsc/PalM TBCConverter) assembles each frame.
  // `black_16b_ire` / `white_16b_ire` are the TBC-domain calibration levels
  // a real capture's metadata would otherwise supply.
  TBCStreamReader(std::istream& input, VideoSystem system, size_t frame_count,
                  size_t buffer_frames, int32_t black_16b_ire,
                  int32_t white_16b_ire);

  TBCStreamReader(const TBCStreamReader&) = delete;
  TBCStreamReader& operator=(const TBCStreamReader&) = delete;

  // Blocks until frame `id`'s samples are available. Returned pointer is
  // valid until the frame scrolls out of the ring buffer or this object is
  // destroyed — do not retain across calls. Returns nullptr when `id` is out
  // of [0, frame_count), the stream has failed, or `id` was requested after
  // it already scrolled out of the buffer window.
  const int16_t* get_frame(FrameID id) const {
    return reader_.get_frame(static_cast<size_t>(id));
  }

  bool failed() const { return reader_.failed(); }
  std::string last_error() const { return reader_.last_error(); }

 private:
  orc::pipe_io::ThrottledRingReader<int16_t> reader_;
};

// A sequential-access counterpart to TBCSourceStage
// (orc/plugins/stages/tbc_source/), for a source that cannot be opened,
// seeked, or re-read — a pipe. See the "-" convention in
// orc/support/pipe_io.h.
//
// Trades away everything that convention can't carry through a single
// stdin stream:
//   - No .tbc.db / .tbc.json metadata: video system is fixed per concrete
//     subclass exactly like FixedFormatCVBSSourceStage, and the TBC-domain
//     calibration levels (normally read from the metadata's black16bIre /
//     white16bIre) are required parameters instead. Composite only — Y/C
//     needs two separate files (.tbcy/.tbcc), which a single "-" cannot
//     carry.
//   - No audio, dropout sidecar, EFM, or AC3 RF data, and no NTSC-J
//     auto-detection (the standard 7.5 IRE setup pedestal is always
//     assumed for NTSC/PAL_M). Video only, and — unlike TBCSourceStage,
//     which measures a per-capture black level — every frame from this
//     stage carries the nominal signal levels for its system.
//
// Wire format is a flat, unframed sequence of 16-bit words identical to the
// on-disk .tbc layout: each frame is two consecutive fields, field 1 then
// field 2, each field stored at field1_lines() * samples_per_line(system)
// words regardless of which field it is (ld-decode pads field 2 out to
// field 1's line count on every system this stage supports; see
// tbc_source's own read_field_samples() callers for the identical stride).
// Piping a real .tbc file through this stage (`cat file.tbc | orc-cli ...
// input_path=-`) reads byte-for-byte the same data reading the file
// directly would, provided frame_count matches the file's real frame count.
//
// Reads strictly forward: frames must be requested in a range no wider
// than `buffer_frames` at any time (see orc::pipe_io::ThrottledRingReader),
// which is why this stage always reports IStreamingCompatibility true
// regardless of parameters — there is no configuration of this stage that
// is NOT streaming-safe.
class FixedFormatTBCStreamSourceStage : public DAGStage,
                                        public ParameterizedStage,
                                        public IStreamingCompatibility {
 public:
  explicit FixedFormatTBCStreamSourceStage(
      const char* stage_name, const char* fixed_display_name,
      const char* description, VideoFormatCompatibility compatible_formats,
      VideoSystem system);
  ~FixedFormatTBCStreamSourceStage() override = default;

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

  // IStreamingCompatibility interface. Always true — see the class comment.
  bool supports_streaming_execution() const override { return true; }

 protected:
  VideoSystem system_;

 private:
  const char* stage_name_;
  std::string display_name_;
  const char* description_;
  VideoFormatCompatibility compatible_formats_;

  std::string input_path_;       // "-" (stdin) or a real named pipe path
  int32_t black_16b_ire_ = 0;    // required; from the capture's black16bIre
  int32_t white_16b_ire_ = 0;    // required; from the capture's white16bIre
  uint32_t frame_count_ = 0;     // required; replaces .tbc.db's frame count
  uint32_t buffer_frames_ = 32;  // ring buffer depth; see ThrottledRingReader

  // execute() is called once per DAGExecutor batch (see triggerAllSinks());
  // the underlying stream can only be read once, so the representation is
  // built on first call and reused on every later call with the same
  // effective parameters — the same instance-level memoization contract
  // every CVBS/TBC source already follows (see project_to_dag.h), and
  // specifically required by IStreamingCompatibility's documented contract
  // for a stage that answers true.
  mutable std::mutex execute_mutex_;
  mutable std::string cached_config_key_;
  mutable ArtifactPtr cached_representation_;
};

class PALTBCStreamSourceStage final : public FixedFormatTBCStreamSourceStage {
 public:
  PALTBCStreamSourceStage();
  ~PALTBCStreamSourceStage() override = default;
};

class NTSCTBCStreamSourceStage final : public FixedFormatTBCStreamSourceStage {
 public:
  NTSCTBCStreamSourceStage();
  ~NTSCTBCStreamSourceStage() override = default;
};

class PALMTBCStreamSourceStage final : public FixedFormatTBCStreamSourceStage {
 public:
  PALMTBCStreamSourceStage();
  ~PALMTBCStreamSourceStage() override = default;
};

}  // namespace orc

#endif  // TBC_STREAM_SOURCE_STAGE_H
