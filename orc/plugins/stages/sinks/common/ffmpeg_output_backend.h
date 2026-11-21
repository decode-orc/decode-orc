/*
 * File:        ffmpeg_output_backend.h
 * Module:      orc-core
 * Purpose:     FFmpeg-based video encoding output backend
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ORC_CORE_FFMPEG_OUTPUT_BACKEND_H
#define ORC_CORE_FFMPEG_OUTPUT_BACKEND_H

#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/field_id.h>
#include <orc/stage/frame_id.h>
#include <orc/support/eia608_decoder.h>

#include <memory>
#include <string>
#include <vector>

#include "bt601_export_grid.h"
#include "disc-metadata/disc_metadata_document.h"
#include "output_backend.h"
#include "subtitle_embed_policy.h"

namespace orc {

/// The hardware_encoder values (other than "none") whose H.264 or H.265
/// encoder the linked FFmpeg has, in alphabetical order. Whether the machine
/// has the hardware is only found out when encoding starts. Empty in a build
/// without FFmpeg.
std::vector<std::string> available_hardware_encoder_types();

}  // namespace orc

#ifdef HAVE_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace orc {

/**
 * @brief FFmpeg-based output backend for encoded video
 *
 * Supports H.264, H.265, FFV1, and other codecs via libav* libraries.
 */
class FFmpegOutputBackend : public OutputBackend {
 public:
  FFmpegOutputBackend();
  ~FFmpegOutputBackend() override;

  bool initialize(const Configuration& config) override;
  bool writeFrame(const ::ComponentFrame& frame) override;
  bool finalize() override;
  std::string getFormatInfo() const override;

 private:
  // FFmpeg context structures
  AVFormatContext* format_ctx_ = nullptr;
  AVCodecContext* codec_ctx_ = nullptr;
  AVStream* stream_ = nullptr;
  AVFrame* frame_ = nullptr;  // Destination frame (encoder's pixel format)
  AVFrame* src_frame_ =
      nullptr;  // Source frame (YUV444P16LE from ComponentFrame)
  AVPacket* packet_ = nullptr;
  SwsContext* sws_ctx_ = nullptr;

  // Video filter graph (bwdif deinterlacing and/or user-supplied -vf chain).
  // When active, the graph replaces the swscale conversion path: frames enter
  // as YUV444P16LE and leave in the encoder's pixel format (libavfilter
  // auto-inserts the conversion). buffersrc/buffersink are owned by the graph.
  AVFilterGraph* filter_graph_ = nullptr;
  AVFilterContext* buffersrc_ctx_ = nullptr;
  AVFilterContext* buffersink_ctx_ = nullptr;
  AVFrame* filtered_frame_ = nullptr;  // Reused for buffersink output
  std::string video_filter_desc_;      // Combined chain; empty = passthrough
  // True while nothing in the chain can disturb the interlaced field
  // structure, i.e. no deinterlacing and no user-supplied filter. The
  // BT.601 geometry filters this backend adds itself are horizontal-only and
  // leave it set, so the container can still declare the field order.
  bool preserves_field_structure_ = true;
  // deinterlace_mode motion_only: frames whose ComponentFrame::weave() is set
  // enter the graph unflagged and leave it woven.
  bool weave_flagged_frames_ = false;
  int64_t frames_woven_ = 0;

  // Audio structures — one encoder per embedded audio channel pair. Every
  // stream is declared at kAudioSampleRateHz (48000 Hz), the only pipeline
  // audio rate (SMPTE 272M-1994 §1.2, exact for all video systems).
  struct AudioPairEncoder {
    size_t pair_index = 0;                  // Pipeline channel pair index
    AudioChannelPairDescriptor descriptor;  // Name and origin
    AVCodecContext* codec_ctx = nullptr;
    AVStream* stream = nullptr;
    AVFrame* frame = nullptr;
    int64_t pts = 0;
    // Pending interleaved 24-bit-in-int32 carrier samples
    std::vector<int32_t> buffer;
  };
  std::vector<AudioPairEncoder> audio_encoders_;
  AVPacket* audio_packet_ = nullptr;  // Shared scratch packet
  // Audio embedding is the only consumer; subtitle and chapter embedding read
  // the observation context and must not be gated on this being non-null.
  const VideoFrameRepresentation* vfr_ = nullptr;
  uint64_t start_field_index_ = 0;
  uint64_t num_fields_ = 0;
  uint64_t current_field_for_audio_ = 0;
  bool embed_audio_ = false;
  std::string audio_channel_pairs_option_;  // "all" or comma-separated indices
  double audio_gain_ = 1.0;  // Linear gain applied to embedded audio samples

  // Subtitle structures (closed captions decoded to a single text subtitle
  // stream - mov_text in MP4/MOV, SubRip in Matroska - fed from the
  // pending-cue queue)
  AVCodecContext* subtitle_codec_ctx_ = nullptr;
  SubtitleCodec subtitle_codec_ = SubtitleCodec::kNone;
  AVStream* subtitle_stream_ = nullptr;
  bool embed_closed_captions_ = false;
  uint64_t current_field_for_captions_ = 0;
  std::vector<CaptionCue> pending_cues_;
  size_t next_cue_index_ = 0;

  // Chapter metadata
  bool embed_chapter_metadata_ = false;

