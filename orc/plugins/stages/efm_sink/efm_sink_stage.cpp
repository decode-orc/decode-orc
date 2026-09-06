/*
 * File:        efm_sink_stage.cpp
 * Module:      orc-core
 * Purpose:     EFM Decoder Sink Stage - decodes EFM t-values to audio WAV or
 * data sectors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "efm_sink_stage.h"

#include <orc/stage/common_types.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>

#include "efm_sink_stage_deps.h"
#include "efm_sink_stage_deps_interface.h"

namespace orc {
namespace {
// Issue #307: bounds for the doubt_erasure_threshold parameter. Doubt is the
// 4-bit field the producer packs into the high nibble of every .efm byte
// (CVBS File Format Specification, EFM extension format), so it spans 0-15;
// 0 as a threshold means "never seed an erasure from doubt" rather than "seed
// every symbol", since doubt 0 is the fully-trusted value.
constexpr int32_t kDoubtErasureThresholdOff = 0;
constexpr int32_t kDoubtErasureThresholdMax = 15;
// Off by default. Measured over BBC Domesday DS2 National A (CAV PAL, 54,000
// frames, 120,219 sections) with thresholds 15 down to 11: seeding erasures
// from the doubt improves C1 slightly (4,047 -> 3,963 uncorrectable at 13) but
// never changes the recovered-sector count, and below 13 it costs sectors as
// the spurious erasures eat C2's capacity. Until a disc is found where it pays,
// the default leaves the decode bit-exact and the setting is there to be
// tried. 13 is the value to start from.
constexpr int32_t kDoubtErasureThresholdDefault = kDoubtErasureThresholdOff;

int32_t get_int32_param(const std::map<std::string, ParameterValue>& params,
                        const std::string& name, int32_t default_val) {
  auto it = params.find(name);
  if (it == params.end()) return default_val;
  if (const int32_t* v = std::get_if<int32_t>(&it->second)) return *v;
  return default_val;
}

}  // namespace

EFMSinkStage::EFMSinkStage() {
  set_configuration_status(orc::ConfigurationStatus::Red);
}

NodeTypeInfo EFMSinkStage::get_node_type_info() const {
  return NodeTypeInfo{
      NodeType::SINK,
      "EFMSink",
      "EFM Decoder Sink",
      "Decodes EFM t-values from the VFR using the full EFM decode pipeline "
      "(EFM -> audio WAV or ECMA-130 binary sector data)",
      1,
      1,  // One input
      0,
      0,  // No outputs (sink)
      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> EFMSinkStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  // Sink stages don't produce outputs in execute()
  // Actual work happens in trigger()
  (void)inputs;
  (void)parameters;
  (void)observation_context;
  return {};
}

std::vector<ParameterDescriptor> EFMSinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)project_format;
  (void)source_type;
  std::vector<ParameterDescriptor> descriptors;

  // output_path
  {
    ParameterDescriptor desc;
    desc.name = "output_path";
    desc.display_name = "Output File";
    desc.description =
        "Path to the decoded output file (.wav for audio mode, .bin for data "
        "mode)";
    desc.type = ParameterType::FILE_PATH;
    desc.constraints.required = true;
    desc.constraints.default_value = std::string("");
    desc.file_extension_hint = ".wav";
    descriptors.push_back(desc);
  }

  // decode_mode  (STRING with allowed_strings acts as an enum combo-box in the
  // GUI)
  {
    ParameterDescriptor desc;
    desc.name = "decode_mode";
    desc.display_name = "Decode Mode";
    desc.description =
        "Select audio decoding (outputs WAV/PCM) or data decoding (outputs "
        "ECMA-130 sectors)";
    desc.type = ParameterType::STRING;
    desc.constraints.required = true;
    desc.constraints.allowed_strings = {"audio", "data"};
    desc.constraints.default_value = std::string("audio");
    descriptors.push_back(desc);
  }

  // no_timecodes  (audio + data)
  {
    ParameterDescriptor desc;
    desc.name = "no_timecodes";
    desc.display_name = "No Timecodes";
    desc.description =
        "Disable timecode verification during decode. Needed for early CAV "
        "discs that pre-date the EFM timecode specification.";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  // audacity_labels  (audio only)
  {
    ParameterDescriptor desc;
    desc.name = "audacity_labels";
    desc.display_name = "Audacity Labels";
    desc.description =
        "Write an Audacity label file alongside the audio output";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    desc.constraints.depends_on = ParameterDependency{"decode_mode", {"audio"}};
    descriptors.push_back(desc);
  }

  // no_audio_concealment  (audio only)
  {
    ParameterDescriptor desc;
    desc.name = "no_audio_concealment";
    desc.display_name = "Disable Audio Concealment";
    desc.description = "Disable interpolation-based audio error concealment";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    desc.constraints.depends_on = ParameterDependency{"decode_mode", {"audio"}};
    descriptors.push_back(desc);
  }

  // ignore_preemphasis  (audio only)
  {
    ParameterDescriptor desc;
    desc.name = "ignore_preemphasis";
    desc.display_name = "Ignore Pre-emphasis Flag";
    desc.description =
        "Ignore the 50/15 us pre-emphasis CONTROL flag and emit audio exactly "
        "as decoded. When unchecked (default), pre-emphasised sections are "
        "de-emphasised during decode.";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    desc.constraints.depends_on = ParameterDependency{"decode_mode", {"audio"}};
    descriptors.push_back(desc);
  }

  // video_sync  (audio only)
  {
    ParameterDescriptor desc;
    desc.name = "video_sync";
    desc.display_name = "Align With Video Timeline";
    desc.description =
        "Align the start of the decoded audio with the video timeline of the "
        "capture: compensates the CIRC de-interleave latency and the input "
        "consumed while the decoder acquired sync. Ignored when Zero-Pad "
        "Audio is enabled (zero-pad anchors the audio to disc absolute time "
        "00:00:00 instead). Disable for bit-exact legacy output.";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = true;
    desc.constraints.depends_on = ParameterDependency{"decode_mode", {"audio"}};
    descriptors.push_back(desc);
  }

  // offset_ms  (audio only)
  {
    ParameterDescriptor desc;
    desc.name = "offset_ms";
    desc.display_name = "Sync Offset (ms)";
    desc.description =
        "Additional sync slip in milliseconds on top of the video-timeline "
        "alignment. Positive values delay the audio relative to the video; "
        "negative values advance it. Leave at 0 unless the decoded audio "
        "still needs nudging.";
    desc.type = ParameterType::DOUBLE;
    // A finite range keeps the GUI spin box a sensible width; ±1 hour is far
    // beyond any real audio/video sync correction.
    desc.constraints.min_value = -3'600'000.0;
    desc.constraints.max_value = 3'600'000.0;
    desc.constraints.default_value = 0.0;
    desc.constraints.depends_on = ParameterDependency{"decode_mode", {"audio"}};
    descriptors.push_back(desc);
  }

  // zero_pad  (audio only)
  {
    ParameterDescriptor desc;
    desc.name = "zero_pad";
    desc.display_name = "Zero-Pad Audio";
    desc.description =
        "Zero-pad missing or short audio samples instead of skipping";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    desc.constraints.depends_on = ParameterDependency{"decode_mode", {"audio"}};
    descriptors.push_back(desc);
  }

  // no_wav_header  (audio only)
  {
    ParameterDescriptor desc;
    desc.name = "no_wav_header";
    desc.display_name = "Raw PCM (No WAV Header)";
    desc.description = "Output raw PCM samples without a WAV header";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    desc.constraints.depends_on = ParameterDependency{"decode_mode", {"audio"}};
    descriptors.push_back(desc);
  }

  // output_metadata  (data only)
  {
    ParameterDescriptor desc;
    desc.name = "output_metadata";
    desc.display_name = "Output Bad Sector Map";
    desc.description =
        "Write a bad-sector map metadata file alongside the sector output";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    desc.constraints.depends_on = ParameterDependency{"decode_mode", {"data"}};
    descriptors.push_back(desc);
  }

  // doubt_erasure_threshold  (audio + data)
  {
    ParameterDescriptor desc;
    desc.name = "doubt_erasure_threshold";
    desc.display_name = "Doubt Erasure Threshold";
    desc.description =
        "Treat an EFM symbol as a Reed-Solomon erasure when the producer's "
        "doubt about it (0 trusted to 15 distrusted, carried in the .efm) "
        "reaches this value. Symbols that demodulate to a legal EFM codeword "
        "but are still wrong are invisible to C1/C2 otherwise, which leaves "
        "them correcting unknown errors instead of erasures. Only the four "
        "most-doubted symbols of a codeword are ever flagged, so the code's "
        "capacity cannot be exceeded. 0 (the default) disables this and keeps "
        "the decode bit-exact; 13 is the value to start from if you want to "
        "try it. An .efm from a producer that carries no confidence "
        "information is unaffected either way.";
    desc.type = ParameterType::INT32;
    desc.constraints.min_value = kDoubtErasureThresholdOff;
    desc.constraints.max_value = kDoubtErasureThresholdMax;
    desc.constraints.default_value = kDoubtErasureThresholdDefault;
    descriptors.push_back(desc);
  }

  // report
  {
    ParameterDescriptor desc;
    desc.name = "report";
    desc.display_name = "Write Decode Report";
    desc.description = "Write a detailed decode statistics report file";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> EFMSinkStage::get_parameters() const {
  return parameters_;
}

bool EFMSinkStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  parameters_ = params;

  const auto it = params.find("output_path");
  const bool has_path =
      (it != params.end() && std::holds_alternative<std::string>(it->second) &&
       !std::get<std::string>(it->second).empty());

  set_configuration_status(has_path ? orc::ConfigurationStatus::Green
                                    : orc::ConfigurationStatus::Red);
  return true;
}

std::string EFMSinkStage::get_trigger_status() const { return last_status_; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool get_bool_param(const std::map<std::string, ParameterValue>& params,
                           const std::string& name, bool default_val = false) {
  auto it = params.find(name);
  if (it == params.end()) return default_val;
  if (const bool* b = std::get_if<bool>(&it->second)) return *b;
  return default_val;
}

static std::string get_string_param(
    const std::map<std::string, ParameterValue>& params,
    const std::string& name, const std::string& default_val = "") {
  auto it = params.find(name);
  if (it == params.end()) return default_val;
  if (const std::string* s = std::get_if<std::string>(&it->second)) return *s;
  return default_val;
}

static double get_double_param(
    const std::map<std::string, ParameterValue>& params,
    const std::string& name, double default_val = 0.0) {
  auto it = params.find(name);
  if (it == params.end()) return default_val;
  if (const double* v = std::get_if<double>(&it->second)) return *v;
  return default_val;
}

// ---------------------------------------------------------------------------
// trigger()
// ---------------------------------------------------------------------------

bool EFMSinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  (void)observation_context;
  is_processing_.store(true);
  cancel_requested_.store(false);

  try {
    // ------------------------------------------------------------------
    // 1. Validate input
    // ------------------------------------------------------------------
    if (inputs.empty()) {
      throw std::runtime_error(
          "EFMSink requires one input (VideoFrameRepresentation)");
    }
    auto vfr = std::dynamic_pointer_cast<VideoFrameRepresentation>(inputs[0]);
    if (!vfr) {
      throw std::runtime_error(
          "EFMSink input must be a VideoFrameRepresentation");
    }
    if (!vfr->has_efm()) {
      throw std::runtime_error(
          "EFMSink: input VFrameR has no EFM data (no EFM file in source?)");
    }

    // ------------------------------------------------------------------
    // 2. Extract parameters
    // ------------------------------------------------------------------
    const std::string output_path = get_string_param(parameters, "output_path");
    if (output_path.empty()) {
      throw std::runtime_error("EFMSink: output_path parameter is required");
    }

    const std::string decode_mode =
        get_string_param(parameters, "decode_mode", "audio");
    const bool audio_mode = (decode_mode != "data");

    const bool no_timecodes = get_bool_param(parameters, "no_timecodes");
    const bool audacity_labels = get_bool_param(parameters, "audacity_labels");
    const bool no_audio_concealment =
        get_bool_param(parameters, "no_audio_concealment");
    const bool ignore_preemphasis =
        get_bool_param(parameters, "ignore_preemphasis");
    const bool zero_pad = get_bool_param(parameters, "zero_pad");
    const bool no_wav_header = get_bool_param(parameters, "no_wav_header");
    const bool output_metadata = get_bool_param(parameters, "output_metadata");
    const bool report = get_bool_param(parameters, "report");
    const int32_t doubt_erasure_threshold =
        std::clamp(get_int32_param(parameters, "doubt_erasure_threshold",
                                   kDoubtErasureThresholdDefault),
                   kDoubtErasureThresholdOff, kDoubtErasureThresholdMax);
    const bool video_sync = get_bool_param(parameters, "video_sync", true);
    const double offset_ms = get_double_param(parameters, "offset_ms");

    ORC_LOG_INFO("EFMSink: mode={}, output={}", audio_mode ? "audio" : "data",
                 output_path);

    EFMSinkOptions options;
    options.output_path = output_path;
    options.audio_mode = audio_mode;
    options.no_timecodes = no_timecodes;
    options.audacity_labels = audacity_labels;
    options.no_audio_concealment = no_audio_concealment;
    options.ignore_preemphasis = ignore_preemphasis;
    options.zero_pad = zero_pad;
    options.no_wav_header = no_wav_header;
    options.output_metadata = output_metadata;
    options.report = report;
    options.doubt_erasure_threshold =
        static_cast<uint8_t>(doubt_erasure_threshold);
    options.video_sync = video_sync;
    options.offset_ms = offset_ms;

    std::shared_ptr<IEFMSinkStageDeps> deps = deps_override_;
    if (!deps) {
      auto deps_impl = std::make_shared<EFMSinkStageDeps>();
      deps_impl->init(progress_callback_, &cancel_requested_);
      deps = deps_impl;
    }

    const auto decode_result = deps->decode_efm(vfr.get(), options);
    if (!decode_result.success) {
      last_status_ = decode_result.status_message;
      ORC_LOG_ERROR("EFMSink: {}", decode_result.status_message);
      is_processing_.store(false);
      return false;
    }

    last_status_ = decode_result.status_message;
    ORC_LOG_INFO("EFMSink: {}", last_status_);
    is_processing_.store(false);
    return true;

  } catch (const std::exception& e) {
    last_status_ = std::string("Error: ") + e.what();
    ORC_LOG_ERROR("EFMSink: {}", e.what());
    is_processing_.store(false);
    return false;
  }
}

}  // namespace orc
