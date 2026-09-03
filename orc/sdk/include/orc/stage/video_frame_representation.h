/*
 * File:        video_frame_representation.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     VideoFrameRepresentation interface for CVBS_U10_4FSC frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/dropout/dropout_run.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/orc_source_parameters.h>
#include <orc/stage/video_metadata_types.h>
#include <orc/support/frame_line_util.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orc {

// Progress callback for a deferred whole-stream audio decode driven through
// prime_audio_decode(): (done, total, message). |total| may be 0 when the work
// size is not yet known, in which case only |message| is meaningful.
using AudioDecodeProgressFn =
    std::function<void(uint64_t done, uint64_t total, const std::string&)>;

// ============================================================================
// EFM t-value byte packing
// ============================================================================
// Each EFM byte carried by the pipeline holds one t-value in its low nibble and
// the producer's doubt about that t-value in its high nibble, as defined by the
// CVBS EFM extension format:
//
//     bit  7 6 5 4   3 2 1 0
//           doubt     t-value
//
// Doubt runs 0 (fully trusted) to 15 (positively distrusted - an
// error-correction erasure candidate) and is ordinal in between. The doubt
// (rather than confidence) sense is deliberate: a fully trusted t-value packs
// to its plain value, so a producer with nothing to doubt emits the raw
// t-value byte stream and older captures read back unchanged.
//
// Consumers that need conventional t-values (T3-T11) must apply that
// constraint to the extracted t-value field, never to the raw byte.
//
// These are inline by design: they sit in per-sample loops, and plugins build
// with default visibility, so an out-of-line helper would cost a PLT call per
// t-value.

inline constexpr uint8_t kEfmTValueMask = 0x0F;
inline constexpr uint8_t kEfmDoubtShift = 4;
inline constexpr uint8_t kEfmDoubtMax = 15;

// The t-value carried by a packed EFM byte.
inline constexpr uint8_t efm_tvalue(uint8_t packed) {
  return static_cast<uint8_t>(packed & kEfmTValueMask);
}

// The producer's doubt about a packed EFM byte: 0 trusted, 15 distrusted.
inline constexpr uint8_t efm_doubt(uint8_t packed) {
  return static_cast<uint8_t>(packed >> kEfmDoubtShift);
}

// Pack a t-value and a doubt back into a single EFM byte.
inline constexpr uint8_t efm_pack(uint8_t tvalue, uint8_t doubt) {
  return static_cast<uint8_t>(
      (static_cast<uint8_t>(doubt & kEfmTValueMask) << kEfmDoubtShift) |
      (tvalue & kEfmTValueMask));
}

// ============================================================================
// VideoFrameRepresentation
// ============================================================================
// Read-only access to a sequence of CVBS_U10_4FSC frames.
//
// Sample domain: int16_t in the CVBS_U10_4FSC 10-bit domain.
// Frames are stored in a flat contiguous buffer:
//   [field1_line0 | field1_line1 | … | field2_line0 | …]
// PAL frames have non-uniform line lengths (1135 or 1136 samples per line).
// Use frame_line_sample_count() / frame_line_sample_offset() from
// frame_line_util.h when stages require per-line access.
//
// Thread safety: all const methods may be called concurrently from multiple
// threads once the implementing object has been fully constructed.
class VideoFrameRepresentation {
 public:
  using sample_type = int16_t;  // CVBS_U10_4FSC 10-bit domain

  virtual ~VideoFrameRepresentation() = default;

  // --------------------------------------------------------------------------
  // Navigation
  // --------------------------------------------------------------------------

  // Half-open [first, last] range of valid FrameIDs.
  virtual FrameIDRange frame_range() const = 0;

  // Total number of frames available.
  virtual size_t frame_count() const = 0;

  // True if the given FrameID is present in this representation.
  virtual bool has_frame(FrameID id) const = 0;

  // Per-frame metadata descriptor. Returns nullopt when id is not present.
  virtual std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const = 0;

  // --------------------------------------------------------------------------
  // Flat sample access
  // --------------------------------------------------------------------------
  // Returned pointers are valid until the next call to any method on this
  // object or until the object is destroyed.  Do not retain pointers across
  // calls.

  // Pointer to the start of the flat frame buffer for the given frame.
  // Returns nullptr when id is not present.
  virtual const sample_type* get_frame(FrameID id) const = 0;

  // Pointer to line |line| (0-based) within the flat frame buffer.
  // For PAL, line lengths vary (1135 or 1136 samples); use
  // frame_line_sample_count() from frame_line_util.h to obtain the exact width
  // before reading. Returns nullptr when id is not present or line is out of
  // range. Override only when the frame layout cannot be derived from
  // get_frame() plus get_video_parameters() — for example, a transform that
  // reorders lines without materialising a new flat buffer.
  virtual const sample_type* get_line(FrameID id, size_t line) const {
    const sample_type* frame = get_frame(id);
    if (!frame) return nullptr;
    const auto params = get_video_parameters();
    if (!params || line >= static_cast<size_t>(params->frame_height))
      return nullptr;
    return frame + frame_line_sample_offset(
                       params->system,
                       static_cast<size_t>(params->frame_width_nominal), line);
  }

  // Returns an owned copy of the complete frame buffer.
  virtual std::vector<sample_type> get_frame_copy(FrameID id) const = 0;

  // Returns an owned copy of the samples for a single line.
  // Prefer this over get_line() when only a few lines are needed per frame:
  // sources can override it to seek+read just the target line from disk
  // rather than loading the full frame (important for analysis sinks that
  // scan the entire recording but only touch 3-6 lines per frame).
  // Default: falls back through get_line() → get_frame() → full load.
  virtual std::vector<sample_type> get_line_samples(FrameID id,
                                                    size_t line) const {
    const auto params = get_video_parameters();
    if (!params) return {};
    const sample_type* ptr = get_line(id, line);
    if (!ptr) return {};
    const size_t width = static_cast<size_t>(params->frame_width_nominal);
    return std::vector<sample_type>(ptr, ptr + width);
  }

  // --------------------------------------------------------------------------
  // YC (separate luma / chroma) access
  // --------------------------------------------------------------------------
  // These methods are meaningful only for YC sources (has_separate_channels()
  // returns true).  For composite sources all YC methods return nullptr / {}.

  virtual bool has_separate_channels() const { return false; }

  virtual const sample_type* get_frame_luma(FrameID /*id*/) const {
    return nullptr;
  }
  virtual const sample_type* get_frame_chroma(FrameID /*id*/) const {
    return nullptr;
  }
  virtual const sample_type* get_line_luma(FrameID /*id*/,
                                           size_t /*line*/) const {
    return nullptr;
  }
  virtual const sample_type* get_line_chroma(FrameID /*id*/,
                                             size_t /*line*/) const {
    return nullptr;
  }

  // --------------------------------------------------------------------------
  // Hints
  // --------------------------------------------------------------------------

  // Dropout regions as frame-flat DropoutRun descriptors.
  virtual std::vector<DropoutRun> get_dropout_hints(FrameID /*id*/) const {
    return {};
  }

  // Per-frame video pass-through declaration.
  //
  // Return the upstream representation when this representation's CVBS video
  // content for |id| — samples (composite and, for YC sources, luma/chroma
  // planes), geometry, and dropout hints — is byte-identical to that upstream
  // representation's frame |id| because this stage applies no modification to
  // it. Return nullptr when the frame is (or may be) modified, when frame IDs
  // are remapped, or when identity cannot be established cheaply; nullptr is
  // always safe. Audio / EFM / AC3 differences do not matter here: the hook
  // covers only the video content that frame-derived analysis (the host's
  // observers) reads.
  //
  // The host uses this to share frame-content-keyed derived data (stored
  // observations) across stages: a transform that passes a frame through
  // unchanged — e.g. dropout correction on a frame with no dropouts — lets the
  // host reuse the upstream frame's stored analysis instead of decoding and
  // re-analysing identical samples. Implementations MUST answer from metadata
  // alone (never decode samples to compare) and must be safe to call
  // concurrently, like every other const accessor.
  virtual std::shared_ptr<const VideoFrameRepresentation>
  video_passthrough_source(FrameID /*id*/) const {
    return nullptr;
  }

  // Video parameters describing the signal geometry and level domain.
  virtual std::optional<SourceParameters> get_video_parameters() const {
    return std::nullopt;
  }

  // --------------------------------------------------------------------------
  // Audio channel pairs
  // --------------------------------------------------------------------------
  // Up to kMaxAudioChannelPairs stereo channel pairs, identified by index
  // 0 … audio_channel_pair_count() - 1. Pair order is stable through the
  // DAG. Every pair is 48000 Hz frame-locked (synchronous), 24-bit-in-int32
  // stereo — see audio_channel_pair.h. The per-frame sample count needs no
  // accessor: it is fully determined by the video system and frame index via
  // audio_pairs_in_frame().

  // Number of stereo audio channel pairs (0 = no audio). Max
  // kMaxAudioChannelPairs. Pipeline pair p maps to container channel pair p.
  virtual size_t audio_channel_pair_count() const { return 0; }

  // Convenience: true when at least one audio channel pair is present.
  bool has_audio() const { return audio_channel_pair_count() > 0; }

  // Descriptor for channel pair |pair|; nullopt when out of range.
  virtual std::optional<AudioChannelPairDescriptor>
  get_audio_channel_pair_descriptor(size_t /*pair*/) const {
    return std::nullopt;
  }

  // Interleaved 24-bit-in-int32 stereo pairs (L, R, L, R, …) of channel pair
  // |pair| for frame |id| — exactly audio_pairs_in_frame(id, system) pairs
  // (silence-filled by the producer where source material is short). Empty
  // when |pair| is out of range.
  virtual std::vector<int32_t> get_audio_samples(size_t /*pair*/,
                                                 FrameID /*id*/) const {
    return {};
  }

  // Force any deferred whole-stream audio decode backing this representation to
  // run now, reporting progress through |progress|. The default is a no-op:
  // representations whose audio is cheap or already resident need do nothing.
  // Representations that defer an expensive decode (e.g. EFM audio, run lazily
  // on first sample access) override this so a sink can meter the decode on its
  // progress dialog instead of stalling silently inside the first
  // get_audio_samples() call. Wrappers forward it down the chain so the hook
  // reaches a nested producer through any number of intervening stages.
  // Idempotent and safe to call with an empty |progress|.
  virtual void prime_audio_decode(
      const AudioDecodeProgressFn& /*progress*/) const {}

  // --------------------------------------------------------------------------
  // EFM
  // --------------------------------------------------------------------------

  virtual bool has_efm() const { return false; }

  // Number of EFM t-values for the given frame.
  virtual uint32_t get_efm_sample_count(FrameID /*id*/) const { return 0; }

  // One packed byte per t-value: t-value in the low nibble, producer doubt in
  // the high nibble. Unpack with efm_tvalue()/efm_doubt() rather than using the
  // byte directly - a raw byte compares and range-checks wrongly the moment a
  // producer has any doubt to record. Doubt is carried across the DAG intact;
  // only stages that consume t-values strip it.
  virtual std::vector<uint8_t> get_efm_samples(FrameID /*id*/) const {
    return {};
  }

  // --------------------------------------------------------------------------
  // AC3 RF
  // --------------------------------------------------------------------------

  virtual bool has_ac3_rf() const { return false; }

  // Number of QPSK dibit symbols for the given frame.
  virtual uint32_t get_ac3_symbol_count(FrameID /*id*/) const { return 0; }

  virtual std::vector<uint8_t> get_ac3_symbols(FrameID /*id*/) const {
    return {};
  }
};

