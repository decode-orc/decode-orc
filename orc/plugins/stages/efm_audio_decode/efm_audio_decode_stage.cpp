/*
 * File:        efm_audio_decode_stage.cpp
 * Module:      orc-stage-plugin-efm_audio_decode
 * Purpose:     EFM audio decode transform stage (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "efm_audio_decode_stage.h"

#include <orc/stage/error_types.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <variant>

#include "audio-resample/audio_resampler.h"
#include "efm_audio_decode_stage_deps.h"

namespace orc {

namespace {

// IEC 60908 (CD-DA): EFM decodes to 44.1 kHz 16-bit stereo audio.
constexpr double kCdSampleRateHz = 44100.0;

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

}  // namespace

// ============================================================================
// EFMAudioChannelPairRepresentation
// ============================================================================

std::optional<AudioChannelPairDescriptor>
EFMAudioChannelPairRepresentation::get_audio_channel_pair_descriptor(
    size_t pair) const {
  if (pair == efm_pair_index()) {
    const std::string name =
        options_.pair_name.empty() ? "EFM digital audio" : options_.pair_name;
    return AudioChannelPairDescriptor{name, AudioOrigin::EFM};
  }
  // Out-of-range indices fall through to the source, which answers nullopt.
  return source_ ? source_->get_audio_channel_pair_descriptor(pair)
                 : std::nullopt;
}

std::vector<int32_t> EFMAudioChannelPairRepresentation::get_audio_samples(
    size_t pair, FrameID id) const {
  if (pair != efm_pair_index()) {
    // Out-of-range indices fall through to the source, which answers {}.
    return source_ ? source_->get_audio_samples(pair, id)
                   : std::vector<int32_t>{};
  }

  ensure_decoded();

  const auto params = source_ ? source_->get_video_parameters() : std::nullopt;
  const VideoSystem system = params ? params->system : VideoSystem::Unknown;
  // The cache is indexed from the start of the decode range, so map the
  // absolute FrameID onto a range-relative index (matching audio_import).
  const uint64_t index = id - frame_range().first;
  const uint32_t block_pairs = audio_pairs_in_frame(index, system);
  if (block_pairs == 0) return {};

  // The contract requires exactly audio_pairs_in_frame(index, system) pairs:
  // start from silence and overlay whatever the cache holds, so a failed
  // decode or a short cache read still serves a full cadence-sized block.
  std::vector<int32_t> samples(static_cast<size_t>(block_pairs) * 2, 0);
  if (synchronous_audio_ready_) {
    const std::vector<int32_t> cached = deps_->read_synchronous_pairs(
        audio_pair_offset(index, system), block_pairs);
    std::copy_n(cached.begin(), std::min(cached.size(), samples.size()),
                samples.begin());
  }
  return samples;
}

void EFMAudioChannelPairRepresentation::ensure_decoded(
    const AudioDecodeProgressFn& progress) const {
  std::call_once(decode_once_, [this, &progress] {
    if (!source_) {
      ORC_LOG_ERROR(
          "EFMAudioDecode: no source representation; the EFM audio channel "
          "pair will be silent");
      return;
    }
    const auto params = source_->get_video_parameters();
    const VideoSystem system = params ? params->system : VideoSystem::Unknown;
    const size_t frame_count = source_->frame_count();
    if (system == VideoSystem::Unknown || frame_count == 0) {
      ORC_LOG_ERROR(
          "EFMAudioDecode: unknown video system or empty frame range; the "
          "EFM audio channel pair will be silent");
      return;
    }

    ORC_LOG_INFO("EFMAudioDecode: starting lazy EFM audio decode");
    const EFMAudioDecodeResult decode_result =
        deps_->decode_to_cache(*source_, options_, progress);
    if (!decode_result.success) {
      ORC_LOG_ERROR(
          "EFMAudioDecode: EFM audio decode failed ({}); the EFM audio "
          "channel pair will be silent",
          decode_result.error_message);
      return;
    }
    ORC_LOG_INFO("EFMAudioDecode: decoded {} stereo pairs of EFM audio",
                 decode_result.stream_pair_count);
    ORC_LOG_INFO(
        "EFMAudioDecode: video-timeline alignment applied: {} stereo pairs "
        "({:.2f} ms) at the stream head",
        decode_result.applied_sync_offset_pairs,
        static_cast<double>(decode_result.applied_sync_offset_pairs) * 1000.0 /
            kCdSampleRateHz);

    // Pull the raw decoded CD audio (44.1 kHz 16-bit stereo) and convert it
    // to the pipeline form: widen to the 24-bit-in-int32 carrier, resample
    // 44100 → 48000 Hz, and segment into cadence-sized per-frame blocks
    // totalling exactly audio_pair_offset(frame_count) pairs.
    const uint32_t raw_pairs = static_cast<uint32_t>(std::min<uint64_t>(
        decode_result.stream_pair_count, std::numeric_limits<uint32_t>::max()));
    // Scope the widened 44.1 kHz carrier so it is freed the moment resampling
    // returns; otherwise it would sit alongside the per-frame blocks and the
    // flattened output below, tripling the converted-stream footprint at peak.
    // The resample of the full stream is itself multi-second work; surface it
    // as a distinct phase so the dialog does not appear to stall after decode.
    if (progress) {
      progress(frame_count, frame_count,
               "Converting EFM audio (resampling)...");
    }
    std::vector<std::vector<int32_t>> frames;
    {
      const std::vector<int32_t> widened =
          AudioResampler::widen_16_to_24(deps_->read_cache_pairs(0, raw_pairs));
      frames = AudioResampler::resample_to_synchronous(widened, kCdSampleRateHz,
                                                       system, frame_count);
    }

    // Flatten the cadence-aligned blocks into the synchronous scratch cache;
    // per-frame serving seeks by audio_pair_offset(id, system).
    std::vector<int32_t> flat;
    flat.reserve(static_cast<size_t>(audio_pair_offset(frame_count, system)) *
                 2);
    for (auto& frame : frames) {
      flat.insert(flat.end(), frame.begin(), frame.end());
      frame.clear();
      frame.shrink_to_fit();
    }
    if (!deps_->write_synchronous_cache(flat)) {
      ORC_LOG_ERROR(
          "EFMAudioDecode: failed to store the converted synchronous audio; "
          "the EFM audio channel pair will be silent");
      return;
    }
    synchronous_audio_ready_ = true;
    ORC_LOG_INFO(
        "EFMAudioDecode: converted EFM audio to {} synchronous 48 kHz stereo "
        "pairs across {} frames",
        flat.size() / 2, frame_count);
  });
}

// ============================================================================
// EFMAudioDecodeStage
// ============================================================================

EFMAudioDecodeStage::EFMAudioDecodeStage() {
  // Both parameters are optional with safe defaults.
  set_configuration_status(orc::ConfigurationStatus::Green);
}

std::vector<ArtifactPtr> EFMAudioDecodeStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  (void)observation_context;
  if (inputs.empty()) {
    throw DAGExecutionError("EFMAudioDecodeStage requires one input");
  }
  auto vfr =
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  if (!vfr) {
    throw DAGExecutionError(
        "EFMAudioDecodeStage input must be VideoFrameRepresentation");
  }

  if (!parameters.empty()) set_parameters(parameters);

  // The pair cap is enforced at every pair-adding stage, not just at the
  // container sink, so a DAG that validates also exports.
  if (vfr->audio_channel_pair_count() >= kMaxAudioChannelPairs) {
    throw DAGExecutionError(
        "EFMAudioDecodeStage: cannot append the EFM audio channel pair: the "
        "input already carries the maximum of " +
        std::to_string(kMaxAudioChannelPairs) + " audio channel pairs");
  }

  auto output = process(vfr);
  cached_output_ = output;

  std::vector<ArtifactPtr> outputs;
  outputs.push_back(std::const_pointer_cast<EFMAudioChannelPairRepresentation>(
      std::dynamic_pointer_cast<const EFMAudioChannelPairRepresentation>(
          output)));
  return outputs;
}

std::shared_ptr<const VideoFrameRepresentation> EFMAudioDecodeStage::process(
    std::shared_ptr<const VideoFrameRepresentation> source) const {
  if (!source) return nullptr;
  auto deps = deps_override_ ? deps_override_
                             : std::static_pointer_cast<IEFMAudioDecodeDeps>(
                                   std::make_shared<EFMAudioDecodeDeps>());
  EFMAudioDecodeOptions options;
  options.no_timecodes = no_timecodes_;
  options.no_audio_concealment = no_audio_concealment_;
  options.ignore_preemphasis = ignore_preemphasis_;
  // A report is written only when the checkbox is enabled; an empty path
  // leaves the report disabled even if the box is checked.
  options.report_path = report_ ? report_path_ : std::string{};
  options.pair_name = pair_name_;
  options.offset_ms = offset_ms_;
  options.doubt_erasure_threshold = static_cast<uint8_t>(
      std::clamp(doubt_erasure_threshold_, kDoubtErasureThresholdOff,
                 kDoubtErasureThresholdMax));
  return std::make_shared<EFMAudioChannelPairRepresentation>(
      std::move(source), std::move(deps), options);
}

std::vector<ParameterDescriptor> EFMAudioDecodeStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)project_format;
  (void)source_type;
  std::vector<ParameterDescriptor> descriptors;

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

  {
    ParameterDescriptor desc;
    desc.name = "no_audio_concealment";
    desc.display_name = "Disable Audio Concealment";
    desc.description = "Disable interpolation-based audio error concealment";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "ignore_preemphasis";
    desc.display_name = "Ignore Pre-emphasis Flag";
    desc.description =
        "Ignore the 50/15 us pre-emphasis CONTROL flag and decode audio "
        "exactly as stored. When unchecked (default), pre-emphasised sections "
        "are de-emphasised during decode.";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "pair_name";
    desc.display_name = "Channel Pair Name";
    desc.description =
        "Human-readable name for the decoded EFM audio channel pair. Surfaces "
        "in the CVBS container and as the embedded stream title in the video "
        "sink. Empty uses \"EFM digital audio\".";
    desc.type = ParameterType::STRING;
    desc.constraints.default_value = std::string("EFM digital audio");
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "offset_ms";
    desc.display_name = "Sync Offset (ms)";
    desc.description =
        "Additional sync slip in milliseconds on top of the automatic "
        "video-timeline alignment. Positive values delay the audio relative "
        "to the video; negative values advance it. Leave at 0 unless the "
        "decoded audio still needs nudging.";
    desc.type = ParameterType::DOUBLE;
    // A finite range keeps the GUI spin box a sensible width; ±1 hour is far
    // beyond any real audio/video sync correction.
    desc.constraints.min_value = -3'600'000.0;
    desc.constraints.max_value = 3'600'000.0;
    desc.constraints.default_value = 0.0;
    descriptors.push_back(desc);
  }

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

  {
    ParameterDescriptor desc;
    desc.name = "report";
    desc.display_name = "Write Decode Report";
    desc.description = "Write a detailed decode statistics report file";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "report_path";
    desc.display_name = "Decode Report File";
    desc.description = "Path for the decode statistics report file";
    desc.type = ParameterType::FILE_PATH;
    desc.constraints.default_value = std::string("");
    desc.constraints.depends_on = ParameterDependency{"report", {"true"}};
    desc.file_extension_hint = ".txt";
    desc.output_path = true;  // Report is written; use a save dialog, not open
    descriptors.push_back(desc);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> EFMAudioDecodeStage::get_parameters()
    const {
  return {{"no_timecodes", no_timecodes_},
          {"no_audio_concealment", no_audio_concealment_},
          {"ignore_preemphasis", ignore_preemphasis_},
          {"pair_name", pair_name_},
          {"offset_ms", offset_ms_},
          {"doubt_erasure_threshold", doubt_erasure_threshold_},
          {"report", report_},
          {"report_path", report_path_}};
}

bool EFMAudioDecodeStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  const auto get_bool = [&params](const std::string& name, bool current) {
    const auto it = params.find(name);
    if (it == params.end()) return current;
    if (const bool* b = std::get_if<bool>(&it->second)) return *b;
    return current;
  };
  no_timecodes_ = get_bool("no_timecodes", no_timecodes_);
  no_audio_concealment_ =
      get_bool("no_audio_concealment", no_audio_concealment_);
  ignore_preemphasis_ = get_bool("ignore_preemphasis", ignore_preemphasis_);
  report_ = get_bool("report", report_);

  const auto name_it = params.find("pair_name");
  if (name_it != params.end()) {
    if (const std::string* s = std::get_if<std::string>(&name_it->second)) {
      pair_name_ = *s;
    }
  }

  const auto doubt_it = params.find("doubt_erasure_threshold");
  if (doubt_it != params.end()) {
    if (const int32_t* v = std::get_if<int32_t>(&doubt_it->second)) {
      doubt_erasure_threshold_ = *v;
    }
  }

  const auto offset_it = params.find("offset_ms");
  if (offset_it != params.end()) {
    if (const double* v = std::get_if<double>(&offset_it->second)) {
      offset_ms_ = *v;
    }
  }

  const auto it = params.find("report_path");
  if (it != params.end()) {
    if (const std::string* s = std::get_if<std::string>(&it->second)) {
      report_path_ = *s;
    }
  }
  return true;
}

StagePreviewCapability EFMAudioDecodeStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

}  // namespace orc
