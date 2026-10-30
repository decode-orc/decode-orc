/*
 * File:        audio_channel_map_stage.h
 * Module:      orc-stage-plugin-audio_channel_map
 * Purpose:     Audio channel routing transform stage (VFrameR) — delete,
 *              SMPTE 272M mono extraction, channel swap for a target audio
 *              channel pair, and L/R averaging where the VBI says mono
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/artifact.h>
#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace orc {

// One channel-routing operation. The source channel pair is the one the
// operation reads from; the routing operations additionally write to a target
// channel pair (a new appended pair, or an existing one).
//
// The mono operations are SMPTE 272M-1994 conformant: a mono programme is
// carried on the left channel only, with the right channel silenced (all
// zeros) — never duplicated across both channels.
enum class AudioChannelMapOperation {
  kDelete,             // remove the source channel pair; later pairs shift down
  kLeftToMono,         // in place: left channel kept, right channel zeroed
  kRightToMono,        // in place: right channel moved to left, right zeroed
  kCopyLeftToTarget,   // write [L, 0] from the source to the target pair
  kCopyRightToTarget,  // write [R, 0] from the source to the target pair
  kVbiMonoAverage,     // in place: [(L+R)/2, (L+R)/2] on frames the VBI
                       // programme status marks mono; other frames untouched
};

// Whether a frame's own VBI programme status marks its analogue sound as mono.
// Reads the biphase observer's "vbi" observations for both fields of
// |frame_id|. A field votes only when its programme status word passed parity:
// mono when the IEC 60857 or the Amendment 2 reading of it says mono, not mono
// otherwise. The frame is mono when at least one field votes mono and none
// votes otherwise; a frame without a readable status is not mono.
bool vbi_frame_is_mono(const IObservationContext& context, FrameID frame_id);

// Whether the two channels of one frame's interleaved stereo samples carry
// the same programme, so that averaging them only averages the noise. A disc
// can say mono while carrying the programme on one carrier alone (Dragon's
// Lair: the left channel is near silent, or carries its own sound), and the
// mean would then halve the programme or mix the two. The channels agree when
// their levels are within 6 dB and either they correlate at 0.5 or better or
// the louder is at or below -50 dBFS: on a silent mono passage the carriers'
// independent noise is all there is, so the correlation says nothing there
// and the balance alone has to do.
bool mono_channels_agree(const std::vector<int32_t>& samples);

// Decides whether frame |id| of a representation carries mono sound. The
// default runs the host "biphase" observer over the frame's VBI lines;
// tests inject their own.
using MonoFrameProbe =
    std::function<bool(const VideoFrameRepresentation&, FrameID)>;

// ============================================================================
// ChannelMappedRepresentation
// ============================================================================
// Wraps a VideoFrameRepresentation and applies one channel-routing operation.
// Video, dropout hints, EFM/AC3 signal data and untouched channel pairs pass
// through unchanged.
//
// - delete removes the source pair; later pairs shift down one index (count
// −1).
// - left_to_mono / right_to_mono overwrite the source pair in place with
//   SMPTE 272M mono (chosen channel on the left, right zeroed); count
//   unchanged.
// - copy_left_to_target / copy_right_to_target read the source pair and write
//   the mono channel to the target pair, leaving the source untouched. When the
//   target is "new" the mono pair is appended (count +1); when it is an
//   existing pair that pair is overwritten (count unchanged).
// - vbi_mono_average replaces both channels of the source pair with their
//   mean on the frames whose channels agree (mono_channels_agree) and which
//   |mono_probe| reports mono. The pair stays a stereo
//   pair, so the mean goes to both channels rather than SMPTE 272M's [M, 0]:
//   the frames around a mono stretch keep their stereo layout. Its origin is
//   kept, since it is still the source's programme.
//
// When the input cannot satisfy the operation — the source pair (or an
// existing routing target) is absent, or appending would exceed the
// channel-pair cap — the wrapper becomes a transparent pass-through: audio and
// video reach the output untouched. Audio routing is never a reason to stop
// video reaching a downstream sink or preview.
//
// The produced mono pair reports origin DERIVED; its name is |description| when
// |override_description| is set, otherwise the source pair's existing name is
// kept unchanged. The remap is a pure per-sample channel operation on the
// synchronous 24-bit-in-int32 stereo pairs; timing and per-frame pair counts
// are untouched.
//
// Thread safety: stateless after construction; safe for concurrent reads.
class ChannelMappedRepresentation : public VideoFrameRepresentationWrapper,
                                    public Artifact {
 public:
  ChannelMappedRepresentation(
      std::shared_ptr<const VideoFrameRepresentation> source,
      size_t source_pair, AudioChannelMapOperation operation,
      bool target_is_new, size_t target_pair, bool override_description,
      std::string description, MonoFrameProbe mono_probe = {})
      : VideoFrameRepresentationWrapper(std::move(source)),
        Artifact(ArtifactID("audio_channel_map"), Provenance{}),
        source_pair_(source_pair),
        operation_(operation),
        target_is_new_(target_is_new),
        target_pair_(target_pair),
        override_description_(override_description),
        description_(std::move(description)),
        mono_probe_(std::move(mono_probe)) {}

  std::string type_name() const override {
    return "channel_mapped_representation";
  }

  // --- Audio channel pairs --------------------------------------------------

  size_t audio_channel_pair_count() const override;
  std::optional<AudioChannelPairDescriptor> get_audio_channel_pair_descriptor(
      size_t pair) const override;
  std::vector<int32_t> get_audio_samples(size_t pair,
                                         FrameID id) const override;

 private:
  // How the channels of an output pair derive from its source pair.
  enum class ChannelFill {
    kNone,      // pass through unchanged
    kLeftMono,  // SMPTE 272M mono: keep L, zero R
    kRightMono  // SMPTE 272M mono: move R to L, zero R
  };

  size_t source_pair_count() const {
    return source_ ? source_->audio_channel_pair_count() : 0;
  }
  bool is_delete() const {
    return operation_ == AudioChannelMapOperation::kDelete;
  }
  bool is_route() const {
    return operation_ == AudioChannelMapOperation::kCopyLeftToTarget ||
           operation_ == AudioChannelMapOperation::kCopyRightToTarget;
  }
  bool is_append() const { return is_route() && target_is_new_; }
  bool is_mono_average() const {
    return operation_ == AudioChannelMapOperation::kVbiMonoAverage;
  }
  // Whether the configured operation can be applied to the wrapped source. It
  // cannot when the source pair (or a routing target) is absent from the
  // input, or when appending would exceed the channel-pair cap. An inactive
  // wrapper is a transparent pass-through: every pair keeps its index, count
  // and samples.
  bool is_active() const;
  // The mono fill implied by a left/right operation.
  ChannelFill mono_fill() const {
    return (operation_ == AudioChannelMapOperation::kLeftToMono ||
            operation_ == AudioChannelMapOperation::kCopyLeftToTarget)
               ? ChannelFill::kLeftMono
               : ChannelFill::kRightMono;
  }
  // Whether output pair |pair| is the mono pair this operation produces.
  bool is_result_pair(size_t pair) const;

  // Source pair index serving output pair |pair|.
  size_t source_pair_for(size_t pair) const;
  ChannelFill fill_for_pair(size_t pair) const;

  // Applies |fill| in place to interleaved stereo samples.
  static void apply_fill(std::vector<int32_t>& samples, ChannelFill fill);
  // Replaces both channels of interleaved stereo samples with their mean.
  static void average_channels(std::vector<int32_t>& samples);
  bool frame_is_mono(FrameID id) const;

  size_t source_pair_;
  AudioChannelMapOperation operation_;
  bool target_is_new_;
  size_t target_pair_;
  bool
      override_description_;  // true: use description_; false: keep source name
  std::string description_;   // custom name for the result pair
  MonoFrameProbe mono_probe_;  // empty: the host biphase observer decides
};

// ============================================================================
// AudioChannelMapStage
// ============================================================================
class AudioChannelMapStage : public DAGStage,
                             public ParameterizedStage,
                             public IStagePreviewCapability {
 public:
  AudioChannelMapStage();

  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD
  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::TRANSFORM,
                        "audio_channel_map",
                        "Audio Channel Map",
                        "Route audio channel pairs: delete a pair, extract a "
                        "left/right channel as SMPTE 272M mono in place, "
                        "copy it as mono to a new or existing target pair, or "
                        "average left and right where the VBI says mono",
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

  // Replaces the host biphase observer as the vbi_mono_average frame test.
  void set_mono_probe(MonoFrameProbe probe) { mono_probe_ = std::move(probe); }

  // Human-readable reason the configured operation cannot be applied to an
  // input carrying |pair_count| audio channel pairs, or an empty string when
  // it can. Used to report the mismatch and fall back to a pass-through.
  std::string inactive_reason(size_t pair_count,
                              AudioChannelMapOperation operation) const;

  // ParameterizedStage
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

 private:
  int32_t channel_pair_ = 0;  // source channel pair
  std::string operation_ = "left_to_mono";
  // Target for the routing operations: "new" appends a pair, otherwise the
  // string is the destination pair index. Ignored by the other operations.
  std::string target_pair_ = "new";
  // When true, description_ overrides the result pair's name; when false the
  // pair keeps its existing description. Gated in the GUI by an "Add
  // description" checkbox.
  bool set_description_ = false;
  // Custom name for the result pair (target for copy ops, source for the
  // in-place mono ops), used only when set_description_ is true.
  std::string description_;

  // Last reported pass-through reason, so a repeated render request does not
  // log the same warning again. Empty while the stage is applying its
  // operation normally.
  std::string last_inactive_reason_;

  MonoFrameProbe mono_probe_;

  mutable std::shared_ptr<const VideoFrameRepresentation> cached_output_;
};

}  // namespace orc