// ============================================================================
// VideoFrameRepresentationWrapper
// ============================================================================
// Base for transform stages that wrap the connected input stage's VFrameR.
// The public read API of a wrapper MUST always describe this stage's OUTPUT;
// callers (downstream stages, observers, analysis sinks) can never reach
// unprocessed upstream data through any accessor.
//
// To guarantee that, accessors fall into two groups:
//
// 1. Pass-through primitives — forwarded to the wrapped input. A stage whose
//    output differs from its input for one of these MUST override it:
//      frame_range / frame_count / has_frame / get_frame_descriptor,
//      get_frame, has_separate_channels, get_frame_luma, get_frame_chroma,
//      get_dropout_hints, get_video_parameters, audio / EFM / AC3 accessors.
//    A stage that remaps frame IDs MUST override every per-frame accessor in
//    this group so IDs are translated on the way through. For audio that
//    means get_audio_samples (per channel pair): the output frame at
//    timeline index p must serve exactly audio_pairs_in_frame(p) pairs, so
//    a mapping that breaks the NTSC/PAL-M cadence phase (source frame index
//    not congruent to the output index mod 5) must truncate or silence-pad
//    the mapped window by the one-pair difference — see
//    audio_channel_pair.h.
//
// 2. Derived accessors — implemented here in terms of this object's own
//    virtual primitives, never forwarded:
//      get_line, get_line_samples  (derive from get_frame)
//      get_frame_copy              (derives from get_frame)
//      get_line_luma / get_line_chroma  (derive from get_frame_luma/chroma)
//    Overriding the frame-level primitives is therefore sufficient for
//    correctness on every read path; overriding a derived accessor is purely
//    an optimisation (e.g. a pass-through stage restoring the wrapped
//    source's seek-one-line-from-disk fast path).
//
// Implementing a transform stage:
//   1. Extend VideoFrameRepresentationWrapper.
//   2. Store the upstream VFrameR in source_ (set in the constructor).
//   3. Override every pass-through primitive whose output this stage changes.
//
// Hint semantics: hints describe the OUTPUT of the stage, not its input.
// A stage that modifies data covered by a hint MUST override that hint method.
class VideoFrameRepresentationWrapper : public VideoFrameRepresentation {
 public:
  ~VideoFrameRepresentationWrapper() override = default;

