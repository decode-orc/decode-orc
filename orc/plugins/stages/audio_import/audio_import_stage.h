/*
 * File:        audio_import_stage.h
 * Module:      orc-stage-plugin-audio_import
 * Purpose:     External WAV import transform stage (VFrameR) — attaches an
 *              external WAV file as a new audio channel pair
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
#include <string>
#include <vector>

#include "audio-resample/synchronous_audio_cache.h"
#include "audio_import_stage_deps_interface.h"

namespace orc {

// ============================================================================
// ImportedAudioChannelPairRepresentation
// ============================================================================
// Wraps a VideoFrameRepresentation and appends one audio channel pair served
// from an external WAV file ({origin: IMPORTED}). Video, dropout hints,
// EFM/AC3 signal data and all existing audio channel pairs pass through
// untouched.
//
// The WAV is converted to the pipeline form (48000 Hz synchronous,
// 24-bit-in-int32 stereo — see audio_channel_pair.h) lazily on first audio
// access: 16-bit material is widened, 24-bit unpacked, then the stream is
// resampled and segmented into cadence-sized per-frame blocks. A 48000 Hz
// WAV needs no resampling and each block is read straight from the file;
// other rates are resampled sequentially into a scratch file in the audio
// cache directory, so no whole-stream buffer is held.
//
// Thread safety: safe for concurrent reads; the one-time ingest conversion
// is guarded by std::call_once.
class ImportedAudioChannelPairRepresentation
    : public VideoFrameRepresentationWrapper,
      public Artifact {
 public:
  ImportedAudioChannelPairRepresentation(
      std::shared_ptr<const VideoFrameRepresentation> source,
      std::shared_ptr<IAudioImportDeps> deps,
      AudioChannelPairDescriptor descriptor, uint32_t wav_sample_rate_hz,
      uint16_t wav_bits_per_sample, VideoSystem system, size_t frame_count)
      : VideoFrameRepresentationWrapper(std::move(source)),
        Artifact(ArtifactID("audio_import"), Provenance{}),
        deps_(std::move(deps)),
        descriptor_(std::move(descriptor)),
        wav_sample_rate_hz_(wav_sample_rate_hz),
        wav_bits_per_sample_(wav_bits_per_sample),
        system_(system),
        frame_count_(frame_count) {}

  std::string type_name() const override {
    return "imported_audio_channel_pair_representation";
  }

  // --- Audio channel pairs: source pairs forward; one pair is appended -----

  size_t audio_channel_pair_count() const override {
    return source_pair_count() + 1;
  }
  std::optional<AudioChannelPairDescriptor> get_audio_channel_pair_descriptor(
      size_t pair) const override;
  std::vector<int32_t> get_audio_samples(size_t pair,
                                         FrameID id) const override;

 private:
  size_t source_pair_count() const {
    return source_ ? source_->audio_channel_pair_count() : 0;
  }
  // The appended pair is always the last index (pair order is stable
  // through the DAG; pair-adding stages append).
  size_t imported_pair_index() const { return source_pair_count(); }

  // Ingest conversion, created on the first audio access: 16-bit material
  // is widened to the 24-bit-in-int32 carrier, 24-bit unpacked, then the
  // stream is converted to cadence-sized per-frame blocks totalling
  // audio_pair_offset(frame_count_) stereo pairs.
  const SynchronousAudioCache& audio_cache() const;

  std::shared_ptr<IAudioImportDeps> deps_;
  AudioChannelPairDescriptor descriptor_;
  uint32_t wav_sample_rate_hz_;
  uint16_t wav_bits_per_sample_;  // 16 or 24
  VideoSystem system_;
  size_t frame_count_;

  mutable std::once_flag convert_once_;
  mutable std::unique_ptr<SynchronousAudioCache> audio_cache_;
};

// ============================================================================
// AudioImportStage
// ============================================================================
class AudioImportStage : public DAGStage,
                         public ParameterizedStage,
                         public IStagePreviewCapability {
 public:
  AudioImportStage();

  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD
  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::TRANSFORM,
                        "audio_import",
                        "Audio Import",
                        "Attach an external WAV file as a new audio channel "
                        "pair",
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

  // ParameterizedStage
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

  // Test seam: replaces the production WAV file deps.
  void set_deps_override(std::shared_ptr<IAudioImportDeps> deps) {
    deps_override_ = std::move(deps);
  }

 private:
  std::string wav_path_;
  std::string pair_name_;

  mutable std::shared_ptr<const VideoFrameRepresentation> cached_output_;
  std::shared_ptr<IAudioImportDeps> deps_override_;
};

}  // namespace orc
