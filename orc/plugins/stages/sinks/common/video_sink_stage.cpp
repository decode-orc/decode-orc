/*
 * File:        video_sink_stage.cpp
 * Module:      orc-core
 * Purpose:     Video sink stage (raw or FFmpeg-encoded output)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "video_sink_stage.h"

#include <orc/abi/orc_plugin_services.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/colour_frame_phase_query.h>
#include <orc/stage/observation/observation_service_interface.h>
#include <orc/support/colour_preview_conversion.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include "bt601_export_grid.h"
#include "carrier_plane_copy.h"
#include "decoders/comb.h"
#include "decoders/componentframe.h"
#include "decoders/decoder.h"
#include "decoders/monodecoder.h"
#include "decoders/ntscdecoder.h"
#include "decoders/outputwriter.h"
#include "decoders/palcolour.h"
#include "decoders/paldecoder.h"
#include "decoders/sourcefield.h"
#include "decoders/vectorscope_extract.h"
#include "video_parameter_safety.h"

// Output backend includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

#include "ffmpeg_output_backend.h"
#include "output_backend.h"

namespace orc {

namespace {

chroma_sink::DecoderVideoProfile decoder_video_profile_for_type(
    const std::string& decoder_type, VideoSystem system) {
  if (decoder_type == "mono") {
    return chroma_sink::DecoderVideoProfile::Mono;
  }

  if (decoder_type == "transform2d" || decoder_type == "transform3d" ||
      decoder_type == "pal2d") {
    return chroma_sink::DecoderVideoProfile::PalColour;
  }

  if (decoder_type == "ntsc1d" || decoder_type == "ntsc2d" ||
      decoder_type == "ntsc3d" || decoder_type == "ntsc3dnoadapt") {
    return chroma_sink::DecoderVideoProfile::NtscColour;
  }

  if (system == VideoSystem::PAL || system == VideoSystem::PAL_M) {
    return chroma_sink::DecoderVideoProfile::PalColour;
  }

  return chroma_sink::DecoderVideoProfile::NtscColour;
}

// Decode parameters carried from the stage into the decoder factory, so the
// factory does not reach back into VideoSinkStage.
struct DecoderParams {
  double chromaGain;
  double chromaPhase;
  double lumaNr;
  double chromaNr;
  bool ntscPhaseComp;
  bool simplePal;
  double transformThreshold;
  double chromaWeight;
  double adaptThreshold;
};

// Build the decoder for an already-resolved decoder_type (the caller has
// applied any transform->pal2d downgrade for Y/C, which is stage policy).
// The config fields set here match what the stage set when it drove the
// filters directly, so output is unchanged.  Transform PAL creates FFTW plans
// in configure(); callers constructing per-thread decoders must serialise this
// behind fftwPlanMutex.
//
// Returns nullptr for an unrecognized decoder_type, or when the decoder rejects
// the source (e.g. a PAL source with an ntsc type), so the caller can abort
// instead of exporting garbage or black frames.
std::unique_ptr<Decoder> make_decoder(const std::string& decoder_type,
                                      const DecoderParams& params,
                                      const SourceParameters& videoParameters) {
  if (decoder_type == "mono") {
    MonoDecoder::MonoConfiguration config;
    config.yNRLevel = params.lumaNr;
    config.filterChroma = false;
    config.videoParameters = videoParameters;
    auto decoder = std::make_unique<MonoDecoder>(config);
    return decoder->configure(videoParameters) ? std::move(decoder) : nullptr;
  }

  if (decoder_type == "pal2d" || decoder_type == "transform2d" ||
      decoder_type == "transform3d") {
    PalColour::Configuration config;
    config.chromaGain = params.chromaGain;
    config.chromaPhase = params.chromaPhase;
    config.yNRLevel = params.lumaNr;
    config.simplePAL = params.simplePal;
    config.transformThreshold = params.transformThreshold;
    config.showFFTs = false;
    if (decoder_type == "transform3d") {
      config.chromaFilter = PalColour::transform3DFilter;
    } else if (decoder_type == "transform2d") {
      config.chromaFilter = PalColour::transform2DFilter;
    } else {
      config.chromaFilter = PalColour::palColourFilter;
    }
    auto decoder = std::make_unique<PalDecoder>(config);
    return decoder->configure(videoParameters) ? std::move(decoder) : nullptr;
  }

  if (decoder_type == "ntsc1d" || decoder_type == "ntsc2d" ||
      decoder_type == "ntsc3d" || decoder_type == "ntsc3dnoadapt") {
    Comb::Configuration config;
    config.chromaGain = params.chromaGain;
    config.chromaPhase = params.chromaPhase;
    config.cNRLevel = params.chromaNr;
    config.yNRLevel = params.lumaNr;
    config.phaseCompensation = params.ntscPhaseComp;
    config.chromaWeight = params.chromaWeight;
    config.adaptThreshold = params.adaptThreshold;
    config.showMap = false;
    if (decoder_type == "ntsc1d") {
      config.dimensions = 1;
      config.adaptive = false;
    } else if (decoder_type == "ntsc3d") {
      config.dimensions = 3;
      config.adaptive = true;
    } else if (decoder_type == "ntsc3dnoadapt") {
      config.dimensions = 3;
      config.adaptive = false;
    } else {
      config.dimensions = 2;
      config.adaptive = false;
    }
    auto decoder = std::make_unique<NtscDecoder>(config);
    return decoder->configure(videoParameters) ? std::move(decoder) : nullptr;
  }

  // Unknown decoder type: return nullptr so the caller's null check aborts the
  // export rather than silently decoding as NTSC 2D.
  return nullptr;
}

bool apply_decoder_safe_video_parameters(
    SourceParameters& video_params, chroma_sink::DecoderVideoProfile profile,
    const char* context, std::string* error_message = nullptr) {
  const auto safety =
      chroma_sink::sanitize_video_parameters(video_params, profile);

  if (!safety.warnings.empty()) {
    ORC_LOG_WARN("{}: Adjusted unsafe video parameters: {}", context,
                 chroma_sink::join_issues(safety.warnings));
  }

  if (!safety.ok) {
    const auto joined_errors = chroma_sink::join_issues(safety.errors);
    ORC_LOG_ERROR("{}: Invalid video parameters: {}", context, joined_errors);
    if (error_message != nullptr) {
      *error_message = joined_errors;
    }
    return false;
  }

  video_params = safety.params;
  return true;
}

bool is_raw_output_format(const std::string& format) {
  return format == "rgb" || format == "yuv" || format == "y4m";
}

bool is_supported_output_format(const std::string& format) {
  const auto formats = orc::OutputBackendFactory::getSupportedFormats();
  return std::find(formats.begin(), formats.end(), format) != formats.end();
}

}  // namespace

VideoSinkStage::VideoSinkStage()
    : output_path_(""),
      decoder_type_("ntsc2d"),
      output_mode_("ffmpeg"),
      raw_format_("rgb"),
      ffmpeg_format_("mp4-h264"),
      output_format_("mp4-h264"),
      chroma_gain_(1.0),
      chroma_phase_(0.0),
      threads_(0)  // 0 means auto-detect
      ,
      luma_nr_(0.0),
      chroma_nr_(0.0),
      ntsc_phase_comp_(true),
      simple_pal_(false),
      transform_threshold_(0.4),
      chroma_weight_(1.0),
      adapt_threshold_(1.0),
      output_padding_(8),
      embed_audio_(false),
      audio_channel_pairs_("all"),
      audio_gain_db_(0.0),
      embed_closed_captions_(false),
      embed_chapter_metadata_(false),
      encoder_preset_("medium"),
      encoder_crf_(18),
      encoder_bitrate_(0)  // 0 = use CRF
      ,
      hardware_encoder_("none"),
      prores_profile_("hq"),
      use_lossless_mode_(false),
      apply_deinterlace_(false),
      display_aspect_ratio_("4:3"),
      video_filter_(""),
      bt601_bit_depth_("8"),
      ffv1_slices_("auto"),
      rawvideo_format_("rgb"),
      embed_disc_metadata_(false),
      disc_metadata_detail_("map") {
  set_configuration_status(orc::ConfigurationStatus::Yellow);
}

VideoSinkStage::~VideoSinkStage() {}

NodeTypeInfo VideoSinkStage::get_node_type_info() const {
  return NodeTypeInfo{
      NodeType::SINK,
      "video_sink",
      "Video Sink",
      "Decodes composite video to a file. Output mode selects raw "
      "(uncompressed RGB/YUV/Y4M) or FFmpeg (MP4/MKV/MOV/MXF with optional "
      "audio and subtitles). Trigger to export.",
      1,  // min_inputs
      1,  // max_inputs
      0,  // min_outputs
      0,  // max_outputs
      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> VideoSinkStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context [[maybe_unused]]) {
  // Apply parameters so decoder_type_ and other settings reflect the current
  // project before the preview is rendered. Without this, the stage retains its
  // constructor default (ntsc2d) when a PAL project is first loaded, causing
  // the chroma preview to use the wrong decoder.
  if (!parameters.empty()) {
    set_parameters(parameters);
  }

  // VideoSinkStage is primarily a video output sink and does not extract
  // observations. Observations are collected by the LD sink or dedicated
  // analysis stages. Cache input for preview rendering (thread-safe)
  if (!inputs.empty()) {
    std::lock_guard<std::mutex> lock(cached_input_mutex_);
    cached_input_ =
        std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
            inputs[0]);
  }

  // Sink stages don't produce outputs during normal execution
  // They are triggered manually to write data
  ORC_LOG_DEBUG(
      "VideoSink execute called on instance {} (cached input for preview)",
      static_cast<void*>(this));
  return {};  // No outputs
}

std::vector<ParameterDescriptor> VideoSinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  // Determine available decoder types based on project video format
  std::vector<std::string> decoder_options;
  std::string decoder_default = "ntsc2d";

  if (project_format == VideoSystem::PAL ||
      project_format == VideoSystem::PAL_M) {
    decoder_options = {"pal2d", "transform2d", "transform3d", "mono"};
    decoder_default = "pal2d";
  } else if (project_format == VideoSystem::NTSC) {
    decoder_options = {"ntsc1d", "ntsc2d", "ntsc3d", "ntsc3dnoadapt", "mono"};
    decoder_default = "ntsc2d";
  } else {
    // Unknown system - show all options
    decoder_options = {"pal2d",  "transform2d", "transform3d",   "ntsc1d",
                       "ntsc2d", "ntsc3d",      "ntsc3dnoadapt", "mono"};
    decoder_default = "ntsc2d";
  }

  std::string decoder_description =
      "Chroma decoder to use: pal2d, transform2d, transform3d, ntsc1d, ntsc2d, "
      "ntsc3d, ntsc3dnoadapt, mono";
  if (source_type == SourceType::YC) {
    decoder_description +=
        "\n"
        "YC sources: transform2d/transform3d are not compatible and will fall "
        "back to pal2d.";
  }

  // FFmpeg (encoded) formats are the supported formats minus the raw ones.
  std::vector<std::string> ffmpeg_formats;
  for (const auto& fmt : OutputBackendFactory::getSupportedFormats()) {
    if (!is_raw_output_format(fmt)) {
      ffmpeg_formats.push_back(fmt);
    }
  }
  // If FFmpeg is not compiled in, keep a placeholder so the UI stays usable.
  if (ffmpeg_formats.empty()) {
    ffmpeg_formats.push_back("mp4-h264");
  }

  std::vector<ParameterDescriptor> params = {
      ParameterDescriptor{
          "output_path", "Output Path",
          "Path to output file. Match the extension to the selected output "
          "mode and format (e.g. .rgb/.yuv/.y4m for raw, "
          ".mp4/.mkv/.mov/.mxf/.nut for FFmpeg output).",
          ParameterType::FILE_PATH,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               std::string(""),
                               {},
                               false,
                               std::nullopt},
          ".mp4|.mkv|.mov|.mxf|.nut|.rgb|.yuv|.y4m"  // file_extension_hint
      },
      ParameterDescriptor{
          "decoder_type",
          "Decoder Type",
          decoder_description,
          ParameterType::STRING,
          {{}, {}, decoder_default, decoder_options, false, std::nullopt}},
      ParameterDescriptor{
          "output_mode",
          "Output Mode",
          "Select how the decoded video is written:\n"
          "  ffmpeg - Encoded output via FFmpeg (MP4/MKV/MOV/MXF containers, "
          "optional audio/captions/chapters)\n"
          "  raw    - Uncompressed raw file output (RGB48, YUV444P16, Y4M)",
          ParameterType::STRING,
          {{},
           {},
           std::string("ffmpeg"),
           {"ffmpeg", "raw"},
           false,
           std::nullopt}},
      ParameterDescriptor{"raw_format",
                          "Raw Format",
                          "Raw output format:\n"
                          "  rgb  - RGB48 (16-bit per channel, planar)\n"
                          "  yuv  - YUV444P16 (16-bit per channel, planar)\n"
                          "  y4m  - YUV444P16 with Y4M headers",
                          ParameterType::STRING,
                          {{},
                           {},
                           std::string("rgb"),
                           {"rgb", "yuv", "y4m"},
                           false,
                           ParameterDependency{"output_mode", {"raw"}}}},
      ParameterDescriptor{
          "ffmpeg_format",
          "FFmpeg Format",
          "Output container and codec combination:\n"
          "Lossless/Archive:\n"
          "  mkv-ffv1 - FFV1 lossless codec in MKV container\n"
          "  mkv-ffv1-bt601 - FFV1 for VP415e: the same lossless codec "
          "resampled onto the ITU-R BT.601 13.5 MHz grid (720 pixels wide, "
          "line count unchanged) for players driving real BT.601 hardware\n"
          "Professional:\n"
          "  mov-prores - ProRes codec (profile set via ProRes Profile "
          "parameter)\n"
          "Uncompressed:\n"
          "  mov-v210 - 10-bit 4:2:2 uncompressed\n"
          "  mov-v410 - 10-bit 4:4:4 uncompressed\n"
          "Pipe (only containers safe on a non-seekable \"-\" output; see "
          "IStreamingCompatibility):\n"
          "  nut-rawvideo - Uncompressed, no encoding at all (RGB48 or "
          "YUV444P16 via rawvideo_format)\n"
          "  nut-ffv1 - FFV1 lossless, same codec as mkv-ffv1\n"
          "Broadcast:\n"
          "  mxf-mpeg2video - D10 (Sony IMX/XDCAM)\n"
          "H.264 (universal compatibility):\n"
          "  mp4-h264 - H.264 in MP4 container\n"
          "  mov-h264 - H.264 in MOV container\n"
          "H.265/HEVC (better compression):\n"
          "  mp4-hevc - H.265/HEVC in MP4 container\n"
          "  mov-hevc - H.265/HEVC in MOV container\n"
          "AV1 (modern, efficient):\n"
          "  mp4-av1 - AV1 codec in MP4 container\n"
          "\n"
          "Note: Hardware acceleration and lossless mode are set via separate "
          "parameters",
          ParameterType::STRING,
          {{},
           {},
           std::string("mp4-h264"),
           ffmpeg_formats,
           false,
           ParameterDependency{"output_mode", {"ffmpeg"}}}},
      ParameterDescriptor{"chroma_gain",
                          "Chroma Gain",
                          "Gain factor applied to chroma components (color "
                          "saturation). Range: 0.0-10.0",
                          ParameterType::DOUBLE,
                          {0.0, 10.0, 1.0, {}, false, std::nullopt}},
      ParameterDescriptor{"chroma_phase",
                          "Chroma Phase",
                          "Phase rotation applied to chroma components in "
                          "degrees. Range: -180 to 180",
                          ParameterType::DOUBLE,
                          {-180.0, 180.0, 0.0, {}, false, std::nullopt}},
      ParameterDescriptor{
          "luma_nr",
          "Luma Noise Reduction",
          "Luma noise reduction level in dB. 0 = disabled. Range: 0.0-10.0",
          ParameterType::DOUBLE,
          {0.0, 10.0, 0.0, {}, false, std::nullopt}},
      ParameterDescriptor{"chroma_nr",
                          "Chroma Noise Reduction",
                          "Chroma noise reduction level in dB (NTSC only). 0 = "
                          "disabled. Range: 0.0-10.0",
                          ParameterType::DOUBLE,
                          {0.0, 10.0, 0.0, {}, false, std::nullopt}},
      ParameterDescriptor{"output_padding",
                          "Output Padding",
                          "Pad output to multiple of this many pixels on both "
                          "axes. Range: 1-32.\n"
                          "Ignored by FFV1 for VP415e, whose width is fixed "
                          "at the BT.601 720.",
                          ParameterType::INT32,
                          {1, 32, 8, {}, false, std::nullopt}},
      ParameterDescriptor{
          "encoder_preset",
          "Encoder Preset",
          "Encoder speed/quality preset (for H.264/H.265): fast, medium, slow, "
          "veryslow",
          ParameterType::STRING,
          {{},
           {},
           std::string("medium"),
           {"fast", "medium", "slow", "veryslow"},
           false,
           ParameterDependency{"ffmpeg_format", {"mp4-h264", "mkv-ffv1"}}}},
      ParameterDescriptor{
          "encoder_crf",
          "Encoder CRF",
          "Constant Rate Factor for quality-based encoding (0-51, "
          "lower=better).\n"
          "AV1 compact delivery typically uses 30-35.\n"
          "Rate-control precedence: lossless mode, non-zero target bitrate, "
          "then CRF quality.",
          ParameterType::INT32,
          {0,
           51,
           18,
           {},
           false,
           ParameterDependency{"ffmpeg_format",
                               {"mp4-h264", "mkv-ffv1", "mp4-av1"}}}},
      ParameterDescriptor{
          "encoder_bitrate",
          "Encoder Bitrate",
          "Target bitrate in bits/sec.\n"
          "A non-zero value takes precedence over CRF; lossless mode takes "
          "precedence over both.\n"
          "0 uses CRF quality. Example: 10000000 = 10 Mbps.",
          ParameterType::INT32,
          {0,
           100000000,
           0,
           {},
           false,
           ParameterDependency{"ffmpeg_format",
                               {"mp4-h264", "mkv-ffv1", "mp4-av1"}}}},
      ParameterDescriptor{
          "hardware_encoder",
          "Hardware Encoder",
          "Use hardware accelerated encoding (none, vaapi, nvenc, qsv, amf, "
          "videotoolbox). Auto-detection in preset dialog.",
          ParameterType::STRING,
          {{},
           {},
           std::string("none"),
           {"none", "vaapi", "nvenc", "qsv", "amf", "videotoolbox"},
           false,
           ParameterDependency{
               "ffmpeg_format",
               {"mp4-h264", "mp4-hevc", "mov-h264", "mov-hevc"}}}},
      ParameterDescriptor{
          "prores_profile",
          "ProRes Profile",
          "ProRes quality profile: proxy, lt, standard, hq (default), 4444, "
          "4444xq",
          ParameterType::STRING,
          {{},
           {},
           std::string("hq"),
           {"proxy", "lt", "standard", "hq", "4444", "4444xq"},
           false,
           ParameterDependency{"ffmpeg_format", {"mov-prores"}}}},
      ParameterDescriptor{
          "use_lossless_mode",
          "Use Lossless Mode",
          "Enable mathematically lossless encoding (H.264/H.265/AV1 only, "
          "overrides bitrate and CRF)",
          ParameterType::BOOL,
          {{},
           {},
           false,
           {},
           false,
           ParameterDependency{
               "ffmpeg_format",
               {"mp4-h264", "mp4-hevc", "mp4-av1", "mov-h264", "mov-hevc"}}}},
      ParameterDescriptor{
          "apply_deinterlace",
          "Apply Deinterlacing",
          "Apply bwdif deinterlacing filter for progressive web playback",
          ParameterType::BOOL,
          {{},
           {},
           false,
           {},
           false,
           ParameterDependency{
               "ffmpeg_format",
               {"mp4-h264", "mp4-hevc", "mp4-av1", "mov-h264", "mov-hevc"}}}},
      ParameterDescriptor{
          "display_aspect_ratio",
          "Display Aspect Ratio",
          "Display aspect ratio signalled to players (metadata only, no "
          "rescaling):\n"
          "  4:3  - standard-definition television aspect (default; most SD "
          "LaserDisc and tape material)\n"
          "  16:9 - widescreen aspect\n"
          "  auto - square pixels (no aspect ratio metadata)",
          ParameterType::STRING,
          {{},
           {},
           std::string("4:3"),
           {"auto", "4:3", "16:9"},
           false,
           ParameterDependency{"output_mode", {"ffmpeg"}}}},
      ParameterDescriptor{
          "video_filter",
          "Custom Video Filter",
          "Custom FFmpeg video filter chain applied before encoding, using "
          "the same syntax as ffmpeg's -vf option. Examples:\n"
          "  fieldmatch,decimate - inverse telecine NTSC film content to "
          "23.976 fps\n"
          "  bwdif=mode=send_frame - deinterlace without doubling the frame "
          "rate\n"
          "  crop=692:554 - crop the output frame\n"
          "Leave empty for no filtering. An invalid filter string causes the "
          "export to fail with the FFmpeg error message.",
          ParameterType::STRING,
          {{},
           {},
           std::string(""),
           {},
           false,
           ParameterDependency{"output_mode", {"ffmpeg"}}}},
      ParameterDescriptor{
          "bt601_bit_depth",
          "BT.601 Bit Depth",
          "Output bit depth for the BT.601 export (\"FFV1 for VP415e\"):\n"
          "  8  - yuv422p, the player default; decodes about 1.5x faster and "
          "is roughly a third smaller\n"
          "  10 - yuv422p10le, matching the 4fsc archival export\n"
          "The 4fsc export remains the master; this one is a derived, "
          "player-ready file.",
          ParameterType::STRING,
          {{},
           {},
           std::string("8"),
           {"8", "10"},
           false,
           ParameterDependency{"ffmpeg_format", {"mkv-ffv1-bt601"}}}},
      ParameterDescriptor{
          "ffv1_slices",
          "FFV1 Slices",
          "Number of FFV1 slices per frame. The slice count is fixed at "
          "encode time and caps how far a decoder can be parallelised, so a "
          "file meant for real-time playback wants enough slices to saturate "
          "the playback machine's cores.\n"
          "  auto - 4 for the 4fsc archival export, 16 for FFV1 for VP415e\n"
          "Higher counts cost a little compression.",
          ParameterType::STRING,
          {{},
           {},
           std::string("auto"),
           {"auto", "4", "12", "16", "24", "30", "36"},
           false,
           ParameterDependency{"ffmpeg_format",
                               {"mkv-ffv1", "mkv-ffv1-bt601", "nut-ffv1"}}}},
      ParameterDescriptor{
          "rawvideo_format",
          "Rawvideo Pixel Format",
          "Pixel format for nut-rawvideo:\n"
          "  rgb - RGB48, full-precision RGB with no YUV rounding\n"
          "  yuv - YUV444P16, the pipeline's own internal format written "
          "through with no conversion at all",
          ParameterType::STRING,
          {{},
           {},
           std::string("rgb"),
           {"rgb", "yuv"},
           false,
           ParameterDependency{"ffmpeg_format", {"nut-rawvideo"}}}},
      ParameterDescriptor{
          "embed_audio",
          "Embed Audio",
          "Embed the input's audio channel pairs in the output file, one "
          "output stream per pair, each titled with its channel pair name "
          "(requires audio in source). The audio codec follows the container: "
          "FLAC for FFV1/rawvideo, PCM S24LE for ProRes/V210/V410/D10, AAC "
          "for H.264/H.265/AV1.",
          ParameterType::BOOL,
          {{},
           {},
           false,
           {},
           false,
           ParameterDependency{"ffmpeg_format", ffmpeg_formats}}},
      ParameterDescriptor{
          "audio_channel_pairs",
          "Audio Channel Pairs",
          "Which audio channel pairs to embed, one output audio stream per "
          "channel pair:\n"
          "  all - embed every channel pair carried by the input (default)\n"
          "  0,2 - comma-separated 0-based channel pair indices\n"
          "Channel pair indices match the CVBS container's _audio_<p>.wav "
          "numbering. Each embedded stream is titled with the channel pair's "
          "name (set at the audio source, import or EFM decode stage). The "
          "export fails if a listed channel pair does not exist.",
          ParameterType::STRING,
          {{},
           {},
           std::string("all"),
           {},
           false,
           ParameterDependency{"embed_audio", {"true"}}}},
      ParameterDescriptor{
          "audio_gain_db",
          "Audio Gain (dB)",
          "Gain applied to the embedded audio in decibels. 0 = unchanged; "
          "positive values boost (6 dB roughly doubles the amplitude), "
          "negative values attenuate. Samples are clipped at full scale. "
          "Range: -24 to 24",
          ParameterType::DOUBLE,
          {-24.0,
           24.0,
           0.0,
           {},
           false,
           ParameterDependency{"embed_audio", {"true"}}}},
      ParameterDescriptor{
          "embed_closed_captions",
          "Embed Closed Captions",
          "Embed closed captions as mov_text subtitles (converts EIA-608 to "
          "text, MP4/MOV only)",
          ParameterType::BOOL,
          {{},
           {},
           false,
           {},
           false,
           ParameterDependency{
               "ffmpeg_format",
               {"mp4-h264", "mp4-hevc", "mp4-av1", "mov-h264", "mov-hevc"}}}},
      ParameterDescriptor{
          "embed_chapter_metadata",
          "Embed Chapter Metadata",
          "Write chapter markers from VBI data to output file (MKV/MP4/MOV "
          "only; requires chapter numbers decoded by the \"biphase\" "
          "observer)",
          ParameterType::BOOL,
          {{},
           {},
           false,
           {},
           false,
           ParameterDependency{"ffmpeg_format",
                               {"mkv-ffv1", "mkv-ffv1-bt601", "mov-prores",
                                "mov-v210", "mov-v410", "mp4-h264", "mov-h264",
                                "mp4-hevc", "mov-hevc", "mp4-av1"}}}},
      ParameterDescriptor{
          "embed_disc_metadata",
          "Embed Disc Metadata",
          "Attach the LaserDisc VBI metadata - picture numbers, time codes, "
          "chapters, stop codes and programme status - inside the output "
          "file, so a player emulator needs no sidecar. Matroska only: MP4 "
          "and MOV cannot carry attachments. Requires VBI decodable by the "
          "\"biphase\" observer.",
          ParameterType::BOOL,
          {{},
           {},
           false,
           {},
           false,
           ParameterDependency{"ffmpeg_format",
                               {"mkv-ffv1", "mkv-ffv1-bt601"}}}},
      ParameterDescriptor{
          "disc_metadata_detail",
          "Disc Metadata Detail",
          "How much of the disc metadata to write.\n"
          "  map  - address map, events and disc status (a few kilobytes)\n"
          "  full - also the raw biphase words for every field, so codes "
          "decode-orc does not yet interpret stay recoverable (about 2.3 MB "
          "for a full side)",
          ParameterType::STRING,
          {{},
           {},
           std::string("map"),
           {"map", "full"},
           false,
           ParameterDependency{"embed_disc_metadata", {"true"}}}}};

  // Add format-specific parameters
  if (project_format == VideoSystem::NTSC) {
    params.push_back(ParameterDescriptor{
        "ntsc_phase_comp",
        "NTSC Phase Compensation",
        "Adjust phase per-line using burst phase (NTSC only)",
        ParameterType::BOOL,
        {{}, {}, true, {}, false, std::nullopt}});
    params.push_back(ParameterDescriptor{
        "chroma_weight",
        "Chroma Weight",
        "Chroma weight for 3D adaptive filter (NTSC 3D only). Higher = prefer "
        "more 2D result. Range: 0.0-10.0",
        ParameterType::DOUBLE,
        {0.0,
         10.0,
         1.0,
         {},
         false,
         ParameterDependency{"decoder_type", {"ntsc3d", "ntsc3dnoadapt"}}}});
    params.push_back(
        ParameterDescriptor{"adapt_threshold",
                            "Adapt Threshold",
                            "3D adaptive filter threshold (NTSC 3D only). "
                            "Higher = prefer more 3D result. Range: 0.0-10.0",
                            ParameterType::DOUBLE,
                            {0.0,
                             10.0,
                             1.0,
                             {},
                             false,
                             ParameterDependency{"decoder_type", {"ntsc3d"}}}});
  } else if (project_format == VideoSystem::PAL ||
             project_format == VideoSystem::PAL_M) {
    params.push_back(ParameterDescriptor{
        "simple_pal",
        "Simple PAL",
        "Use 1D UV filter for Transform PAL (simpler, faster, lower quality)",
        ParameterType::BOOL,
        {{},
         {},
         false,
         {},
         false,
         ParameterDependency{"decoder_type", {"transform2d", "transform3d"}}}});
    params.push_back(ParameterDescriptor{
        "transform_threshold",
        "Transform Threshold",
        "Similarity threshold for Transform PAL decoder (default 0.4). Higher "
        "= more transform filtering applied. Range: 0.0-1.0",
        ParameterType::DOUBLE,
        {0.0,
         1.0,
         0.4,
         {},
         false,
         ParameterDependency{"decoder_type", {"transform2d", "transform3d"}}}});
  } else {
    // Unknown format - include both for backwards compatibility
    params.push_back(ParameterDescriptor{
        "ntsc_phase_comp",
        "NTSC Phase Compensation",
        "Adjust phase per-line using burst phase (NTSC only)",
        ParameterType::BOOL,
        {{}, {}, true, {}, false, std::nullopt}});
    params.push_back(ParameterDescriptor{
        "chroma_weight",
        "Chroma Weight",
        "Chroma weight for 3D adaptive filter (NTSC 3D only). Higher = prefer "
        "more 2D result. Range: 0.0-10.0",
        ParameterType::DOUBLE,
        {0.0,
         10.0,
         1.0,
         {},
         false,
         ParameterDependency{"decoder_type", {"ntsc3d", "ntsc3dnoadapt"}}}});
    params.push_back(
        ParameterDescriptor{"adapt_threshold",
                            "Adapt Threshold",
                            "3D adaptive filter threshold (NTSC 3D only). "
                            "Higher = prefer more 3D result. Range: 0.0-10.0",
                            ParameterType::DOUBLE,
                            {0.0,
                             10.0,
                             1.0,
                             {},
                             false,
                             ParameterDependency{"decoder_type", {"ntsc3d"}}}});
    params.push_back(ParameterDescriptor{
        "simple_pal",
        "Simple PAL",
        "Use 1D UV filter for Transform PAL (simpler, faster, lower quality)",
        ParameterType::BOOL,
        {{},
         {},
         false,
         {},
         false,
         ParameterDependency{"decoder_type", {"transform2d", "transform3d"}}}});
    params.push_back(ParameterDescriptor{
        "transform_threshold",
        "Transform Threshold",
        "Similarity threshold for Transform PAL decoder (default 0.4). Higher "
        "= more transform filtering applied. Range: 0.0-1.0",
        ParameterType::DOUBLE,
        {0.0,
         1.0,
         0.4,
         {},
         false,
         ParameterDependency{"decoder_type", {"transform2d", "transform3d"}}}});
  }

  return params;
}

std::map<std::string, ParameterValue> VideoSinkStage::get_parameters() const {
  std::map<std::string, ParameterValue> params;
  params["output_path"] = output_path_;
  params["decoder_type"] = decoder_type_;
  params["output_mode"] = output_mode_;
  params["raw_format"] = raw_format_;
  params["ffmpeg_format"] = ffmpeg_format_;
  params["chroma_gain"] = chroma_gain_;
  params["chroma_phase"] = chroma_phase_;
  params["luma_nr"] = luma_nr_;
  params["chroma_nr"] = chroma_nr_;
  params["ntsc_phase_comp"] = ntsc_phase_comp_;
  params["simple_pal"] = simple_pal_;
  params["transform_threshold"] = transform_threshold_;
  params["chroma_weight"] = chroma_weight_;
  params["adapt_threshold"] = adapt_threshold_;
  params["output_padding"] = output_padding_;
  params["encoder_preset"] = encoder_preset_;
  params["encoder_crf"] = encoder_crf_;
  params["encoder_bitrate"] = encoder_bitrate_;
  params["embed_audio"] = embed_audio_;
  params["audio_channel_pairs"] = audio_channel_pairs_;
  params["audio_gain_db"] = audio_gain_db_;
  params["embed_closed_captions"] = embed_closed_captions_;
  params["embed_chapter_metadata"] = embed_chapter_metadata_;
  params["hardware_encoder"] = hardware_encoder_;
  params["prores_profile"] = prores_profile_;
  params["use_lossless_mode"] = use_lossless_mode_;
  params["apply_deinterlace"] = apply_deinterlace_;
  params["display_aspect_ratio"] = display_aspect_ratio_;
  params["video_filter"] = video_filter_;
  params["bt601_bit_depth"] = bt601_bit_depth_;
  params["ffv1_slices"] = ffv1_slices_;
  params["rawvideo_format"] = rawvideo_format_;
  params["embed_disc_metadata"] = embed_disc_metadata_;
  params["disc_metadata_detail"] = disc_metadata_detail_;
  return params;
}

bool VideoSinkStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  bool decoder_config_changed = false;

  // Validate output mode/format selections before applying anything so an
  // invalid request leaves the stage state untouched.
  {
    auto it = params.find("output_mode");
    if (it != params.end() && std::holds_alternative<std::string>(it->second)) {
      const auto& mode = std::get<std::string>(it->second);
      if (mode != "raw" && mode != "ffmpeg") {
        ORC_LOG_ERROR(
            "VideoSink: Invalid output mode '{}' - must be raw or ffmpeg",
            mode);
        return false;
      }
#ifndef HAVE_FFMPEG
      if (mode == "ffmpeg") {
        ORC_LOG_ERROR(
            "VideoSink: FFmpeg support not compiled in, cannot use output "
            "mode 'ffmpeg'");
        return false;
      }
#endif
    }

    it = params.find("raw_format");
    if (it != params.end() && std::holds_alternative<std::string>(it->second)) {
      const auto& format = std::get<std::string>(it->second);
      if (!is_raw_output_format(format)) {
        ORC_LOG_ERROR(
            "VideoSink: Invalid raw format '{}' - must be rgb, yuv, or y4m",
            format);
        return false;
      }
    }

    it = params.find("ffmpeg_format");
    if (it != params.end() && std::holds_alternative<std::string>(it->second)) {
      const auto& format = std::get<std::string>(it->second);
      if (is_raw_output_format(format) || !is_supported_output_format(format)) {
        ORC_LOG_ERROR(
            "VideoSink: FFmpeg format '{}' is not a supported encoded format",
            format);
        return false;
      }
    }

    it = params.find("display_aspect_ratio");
    if (it != params.end() && std::holds_alternative<std::string>(it->second)) {
      const auto& dar = std::get<std::string>(it->second);
      if (dar != "auto" && dar != "4:3" && dar != "16:9") {
        ORC_LOG_ERROR(
            "VideoSink: Invalid display aspect ratio '{}' - must be auto, "
            "4:3, or 16:9",
            dar);
        return false;
      }
    }

    // Legacy key from projects saved before the raw and FFmpeg sinks were
    // merged into the single Video Sink stage.
    it = params.find("output_format");
    if (it != params.end() && std::holds_alternative<std::string>(it->second)) {
      const auto& format = std::get<std::string>(it->second);
      if (!is_raw_output_format(format) &&
          !is_supported_output_format(format)) {
        ORC_LOG_ERROR("VideoSink: Invalid output format '{}'", format);
        return false;
      }
    }
  }

  for (const auto& [key, value] : params) {
    if (key == "output_path") {
      if (std::holds_alternative<std::string>(value)) {
        output_path_ = std::get<std::string>(value);
      }
    } else if (key == "decoder_type") {
      if (std::holds_alternative<std::string>(value)) {
        auto new_val = std::get<std::string>(value);
        if (new_val == "auto") {
          ORC_LOG_WARN(
              "VideoSink: decoder_type 'auto' is no longer supported (loaded "
              "from old project). Migrating to 'ntsc2d'.");
          new_val = "ntsc2d";
        }
        if (new_val != decoder_type_) {
          ORC_LOG_DEBUG("VideoSink: decoder_type changed from '{}' to '{}'",
                        decoder_type_, new_val);
          decoder_type_ = new_val;
          decoder_config_changed = true;
        }
      }
    } else if (key == "output_mode") {
      if (std::holds_alternative<std::string>(value)) {
        output_mode_ = std::get<std::string>(value);
      }
    } else if (key == "raw_format") {
      if (std::holds_alternative<std::string>(value)) {
        raw_format_ = std::get<std::string>(value);
      }
    } else if (key == "ffmpeg_format") {
      if (std::holds_alternative<std::string>(value)) {
        ffmpeg_format_ = std::get<std::string>(value);
        // Not simply "the key was present": project_to_dag() pre-fills every
        // declared parameter's default (parameter_types.h's
        // ParameterConstraints::default_value, "mp4-h264" for this one)
        // before overlaying what a project file/CLI graph actually set, so
        // this key is present — with the constructor's own default value —
        // on essentially every stage instance, piped or not. Comparing
        // against that known default is the only way left to tell "the
        // caller asked for this" apart from "nothing overrode the default",
        // which is exactly the distinction FFmpegOutputBackend::initialize()
        // needs (see its own comment) to decide whether a non-pipe-safe
        // choice was deliberate.
        ffmpeg_format_explicit_ = (ffmpeg_format_ != "mp4-h264");
      }
    } else if (key == "output_format") {
      // Legacy key (pre Video Sink merge): route to the matching mode/format
      // pair. std::map iteration order means explicit output_mode/raw_format
      // values (later keys) still override this inference.
      if (std::holds_alternative<std::string>(value)) {
        const auto& format = std::get<std::string>(value);
        if (is_raw_output_format(format)) {
          output_mode_ = "raw";
          raw_format_ = format;
        } else {
          output_mode_ = "ffmpeg";
          ffmpeg_format_ = format;
          ffmpeg_format_explicit_ = (ffmpeg_format_ != "mp4-h264");
        }
      }
    } else if (key == "chroma_gain") {
      if (std::holds_alternative<double>(value)) {
        auto new_val = std::get<double>(value);
        if (new_val != chroma_gain_) {
          ORC_LOG_DEBUG("VideoSink: chroma_gain changed from {} to {}",
                        chroma_gain_, new_val);
          chroma_gain_ = new_val;
          decoder_config_changed = true;
        }
      }
    } else if (key == "chroma_phase") {
      if (std::holds_alternative<double>(value)) {
        auto new_val = std::get<double>(value);
        if (new_val != chroma_phase_) {
          ORC_LOG_DEBUG("VideoSink: chroma_phase changed from {} to {}",
                        chroma_phase_, new_val);
          chroma_phase_ = new_val;
          decoder_config_changed = true;
        }
      }
    } else if (key == "luma_nr") {
      if (std::holds_alternative<double>(value)) {
        auto new_val = std::get<double>(value);
        if (new_val != luma_nr_) {
          ORC_LOG_DEBUG("VideoSink: luma_nr changed from {} to {}", luma_nr_,
                        new_val);
          luma_nr_ = new_val;
          decoder_config_changed = true;
        }
      }
    } else if (key == "chroma_nr") {
      if (std::holds_alternative<double>(value)) {
        auto new_val = std::get<double>(value);
        if (new_val != chroma_nr_) {
          ORC_LOG_DEBUG("VideoSink: chroma_nr changed from {} to {}",
                        chroma_nr_, new_val);
          chroma_nr_ = new_val;
          decoder_config_changed = true;
        }
      }
    } else if (key == "ntsc_phase_comp") {
      if (std::holds_alternative<bool>(value)) {
        auto new_val = std::get<bool>(value);
        if (new_val != ntsc_phase_comp_) {
          ORC_LOG_DEBUG("VideoSink: ntsc_phase_comp changed from {} to {}",
                        ntsc_phase_comp_, new_val);
          ntsc_phase_comp_ = new_val;
          decoder_config_changed = true;
        }
      } else if (std::holds_alternative<std::string>(value)) {
        // Handle string representation of boolean (from YAML parsing)
        auto str_val = std::get<std::string>(value);
        bool new_val =
            (str_val == "true" || str_val == "1" || str_val == "yes");
        if (new_val != ntsc_phase_comp_) {
          ORC_LOG_DEBUG(
              "VideoSink: ntsc_phase_comp changed from {} to {} (from string "
              "'{}')",
              ntsc_phase_comp_, new_val, str_val);
          ntsc_phase_comp_ = new_val;
          decoder_config_changed = true;
        }
      }
    } else if (key == "chroma_weight") {
      if (std::holds_alternative<double>(value)) {
        auto new_val = std::get<double>(value);
        if (new_val != chroma_weight_) {
          ORC_LOG_DEBUG("VideoSink: chroma_weight changed from {} to {}",
                        chroma_weight_, new_val);
          chroma_weight_ = new_val;
          decoder_config_changed = true;
        }
      }
    } else if (key == "adapt_threshold") {
      if (std::holds_alternative<double>(value)) {
        auto new_val = std::get<double>(value);
        if (new_val != adapt_threshold_) {
          ORC_LOG_DEBUG("VideoSink: adapt_threshold changed from {} to {}",
                        adapt_threshold_, new_val);
          adapt_threshold_ = new_val;
          decoder_config_changed = true;
        }
      }
    } else if (key == "simple_pal") {
      if (std::holds_alternative<bool>(value)) {
        auto new_val = std::get<bool>(value);
        if (new_val != simple_pal_) {
          ORC_LOG_DEBUG("VideoSink: simple_pal changed from {} to {}",
                        simple_pal_, new_val);
          simple_pal_ = new_val;
          decoder_config_changed = true;
        }
      } else if (std::holds_alternative<std::string>(value)) {
        // Handle string representation of boolean (from YAML parsing)
        auto str_val = std::get<std::string>(value);
        bool new_val =
            (str_val == "true" || str_val == "1" || str_val == "yes");
        if (new_val != simple_pal_) {
          ORC_LOG_DEBUG(
              "VideoSink: simple_pal changed from {} to {} (from string '{}')",
              simple_pal_, new_val, str_val);
          simple_pal_ = new_val;
          decoder_config_changed = true;
        }
      }
    } else if (key == "transform_threshold") {
      if (std::holds_alternative<double>(value)) {
        auto new_val = std::get<double>(value);
        if (new_val != transform_threshold_) {
          ORC_LOG_DEBUG("VideoSink: transform_threshold changed from {} to {}",
                        transform_threshold_, new_val);
          transform_threshold_ = new_val;
          decoder_config_changed = true;
        }
      }
    } else if (key == "output_padding") {
      if (std::holds_alternative<int>(value)) {
        output_padding_ = std::get<int>(value);
      }
    } else if (key == "encoder_preset") {
      if (std::holds_alternative<std::string>(value)) {
        encoder_preset_ = std::get<std::string>(value);
      }
    } else if (key == "encoder_crf") {
      if (std::holds_alternative<int>(value)) {
        encoder_crf_ = std::get<int>(value);
      }
    } else if (key == "encoder_bitrate") {
      if (std::holds_alternative<int>(value)) {
        encoder_bitrate_ = std::get<int>(value);
      }
    } else if (key == "embed_audio") {
      if (std::holds_alternative<bool>(value)) {
        embed_audio_ = std::get<bool>(value);
      } else if (std::holds_alternative<std::string>(value)) {
        auto str_val = std::get<std::string>(value);
        embed_audio_ =
            (str_val == "true" || str_val == "1" || str_val == "yes");
      }
    } else if (key == "audio_channel_pairs") {
      if (std::holds_alternative<std::string>(value)) {
        audio_channel_pairs_ = std::get<std::string>(value);
        if (audio_channel_pairs_.empty()) {
          audio_channel_pairs_ = "all";
        }
      }
    } else if (key == "audio_gain_db") {
      if (std::holds_alternative<double>(value)) {
        audio_gain_db_ = std::get<double>(value);
      } else if (std::holds_alternative<int>(value)) {
        audio_gain_db_ = static_cast<double>(std::get<int>(value));
      }
    } else if (key == "embed_closed_captions") {
      if (std::holds_alternative<bool>(value)) {
        embed_closed_captions_ = std::get<bool>(value);
      } else if (std::holds_alternative<std::string>(value)) {
        auto str_val = std::get<std::string>(value);
        embed_closed_captions_ =
            (str_val == "true" || str_val == "1" || str_val == "yes");
      }
    } else if (key == "embed_chapter_metadata") {
      if (std::holds_alternative<bool>(value)) {
        embed_chapter_metadata_ = std::get<bool>(value);
      } else if (std::holds_alternative<std::string>(value)) {
        auto str_val = std::get<std::string>(value);
        embed_chapter_metadata_ =
            (str_val == "true" || str_val == "1" || str_val == "yes");
      }
    } else if (key == "hardware_encoder") {
      if (std::holds_alternative<std::string>(value)) {
        hardware_encoder_ = std::get<std::string>(value);
      }
    } else if (key == "prores_profile") {
      if (std::holds_alternative<std::string>(value)) {
        prores_profile_ = std::get<std::string>(value);
      }
    } else if (key == "use_lossless_mode") {
      if (std::holds_alternative<bool>(value)) {
        use_lossless_mode_ = std::get<bool>(value);
      } else if (std::holds_alternative<std::string>(value)) {
        auto str_val = std::get<std::string>(value);
        use_lossless_mode_ =
            (str_val == "true" || str_val == "1" || str_val == "yes");
      }
    } else if (key == "apply_deinterlace") {
      if (std::holds_alternative<bool>(value)) {
        apply_deinterlace_ = std::get<bool>(value);
      } else if (std::holds_alternative<std::string>(value)) {
        auto str_val = std::get<std::string>(value);
        apply_deinterlace_ =
            (str_val == "true" || str_val == "1" || str_val == "yes");
      }
    } else if (key == "display_aspect_ratio") {
      if (std::holds_alternative<std::string>(value)) {
        display_aspect_ratio_ = std::get<std::string>(value);
      }
    } else if (key == "video_filter") {
      if (std::holds_alternative<std::string>(value)) {
        // Validated by the FFmpeg backend when the export starts; an invalid
        // chain fails the trigger with the FFmpeg error message.
        video_filter_ = std::get<std::string>(value);
      }
    } else if (key == "bt601_bit_depth") {
      if (std::holds_alternative<std::string>(value)) {
        const auto& depth = std::get<std::string>(value);
        bt601_bit_depth_ = (depth == "10") ? "10" : "8";
      } else if (std::holds_alternative<int>(value)) {
        bt601_bit_depth_ = (std::get<int>(value) == 10) ? "10" : "8";
      }
    } else if (key == "ffv1_slices") {
      if (std::holds_alternative<std::string>(value)) {
        ffv1_slices_ = std::get<std::string>(value);
        if (ffv1_slices_.empty()) {
          ffv1_slices_ = "auto";
        }
      } else if (std::holds_alternative<int>(value)) {
        ffv1_slices_ = std::to_string(std::get<int>(value));
      }
    } else if (key == "rawvideo_format") {
      if (std::holds_alternative<std::string>(value)) {
        const auto& fmt = std::get<std::string>(value);
        rawvideo_format_ = (fmt == "yuv") ? "yuv" : "rgb";
      }
    } else if (key == "embed_disc_metadata") {
      if (std::holds_alternative<bool>(value)) {
        embed_disc_metadata_ = std::get<bool>(value);
      } else if (std::holds_alternative<std::string>(value)) {
        const auto& v = std::get<std::string>(value);
        embed_disc_metadata_ = (v == "true" || v == "1" || v == "yes");
      }
    } else if (key == "disc_metadata_detail") {
      if (std::holds_alternative<std::string>(value)) {
        disc_metadata_detail_ =
            (std::get<std::string>(value) == "full") ? "full" : "map";
      }
    }
  }

  // Derive the effective output format from the selected mode.
  output_format_ = (output_mode_ == "raw") ? raw_format_ : ffmpeg_format_;

  // Log if decoder configuration was changed
  if (decoder_config_changed) {
    ORC_LOG_DEBUG(
        "VideoSink: Decoder configuration changed - cached decoder will be "
        "recreated on next preview");
  }

  set_configuration_status(output_path_.empty()
                               ? orc::ConfigurationStatus::Yellow
                               : orc::ConfigurationStatus::Green);
  return true;
}

bool VideoSinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  // Reset cancel flag at the start of each trigger so a previous cancellation
  // (e.g. during CC collection) doesn't cause subsequent triggers to fail
  // immediately.
  cancel_requested_.store(false);

  // Apply parameters up front so the effective output mode reflects this
  // trigger request (run_export_trigger applies them again, harmless).
  if (!set_parameters(parameters)) {
    return false;
  }

  // Observation collection only applies to FFmpeg output; raw output cannot
  // embed closed captions or chapter data.
  const bool ffmpeg_output = (output_mode_ == "ffmpeg");

  // Closed caption / chapter / disc-metadata collection below scans the
  // whole input frame-by-frame before a single frame is exported — a full
  // a-priori pass that a piped/live source's placeholder frame_range()
  // cannot support (it would loop over that placeholder, not the source's
  // real length). Checked once here; both collection blocks below gate on
  // it.
  bool trigger_unbounded_source = false;
  if (!inputs.empty()) {
    if (auto trigger_vfr =
            std::dynamic_pointer_cast<VideoFrameRepresentation>(inputs[0])) {
      trigger_unbounded_source = trigger_vfr->has_unbounded_frame_range();
    }
  }

  // Check if closed caption embedding is enabled in parameters
  bool embed_cc = false;
  auto cc_param = parameters.find("embed_closed_captions");
  if (cc_param != parameters.end()) {
    if (std::holds_alternative<bool>(cc_param->second)) {
      embed_cc = std::get<bool>(cc_param->second);
    } else if (std::holds_alternative<std::string>(cc_param->second)) {
      std::string val = std::get<std::string>(cc_param->second);
      embed_cc = (val == "true" || val == "1" || val == "yes");
    }
  }
  embed_cc = embed_cc && ffmpeg_output;
  if (embed_cc && trigger_unbounded_source) {
    ORC_LOG_WARN(
        "VideoSink: input reports an unbounded frame range — closed caption "
        "embedding needs a full pass over the whole capture ahead of time, "
        "which an unbounded source cannot provide; skipping it for this "
        "export.");
    embed_cc = false;
  }

  // If closed caption embedding is enabled, run the host "closed_caption"
  // observer to populate the observation context before running the export
  if (embed_cc) {
    ORC_LOG_DEBUG(
        "VideoSink: Closed caption embedding enabled, extracting CC "
        "observations");

    // Extract VideoFrameRepresentation from input
    if (!inputs.empty()) {
      auto vfr = std::dynamic_pointer_cast<VideoFrameRepresentation>(inputs[0]);
      if (vfr) {
        // Obtain a "closed_caption" observer session from the host service. The
        // handle is reused across every frame so its cross-field pairing state
        // accumulates. A null service (older host) skips CC collection.
        IObservationService* obs_service =
            orc::plugin::get_observation_service();
        std::unique_ptr<IObserverHandle> cc_observer =
            obs_service ? obs_service->create_observer("closed_caption")
                        : nullptr;
        if (!cc_observer) {
          ORC_LOG_WARN(
              "VideoSink: observation service unavailable; closed caption data "
              "not collected");
        }

        auto frame_range = vfr->frame_range();
        const size_t total_cc_frames = frame_range.count();

        if (progress_callback_) {
          progress_callback_(0, total_cc_frames,
                             "Collecting closed caption data...");
        }

        size_t cc_frames_processed = 0;
        for (FrameID frame_id = frame_range.first; frame_id <= frame_range.last;
             ++frame_id) {
          if (cc_observer && vfr->has_frame(frame_id)) {
            cc_observer->process_frame(*vfr, frame_id, observation_context);
          }
          ++cc_frames_processed;
          if (progress_callback_) {
            progress_callback_(cc_frames_processed, total_cc_frames,
                               "Collecting closed caption data...");
          }
          if (cancel_requested_.load()) {
            ORC_LOG_WARN(
                "VideoSink: Cancelled during closed caption collection");
            return false;
          }
        }

        ORC_LOG_DEBUG("VideoSink: CC observations extracted for {} frames",
                      total_cc_frames);
      }
    }
  }

  // If chapter metadata embedding is enabled, run BiphaseObserver to populate
  // VBI observations (including chapter_number) in the observation context.
  // The pipeline does not guarantee a BiphaseObserver pass for FFmpeg exports.
  bool embed_chapters = false;
  auto ch_param = parameters.find("embed_chapter_metadata");
  if (ch_param != parameters.end()) {
    if (std::holds_alternative<bool>(ch_param->second)) {
      embed_chapters = std::get<bool>(ch_param->second);
    } else if (std::holds_alternative<std::string>(ch_param->second)) {
      std::string val = std::get<std::string>(ch_param->second);
      embed_chapters = (val == "true" || val == "1" || val == "yes");
    }
  }
  embed_chapters = embed_chapters && ffmpeg_output;

  // Disc metadata is built from the same biphase pass, so the two options
  // share one scan rather than decoding every frame twice.
  bool embed_disc = false;
  auto disc_param = parameters.find("embed_disc_metadata");
  if (disc_param != parameters.end()) {
    if (std::holds_alternative<bool>(disc_param->second)) {
      embed_disc = std::get<bool>(disc_param->second);
    } else if (std::holds_alternative<std::string>(disc_param->second)) {
      std::string val = std::get<std::string>(disc_param->second);
      embed_disc = (val == "true" || val == "1" || val == "yes");
    }
  }
  embed_disc = embed_disc && ffmpeg_output;
  if ((embed_chapters || embed_disc) && trigger_unbounded_source) {
    ORC_LOG_WARN(
        "VideoSink: input reports an unbounded frame range — chapter/disc "
        "metadata embedding needs a full pass over the whole capture ahead "
        "of time, which an unbounded source cannot provide; skipping it for "
        "this export.");
    embed_chapters = false;
    embed_disc = false;
  }

  if (embed_chapters || embed_disc) {
    ORC_LOG_DEBUG(
        "VideoSink: Chapter/disc metadata enabled, extracting VBI "
        "observations");

    if (!inputs.empty()) {
      auto vfr = std::dynamic_pointer_cast<VideoFrameRepresentation>(inputs[0]);
      if (vfr) {
        // Obtain a "biphase" observer session from the host service to decode
        // VBI chapter numbers. A null service (older host) skips collection.
        IObservationService* obs_service =
            orc::plugin::get_observation_service();
        std::unique_ptr<IObserverHandle> biphase_observer =
            obs_service ? obs_service->create_observer("biphase") : nullptr;
        if (!biphase_observer) {
          ORC_LOG_WARN(
              "VideoSink: observation service unavailable; VBI data not "
              "collected");
        }
        auto frame_range = vfr->frame_range();
        const size_t total_frames = frame_range.count();

        if (progress_callback_) {
          progress_callback_(0, total_frames, "Collecting VBI data...");
        }

        size_t frames_processed = 0;
        for (FrameID frame_id = frame_range.first; frame_id <= frame_range.last;
             ++frame_id) {
          if (biphase_observer && vfr->has_frame(frame_id)) {
            biphase_observer->process_frame(*vfr, frame_id,
                                            observation_context);
          }
          ++frames_processed;
          if (progress_callback_) {
            progress_callback_(frames_processed, total_frames,
                               "Collecting VBI data...");
          }
          if (cancel_requested_.load()) {
            ORC_LOG_WARN("VideoSink: Cancelled during VBI collection");
            return false;
          }
        }

        ORC_LOG_DEBUG("VideoSink: VBI observations extracted for {} frames",
                      total_frames);
      }
    }
  }

  // If an upstream representation carries audio produced by a deferred
  // whole-stream decode (e.g. EFM audio), prime it here so its cost is metered
  // on the progress dialog rather than freezing frame 0 of the export loop.
  // Only worthwhile when audio is actually embedded (FFmpeg output). The hook
  // is a no-op for representations with no deferred decode, and forwards down
  // the wrapper chain so a producer nested beneath other stages (e.g. an
  // audio_channel_map between EFM decode and the sink) is still reached.
  if (embed_audio_ && ffmpeg_output && !inputs.empty()) {
    if (auto vfr =
            std::dynamic_pointer_cast<VideoFrameRepresentation>(inputs[0])) {
      ORC_LOG_DEBUG("VideoSink: priming deferred audio decode before export");
      vfr->prime_audio_decode(
          [this](uint64_t done, uint64_t total, const std::string& message) {
            if (progress_callback_) progress_callback_(done, total, message);
          });
      if (cancel_requested_.load()) {
        ORC_LOG_WARN("VideoSink: Cancelled during audio decode priming");
        return false;
      }
    }
  }

  return run_export_trigger(inputs, parameters, observation_context);
}

bool VideoSinkStage::run_export_trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  // VideoSinkStage is a video output sink and does not require or populate
  // observations. The LD sink and dedicated analysis stages handle observation
  // extraction.
  ORC_LOG_DEBUG("VideoSink: Trigger called - starting decode");

  // Mark trigger as in progress and reset cancel flag
  trigger_in_progress_.store(true);
  cancel_requested_.store(false);

  // Drop any cached colour-sequence phase from a previous run; a reused stage
  // instance may now be pointed at a different source.
  {
    std::lock_guard<std::mutex> lock(colour_phase_mutex_);
    colour_phase_cache_.clear();
  }

  // Apply any parameter updates
  set_parameters(parameters);

  // Validate output path is set
  if (output_path_.empty()) {
    ORC_LOG_ERROR("VideoSink: No output path specified");
    trigger_status_ = "Error: No output path specified";
    trigger_in_progress_.store(false);
    return false;
  }

  // 1. Extract VideoFrameRepresentation from input
  if (inputs.empty()) {
    ORC_LOG_ERROR("VideoSink: No input provided");
    trigger_status_ = "Error: No input";
    trigger_in_progress_.store(false);
    return false;
  }

  auto vfr =
      std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(inputs[0]);
  if (!vfr) {
    ORC_LOG_ERROR("VideoSink: Input is not a VideoFrameRepresentation");
    trigger_status_ = "Error: Invalid input type";
    trigger_in_progress_.store(false);
    return false;
  }

  // 2. Get video parameters from VFR
  auto video_params_opt = vfr->get_video_parameters();
  if (!video_params_opt) {
    ORC_LOG_ERROR("VideoSink: Input has no video parameters");
    trigger_status_ = "Error: No video parameters";
    trigger_in_progress_.store(false);
    return false;
  }

  // 3. Use orc-core SourceParameters directly
  auto videoParams = *video_params_opt;  // Make a copy so we can modify it

  ORC_LOG_DEBUG("VideoSink: Video parameters from source metadata:");
  ORC_LOG_DEBUG("  Horizontal active region: {} to {} ({} samples)",
                videoParams.active_video_start, videoParams.active_video_end,
                videoParams.active_video_end - videoParams.active_video_start);
  ORC_LOG_DEBUG(
      "  Vertical active region: {} to {} ({} lines)",
      videoParams.first_active_frame_line, videoParams.last_active_frame_line,
      videoParams.last_active_frame_line - videoParams.first_active_frame_line);

  // Apply padding adjustments to active video region BEFORE configuring decoder
  // This ensures the decoder processes the correct region that will be written
  // to output
  // The BT.601 export decodes the 4fsc window that sits under the BT.601
  // 720-sample digital active line, rather than the source's own active
  // window. Both windows are anchored to the 0H timing datum, so the one is
  // derived from the other by time; taking the whole 720 from source means
  // the margins carry the source's own blanking rather than synthetic black,
  // and nothing is cropped. Codec padding would widen the window again and
  // shift the mapping, so it does not apply.
  const bool bt601_export =
      (output_mode_ == "ffmpeg") && orc::is_bt601_export_format(ffmpeg_format_);
  if (bt601_export) {
    const auto window = orc::resolve_bt601_source_window(videoParams.system);
    ORC_LOG_INFO(
        "VideoSink: BT.601 export: decoding the 13.5 MHz active line window "
        "{}..{} ({} samples) instead of {}..{}",
        window.active_video_start, window.active_video_end,
        window.active_video_end - window.active_video_start,
        videoParams.active_video_start, videoParams.active_video_end);
    videoParams.active_video_start = window.active_video_start;
    videoParams.active_video_end = window.active_video_end;
  }

  {
    OutputWriter::Configuration writerConfig;
    writerConfig.paddingAmount = bt601_export ? 1 : output_padding_;
    if (bt601_export && output_padding_ > 1) {
      ORC_LOG_DEBUG(
          "VideoSink: Output padding ({}) ignored for the BT.601 export",
          output_padding_);
    }

    ORC_LOG_DEBUG(
        "VideoSink: BEFORE padding adjustment: first_active_frame_line={}, "
        "last_active_frame_line={} (paddingAmount={})",
        videoParams.first_active_frame_line, videoParams.last_active_frame_line,
        writerConfig.paddingAmount);

    // Create temporary output writer just to apply padding adjustments
    OutputWriter tempWriter;
    tempWriter.updateConfiguration(videoParams, writerConfig);
    // videoParams now has adjusted activeVideoStart/End values

    ORC_LOG_DEBUG(
        "VideoSink: AFTER padding adjustment: first_active_frame_line={}, "
        "last_active_frame_line={}",
        videoParams.first_active_frame_line,
        videoParams.last_active_frame_line);
  }

  // Use active area boundaries from video parameters (metadata/hints)
  // Set flag so decoders know to use relative indexing when writing to
  // ComponentFrame
  videoParams.active_area_cropping_applied = true;

  ORC_LOG_DEBUG(
      "VideoSink: Using active area from video parameters: {}x{}",
      videoParams.active_video_end - videoParams.active_video_start,
      videoParams.last_active_frame_line - videoParams.first_active_frame_line);

  // 4. Create appropriate decoder
  // Note: We'll use the decoder classes directly (synchronously)
  // without the threading infrastructure for now

  const bool is_yc_source = vfr->has_separate_channels();
  // PAL_M has NTSC-like frame geometry (525 lines, 909 samples/line); only
  // pure PAL uses the 625-line non-uniform layout.
  const bool isPal = (videoParams.system == orc::VideoSystem::PAL);

  // Transform PAL filters operate on composite chroma; a Y/C source has no
  // composite chroma to filter, so fall back to pal2d.  Mutates decoder_type_
  // (persistent stage state, surfaced in the parameter UI).
  if (is_yc_source &&
      (decoder_type_ == "transform2d" || decoder_type_ == "transform3d")) {
    ORC_LOG_ERROR(
        "VideoSink: Transform PAL filters (transform2d/transform3d) are not "
        "compatible with YC sources.");
    ORC_LOG_ERROR(
        "VideoSink: YC sources have already-separated Y and C channels and "
        "do not need frequency-domain filtering.");
    ORC_LOG_ERROR("VideoSink: Please use 'pal2d' or 'mono' decoder instead.");
    ORC_LOG_ERROR("VideoSink: Falling back to 'pal2d' decoder for YC source.");
    decoder_type_ = "pal2d";
  }

  std::string parameter_error;
  const auto decoder_profile =
      decoder_video_profile_for_type(decoder_type_, videoParams.system);
  if (!apply_decoder_safe_video_parameters(videoParams, decoder_profile,
                                           "VideoSink trigger",
                                           &parameter_error)) {
    trigger_status_ = "Error: Invalid video parameters - " + parameter_error;
    trigger_in_progress_.store(false);
    return false;
  }

  DecoderParams decoderParams;
  decoderParams.chromaGain = chroma_gain_;
  decoderParams.chromaPhase = chroma_phase_;
  decoderParams.lumaNr = luma_nr_;
  decoderParams.chromaNr = chroma_nr_;
  decoderParams.ntscPhaseComp = ntsc_phase_comp_;
  decoderParams.simplePal = simple_pal_;
  decoderParams.transformThreshold = transform_threshold_;
  decoderParams.chromaWeight = chroma_weight_;
  decoderParams.adaptThreshold = adapt_threshold_;

  std::unique_ptr<Decoder> decoder =
      make_decoder(decoder_type_, decoderParams, videoParams);
  if (!decoder) {
    ORC_LOG_ERROR("VideoSink: Unknown decoder type: {}", decoder_type_);
    trigger_status_ = "Error: Unknown decoder type";
    trigger_in_progress_.store(false);
    return false;
  }
  ORC_LOG_DEBUG("VideoSink: Using decoder: {}", decoder_type_);

  // 5. Determine frame range to process.
  // Use the frame_range from VFrameR (may be filtered by upstream stages like
  // field_map). FrameIDRange is inclusive on both ends [first, last].
  orc::FrameIDRange frame_range = vfr->frame_range();
  size_t total_source_frames = vfr->frame_count();

  // A piped/live source left at its default unbounded frame_count (0)
  // reports a huge placeholder here, not its real length (which is not
  // known until it ends). The batch path below pre-builds a full frame
  // list and a worker pool sized to the whole declared range before
  // decoding a single frame — meaningless (and, taken literally, an attempt
  // to reserve billions of entries) when that range is a placeholder — so
  // this is routed to run_streaming_export() instead, once the shared setup
  // below (backend) is ready. That function runs its own, differently-sized
  // worker pool — see its own comment for why.
  const bool unbounded_source = vfr->has_unbounded_frame_range();
  if (unbounded_source) {
    ORC_LOG_INFO(
        "VideoSink: input reports an unbounded frame range (a piped/live "
        "source left frame_count at 0) — streaming export: frames are "
        "decoded and written one at a time as they arrive, stopping once "
        "the source reaches a real end-of-stream.");
  }

  // Convert inclusive [first, last] to exclusive end [start, end).
  size_t start_frame = frame_range.first;
  size_t end_frame = frame_range.last + 1;  // exclusive

  ORC_LOG_DEBUG(
      "VideoSink: Processing frames {} to {} (of {} in source, frame range "
      "{}-{})",
      start_frame + 1, end_frame, total_source_frames, frame_range.first,
      frame_range.last);

  // 6. Field ordering and interlacing structure
  // In interlaced video, each frame consists of two fields captured
  // sequentially. Fields are stored in chronological order: 0, 1, 2, 3, 4, 5...
  //
  // Field parity is assigned based on field index:
  //   - Even field indices (0, 2, 4...) → FieldParity::Top    → first field
  //   - Odd field indices (1, 3, 5...)  → FieldParity::Bottom → second field
  //
  // This relationship is consistent across both NTSC and PAL systems.
  // Frame N (1-based) consists of fields (2*N-2, 2*N-1) in 0-based indexing.

  // 6. Determine decoder lookbehind/lookahead requirements from the decoder
  // itself.  For Y/C sources the wrapper decodes luma and chroma over the same
  // range internally, so a single look-around covers both.
  int32_t lookBehindFrames = decoder->getLookBehind();
  int32_t lookAheadFrames = decoder->getLookAhead();

  ORC_LOG_DEBUG(
      "VideoSink: Decoder requires lookBehind={} frames, lookAhead={} frames",
      lookBehindFrames, lookAheadFrames);

  // 7. Calculate extended frame range including lookbehind/lookahead
  // Note: extended_start_frame can be negative (will use black padding)
  int32_t extended_start_frame =
      static_cast<int32_t>(start_frame) - lookBehindFrames;
  int32_t extended_end_frame =
      static_cast<int32_t>(end_frame) + lookAheadFrames;

  // 8. Build the frame info list — one entry per frame including look-around.
  // Frame data is loaded on-demand in worker threads.
  struct FrameInfo {
    orc::FrameID frame_id;
    bool use_blank;
  };

  // Declared unconditionally (stays empty for the streaming/unbounded path,
  // which never reads it) so the bounded path's later use of it below needs
  // no other change. Only actually populated when NOT unbounded: extended_end
  // /extended_start_frame come from end_frame/start_frame, which are
  // themselves derived from frame_range() — a huge placeholder, not a real
  // count, for an unbounded source — so reserving space for it here would
  // hit exactly the crash this whole streaming path exists to avoid.
  std::vector<FrameInfo> frameInfoList;
  if (!unbounded_source) {
    frameInfoList.reserve(
        static_cast<size_t>(extended_end_frame - extended_start_frame));

    ORC_LOG_DEBUG(
        "VideoSink: Preparing {} frame descriptors (frames {}-{}) for decode",
        extended_end_frame - extended_start_frame, extended_start_frame + 1,
        extended_end_frame);

    for (int32_t frame = extended_start_frame; frame < extended_end_frame;
         frame++) {
      bool useBlankFrame =
          (frame < 0) ||
          (static_cast<orc::FrameID>(frame) < frame_range.first) ||
          (static_cast<orc::FrameID>(frame) > frame_range.last);

      orc::FrameID fid = useBlankFrame ? 0 : static_cast<orc::FrameID>(frame);
      if (!useBlankFrame && !vfr->has_frame(fid)) {
        ORC_LOG_WARN("VideoSink: Skipping frame {} (not present in VFrameR)",
                     frame + 1);
        useBlankFrame = true;
      }
      frameInfoList.push_back({fid, useBlankFrame});
    }
  }

  // 10. Process frames in parallel using worker threads
  // CRITICAL: Transform3D is a 3D temporal FFT filter that processes frames at
  // specific Z-positions (temporal indices). Each frame MUST be at the SAME
  // Z-position (field indices lookBehind*2 to lookBehind*2+2) regardless of its
  // frame number, otherwise the FFT results will differ. Workers process frames
  // independently with proper context.
  //
  // THREAD SAFETY: Each worker thread creates its own decoder instance to avoid
  // state conflicts. Transform PAL decoders use FFT buffers that cannot be
  // shared between threads.

  // Calculate how many frames to OUTPUT
  // frame_range represents the actual frames to output (already filtered by
  // upstream stages). Lookahead is only for decoder context (extended_range
  // handles that).
  // Meaningless (and, cast to int32_t, liable to overflow) for an unbounded
  // source, whose end_frame/start_frame come from frame_range()'s huge
  // placeholder rather than a real count — never read below when
  // unbounded_source, since run_streaming_export() takes over before either
  // variable's only other uses (outputFrames.resize(), numThreads capping).
  int32_t numOutputFrames = static_cast<int32_t>(end_frame - start_frame);
  int32_t numFrames = numOutputFrames;

  if (!unbounded_source) {
    ORC_LOG_DEBUG("VideoSink: Will output {} frames from frame range {}-{}",
                  numOutputFrames, frame_range.first, frame_range.last);
  }

  // Initialize output backend BEFORE decoding to enable streaming writes
  auto backend = OutputBackendFactory::create(output_format_);
  if (!backend) {
    ORC_LOG_ERROR("VideoSink: Unknown or unsupported output format: {}",
                  output_format_);
    trigger_status_ = "Error: Unknown format '" + output_format_ + "'";
    trigger_in_progress_.store(false);
    return false;
  }

  // Audio/closed-caption/chapter/disc-metadata embedding all need a full
  // pass over the whole capture's field range up front (the block below
  // computes start_field_index/num_fields from it, and the backend expects
  // that range to be real) — meaningless for an unbounded source, whose
  // frame_range() is a placeholder rather than its real length. Local
  // "effective" copies keep that restriction scoped to this trigger rather
  // than mutating the stage's own persisted parameters.
  const bool effective_embed_audio = embed_audio_ && !unbounded_source;
  const bool effective_embed_cc = embed_closed_captions_ && !unbounded_source;
  const bool effective_embed_chapters =
      embed_chapter_metadata_ && !unbounded_source;
  const bool effective_embed_disc = embed_disc_metadata_ && !unbounded_source;
  if (unbounded_source && (embed_audio_ || embed_closed_captions_ ||
                           embed_chapter_metadata_ || embed_disc_metadata_)) {
    ORC_LOG_WARN(
        "VideoSink: input reports an unbounded frame range — audio/closed "
        "caption/chapter/disc-metadata embedding all need a full pass over "
        "the whole capture ahead of time, which an unbounded source cannot "
        "provide; disabling them for this export (plain video only).");
  }

  OutputBackend::Configuration backendConfig;
  backendConfig.output_path = output_path_;
  backendConfig.video_params = videoParams;
  backendConfig.padding_amount = output_padding_;
  backendConfig.options["format"] = output_format_;
  backendConfig.encoder_preset = encoder_preset_;
  backendConfig.encoder_crf = encoder_crf_;
  backendConfig.encoder_bitrate = encoder_bitrate_;
  backendConfig.embed_audio = effective_embed_audio;
  backendConfig.embed_closed_captions = effective_embed_cc;
  backendConfig.embed_chapter_metadata = effective_embed_chapters;
  backendConfig.options["hardware_encoder"] = hardware_encoder_;
  backendConfig.options["prores_profile"] = prores_profile_;
  backendConfig.options["use_lossless_mode"] =
      use_lossless_mode_ ? "true" : "false";
  backendConfig.options["apply_deinterlace"] =
      apply_deinterlace_ ? "true" : "false";
  backendConfig.options["display_aspect_ratio"] = display_aspect_ratio_;
  backendConfig.options["video_filter"] = video_filter_;
  backendConfig.options["bt601_bit_depth"] = bt601_bit_depth_;
  backendConfig.options["ffv1_slices"] = ffv1_slices_;
  backendConfig.options["rawvideo_format"] = rawvideo_format_;
  backendConfig.options["ffmpeg_format_explicit"] =
      ffmpeg_format_explicit_ ? "true" : "false";
  backendConfig.embed_disc_metadata =
      effective_embed_disc && (output_mode_ == "ffmpeg");
  backendConfig.disc_metadata_detail = disc_metadata_detail_;
  backendConfig.options["audio_gain_db"] = std::to_string(audio_gain_db_);
  backendConfig.options["audio_channel_pairs"] = audio_channel_pairs_;
  backendConfig.observation_context = &observation_context;

  // The backend reads audio samples straight from the representation, so hand
  // it over whenever one is available. It is not gated on any embed option —
  // the backend decides what to use it for.
  backendConfig.vfr = vfr.get();

  // Set field-equivalent range for audio, closed caption, chapter and/or disc
  // metadata extraction. The ffmpeg backend uses field-based indexing
  // internally; convert frame range to field units (1 frame = 2 fields).
  if ((effective_embed_audio && vfr && vfr->has_audio()) ||
      effective_embed_cc || effective_embed_chapters || effective_embed_disc) {
    backendConfig.start_field_index = frame_range.first * 2;
    backendConfig.num_fields = (frame_range.last - frame_range.first + 1) * 2;

    if (effective_embed_audio && vfr && vfr->has_audio()) {
      ORC_LOG_DEBUG(
          "VideoSink: Audio embedding enabled (frames {} to {} = {} frames, "
          "{} field-equiv)",
          frame_range.first, frame_range.last, numOutputFrames,
          backendConfig.num_fields);
    }

    if (effective_embed_cc) {
      ORC_LOG_DEBUG(
          "VideoSink: Closed caption embedding enabled (frames {} to {} = "
          "{} frames)",
          frame_range.first, frame_range.last, numOutputFrames);
    }

    if (effective_embed_chapters) {
      ORC_LOG_DEBUG(
          "VideoSink: Chapter metadata embedding enabled (frames {} to {} = "
          "{} frames)",
          frame_range.first, frame_range.last, numOutputFrames);
    }
  }

  if (!backend->initialize(backendConfig)) {
    ORC_LOG_ERROR("VideoSink: Failed to initialize {} output backend",
                  output_format_);
    trigger_status_ =
        "Error: Failed to initialize " + output_format_ + " output";
    // Surface user-actionable backend detail (e.g. an invalid video filter
    // string) in the trigger status shown by the GUI.
    const std::string backend_error = backend->getLastError();
    if (!backend_error.empty()) {
      trigger_status_ += ": " + backend_error;
    }
    trigger_in_progress_.store(false);
    return false;
  }

  if (unbounded_source) {
    const bool streaming_ok = run_streaming_export(
        vfr, videoParams, *backend, static_cast<orc::FrameID>(start_frame),
        isPal, is_yc_source, lookBehindFrames, lookAheadFrames);
    if (!streaming_ok) {
      backend->finalize();  // best-effort close, mirrors the batch path
      trigger_in_progress_.store(false);
      return false;
    }
    if (!backend->finalize()) {
      ORC_LOG_ERROR("VideoSink: Failed to finalize output");
      trigger_status_ = "Error: Failed to finalize output file";
      trigger_in_progress_.store(false);
      return false;
    }
    trigger_in_progress_.store(false);
    return true;
  }

  ORC_LOG_DEBUG("VideoSink: Streaming {} frames to {}", numOutputFrames,
                backend->getFormatInfo());

  // Use vector of optional frames to track which have been written
  // This allows out-of-order completion while maintaining sequential writes
  std::vector<std::optional<::ComponentFrame>> outputFrames;
  outputFrames.resize(numOutputFrames);

  // Determine number of threads to use
  int32_t numThreads = threads_;
  if (numThreads <= 0) {
    numThreads = static_cast<int32_t>(std::thread::hardware_concurrency());
    if (numThreads <= 0) numThreads = 4;  // Fallback
  }
  // Don't use more threads than frames
  numThreads = std::min(numThreads, numFrames);

  ORC_LOG_DEBUG("VideoSink: Processing {} frames using {} worker threads",
                numFrames, numThreads);

  // Start timing for performance measurement
  auto decode_start_time = std::chrono::high_resolution_clock::now();

  // Report initial progress
  if (progress_callback_) {
    progress_callback_(0, numFrames, "Starting decoding...");
  }

  // Shared state for work distribution and output writing
  std::atomic<int32_t> nextFrameIdx{0};
  std::atomic<bool> abortFlag{false};
  std::atomic<int32_t> completedFrames{0};
  std::atomic<int32_t> nextFrameToWrite{0};
  std::mutex outputMutex;  // Protects backend writes and frame buffering

  // CRITICAL: FFTW plan creation with FFTW_MEASURE is NOT thread-safe
  // (see FFTW docs: http://www.fftw.org/fftw3_doc/Thread-safety.html)
  // We must serialize all decoder instantiations that create FFTW plans
  std::mutex fftwPlanMutex;

  // Worker thread function - each worker creates its own decoder instance.
  auto workerFunc = [&]() {
    // Transform PAL builds FFTW plans in the factory (FFTW_MEASURE is not
    // thread-safe), so serialise construction across workers.
    std::unique_ptr<Decoder> threadDecoder;
    {
      std::lock_guard<std::mutex> lock(fftwPlanMutex);
      threadDecoder = make_decoder(decoder_type_, decoderParams, videoParams);
    }

    while (!abortFlag) {
      // Check for cancellation
      if (cancel_requested_.load()) {
        abortFlag.store(true);
        break;
      }

      // Get next frame to process
      int32_t frameIdx = nextFrameIdx.fetch_add(1);
      if (frameIdx >= numFrames) {
        break;  // No more frames to process
      }

      // Build a field array for this ONE frame by loading data on-demand.
      // [lookbehind fields... target frame fields... lookahead fields...]
      // frameInfoList has one entry per frame; we expand each to 2
      // SourceFields.
      std::vector<SourceField> frameFields;

      // Owned buffers backing every SourceField (blank padding and copies of
      // VFrameR frame data); must outlive frameFields. Use a deque so that
      // push_back never invalidates existing data() pointers.
      std::deque<std::vector<int16_t>> ownedFieldBuffers;

      // The actual frame number we're processing
      int32_t actualFrameNum = static_cast<int32_t>(start_frame) + frameIdx;

      // Position in frameInfoList where this frame's entry is
      int32_t frameStartIdx = (actualFrameNum - extended_start_frame);

      // Calculate the range to load: lookbehind + target + lookahead (in
      // frames)
      int32_t copyStartIdx = frameStartIdx - lookBehindFrames;
      int32_t copyEndIdx = frameStartIdx + 1 + lookAheadFrames;

      // Clamp to valid range
      copyStartIdx = std::max(0, copyStartIdx);
      copyEndIdx =
          std::min(static_cast<int32_t>(frameInfoList.size()), copyEndIdx);

      // Blanking level for black fields (CVBS 10-bit domain)
      const int16_t blankingLevel =
          isPal ? static_cast<int16_t>(orc::kPalBlanking)
                : static_cast<int16_t>(orc::kNtscBlanking);

      // Helper: compute sample count for field 1 or 2 given video params
      auto fieldSampleCount = [&](bool is_first) -> size_t {
        if (isPal) {
          // EBU Tech. 3280-E §1.3.1: field 1 = 312×1135 + 1×1137 = 355,257,
          // field 2 = 311×1135 + 1×1137 = 354,122
          return is_first ? (312 * 1135 + 1 * 1137) : (311 * 1135 + 1 * 1137);
        }
        // NTSC / PAL_M: uniform sampling
        size_t spl = (videoParams.system == orc::VideoSystem::PAL_M)
                         ? static_cast<size_t>(orc::kPalMSamplesPerLine)
                         : static_cast<size_t>(orc::kNtscSamplesPerLine);
        size_t field_lines = is_first
                                 ? static_cast<size_t>(orc::kNtscField1Lines)
                                 : static_cast<size_t>(orc::kNtscFrameLines -
                                                       orc::kNtscField1Lines);
        return spl * field_lines;
      };

      // Helper: build a blank SourceField backed by an entry in
      // ownedFieldBuffers
      auto makeBlankField = [&](bool is_first_field) -> SourceField {
        size_t samples = fieldSampleCount(is_first_field);
        ownedFieldBuffers.emplace_back(samples, blankingLevel);
        const int16_t* buf = ownedFieldBuffers.back().data();

        SourceField sf;
        sf.is_first_field = is_first_field;
        sf.data = buf;
        sf.is_yc = is_yc_source;
        if (is_yc_source) {
          sf.luma_data = buf;
          sf.chroma_data = buf;
        }

        if (isPal) {
          sf.line_count = is_first_field
                              ? static_cast<size_t>(orc::kPalField1Lines)
                              : static_cast<size_t>(orc::kPalFrameLines -
                                                    orc::kPalField1Lines);
          sf.samples_per_line = 1135;
          sf.line_ptrs.reserve(sf.line_count);
          const size_t blank_field_base =
              is_first_field ? 0 : static_cast<size_t>(orc::kPalField1Lines);
          size_t offset = 0;
          for (size_t ln = 0; ln < sf.line_count; ++ln) {
            sf.line_ptrs.push_back(buf + offset);
            offset += frame_line_sample_count(
                orc::VideoSystem::PAL,
                static_cast<size_t>(orc::kPalSamplesPerLineNominal),
                blank_field_base + ln);
          }
          if (is_yc_source) {
            sf.luma_line_ptrs = sf.line_ptrs;
            sf.chroma_line_ptrs = sf.line_ptrs;
          }
        } else {
          size_t spl = (videoParams.system == orc::VideoSystem::PAL_M)
                           ? static_cast<size_t>(orc::kPalMSamplesPerLine)
                           : static_cast<size_t>(orc::kNtscSamplesPerLine);
          sf.line_count = is_first_field
                              ? static_cast<size_t>(orc::kNtscField1Lines)
                              : static_cast<size_t>(orc::kNtscFrameLines -
                                                    orc::kNtscField1Lines);
          sf.samples_per_line = spl;
          // NTSC: uniform lines, no line_ptrs needed
        }
        return sf;
      };

      for (int32_t i = copyStartIdx; i < copyEndIdx; i++) {
        const auto& fi = frameInfoList[i];

        if (fi.use_blank ||
            !appendSourceFields(vfr.get(), fi.frame_id, videoParams,
                                ownedFieldBuffers, frameFields)) {
          // Blank padding, or the frame unexpectedly had no data; substitute
          // black fields so the window keeps two fields per frame and the
          // target frame stays at the expected Z-position.
          frameFields.push_back(makeBlankField(true));
          frameFields.push_back(makeBlankField(false));
        }
      }

      // The target frame's position within frameFields (in field units)
      int32_t actualLookbehindFields = (frameStartIdx - copyStartIdx) * 2;

      // CRITICAL: For Transform3D temporal consistency, all frames must be
      // decoded at the SAME Z-position (temporal index) regardless of their
      // frame number. Always decode at lookBehindFrames * 2 field indices.
      // If we don't have full lookbehind (edge frames), pad with black fields.
      std::vector<SourceField> paddedFrameFields;
      int32_t requiredLookbehindFields = lookBehindFrames * 2;

      if (actualLookbehindFields < requiredLookbehindFields) {
        int32_t paddingNeeded =
            requiredLookbehindFields - actualLookbehindFields;

        // Create black fields for padding (always in field pairs)
        for (int32_t p = 0; p < paddingNeeded; p++) {
          paddedFrameFields.push_back(makeBlankField(p % 2 == 0));
        }

        for (const auto& field : frameFields) {
          paddedFrameFields.push_back(field);
        }

        frameFields = std::move(paddedFrameFields);
      }

      // Now all frames decode at the same Z-position: after lookBehindFrames *
      // 2 fields
      int32_t frameStartIndex = requiredLookbehindFields;
      int32_t frameEndIndex = frameStartIndex + 2;

      // Prepare single-frame output buffer
      std::vector<::ComponentFrame> singleOutput;
      singleOutput.resize(1);

      // Decode this ONE frame using the thread-local decoder.  Y/C splitting
      // and luma merge happen inside the decoder for Y/C sources.
      threadDecoder->decodeFrames(frameFields, frameStartIndex, frameEndIndex,
                                  singleOutput);

      // Store the result in the buffer
      {
        std::lock_guard<std::mutex> lock(outputMutex);
        outputFrames[frameIdx] = singleOutput[0];

        // Write completed frames to backend in sequential order
        while (nextFrameToWrite < numFrames &&
               outputFrames[nextFrameToWrite].has_value()) {
          if (!backend->writeFrame(*outputFrames[nextFrameToWrite])) {
            int32_t failedFrame = nextFrameToWrite.load();
            ORC_LOG_ERROR("VideoSink: Failed to write frame {}", failedFrame);
            abortFlag.store(true);
            break;
          }
          // Free the frame memory immediately after writing
          outputFrames[nextFrameToWrite].reset();
          nextFrameToWrite++;
        }
      }

      // Update progress
      int32_t completed = completedFrames.fetch_add(1) + 1;
      if (progress_callback_ &&
          (completed % 10 == 0 || completed == numFrames)) {
        progress_callback_(completed, numFrames,
                           "Decoding frames: " + std::to_string(completed) +
                               "/" + std::to_string(numFrames));
      }
    }
  };

  // Create and start worker threads
  std::vector<std::thread> workers;
  workers.reserve(numThreads);
  for (int32_t i = 0; i < numThreads; i++) {
    workers.emplace_back(workerFunc);
  }

  // Wait for all workers to finish
  for (auto& worker : workers) {
    worker.join();
  }

  // Check if cancelled or error
  if (cancel_requested_.load() || abortFlag.load()) {
    ORC_LOG_WARN("VideoSink: Decoding cancelled or failed");
    backend->finalize();  // Try to close cleanly
    trigger_status_ =
        cancel_requested_.load() ? "Cancelled by user" : "Error during decode";
    trigger_in_progress_.store(false);
    return false;
  }

  // Finalize output backend
  if (progress_callback_) {
    progress_callback_(numFrames, numFrames, "Finalizing output file...");
  }

  if (!backend->finalize()) {
    ORC_LOG_ERROR("VideoSink: Failed to finalize output");
    trigger_status_ = "Error: Failed to finalize output file";
    trigger_in_progress_.store(false);
    return false;
  }

  // Calculate performance metrics
  auto decode_end_time = std::chrono::high_resolution_clock::now();
  auto decode_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      decode_end_time - decode_start_time);
  double decode_seconds = static_cast<double>(decode_duration.count()) / 1000.0;
  double fps = numFrames / decode_seconds;
  int32_t total_fields = numFrames * 2;
  [[maybe_unused]] double fields_per_second = total_fields / decode_seconds;

  ORC_LOG_INFO("VideoSink: Successfully wrote {} frames to: {}", numFrames,
               output_path_);
  ORC_LOG_DEBUG(
      "VideoSink: Performance - {:.2f} seconds, {:.2f} fps, {:.2f} fields/sec",
      decode_seconds, fps, fields_per_second);

  trigger_status_ = "Decode complete: " + std::to_string(numFrames) +
                    " frames (" +
                    std::to_string(static_cast<int>(fps * 10) / 10.0) + " fps)";
  trigger_in_progress_.store(false);

  if (progress_callback_) {
    progress_callback_(numFrames, numFrames, trigger_status_);
  }

  return true;
}

bool VideoSinkStage::run_streaming_export(
    const std::shared_ptr<orc::VideoFrameRepresentation>& vfr,
    const orc::SourceParameters& videoParams, OutputBackend& backend,
    orc::FrameID start_frame_id, bool isPal, bool is_yc_source,
    int32_t lookBehindFrames, int32_t lookAheadFrames) {
  const int16_t blankingLevel = isPal
                                    ? static_cast<int16_t>(orc::kPalBlanking)
                                    : static_cast<int16_t>(orc::kNtscBlanking);

  auto fieldSampleCount = [&](bool is_first) -> size_t {
    if (isPal) {
      // EBU Tech. 3280-E §1.3.1: field 1 = 312×1135 + 1×1137 = 355,257,
      // field 2 = 311×1135 + 1×1137 = 354,122.
      return is_first ? (312 * 1135 + 1 * 1137) : (311 * 1135 + 1 * 1137);
    }
    size_t spl = (videoParams.system == orc::VideoSystem::PAL_M)
                     ? static_cast<size_t>(orc::kPalMSamplesPerLine)
                     : static_cast<size_t>(orc::kNtscSamplesPerLine);
    size_t field_lines =
        is_first
            ? static_cast<size_t>(orc::kNtscField1Lines)
            : static_cast<size_t>(orc::kNtscFrameLines - orc::kNtscField1Lines);
    return spl * field_lines;
  };

  // Pure function of its arguments (all captures are read-only), so — unlike
  // the shared sliding window an earlier, single-threaded version of this
  // function used — every worker below can call it concurrently on its own
  // local buffers with no synchronisation.
  auto makeBlankField =
      [&](bool is_first_field,
          std::deque<std::vector<int16_t>>& owned) -> SourceField {
    size_t samples = fieldSampleCount(is_first_field);
    owned.emplace_back(samples, blankingLevel);
    const int16_t* buf = owned.back().data();

    SourceField sf;
    sf.is_first_field = is_first_field;
    sf.data = buf;
    sf.is_yc = is_yc_source;
    if (is_yc_source) {
      sf.luma_data = buf;
      sf.chroma_data = buf;
    }

    if (isPal) {
      sf.line_count =
          is_first_field
              ? static_cast<size_t>(orc::kPalField1Lines)
              : static_cast<size_t>(orc::kPalFrameLines - orc::kPalField1Lines);
      sf.samples_per_line = 1135;
      sf.line_ptrs.reserve(sf.line_count);
      const size_t blank_field_base =
          is_first_field ? 0 : static_cast<size_t>(orc::kPalField1Lines);
      size_t offset = 0;
      for (size_t ln = 0; ln < sf.line_count; ++ln) {
        sf.line_ptrs.push_back(buf + offset);
        offset += frame_line_sample_count(
            orc::VideoSystem::PAL,
            static_cast<size_t>(orc::kPalSamplesPerLineNominal),
            blank_field_base + ln);
      }
      if (is_yc_source) {
        sf.luma_line_ptrs = sf.line_ptrs;
        sf.chroma_line_ptrs = sf.line_ptrs;
      }
    } else {
      size_t spl = (videoParams.system == orc::VideoSystem::PAL_M)
                       ? static_cast<size_t>(orc::kPalMSamplesPerLine)
                       : static_cast<size_t>(orc::kNtscSamplesPerLine);
      sf.line_count = is_first_field
                          ? static_cast<size_t>(orc::kNtscField1Lines)
                          : static_cast<size_t>(orc::kNtscFrameLines -
                                                orc::kNtscField1Lines);
      sf.samples_per_line = spl;
    }
    return sf;
  };

  DecoderParams decoderParams;
  decoderParams.chromaGain = chroma_gain_;
  decoderParams.chromaPhase = chroma_phase_;
  decoderParams.lumaNr = luma_nr_;
  decoderParams.chromaNr = chroma_nr_;
  decoderParams.ntscPhaseComp = ntsc_phase_comp_;
  decoderParams.simplePal = simple_pal_;
  decoderParams.transformThreshold = transform_threshold_;
  decoderParams.chromaWeight = chroma_weight_;
  decoderParams.adaptThreshold = adapt_threshold_;

  // Bound the worker pool so no more than the source's own read-ahead
  // window (max_concurrent_frame_requests() — buffer_frames on the stream
  // source) worth of frames are ever simultaneously requested. Up to
  // pool_size workers can be decoding pool_size roughly-consecutive frames
  // at once, each needing lookBehindFrames..lookAheadFrames of its own
  // neighbours, so the total span requested at once is at most
  // pool_size + lookBehindFrames + lookAheadFrames. Beyond the window,
  // ThrottledRingReader doesn't corrupt anything — it throttles production
  // to the slowest outstanding request — so a larger pool would just stall
  // without decoding any faster; there is no correctness reason for the
  // cap, only a throughput one.
  const size_t window_cap = vfr->max_concurrent_frame_requests();
  const int32_t margin = lookBehindFrames + lookAheadFrames;
  int32_t max_pool = std::numeric_limits<int32_t>::max();
  if (window_cap < static_cast<size_t>(max_pool)) {
    max_pool = std::max<int32_t>(1, static_cast<int32_t>(window_cap) - margin);
  }
  int32_t requested_threads = threads_;
  if (requested_threads <= 0) {
    requested_threads =
        static_cast<int32_t>(std::thread::hardware_concurrency());
    if (requested_threads <= 0) requested_threads = 4;
  }
  const int32_t pool_size = std::max(1, std::min(requested_threads, max_pool));

  ORC_LOG_DEBUG(
      "VideoSink: Streaming export using {} worker thread(s) (requested {}, "
      "capped by the source's read-ahead window of {})",
      pool_size, requested_threads, window_cap);

  std::atomic<int64_t> next_claim{static_cast<int64_t>(start_frame_id)};
  // -1 = the real end hasn't been discovered yet; once set, it never
  // decreases below its first (lowest, most accurate) value — see
  // record_known_end()'s compare-exchange loop.
  std::atomic<int64_t> known_end{-1};
  std::atomic<bool> abort_flag{false};
  std::mutex fftw_mutex;    // serialises FFTW plan creation across decoders
  std::mutex output_mutex;  // protects pending_output/next_to_write/backend
  std::map<int64_t, ::ComponentFrame> pending_output;
  int64_t next_to_write = static_cast<int64_t>(start_frame_id);
  int64_t frames_written = 0;
  std::string write_error;

  const auto decode_start_time = std::chrono::high_resolution_clock::now();
  if (progress_callback_) {
    progress_callback_(0, 0, "Streaming: waiting for the first frame...");
  }

  auto claim_is_live = [&](int64_t claim) {
    const int64_t end = known_end.load();
    return end < 0 || claim <= end;
  };
  auto record_known_end = [&](int64_t candidate) {
    int64_t cur = known_end.load();
    while ((cur < 0 || candidate < cur) &&
           !known_end.compare_exchange_weak(cur, candidate)) {
    }
  };

  auto workerFunc = [&]() {
    std::unique_ptr<Decoder> local_decoder;
    {
      std::lock_guard<std::mutex> lock(fftw_mutex);
      local_decoder = make_decoder(decoder_type_, decoderParams, videoParams);
    }
    if (!local_decoder) {
      abort_flag.store(true);
      return;
    }

    for (;;) {
      if (cancel_requested_.load() || abort_flag.load()) return;

      const int64_t claim = next_claim.fetch_add(1);
      if (!claim_is_live(claim)) return;

      // Build this claim's own field window independently — every
      // outstanding worker does the same for its own claim, with no shared
      // state beyond next_claim/known_end/pending_output, so no window to
      // synchronise. Neighbouring claims re-fetch overlapping frames
      // redundantly; the batch path above already accepts the same
      // trade-off per-thread for the same reason.
      std::vector<SourceField> flatFields;
      std::deque<std::vector<int16_t>> owned;
      bool claim_is_gone = false;

      for (int64_t f = claim - lookBehindFrames; f <= claim + lookAheadFrames;
           ++f) {
        if (f >= static_cast<int64_t>(start_frame_id)) {
          const orc::FrameID fid = static_cast<orc::FrameID>(f);
          if (appendSourceFields(vfr.get(), fid, videoParams, owned,
                                 flatFields)) {
            continue;
          }
          if (vfr->is_exhausted()) {
            if (f == claim) claim_is_gone = true;
            record_known_end(f - 1);
          }
        }
        flatFields.push_back(makeBlankField(true, owned));
        flatFields.push_back(makeBlankField(false, owned));
      }

      if (claim_is_gone || !claim_is_live(claim)) continue;

      std::vector<::ComponentFrame> singleOutput;
      singleOutput.resize(1);
      const int32_t frameStartIndex = lookBehindFrames * 2;
      local_decoder->decodeFrames(flatFields, frameStartIndex,
                                  frameStartIndex + 2, singleOutput);

      std::lock_guard<std::mutex> lock(output_mutex);
      pending_output[claim] = std::move(singleOutput[0]);
      while (pending_output.count(next_to_write)) {
        if (!backend.writeFrame(pending_output[next_to_write])) {
          write_error =
              "Failed to write streamed frame " + std::to_string(next_to_write);
          abort_flag.store(true);
          break;
        }
        pending_output.erase(next_to_write);
        ++next_to_write;
        ++frames_written;
        if (progress_callback_ && frames_written % 10 == 0) {
          progress_callback_(static_cast<size_t>(frames_written), 0,
                             "Streaming: " + std::to_string(frames_written) +
                                 " frames written...");
        }
      }
    }
  };

  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(pool_size));
  for (int32_t i = 0; i < pool_size; ++i) workers.emplace_back(workerFunc);
  for (auto& w : workers) w.join();

  if (cancel_requested_.load()) {
    ORC_LOG_WARN("VideoSink: Streaming export cancelled by user");
    trigger_status_ = "Cancelled by user";
    return false;
  }
  if (abort_flag.load()) {
    const std::string message =
        write_error.empty() ? "Streaming export failed" : write_error;
    ORC_LOG_ERROR("VideoSink: {}", message);
    trigger_status_ = message;
    return false;
  }
  // Exhausted on a read error, not a clean end: fail rather than report a
  // truncated output as a success.
  if (const std::string stream_error = vfr->stream_error();
      !stream_error.empty()) {
    trigger_status_ = "Input stream failed: " + stream_error;
    ORC_LOG_ERROR("VideoSink: {}", trigger_status_);
    return false;
  }

  const auto decode_end_time = std::chrono::high_resolution_clock::now();
  const double decode_seconds =
      std::chrono::duration<double>(decode_end_time - decode_start_time)
          .count();
  const double fps = decode_seconds > 0.0
                         ? static_cast<double>(frames_written) / decode_seconds
                         : 0.0;

  ORC_LOG_INFO("VideoSink: Streaming export wrote {} frames to: {}",
               frames_written, output_path_);
  trigger_status_ =
      "Streaming export complete: " + std::to_string(frames_written) +
      " frames (" + std::to_string(static_cast<int>(fps * 10) / 10.0) + " fps)";
  if (progress_callback_) {
    progress_callback_(static_cast<size_t>(frames_written),
                       static_cast<size_t>(frames_written), trigger_status_);
  }
  return true;
}

std::string VideoSinkStage::get_trigger_status() const {
  return trigger_status_;
}

orc::observation::FramePhase VideoSinkStage::observer_frame_phase(
    const orc::VideoFrameRepresentation& vfr, orc::FrameID frame_id) const {
  const int32_t key = static_cast<int32_t>(frame_id);
  {
    std::lock_guard<std::mutex> lock(colour_phase_mutex_);
    auto it = colour_phase_cache_.find(key);
    if (it != colour_phase_cache_.end()) return it->second;
  }

  // Measure from the burst signal via the host observer (see
  // colour_frame_phase_query.h). measure_frame_phase() is thread-safe, so the
  // render path's worker threads can measure concurrently.
  const orc::observation::FramePhase phase =
      orc::observation::measure_frame_phase(vfr, frame_id);

  std::lock_guard<std::mutex> lock(colour_phase_mutex_);
  colour_phase_cache_[key] = phase;
  return phase;
}

// Build a non-owning SourceField view into the VFrameR flat frame buffer.
// The VFrameR frame buffer layout is:
//   [field1_line0 | field1_line1 | … | field2_line0 | …]
// For PAL, frame-flat lines 312 and 624 carry 1137 samples (all other lines
// carry 1135 samples).  EBU Tech. 3280-E §1.3.1.
bool VideoSinkStage::appendSourceFields(
    const orc::VideoFrameRepresentation* vfr, orc::FrameID frame_id,
    const orc::SourceParameters& videoParams,
    std::deque<std::vector<int16_t>>& owned_buffers,
    std::vector<SourceField>& out_fields) const {
  // SDK contract (video_frame_representation.h): pointers returned by
  // get_frame()/get_frame_luma()/get_frame_chroma() are only valid until the
  // next call on the representation. Decoders with temporal look-around
  // (Transform 3D, NTSC 3D) hold fields from several frames at once while
  // worker threads pull neighbouring frames through the same upstream caches,
  // so the decoder input must view sink-owned copies.
  std::vector<int16_t> composite = vfr->get_frame_copy(frame_id);
  if (composite.empty()) {
    ORC_LOG_WARN("VideoSink: Frame {} has no data in VFrameR", frame_id);
    return false;
  }
  const size_t frame_samples = composite.size();
  owned_buffers.push_back(std::move(composite));
  const int16_t* frame_ptr = owned_buffers.back().data();

  const bool is_yc = vfr->has_separate_channels();
  const int16_t* luma_ptr = nullptr;
  const int16_t* chroma_ptr = nullptr;
  if (is_yc) {
    // Luma and chroma planes share the composite frame buffer layout.
    const int16_t* src_luma = vfr->get_frame_luma(frame_id);
    if (src_luma) {
      owned_buffers.emplace_back(src_luma, src_luma + frame_samples);
      luma_ptr = owned_buffers.back().data();
    }
    const int16_t* src_chroma = vfr->get_frame_chroma(frame_id);
    if (src_chroma) {
      owned_buffers.emplace_back(src_chroma, src_chroma + frame_samples);
      chroma_ptr = owned_buffers.back().data();
    }
    if (!luma_ptr || !chroma_ptr) {
      luma_ptr = nullptr;
      chroma_ptr = nullptr;
    }
  }

  // Colour-sequence phase is measured from the burst signal by the
  // "colour_frame_phase" observer rather than read from source-side metadata,
  // so it is available uniformly for TBC and CVBS sources (PAL / NTSC / PAL_M).
  // Each field gets its own phase id: the two fields of an NTSC frame sit at
  // opposite subcarrier phase, so a frame-level colour_frame_index cannot
  // tell the NTSC comb which lines carry positive burst phase.
  const orc::observation::FramePhase phase =
      observer_frame_phase(*vfr, frame_id);
  auto known = [](int32_t id) -> std::optional<int32_t> {
    if (id < 0) return std::nullopt;
    return id;
  };
  ORC_LOG_TRACE("VideoSink: Frame {} observed field_phase_id={}/{}", frame_id,
                phase.field1_phase_id, phase.field2_phase_id);

  out_fields.push_back(buildSourceField(frame_ptr, luma_ptr, chroma_ptr, is_yc,
                                        known(phase.field1_phase_id), frame_id,
                                        true, videoParams));
  out_fields.push_back(buildSourceField(frame_ptr, luma_ptr, chroma_ptr, is_yc,
                                        known(phase.field2_phase_id), frame_id,
                                        false, videoParams));
  return true;
}

SourceField VideoSinkStage::buildSourceField(
    const int16_t* frame_ptr, const int16_t* luma_ptr,
    const int16_t* chroma_ptr, bool is_yc,
    std::optional<int32_t> field_phase_id, orc::FrameID frame_id,
    bool is_first_field, const orc::SourceParameters& videoParams) const {
  SourceField sf;

  sf.seq_no = static_cast<int32_t>(frame_id) + 1;
  sf.is_first_field = is_first_field;
  sf.field_phase_id = field_phase_id;

  // PAL_M has NTSC-like frame geometry (525 lines, 909 samples/line); only
  // pure PAL uses the 625-line non-uniform layout.
  const bool is_pal = (videoParams.system == orc::VideoSystem::PAL);

  // EBU Tech. 3280-E §1.3.1: frame-flat offset of the start of field 2.
  const size_t kPalField1Samples = frame_line_sample_offset(
      orc::VideoSystem::PAL,
      static_cast<size_t>(orc::kPalSamplesPerLineNominal),
      static_cast<size_t>(orc::kPalField1Lines));

  // Helper: build PAL per-line pointer table for non-uniform line lengths.
  // frame_base_line: 0 for field 1, kPalField1Lines for field 2.
  auto buildPalLinePtrs =
      [](const int16_t* base, size_t line_count,
         size_t frame_base_line) -> std::vector<const int16_t*> {
    std::vector<const int16_t*> ptrs;
    ptrs.reserve(line_count);
    size_t offset = 0;
    for (size_t ln = 0; ln < line_count; ++ln) {
      ptrs.push_back(base + offset);
      offset += frame_line_sample_count(
          orc::VideoSystem::PAL,
          static_cast<size_t>(orc::kPalSamplesPerLineNominal),
          frame_base_line + ln);
    }
    return ptrs;
  };

  if (is_pal) {
    const size_t field1_lines =
        static_cast<size_t>(orc::kPalField1Lines);  // 313
    const size_t field2_lines =
        static_cast<size_t>(orc::kPalFrameLines - orc::kPalField1Lines);  // 312

    sf.samples_per_line = 1135;  // nominal; lines 312 and 624 carry 1137

    if (is_first_field) {
      sf.data = frame_ptr;
      sf.line_count = field1_lines;
      sf.line_ptrs = buildPalLinePtrs(frame_ptr, field1_lines, 0);
    } else {
      sf.data = frame_ptr + kPalField1Samples;
      sf.line_count = field2_lines;
      sf.line_ptrs = buildPalLinePtrs(
          sf.data, field2_lines, static_cast<size_t>(orc::kPalField1Lines));
    }
  } else {
    // NTSC or PAL_M: uniform sampling, no line_ptrs needed
    const size_t spl = (videoParams.system == orc::VideoSystem::PAL_M)
                           ? static_cast<size_t>(orc::kPalMSamplesPerLine)
                           : static_cast<size_t>(orc::kNtscSamplesPerLine);
    const size_t field1_lines =
        static_cast<size_t>(orc::kNtscField1Lines);  // 263 (top spatial)
    const size_t field2_lines = static_cast<size_t>(
        orc::kNtscFrameLines - orc::kNtscField1Lines);  // 262 (bottom spatial)

    sf.samples_per_line = spl;
    if (is_first_field) {
      sf.data = frame_ptr;
      sf.line_count = field1_lines;
    } else {
      sf.data = frame_ptr + field1_lines * spl;
      sf.line_count = field2_lines;
    }
  }

  if (is_yc) {
    sf.is_yc = true;

    if (luma_ptr && chroma_ptr) {
      if (is_pal) {
        if (is_first_field) {
          sf.luma_data = luma_ptr;
          sf.chroma_data = chroma_ptr;
          sf.luma_line_ptrs = buildPalLinePtrs(luma_ptr, sf.line_count, 0);
          sf.chroma_line_ptrs = buildPalLinePtrs(chroma_ptr, sf.line_count, 0);
        } else {
          sf.luma_data = luma_ptr + kPalField1Samples;
          sf.chroma_data = chroma_ptr + kPalField1Samples;
          sf.luma_line_ptrs =
              buildPalLinePtrs(sf.luma_data, sf.line_count,
                               static_cast<size_t>(orc::kPalField1Lines));
          sf.chroma_line_ptrs =
              buildPalLinePtrs(sf.chroma_data, sf.line_count,
                               static_cast<size_t>(orc::kPalField1Lines));
        }
      } else {
        const size_t spl = sf.samples_per_line;
        const size_t field1_lines = static_cast<size_t>(orc::kNtscField1Lines);
        size_t field_offset = is_first_field ? 0 : field1_lines * spl;
        sf.luma_data = luma_ptr + field_offset;
        sf.chroma_data = chroma_ptr + field_offset;
      }
    }
  }

  ORC_LOG_TRACE(
      "VideoSink: Frame {} {} field: line_count={} samples_per_line={} "
      "is_yc={} line_ptrs={}",
      frame_id, is_first_field ? "field1" : "field2", sf.line_count,
      sf.samples_per_line, sf.is_yc, sf.line_ptrs.size());

  return sf;
}

bool VideoSinkStage::supports_streaming_execution() const {
  if (output_path_.empty()) return false;

  if (output_mode_ == "raw") {
    // rgb/yuv/y4m are a pure sequential byte stream — no seeking, no
    // pre-scan of anything, always safe to pipe.
    return true;
  }

  if (output_mode_ != "ffmpeg") return false;

#ifdef HAVE_FFMPEG
  // Chapter/disc-metadata/closed-caption embedding all gather their data
  // (VBI observations, or the EIA-608 decode) before the first frame is
  // written to the container — the same reason a seek-requiring container
  // can't be used: whatever gets written into the header has to already be
  // complete by then.
  if (embed_chapter_metadata_ || embed_disc_metadata_ ||
      embed_closed_captions_) {
    return false;
  }

  const size_t dash_pos = ffmpeg_format_.find('-');
  const std::string container = dash_pos == std::string::npos
                                    ? ffmpeg_format_
                                    : ffmpeg_format_.substr(0, dash_pos);
  if (FFmpegOutputBackend::is_container_pipe_safe(container)) {
    return true;
  }
  // A non-pipe-safe container isn't necessarily fatal: FFmpegOutputBackend::
  // initialize() only hard-refuses a non-seekable destination when
  // ffmpeg_format was explicitly requested (see its own comment there) —
  // otherwise (the constructor default landed here, not a deliberate
  // choice) it silently falls back to nut-ffv1, a pipe-safe format. Mirror
  // that exact branch so this pre-flight check and the runtime behaviour
  // never disagree.
  return !ffmpeg_format_explicit_;
#else
  return false;
#endif
}

StagePreviewCapability VideoSinkStage::get_preview_capability() const {
  StagePreviewCapability capability{};

  std::shared_ptr<const orc::VideoFrameRepresentation> local_input;
  {
    std::lock_guard<std::mutex> lock(cached_input_mutex_);
    local_input = cached_input_;
  }

  if (!local_input) {
    return capability;
  }

  auto video_params_opt = local_input->get_video_parameters();
  if (!video_params_opt) {
    return capability;
  }

  const auto& video_params = *video_params_opt;
  const bool is_pal = (video_params.system == VideoSystem::PAL ||
                       video_params.system == VideoSystem::PAL_M);

  capability.supported_data_types.push_back(is_pal ? VideoDataType::ColourPAL
                                                   : VideoDataType::ColourNTSC);
  capability.supported_data_types.push_back(
      local_input->has_separate_channels()
          ? (is_pal ? VideoDataType::YC_PAL : VideoDataType::YC_NTSC)
          : (is_pal ? VideoDataType::CompositePAL
                    : VideoDataType::CompositeNTSC));

  capability.navigation_extent.item_count = local_input->frame_count();
  capability.navigation_extent.granularity = 1;
  capability.navigation_extent.item_label = "frame";

  // Keep capability geometry aligned with legacy VFR preview sizing so
  // 4:3 mode remains consistent when switching between signal-domain and
  // colour-domain preview paths.
  capability.geometry.active_width = 702;
  capability.geometry.active_height = 576;
  if (video_params.active_video_start >= 0 &&
      video_params.active_video_end > video_params.active_video_start) {
    capability.geometry.active_width = static_cast<uint32_t>(
        video_params.active_video_end - video_params.active_video_start);
  }
  if (video_params.first_active_frame_line >= 0 &&
      video_params.last_active_frame_line >
          video_params.first_active_frame_line) {
    capability.geometry.active_height =
        static_cast<uint32_t>(video_params.last_active_frame_line -
                              video_params.first_active_frame_line);
  }

  if (capability.geometry.active_height == 0) {
    capability.geometry.active_height = is_pal ? 576u : 486u;
  }

  capability.geometry.display_aspect_ratio = 4.0 / 3.0;

  // Fixed per-system pixel aspect (see standard_dar_correction), matching
  // make_signal_preview_capability().  Deriving this from the active-area size
  // would force every output to display at 4:3 and so vertically squash/stretch
  // the picture whenever the active line range changed; a fixed factor lets the
  // active window re-frame (crop/extend) while pixels keep their shape.
  capability.geometry.dar_correction_factor =
      orc::standard_dar_correction(video_params.system);

  return capability;
}

std::optional<ColourFrameCarrier> VideoSinkStage::get_colour_preview_carrier(
    uint64_t index, PreviewNavigationHint hint [[maybe_unused]]) const {
  ORC_LOG_DEBUG(
      "VideoSink: get_colour_preview_carrier called on instance {} for frame "
      "{}, has_cached_input={}",
      static_cast<const void*>(this), index, (cached_input_ != nullptr));

  std::shared_ptr<const orc::VideoFrameRepresentation> local_input;
  {
    std::lock_guard<std::mutex> lock(cached_input_mutex_);
    local_input = cached_input_;
  }

  if (!local_input) {
    return std::nullopt;
  }

  auto video_params_opt = local_input->get_video_parameters();
  if (!video_params_opt) {
    return std::nullopt;
  }
  const SourceParameters& videoParams = *video_params_opt;
  SourceParameters safeVideoParams = videoParams;

  const auto preview_profile =
      decoder_video_profile_for_type(decoder_type_, safeVideoParams.system);
  if (!apply_decoder_safe_video_parameters(safeVideoParams, preview_profile,
                                           "VideoSink preview")) {
    return std::nullopt;
  }

  // With VFrameR, frames are indexed directly (no field-parity offset needed).
  // Preview frame index == VFrameR frame_id offset into the frame range.
  const orc::FrameIDRange preview_frame_range = local_input->frame_range();
  const uint64_t frame_a_index = preview_frame_range.first + index;

  std::vector<SourceField> inputFields;

  const bool is_yc_source = local_input->has_separate_channels();

  // Build (or reuse) the preview decoder before gathering fields so the
  // look-around comes from the decoder itself, matching the render path
  // instead of duplicating per-type values here.
  std::string effectiveDecoderType = decoder_type_;

  // Transform PAL filters operate on composite chroma; a Y/C source has no
  // composite chroma to filter, so fall back to pal2d.  The render path applies
  // the same downgrade (there it mutates decoder_type_ to surface it in the
  // UI); the preview only needs it locally so it decodes what the export will.
  if (is_yc_source && (effectiveDecoderType == "transform2d" ||
                       effectiveDecoderType == "transform3d")) {
    effectiveDecoderType = "pal2d";
  }

  if (!preview_decoder_cache_.matches_config(
          effectiveDecoderType, chroma_gain_, chroma_phase_, luma_nr_,
          chroma_nr_, ntsc_phase_comp_, simple_pal_, false,
          transform_threshold_, chroma_weight_, adapt_threshold_)) {
    preview_decoder_cache_.decoder.reset();
    preview_decoder_cache_.decoder_type = effectiveDecoderType;
    preview_decoder_cache_.chroma_gain = chroma_gain_;
    preview_decoder_cache_.chroma_phase = chroma_phase_;
    preview_decoder_cache_.luma_nr = luma_nr_;
    preview_decoder_cache_.chroma_nr = chroma_nr_;
    preview_decoder_cache_.ntsc_phase_comp = ntsc_phase_comp_;
    preview_decoder_cache_.simple_pal = simple_pal_;
    preview_decoder_cache_.blackandwhite = false;
    preview_decoder_cache_.transform_threshold = transform_threshold_;
    preview_decoder_cache_.chroma_weight = chroma_weight_;
    preview_decoder_cache_.adapt_threshold = adapt_threshold_;

    DecoderParams decoderParams;
    decoderParams.chromaGain = chroma_gain_;
    decoderParams.chromaPhase = chroma_phase_;
    decoderParams.lumaNr = luma_nr_;
    decoderParams.chromaNr = chroma_nr_;
    decoderParams.ntscPhaseComp = ntsc_phase_comp_;
    decoderParams.simplePal = simple_pal_;
    decoderParams.transformThreshold = transform_threshold_;
    decoderParams.chromaWeight = chroma_weight_;
    decoderParams.adaptThreshold = adapt_threshold_;
    preview_decoder_cache_.decoder =
        make_decoder(effectiveDecoderType, decoderParams, safeVideoParams);
  }

  if (!preview_decoder_cache_.decoder) {
    return std::nullopt;
  }

  const int32_t num_lookbehind_frames =
      preview_decoder_cache_.decoder->getLookBehind();
  const int32_t num_lookahead_frames =
      preview_decoder_cache_.decoder->getLookAhead();

  int64_t start_frame_idx =
      static_cast<int64_t>(frame_a_index) - num_lookbehind_frames;
  int64_t end_frame_idx =
      static_cast<int64_t>(frame_a_index) + num_lookahead_frames;

  // PAL_M has NTSC-like frame geometry (525 lines, 909 samples/line); only
  // pure PAL uses the 625-line non-uniform layout.
  const bool preview_is_pal = (safeVideoParams.system == orc::VideoSystem::PAL);
  const int16_t preview_blanking =
      preview_is_pal ? static_cast<int16_t>(orc::kPalBlanking)
                     : static_cast<int16_t>(orc::kNtscBlanking);

  // Owned buffers backing every preview SourceField (blank padding and copies
  // of VFrameR frame data); must outlive inputFields.
  std::deque<std::vector<int16_t>> previewOwnedBuffers;

  // Helper: build a blank SourceField for preview
  auto makePreviewBlankField = [&](int64_t fi,
                                   bool is_first_field) -> SourceField {
    SourceField blank;
    blank.seq_no = static_cast<int32_t>(fi) + 1;
    blank.is_first_field = is_first_field;
    blank.is_yc = is_yc_source;

    // Compute samples for this field
    size_t samples;
    if (preview_is_pal) {
      const size_t pal_f1_samples = frame_line_sample_offset(
          orc::VideoSystem::PAL,
          static_cast<size_t>(orc::kPalSamplesPerLineNominal),
          static_cast<size_t>(orc::kPalField1Lines));
      samples =
          is_first_field
              ? pal_f1_samples
              : (static_cast<size_t>(orc::kPalFrameSamples) - pal_f1_samples);
      blank.samples_per_line = 1135;
      blank.line_count =
          is_first_field
              ? static_cast<size_t>(orc::kPalField1Lines)
              : static_cast<size_t>(orc::kPalFrameLines - orc::kPalField1Lines);
    } else {
      size_t spl = (safeVideoParams.system == orc::VideoSystem::PAL_M)
                       ? static_cast<size_t>(orc::kPalMSamplesPerLine)
                       : static_cast<size_t>(orc::kNtscSamplesPerLine);
      size_t fl = is_first_field ? static_cast<size_t>(orc::kNtscField1Lines)
                                 : static_cast<size_t>(orc::kNtscFrameLines -
                                                       orc::kNtscField1Lines);
      samples = spl * fl;
      blank.samples_per_line = spl;
      blank.line_count = fl;
    }

    previewOwnedBuffers.emplace_back(samples, preview_blanking);
    const int16_t* buf = previewOwnedBuffers.back().data();

    blank.data = buf;
    if (is_yc_source) {
      blank.luma_data = buf;
      blank.chroma_data = buf;
    }

    if (preview_is_pal) {
      blank.line_ptrs.reserve(blank.line_count);
      const size_t preview_field_base =
          is_first_field ? 0 : static_cast<size_t>(orc::kPalField1Lines);
      size_t off = 0;
      for (size_t ln = 0; ln < blank.line_count; ++ln) {
        blank.line_ptrs.push_back(buf + off);
        off += frame_line_sample_count(
            orc::VideoSystem::PAL,
            static_cast<size_t>(orc::kPalSamplesPerLineNominal),
            preview_field_base + ln);
      }
      if (is_yc_source) {
        blank.luma_line_ptrs = blank.line_ptrs;
        blank.chroma_line_ptrs = blank.line_ptrs;
      }
    }
    return blank;
  };

  for (int64_t fi = start_frame_idx; fi <= end_frame_idx; ++fi) {
    orc::FrameID fid = static_cast<orc::FrameID>(fi);
    if (fi < 0 || !local_input->has_frame(fid) ||
        !appendSourceFields(local_input.get(), fid, safeVideoParams,
                            previewOwnedBuffers, inputFields)) {
      // Out-of-range, or the frame unexpectedly had no data; substitute
      // black fields so the target frame stays at frameStartIndex.
      inputFields.push_back(makePreviewBlankField(fi, true));
      inputFields.push_back(makePreviewBlankField(fi, false));
    }
  }

  if (inputFields.size() < 2) {
    return std::nullopt;
  }

  // Determine the target frame's position within inputFields (field units).
  // Each frame contributes 2 SourceFields; lookbehind frames come first.
  const int32_t frameStartIndex = num_lookbehind_frames * 2;

  std::vector<::ComponentFrame> outputFrames(1);
  const int32_t frameEndIndex = frameStartIndex + 2;

  preview_decoder_cache_.decoder->decodeFrames(inputFields, frameStartIndex,
                                               frameEndIndex, outputFrames);

  ::ComponentFrame& frame = outputFrames[0];
  int32_t width = frame.getWidth();
  int32_t height = frame.getHeight();
  if (width <= 0 || height <= 0) {
    return std::nullopt;
  }

  ColourFrameCarrier carrier{};
  carrier.data_type = (videoParams.system == VideoSystem::PAL ||
                       videoParams.system == VideoSystem::PAL_M)
                          ? VideoDataType::ColourPAL
                          : VideoDataType::ColourNTSC;
  // Colorimetry follows the line standard, not the chroma encoding: PAL-M is
  // a 525-line System M signal, so it takes System M's 2.2 gamma and SMPTE C
  // primaries rather than the 625-line PAL defaults it would inherit from
  // data_type.  This matches how the export path tags PAL-M (SMPTE 170M).
  switch (videoParams.system) {
    case VideoSystem::PAL:
      carrier.colorimetry = ColorimetricMetadata::default_pal();
      break;
    case VideoSystem::PAL_M:
      carrier.colorimetry = ColorimetricMetadata::default_pal_m();
      break;
    default:
      carrier.colorimetry = ColorimetricMetadata::default_ntsc();
      break;
  }
  carrier.system = videoParams.system;
  carrier.frame_index = index;
  carrier.width = static_cast<uint32_t>(width);
  carrier.height = static_cast<uint32_t>(height);
  carrier.active_x_start =
      (videoParams.active_video_start >= 0 &&
       videoParams.active_video_start < width)
          ? static_cast<uint32_t>(videoParams.active_video_start)
          : 0U;
  carrier.active_x_end =
      (videoParams.active_video_end > videoParams.active_video_start &&
       videoParams.active_video_end <= width)
          ? static_cast<uint32_t>(videoParams.active_video_end)
          : carrier.width;
  carrier.active_y_start =
      (videoParams.first_active_frame_line >= 0 &&
       videoParams.first_active_frame_line < height)
          ? static_cast<uint32_t>(videoParams.first_active_frame_line)
          : 0U;
  carrier.active_y_end =
      (videoParams.last_active_frame_line >
           videoParams.first_active_frame_line &&
       videoParams.last_active_frame_line <= height)
          ? static_cast<uint32_t>(videoParams.last_active_frame_line)
          : carrier.height;
  // CVBS_U10_4FSC normative levels from source parameters.
  carrier.cvbs_blanking = videoParams.blanking_level;
  carrier.cvbs_black = videoParams.black_level;
  carrier.cvbs_white = videoParams.white_level;

  carrier.vectorscope_data = extract_vectorscope_from_component_frame(
      frame, videoParams, frame_a_index * 2, 4);
  if (carrier.vectorscope_data.has_value()) {
    carrier.vectorscope_data->system = videoParams.system;
    carrier.vectorscope_data->cvbs_white = videoParams.white_level;
    carrier.vectorscope_data->cvbs_blanking = videoParams.blanking_level;
  }

  copy_component_planes_to_carrier(frame, width, height, carrier.y_plane,
                                   carrier.u_plane, carrier.v_plane);

  if (!carrier.is_valid()) {
    return std::nullopt;
  }

  return carrier;
}

}  // namespace orc