  // Navigation
  FrameIDRange frame_range() const override {
    return source_ ? source_->frame_range() : FrameIDRange{};
  }
  size_t frame_count() const override {
    return source_ ? source_->frame_count() : 0;
  }
  bool has_frame(FrameID id) const override {
    return source_ ? source_->has_frame(id) : false;
  }
  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    return source_ ? source_->get_frame_descriptor(id) : std::nullopt;
  }

  // Flat access. Only get_frame() forwards; get_line() and get_line_samples()
  // use the base-class defaults, which route through this object's virtual
  // get_frame()/get_line() so derived overrides apply on every path.
  const sample_type* get_frame(FrameID id) const override {
    return source_ ? source_->get_frame(id) : nullptr;
  }
  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    const sample_type* frame = get_frame(id);
    if (!frame) return {};
    const size_t total = frame_total_sample_count(id);
    if (total == 0) return {};
    return std::vector<sample_type>(frame, frame + total);
  }

  // YC. Frame-level accessors forward; line-level accessors derive from this
  // object's virtual frame-level accessors.
  bool has_separate_channels() const override {
    return source_ ? source_->has_separate_channels() : false;
  }
  const sample_type* get_frame_luma(FrameID id) const override {
    return source_ ? source_->get_frame_luma(id) : nullptr;
  }
  const sample_type* get_frame_chroma(FrameID id) const override {
    return source_ ? source_->get_frame_chroma(id) : nullptr;
  }
  const sample_type* get_line_luma(FrameID id, size_t line) const override {
    return line_within_plane(get_frame_luma(id), line);
  }
  const sample_type* get_line_chroma(FrameID id, size_t line) const override {
    return line_within_plane(get_frame_chroma(id), line);
  }

  // Hints
  std::vector<DropoutRun> get_dropout_hints(FrameID id) const override {
    return source_ ? source_->get_dropout_hints(id) : std::vector<DropoutRun>{};
  }
  std::optional<SourceParameters> get_video_parameters() const override {
    return source_ ? source_->get_video_parameters() : std::nullopt;
  }
  // Audio channel pairs
  size_t audio_channel_pair_count() const override {
    return source_ ? source_->audio_channel_pair_count() : 0;
  }
  std::optional<AudioChannelPairDescriptor> get_audio_channel_pair_descriptor(
      size_t pair) const override {
    return source_ ? source_->get_audio_channel_pair_descriptor(pair)
                   : std::nullopt;
  }
  std::vector<int32_t> get_audio_samples(size_t pair,
                                         FrameID id) const override {
    return source_ ? source_->get_audio_samples(pair, id)
                   : std::vector<int32_t>{};
  }
  // Forward priming down the chain so a deferred decode nested beneath any
  // number of wrappers (e.g. an audio_channel_map between EFM decode and the
  // sink) is still reached. A wrapper that itself owns a deferred decode
  // overrides this instead of forwarding.
  void prime_audio_decode(
      const AudioDecodeProgressFn& progress) const override {
    if (source_) source_->prime_audio_decode(progress);
  }

  // EFM
  bool has_efm() const override { return source_ ? source_->has_efm() : false; }
  uint32_t get_efm_sample_count(FrameID id) const override {
    return source_ ? source_->get_efm_sample_count(id) : 0;
  }
  std::vector<uint8_t> get_efm_samples(FrameID id) const override {
    return source_ ? source_->get_efm_samples(id) : std::vector<uint8_t>{};
  }

  // AC3 RF
  bool has_ac3_rf() const override {
    return source_ ? source_->has_ac3_rf() : false;
  }
  uint32_t get_ac3_symbol_count(FrameID id) const override {
    return source_ ? source_->get_ac3_symbol_count(id) : 0;
  }
  std::vector<uint8_t> get_ac3_symbols(FrameID id) const override {
    return source_ ? source_->get_ac3_symbols(id) : std::vector<uint8_t>{};
  }

 protected:
  explicit VideoFrameRepresentationWrapper(
      std::shared_ptr<const VideoFrameRepresentation> source)
      : source_(std::move(source)) {}

  std::shared_ptr<const VideoFrameRepresentation> source_;

 private:
  // Total sample count of frame |id| as exposed by THIS representation
  // (descriptor first, video-parameter geometry as fallback).
  size_t frame_total_sample_count(FrameID id) const {
    if (const auto desc = get_frame_descriptor(id)) {
      return desc->samples_total;
    }
    if (const auto params = get_video_parameters()) {
      return frame_line_sample_offset(
          params->system, static_cast<size_t>(params->frame_width_nominal),
          static_cast<size_t>(params->frame_height));
    }
    return 0;
  }

  // Pointer to line |line| within a flat luma/chroma plane returned by this
  // object's frame-level accessors. Planes share the composite frame layout.
  const sample_type* line_within_plane(const sample_type* plane,
                                       size_t line) const {
    if (!plane) return nullptr;
    const auto params = get_video_parameters();
    if (!params || line >= static_cast<size_t>(params->frame_height)) {
      return nullptr;
    }
    return plane + frame_line_sample_offset(
                       params->system,
                       static_cast<size_t>(params->frame_width_nominal), line);
  }
};

using VideoFrameRepresentationPtr = std::shared_ptr<VideoFrameRepresentation>;

}  // namespace orc
