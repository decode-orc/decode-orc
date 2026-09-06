/*
 * File:        efm_audio_decode_stage.h
 * Module:      orc-stage-plugin-efm_audio_decode
 * Purpose:     EFM audio decode transform stage (VFrameR) — decodes the EFM
 *              t-value stream to CD audio and appends it as a synchronous
 *              audio channel pair
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/artifact.h>
#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>

#include <memory>
#include <mutex>

#include "efm_audio_decode_stage_deps_interface.h"

namespace orc {

// ============================================================================
// EFMAudioChannelPairRepresentation
// ============================================================================
// Wraps a VideoFrameRepresentation and appends one audio channel pair
// carrying the decoded EFM digital audio ({name: options.pair_name, default
// "EFM digital audio"; origin: EFM}) in the pipeline form: 48 kHz synchronous
// (frame-locked)
// 24-bit-in-int32 stereo. Video, dropout hints, EFM/AC3 signal data and all
// existing audio channel pairs pass through untouched.
//
// EFM decode is whole-stream sequential (CIRC interleaving spans sectors), so
// it cannot run per-frame. The decode runs lazily, at most once, on the first
// sample access to the appended pair (std::call_once). The decoded CD audio
// (IEC 60908: 44.1 kHz 16-bit stereo) is widened to the 24-bit carrier,
// resampled 44100 → 48000 Hz, cadence-segmented via the shared resampler,
// and stored in the deps' scratch cache as a raw int32 cadence-aligned
// stream; per-frame serving seeks by audio_pair_offset(id, system).
// Video-only access never pays for the decode. The decoder aligns the head
// of the stream with the input (video) timeline — compensating the CIRC
// de-interleave latency and the input consumed during sync acquisition, plus
// the user's offset_ms slip (issue #231) — so pair 0 of the decoded stream
// is synchronous with the first frame of the wrapped representation. A
// failed decode leaves the appended pair silent (cadence-sized zero blocks).
// Bit-exact un-resampled CD audio remains available via the EFM Decoder
// Sink.
//
// Thread safety: safe for concurrent reads; the lazy decode+conversion is
// serialised by std::call_once and cache reads are stateless.
class EFMAudioChannelPairRepresentation
    : public VideoFrameRepresentationWrapper,
      public Artifact {
 public:
  EFMAudioChannelPairRepresentation(
      std::shared_ptr<const VideoFrameRepresentation> source,
      std::shared_ptr<IEFMAudioDecodeDeps> deps,
      const EFMAudioDecodeOptions& options)
      : VideoFrameRepresentationWrapper(std::move(source)),
        Artifact(ArtifactID("efm_audio_channel_pair"), Provenance{}),
        deps_(std::move(deps)),
        options_(options) {}

  std::string type_name() const override {
    return "efm_audio_channel_pair_representation";
  }

  // --- Audio channel pairs: source pairs forward; one EFM pair is appended --

  size_t audio_channel_pair_count() const override {
    return source_pair_count() + 1;
  }

  std::optional<AudioChannelPairDescriptor> get_audio_channel_pair_descriptor(
      size_t pair) const override;

  std::vector<int32_t> get_audio_samples(size_t pair,
                                         FrameID id) const override;

  // Runs the deferred whole-stream decode now (overrides the base VFR hook),
  // forwarding its per-frame progress. Lets a sink meter the decode on its
  // progress dialog instead of stalling silently inside the first
  // get_audio_samples() call.
  void prime_audio_decode(
      const AudioDecodeProgressFn& progress) const override {
    ensure_decoded(progress);
  }

  // This stage only appends an audio channel pair; the CVBS video content of
  // every frame passes through byte-identical, so the host may share
  // frame-content-keyed analysis with the upstream node.
  std::shared_ptr<const VideoFrameRepresentation> video_passthrough_source(
      FrameID /*id*/) const override {
    return source_;
  }

 private:
  size_t source_pair_count() const {
    return source_ ? source_->audio_channel_pair_count() : 0;
  }
  // The appended pair is always the last index (pair order is stable through
  // the DAG; pair-adding stages append).
  size_t efm_pair_index() const { return source_pair_count(); }

  // Runs the whole-stream EFM decode and the 48 kHz synchronous conversion
  // at most once; safe to call from any accessor thread. Failures are logged
  // and leave the appended pair silent. When |progress| is set (priming path)
  // it receives frame-granular decode progress and phase messages.
  void ensure_decoded(const AudioDecodeProgressFn& progress = {}) const;

  std::shared_ptr<IEFMAudioDecodeDeps> deps_;
  EFMAudioDecodeOptions options_;

  mutable std::once_flag decode_once_;
  // True when the synchronous cache holds the converted stream. Written
  // under decode_once_; call_once synchronises readers.
  mutable bool synchronous_audio_ready_ = false;
};

// ============================================================================
// EFMAudioDecodeStage
// ============================================================================
class EFMAudioDecodeStage : public DAGStage,
                            public ParameterizedStage,
                            public IStagePreviewCapability {
 public:
  EFMAudioDecodeStage();

  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD
  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::TRANSFORM,
                        "efm_audio_decode",
                        "EFM Audio Decode",
                        "Decodes the EFM t-value stream to CD audio and "
                        "appends it as a synchronous audio channel pair",
                        1,
                        1,
                        1,
                        UINT32_MAX,
                        VideoFormatCompatibility::ALL};
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 1; }

  std::shared_ptr<const VideoFrameRepresentation> process(
      std::shared_ptr<const VideoFrameRepresentation> source) const;

  // ParameterizedStage
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

  // Test seam: replaces the production decode/cache deps.
  void set_deps_override(std::shared_ptr<IEFMAudioDecodeDeps> deps) {
    deps_override_ = std::move(deps);
  }

 private:
  bool no_timecodes_ = false;
  bool no_audio_concealment_ = false;
  // Q-8: ignore the 50/15 us pre-emphasis flag and skip de-emphasis when true.
  bool ignore_preemphasis_ = false;
  // Name for the appended EFM audio channel pair; empty falls back to the
  // "EFM digital audio" default at the descriptor.
  std::string pair_name_ = "EFM digital audio";
  bool report_ = false;      // checkbox: write a decode statistics report
  std::string report_path_;  // report destination when report_ is enabled
  // User sync slip in milliseconds on top of the automatic video-timeline
  // alignment; positive delays the audio, negative advances it (issue #231).
  double offset_ms_ = 0.0;
  // Issue #307: smallest producer doubt (0 trusted - 15 distrusted) that makes
  // an otherwise-clean EFM symbol a C1/C2 erasure candidate; 0 (the default)
  // disables it, leaving the decode bit-exact.
  int32_t doubt_erasure_threshold_ = 0;

  mutable std::shared_ptr<const VideoFrameRepresentation> cached_output_;
  std::shared_ptr<IEFMAudioDecodeDeps> deps_override_;
};

}  // namespace orc
