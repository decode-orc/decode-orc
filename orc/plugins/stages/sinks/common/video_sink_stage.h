/*
 * File:        video_sink_stage.h
 * Module:      orc-core
 * Purpose:     Video sink stage (raw or FFmpeg-encoded output)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ORC_CORE_VIDEO_SINK_STAGE_H
#define ORC_CORE_VIDEO_SINK_STAGE_H

#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include sinks/common/video_sink_stage.h. Use VectorscopePresenter or RenderPresenter instead."
#endif

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/plugin/orc_stage_tooling.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/node_type.h>
#include <orc/stage/orc_source_parameters.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/preview/orc_rendering.h>  // For PreviewImage definition
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Forward declarations for decoder classes
struct SourceField;
class Decoder;
class ComponentFrame;

namespace orc {

/**
 * @brief Video Sink Stage
 *
 * Decodes composite PAL or NTSC video into component output and writes it to
 * disk. This is a SINK stage - it has inputs but no outputs.
 *
 * When triggered, it reads all fields from its input and decodes them using
 * the selected chroma decoder, writing the result to an output file. The
 * output_mode parameter selects between two output paths:
 *
 * - "raw": Uncompressed raw file output (rgb, yuv, y4m via raw_format)
 * - "ffmpeg": Encoded container output (mp4-h264, mkv-ffv1, ... via
 *   ffmpeg_format) with optional embedded audio, closed captions, and chapter
 *   metadata
 *
 * Supported Decoders:
 * - PAL: pal2d, transform2d, transform3d
 * - NTSC: ntsc1d, ntsc2d, ntsc3d, ntsc3dnoadapt
 * - Other: mono
 *
 * This sink supports preview - it decodes fields on-demand for GUI
 * visualization.
 *
 * The FFmpeg Preset Config stage tool configures FFmpeg output; applying a
 * preset switches output_mode to "ffmpeg".
 *
 * Parameters:
 * - output_path: Output file path for video
 * - decoder_type: Which decoder to use (pal2d, ntsc2d, etc.)
 * - output_mode: "raw" (uncompressed file) or "ffmpeg" (encoded container)
 * - raw_format: Raw output format (rgb, yuv, y4m) when output_mode is "raw"
 * - ffmpeg_format: Container/codec (mp4-h264, mkv-ffv1, ...) when output_mode
 *   is "ffmpeg"
 * - chroma_gain: Chroma gain factor (0.0-10.0, default 1.0)
 * - chroma_phase: Chroma phase rotation in degrees (-180 to 180, default 0)
 * - encoder_preset/encoder_crf/encoder_bitrate/hardware_encoder/
 *   prores_profile/use_lossless_mode/apply_deinterlace: FFmpeg encoder options
 * - display_aspect_ratio: Display aspect ratio metadata for playback (auto,
 *   4:3, 16:9); FFmpeg mode only
 * - video_filter: Custom FFmpeg video filter chain (same syntax as -vf,
 *   e.g. "fieldmatch,decimate" for inverse telecine); FFmpeg mode only
 * - embed_audio: Embed pipeline audio in output (MP4/MKV only, default: false)
 * - audio_channel_pairs: Which audio channel pairs to embed, one output
 *   stream per pair ("all" or comma-separated 0-based indices; requires
 *   embed_audio)
 * - audio_gain_db: Gain applied to all embedded audio channel pairs in dB
 *   (requires embed_audio; 0 = unchanged)
 * - embed_closed_captions: Embed closed captions as mov_text (MP4/MOV only)
 * - embed_chapter_metadata: Write chapter markers from VBI data (MKV/MP4/MOV)
 */