  // Disc metadata document, attached to the Matroska container so an emulator
  // needs no sidecar. See disc_metadata_document.h.
  bool embed_disc_metadata_ = false;
  DiscMetadataDetail disc_metadata_detail_ = DiscMetadataDetail::Map;

  // Configuration::vbi_metadata_after_frames: the frames go to
  // partial_path_, and finalize() remuxes them into output_path_ with the
  // chapters and disc metadata read from vbi_digest_ then.
  bool metadata_after_frames_ = false;
  std::string output_path_;
  std::string partial_path_;
  const class VbiObservationDigest* vbi_digest_ = nullptr;

  // State
  int64_t pts_ = 0;
  int frames_written_ = 0;
  std::string codec_name_;
  std::string container_format_;

  // Video parameters
  int width_ = 0;  // Encoder input dimensions (active area padded to even)
  int height_ = 0;
  int src_width_ = 0;  // Source ComponentFrame dimensions (before padding)
  int src_height_ = 0;
  int active_width_ = 0;  // Active video region dimensions
  int active_height_ = 0;
  AVRational time_base_;               // Source (pre-filter) frame timing
  AVRational enc_time_base_ = {0, 1};  // Encoder/stream time base; differs
                                       // from time_base_ when a filter chain
                                       // changes the frame rate
  AVRational requested_dar_ = {0, 1};  // Display aspect ratio override
                                       // ({0,1} = auto/unspecified)
  AVRational sample_aspect_ratio_ = {0, 1};  // Effective SAR stamped on
                                             // output ({0,1} = unspecified)
  VideoSystem video_system_ = VideoSystem::PAL;
  double black_ire_ = 0.0;
  double white_ire_ = 0.0;
  orc::SourceParameters video_params_;

  // Crop parameters
  int crop_top_ = 0;

  // Encoder quality settings
  std::string encoder_preset_ = "medium";
  int encoder_crf_ = 18;
  int encoder_bitrate_ = 0;
  bool use_lossless_mode_ = false;
  std::string prores_profile_ = "hq";
  bool is_tff_ = false;  // True when the padded output frame should be marked
                         // top-field-first.

  // BT.601 13.5 MHz export grid (the two LaserDisc emulation formats). When
  // set, the backend appends its own horizontal-only geometry filters and
  // signals the BT.601 sample aspect ratio; see bt601_export_grid.h.
  bool bt601_grid_ = false;
  // The lossy LaserDisc emulation format (mkv-h264): BT.601 grid, libx264
  // 4:2:0, one-second closed GOP. See bt601_export_grid.h.
  bool lossy_emulation_ = false;
  int bt601_bit_depth_ = 8;              // 8 (default) or 10
  std::string ffv1_slices_ = "auto";     // "auto" or an FFV1 slice count
  Bt601ExportGrid bt601_grid_geometry_;  // Valid only while bt601_grid_ is set

  // Helper methods
  bool setupEncoder(const std::string& codec_id,
                    const orc::SourceParameters& params);
  // Build and configure the libavfilter graph from video_filter_desc_.
  // Requires width_/height_/time_base_ and codec_ctx_->pix_fmt to be set.
  bool setupFilterGraph();
  void freeFilterGraph();
  // Pull all currently available frames from the buffersink and encode them.
  bool drainFilterGraph();
  // Send one frame to the encoder and write the resulting packets.
  // frame may be nullptr to flush the encoder.
  bool encodeVideoFrame(AVFrame* frame);
  // Set up one encoder per selected audio channel pair
  // (audio_channel_pairs_option_).
  bool setupAudioEncoders();
  bool setupAudioEncoderForPair(AudioPairEncoder& pair);
  // Gather one video frame's worth of 24-bit-in-int32 carrier samples for
  // the given channel pair; frames without audio yield cadence-sized silence
  // (audio_pairs_in_frame()). Conversion to the encoder's sample format
  // happens at encode time (audio_sample_feed.h).
  std::vector<int32_t> gatherAudioForFrame(AudioPairEncoder& pair,
                                           FrameID frame_id);
  // Encode full encoder-frame-size chunks from the pair's pending buffer.
  bool encodeBufferedAudio(AudioPairEncoder& pair);
  // Send one audio frame (nullptr flushes) and write the resulting packets.
  bool sendAudioFrame(AudioPairEncoder& pair, AVFrame* frame);
  bool setupSubtitleEncoder();
  void extractClosedCaptionsFromObservations(
      const class VbiObservationDigest& digest, uint64_t field_start,
      uint64_t field_count);
  void setupChapterMetadata(const class VbiObservationDigest& digest);
  // Build the disc metadata document and attach it. Must run before
  // avformat_write_header(): Matroska writes attachments and tags into the
  // header, so the whole document has to exist before the first frame.
  bool setupDiscMetadata(const class VbiObservationDigest& digest);
  // Copy every stream of partial_path_ into output_path_, adding the chapters
  // and disc metadata to the new header, then remove partial_path_.
  bool remuxWithVbiMetadata(const AVOutputFormat* muxer);
  bool encodeAudioForFrame();
  bool encodeClosedCaptionsForFrame();
  bool convertAndEncode(const ComponentFrame& component_frame);
  void cleanup();
};

}  // namespace orc

#endif  // HAVE_FFMPEG

#endif  // ORC_CORE_FFMPEG_OUTPUT_BACKEND_H