class VideoSinkStage : public DAGStage,
                       public ParameterizedStage,
                       public TriggerableStage,
                       public IStagePreviewCapability,
                       public IColourPreviewProvider,
                       public StageToolProvider {
 public:
  ORC_STAGE_INSTRUCTIONS_MD
  VideoSinkStage();
  ~VideoSinkStage()
      override;  // Need custom destructor for unique_ptr with incomplete types

  // DAGStage interface
  std::string version() const override { return "1.0"; }
  NodeTypeInfo get_node_type_info() const override;

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 0; }  // Sink has no outputs

  // ParameterizedStage interface
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // TriggerableStage interface. Collects closed caption and VBI chapter
  // observations first when producing FFmpeg output, then decodes and writes
  // the output file.
  bool trigger(const std::vector<ArtifactPtr>& inputs,
               const std::map<std::string, ParameterValue>& parameters,
               IObservationContext& observation_context) override;

  std::string get_trigger_status() const override;

  void set_progress_callback(TriggerProgressCallback callback) override {
    progress_callback_ = callback;
  }

  bool is_trigger_in_progress() const override {
    return trigger_in_progress_.load();
  }

  void cancel_trigger() override { cancel_requested_.store(true); }

  // IStagePreviewCapability interface
  StagePreviewCapability get_preview_capability() const override;

  // IColourPreviewProvider interface
  std::optional<ColourFrameCarrier> get_colour_preview_carrier(
      uint64_t frame_index, PreviewNavigationHint hint) const override;

  // StageToolProvider interface
  std::vector<StageToolDescriptor> get_stage_tools() const override {
    return {StageToolDescriptor{"ffmpeg_preset_config", "FFmpeg Preset Config",
                                "Open FFmpeg preset helper dialog",
                                StageToolKind::ConfigDialog, false,
                                "decode-orc.stage-tools.ffmpeg-preset.v1"}};
  }

 private:
  mutable std::mutex
      cached_input_mutex_;  // Protects cached_input_ from race conditions
  mutable std::shared_ptr<const orc::VideoFrameRepresentation>
      cached_input_;  // For preview

  // Cached decoder for preview (avoid recreating expensive FFTW plans)
  struct PreviewDecoderCache {
    std::string decoder_type;
    double chroma_gain;
    double chroma_phase;
    double luma_nr;
    double chroma_nr;
    bool ntsc_phase_comp;
    bool simple_pal;
    bool blackandwhite;
    double transform_threshold;
    double chroma_weight;
    double adapt_threshold;

    std::unique_ptr<Decoder> decoder;

    bool matches_config(const std::string& dec_type, double cg, double cp,
                        double ln, double cn, bool npc, bool sp, bool bw,
                        double tt, double cw, double at) const {
      // decoder_type is part of this comparison, so a type change already
      // invalidates the cache; a single built decoder needs no pointer-shape
      // check beyond being present.
      return decoder != nullptr && decoder_type == dec_type &&
             chroma_gain == cg && chroma_phase == cp && luma_nr == ln &&
             chroma_nr == cn && ntsc_phase_comp == npc && simple_pal == sp &&
             blackandwhite == bw && transform_threshold == tt &&
             chroma_weight == cw && adapt_threshold == at;
    }
  };
  mutable PreviewDecoderCache preview_decoder_cache_;

  // Current parameters
  std::string output_path_;
  std::string decoder_type_;
  std::string output_mode_;    // "raw" or "ffmpeg"
  std::string raw_format_;     // rgb, yuv, y4m
  std::string ffmpeg_format_;  // mp4-h264, mkv-ffv1, ...
  std::string output_format_;  // Effective format derived from the above
  double chroma_gain_;
  double chroma_phase_;
  int threads_;
  double luma_nr_;
  double chroma_nr_;
  bool ntsc_phase_comp_;
  bool simple_pal_;
  double transform_threshold_;
  double chroma_weight_;
  double adapt_threshold_;
  int output_padding_;
  bool embed_audio_;  // Embed pipeline audio in output (MP4/MKV only)
  std::string audio_channel_pairs_;  // "all" or comma-separated 0-based
                                     // indices
  double audio_gain_db_;         // Gain for embedded audio in dB (0 = unity)
  bool embed_closed_captions_;   // Embed closed captions in MP4 output (MP4
                                 // only, converted to mov_text)
  bool embed_chapter_metadata_;  // Write chapter markers from VBI data
                                 // (MKV/MP4/MOV)

  // Encoder quality parameters (for FFmpeg output)
  std::string encoder_preset_;    // "fast", "medium", "slow", "veryslow"
  int encoder_crf_;               // 0-51, typically 18-28 for good quality
  int encoder_bitrate_;           // bits per second, 0 = use CRF instead
  std::string hardware_encoder_;  // "none", "vaapi", "nvenc", "qsv", "amf",
                                  // "videotoolbox"
  std::string
      prores_profile_;      // "proxy", "lt", "standard", "hq", "4444", "4444xq"
  bool use_lossless_mode_;  // Enable lossless H.264/H.265/AV1 encoding
  bool apply_deinterlace_;  // Apply bwdif deinterlacing filter
  std::string display_aspect_ratio_;  // "auto", "4:3", "16:9"
  std::string video_filter_;     // Custom FFmpeg -vf filter chain ("" = none)
  std::string bt601_bit_depth_;  // "8" or "10" (FFV1 for VP415e only)
  std::string ffv1_slices_;      // "auto" or an explicit FFV1 slice count
  bool embed_disc_metadata_;     // Attach the LaserDisc VBI document (MKV)
  std::string disc_metadata_detail_;  // "map" or "full"

  // Status tracking
  std::string trigger_status_;
  std::atomic<bool> trigger_in_progress_{false};
  std::atomic<bool> cancel_requested_{false};
  TriggerProgressCallback progress_callback_;

  // Colour-sequence phase for phase-aware chroma decoders (currently the NTSC
  // 3D adaptive comb) is measured from the burst signal by the host
  // "colour_frame_phase" observer, not taken from source-side metadata, so it
  // is available uniformly for TBC and CVBS sources across PAL / NTSC / PAL_M.
  // Results are cached per frame id; the observer handle is not thread-safe, so
  // the render path's worker threads serialise measurement behind this mutex.
  mutable std::mutex colour_phase_mutex_;
  mutable std::unordered_map<int32_t, int32_t> colour_phase_cache_;

  // Decode the input and write the output file. Called by trigger() after
  // any observation collection has completed.
  bool run_export_trigger(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      IObservationContext& observation_context);

  // Helper methods for integration

  // Copy frame_id's buffers out of the VFrameR into owned_buffers and append
  // SourceFields for field 1 and field 2 viewing those copies. The SDK
  // forbids retaining get_frame() pointers across calls (upstream caches may
  // evict or replace buffers while other threads still read them), so
  // decoder input must view sink-owned memory. owned_buffers must outlive
  // the appended fields. Returns false (appending nothing) when the frame
  // has no data.
  bool appendSourceFields(const orc::VideoFrameRepresentation* vfr,
                          orc::FrameID frame_id,
                          const orc::SourceParameters& videoParams,
                          std::deque<std::vector<int16_t>>& owned_buffers,
                          std::vector<SourceField>& out_fields) const;

  // Measure the colour-sequence phase (colour_frame_index) for frame_id by
  // running the host "colour_frame_phase" observer over the frame's burst
  // signal. Thread-safe and cached per frame id. Returns -1 when the phase is
  // unknown (no observation service, or burst not detectable).
  int32_t observer_colour_frame_index(const orc::VideoFrameRepresentation& vfr,
                                      orc::FrameID frame_id) const;

  // Build a SourceField view over caller-owned frame buffers. For PAL,
  // populates line_ptrs (and luma/chroma_line_ptrs for YC sources) to handle
  // non-uniform 1135/1136-sample lines. luma_ptr/chroma_ptr may be null for
  // composite-only sources (is_yc false).
  SourceField buildSourceField(const int16_t* frame_ptr,
                               const int16_t* luma_ptr,
                               const int16_t* chroma_ptr, bool is_yc,
                               std::optional<int32_t> frame_phase_id,
                               orc::FrameID frame_id, bool is_first_field,
                               const orc::SourceParameters& videoParams) const;
};

}  // namespace orc

#endif  // ORC_CORE_VIDEO_SINK_STAGE_H
