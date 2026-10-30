/*
 * File:        audio_channel_map_stage.cpp
 * Module:      orc-stage-plugin-audio_channel_map
 * Purpose:     Audio channel routing transform stage (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "audio_channel_map_stage.h"

#include <orc/stage/error_types.h>
#include <orc/stage/observation/colour_frame_phase_query.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>
#include <orc/support/vbi_types.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace orc {

namespace {

constexpr const char* kOpDelete = "delete";
constexpr const char* kOpLeftToMono = "left_to_mono";
constexpr const char* kOpRightToMono = "right_to_mono";
constexpr const char* kOpCopyLeftToTarget = "copy_left_to_target";
constexpr const char* kOpCopyRightToTarget = "copy_right_to_target";
constexpr const char* kOpVbiMonoAverage = "vbi_mono_average";

// Sentinel target value meaning "append a new channel pair".
constexpr const char* kTargetNew = "new";

std::optional<AudioChannelMapOperation> parse_operation(
    const std::string& name) {
  if (name == kOpDelete) return AudioChannelMapOperation::kDelete;
  if (name == kOpLeftToMono) return AudioChannelMapOperation::kLeftToMono;
  if (name == kOpRightToMono) return AudioChannelMapOperation::kRightToMono;
  if (name == kOpCopyLeftToTarget) {
    return AudioChannelMapOperation::kCopyLeftToTarget;
  }
  if (name == kOpCopyRightToTarget) {
    return AudioChannelMapOperation::kCopyRightToTarget;
  }
  if (name == kOpVbiMonoAverage) {
    return AudioChannelMapOperation::kVbiMonoAverage;
  }
  return std::nullopt;
}

bool is_route_op(AudioChannelMapOperation op) {
  return op == AudioChannelMapOperation::kCopyLeftToTarget ||
         op == AudioChannelMapOperation::kCopyRightToTarget;
}

// Parses a channel-pair index string in the range [0, kMaxAudioChannelPairs).
// Returns nullopt on any non-numeric or out-of-range value.
std::optional<int> parse_pair_index(const std::string& text) {
  int parsed = 0;
  try {
    size_t consumed = 0;
    parsed = std::stoi(text, &consumed);
    if (consumed != text.size()) return std::nullopt;
  } catch (const std::exception&) {
    return std::nullopt;
  }
  if (parsed < 0 || parsed >= static_cast<int>(kMaxAudioChannelPairs)) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<int32_t> get_int(const IObservationContext& context,
                               FieldID field_id, const char* key) {
  const auto value = context.get(field_id, "vbi", key);
  if (value && std::holds_alternative<int32_t>(*value)) {
    return std::get<int32_t>(*value);
  }
  return std::nullopt;
}

// Runs the host biphase observer over frame |id| and reads its programme
// status. Without an observation service no frame is mono.
bool observe_frame_is_mono(const VideoFrameRepresentation& vfr, FrameID id) {
  IObservationService* service = orc::plugin::get_observation_service();
  if (!service) return false;
  std::unique_ptr<IObserverHandle> observer =
      service->create_observer("biphase");
  if (!observer) return false;
  observation::detail::CaptureContext context;
  observer->process_frame(vfr, id, context);
  return vbi_frame_is_mono(context, id);
}

}  // namespace

bool mono_channels_agree(const std::vector<int32_t>& samples) {
  // Measured per frame on one capture: Community North's mono passages are
  // within 2 dB on 99 % of frames and correlate at 0.94 or better wherever the
  // louder channel is above -40 dBFS; Dragon's Lair's channels are 30-45 dB
  // apart, and where they are close they do not correlate (median 0.01). The
  // rule averages 92 % of North's frames and none of Dragon's Lair's above
  // -50 dBFS.
  constexpr double kMaxImbalance = 3.981;  // 6 dB in power
  constexpr double kMinCorrelation = 0.5;
  constexpr double kFullScale = 8388608.0;
  const double quiet_power = kFullScale * kFullScale * 1e-5;  // -50 dBFS

  const size_t pairs = samples.size() / 2;
  if (pairs == 0) return false;
  double mean_l = 0.0;
  double mean_r = 0.0;
  for (size_t p = 0; p < pairs; ++p) {
    mean_l += samples[p * 2];
    mean_r += samples[p * 2 + 1];
  }
  mean_l /= static_cast<double>(pairs);
  mean_r /= static_cast<double>(pairs);
  double power_l = 0.0;
  double power_r = 0.0;
  double cross = 0.0;
  for (size_t p = 0; p < pairs; ++p) {
    const double l = samples[p * 2] - mean_l;
    const double r = samples[p * 2 + 1] - mean_r;
    power_l += l * l;
    power_r += r * r;
    cross += l * r;
  }
  // Both silent: the mean changes nothing either way.
  if (power_l == 0.0 && power_r == 0.0) return true;
  const double louder = std::max(power_l, power_r);
  const double quieter = std::min(power_l, power_r);
  if (quieter * kMaxImbalance < louder) return false;
  if (louder / static_cast<double>(pairs) <= quiet_power) return true;
  return cross >= kMinCorrelation * std::sqrt(power_l * power_r);
}

bool vbi_frame_is_mono(const IObservationContext& context, FrameID frame_id) {
  constexpr auto kMono = static_cast<int32_t>(VbiSoundMode::MONO);
  bool any_mono = false;
  for (uint64_t field = 0; field < 2; ++field) {
    const FieldID field_id(static_cast<FieldID::value_type>(frame_id) * 2 +
                           field);
    // The parity bits cover the audio status digit, so an unchecked word
    // could turn stereo into mono.
    if (get_int(context, field_id, "programme_status_parity_valid")
            .value_or(0) == 0) {
      continue;
    }
    const auto mode = get_int(context, field_id, "programme_status_sound_mode");
    const auto am2_mode =
        get_int(context, field_id, "amendment2_status_sound_mode");
    if (mode == kMono || am2_mode == kMono) {
      any_mono = true;
    } else {
      return false;
    }
  }
  return any_mono;
}

// ============================================================================
// ChannelMappedRepresentation
// ============================================================================

bool ChannelMappedRepresentation::is_active() const {
  const size_t count = source_pair_count();
  if (source_pair_ >= count) return false;
  if (is_route()) {
    return target_is_new_ ? count < kMaxAudioChannelPairs
                          : target_pair_ < count;
  }
  return true;
}

bool ChannelMappedRepresentation::is_result_pair(size_t pair) const {
  if (!is_active()) return false;
  switch (operation_) {
    case AudioChannelMapOperation::kLeftToMono:
    case AudioChannelMapOperation::kRightToMono:
    case AudioChannelMapOperation::kVbiMonoAverage:
      return pair == source_pair_;
    case AudioChannelMapOperation::kCopyLeftToTarget:
    case AudioChannelMapOperation::kCopyRightToTarget:
      return pair == (target_is_new_ ? source_pair_count() : target_pair_);
    case AudioChannelMapOperation::kDelete:
      return false;
  }
  return false;
}

size_t ChannelMappedRepresentation::audio_channel_pair_count() const {
  const size_t count = source_pair_count();
  // An operation the input cannot satisfy leaves the pair count untouched.
  if (!is_active()) return count;
  if (is_delete() && count > 0) return count - 1;
  if (is_append()) return count + 1;
  return count;
}

size_t ChannelMappedRepresentation::source_pair_for(size_t pair) const {
  // Inactive: every output pair maps straight to the same source pair.
  if (!is_active()) return pair;
  // Deleting the source pair shifts every later pair up by one source index.
  if (is_delete()) return pair >= source_pair_ ? pair + 1 : pair;
  // The mono result pair (in place, appended, or overwritten target) always
  // reads from the source pair; every other pair passes through by index.
  if (is_result_pair(pair)) return source_pair_;
  return pair;
}

ChannelMappedRepresentation::ChannelFill
ChannelMappedRepresentation::fill_for_pair(size_t pair) const {
  // The averaging operation decides per frame, in get_audio_samples().
  if (is_mono_average()) return ChannelFill::kNone;
  return is_result_pair(pair) ? mono_fill() : ChannelFill::kNone;
}

void ChannelMappedRepresentation::apply_fill(std::vector<int32_t>& samples,
                                             ChannelFill fill) {
  if (fill == ChannelFill::kNone) return;
  // Interleaved stereo pairs (L, R, L, R, …); ignore a trailing odd sample.
  // SMPTE 272M-1994 §6.4: a mono programme occupies the left channel only; the
  // right channel is silenced (all zeros), never a duplicate of the left.
  const size_t pairs = samples.size() / 2;
  switch (fill) {
    case ChannelFill::kLeftMono:
      for (size_t p = 0; p < pairs; ++p) {
        samples[p * 2 + 1] = 0;
      }
      break;
    case ChannelFill::kRightMono:
      for (size_t p = 0; p < pairs; ++p) {
        samples[p * 2] = samples[p * 2 + 1];
        samples[p * 2 + 1] = 0;
      }
      break;
    case ChannelFill::kNone:
      break;
  }
}

void ChannelMappedRepresentation::average_channels(
    std::vector<int32_t>& samples) {
  const size_t pairs = samples.size() / 2;
  for (size_t p = 0; p < pairs; ++p) {
    // The sum of two 24-bit samples fits easily; the shift rounds down.
    const int32_t mean = (samples[p * 2] + samples[p * 2 + 1]) >> 1;
    samples[p * 2] = mean;
    samples[p * 2 + 1] = mean;
  }
}

bool ChannelMappedRepresentation::frame_is_mono(FrameID id) const {
  if (!source_) return false;
  return mono_probe_ ? mono_probe_(*source_, id)
                     : observe_frame_is_mono(*source_, id);
}

std::optional<AudioChannelPairDescriptor>
ChannelMappedRepresentation::get_audio_channel_pair_descriptor(
    size_t pair) const {
  if (!source_ || pair >= audio_channel_pair_count()) return std::nullopt;
  if (is_result_pair(pair) && is_mono_average()) {
    // Still the source's programme, mono only where the disc says so.
    auto desc = source_->get_audio_channel_pair_descriptor(source_pair_);
    if (desc && override_description_) desc->name = description_;
    return desc;
  }
  if (is_result_pair(pair)) {
    // A mono pair derived from the source pair's chosen channel. Its name is
    // the caller-supplied description when overridden, otherwise the source
    // pair's existing name is kept unchanged.
    auto desc = source_->get_audio_channel_pair_descriptor(source_pair_);
    if (!desc && !override_description_) return std::nullopt;
    AudioChannelPairDescriptor result;
    if (desc) result = *desc;
    if (override_description_) result.name = description_;
    result.origin = AudioOrigin::DERIVED;
    return result;
  }
  return source_->get_audio_channel_pair_descriptor(source_pair_for(pair));
}

std::vector<int32_t> ChannelMappedRepresentation::get_audio_samples(
    size_t pair, FrameID id) const {
  if (!source_ || pair >= audio_channel_pair_count()) return {};
  auto samples = source_->get_audio_samples(source_pair_for(pair), id);
  apply_fill(samples, fill_for_pair(pair));
  // The channel test reads only the samples in hand, so it goes before the
  // status test, which reads the frame's video.
  if (is_mono_average() && is_result_pair(pair) &&
      mono_channels_agree(samples) && frame_is_mono(id)) {
    average_channels(samples);
  }
  return samples;
}

// ============================================================================
// AudioChannelMapStage
// ============================================================================

AudioChannelMapStage::AudioChannelMapStage() {
  // All parameters have complete defaults.
  set_configuration_status(orc::ConfigurationStatus::Green);
}

std::vector<ArtifactPtr> AudioChannelMapStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  (void)observation_context;
  if (inputs.empty()) {
    throw DAGExecutionError("AudioChannelMapStage requires one input");
  }
  auto vfr =
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  if (!vfr) {
    throw DAGExecutionError(
        "AudioChannelMapStage input must be VideoFrameRepresentation");
  }

  if (!parameters.empty()) set_parameters(parameters);

  const auto operation = parse_operation(operation_)
                             .value_or(AudioChannelMapOperation::kLeftToMono);

  // A channel-pair selection the input cannot satisfy (most commonly a source
  // that carries no audio at all) is a configuration mismatch, not a pipeline
  // failure. The stage degrades to a transparent pass-through instead of
  // throwing, because a DAG execution error here blanks the preview of every
  // downstream node — including the video sink — for a fault that only
  // concerns audio.
  const std::string reason =
      inactive_reason(vfr->audio_channel_pair_count(), operation);
  if (!reason.empty()) {
    // Executed once per render request, so only log when the reason changes.
    if (reason != last_inactive_reason_) {
      ORC_LOG_WARN(
          "AudioChannelMapStage: {}; passing the input through unchanged",
          reason);
      last_inactive_reason_ = reason;
    }
    cached_output_ = vfr;
    return {inputs[0]};
  }
  last_inactive_reason_.clear();

  auto output = process(vfr);
  cached_output_ = output;

  std::vector<ArtifactPtr> outputs;
  outputs.push_back(std::const_pointer_cast<ChannelMappedRepresentation>(
      std::dynamic_pointer_cast<const ChannelMappedRepresentation>(output)));
  return outputs;
}

std::string AudioChannelMapStage::inactive_reason(
    size_t pair_count, AudioChannelMapOperation operation) const {
  if (static_cast<size_t>(channel_pair_) >= pair_count) {
    return "channel pair " + std::to_string(channel_pair_) +
           " is out of range: the input carries " + std::to_string(pair_count) +
           " audio channel pair(s)";
  }
  if (!is_route_op(operation)) return {};
  if (target_pair_ == kTargetNew) {
    // Appending a pair must not exceed the pipeline's channel-pair cap; the
    // limit is checked here, not just at the container sink, so a DAG that
    // previews also exports.
    if (pair_count >= kMaxAudioChannelPairs) {
      return "cannot append a channel pair: the input already carries the "
             "maximum of " +
             std::to_string(kMaxAudioChannelPairs) + " audio channel pairs";
    }
    return {};
  }
  const auto target = parse_pair_index(target_pair_);
  if (!target || static_cast<size_t>(*target) >= pair_count) {
    return "target channel pair '" + target_pair_ +
           "' is out of range: the input carries " +
           std::to_string(pair_count) + " audio channel pair(s)";
  }
  return {};
}

std::shared_ptr<const VideoFrameRepresentation> AudioChannelMapStage::process(
    std::shared_ptr<const VideoFrameRepresentation> source) const {
  if (!source) return nullptr;
  const auto operation = parse_operation(operation_)
                             .value_or(AudioChannelMapOperation::kLeftToMono);
  const bool target_is_new = (target_pair_ == kTargetNew);
  const size_t target_index =
      target_is_new
          ? 0
          : static_cast<size_t>(parse_pair_index(target_pair_).value_or(0));
  return std::make_shared<ChannelMappedRepresentation>(
      std::move(source), static_cast<size_t>(channel_pair_), operation,
      target_is_new, target_index, set_description_, description_, mono_probe_);
}

std::vector<ParameterDescriptor>
AudioChannelMapStage::get_parameter_descriptors(VideoSystem project_format,
                                                SourceType source_type) const {
  (void)project_format;
  (void)source_type;
  std::vector<ParameterDescriptor> descriptors;

  {
    ParameterDescriptor desc;
    desc.name = "channel_pair";
    desc.display_name = "Channel pair";
    desc.description =
        "The channel pair the operation reads from — the source for the copy "
        "operations, or the pair modified/deleted for the others (0-based, "
        "matching the CVBS container channel-pair numbering). The GUI "
        "restricts "
        "the choices to the channel pairs the input actually carries.";
    desc.type = ParameterType::STRING;
    for (size_t p = 0; p < kMaxAudioChannelPairs; ++p) {
      desc.constraints.allowed_strings.push_back(std::to_string(p));
    }
    desc.constraints.default_value = std::string("0");
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "operation";
    desc.display_name = "Operation";
    desc.description =
        "Channel routing operation: 'delete' removes the channel pair (later "
        "pairs shift down one index); 'left_to_mono' / 'right_to_mono' extract "
        "the left or right channel as SMPTE 272M mono in place (chosen channel "
        "on the left, right silenced); 'copy_left_to_target' / "
        "'copy_right_to_target' copy that mono channel to the target pair, "
        "leaving the source untouched; 'vbi_mono_average' replaces both "
        "channels with their mean on the frames whose VBI programme status "
        "says the sound is mono and whose two channels carry the same "
        "programme, and leaves every other frame untouched.";
    desc.type = ParameterType::STRING;
    desc.constraints.default_value = std::string(kOpLeftToMono);
    desc.constraints.allowed_strings = {
        kOpDelete,           kOpLeftToMono,        kOpRightToMono,
        kOpCopyLeftToTarget, kOpCopyRightToTarget, kOpVbiMonoAverage};
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "target_pair";
    desc.display_name = "Target channel pair";
    desc.description =
        "Destination for the copy operations: 'new' appends a new channel pair "
        "(subject to the 8-pair limit), otherwise the mono channel overwrites "
        "the chosen existing pair. The GUI offers 'new' plus the pairs the "
        "input carries.";
    desc.type = ParameterType::STRING;
    desc.constraints.allowed_strings.push_back(kTargetNew);
    for (size_t p = 0; p < kMaxAudioChannelPairs; ++p) {
      desc.constraints.allowed_strings.push_back(std::to_string(p));
    }
    desc.constraints.default_value = std::string(kTargetNew);
    // Only relevant to the copy operations; hidden otherwise.
    desc.constraints.depends_on =
        ParameterDependency{"operation",
                            {kOpCopyLeftToTarget, kOpCopyRightToTarget},
                            /*hide_when_disabled=*/true};
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "set_description";
    desc.display_name = "Add description";
    desc.description =
        "When enabled, give the channel pair the operation produces a new "
        "description; otherwise the pair keeps its existing description.";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    // Not meaningful for delete (which produces no pair); hidden otherwise.
    desc.constraints.depends_on =
        ParameterDependency{"operation",
                            {kOpLeftToMono, kOpRightToMono, kOpCopyLeftToTarget,
                             kOpCopyRightToTarget, kOpVbiMonoAverage},
                            /*hide_when_disabled=*/true};
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "description";
    desc.display_name = "Description";
    desc.description =
        "Name for the channel pair the operation produces — the target pair "
        "for the copy operations, or the source pair for the in-place mono "
        "operations (e.g. \"English language\"). Shown only when 'Add "
        "description' is enabled.";
    desc.type = ParameterType::STRING;
    desc.constraints.default_value = std::string("");
    // Shown only when the 'Add description' checkbox is enabled.
    desc.constraints.depends_on = ParameterDependency{
        "set_description", {"true"}, /*hide_when_disabled=*/true};
    descriptors.push_back(desc);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> AudioChannelMapStage::get_parameters()
    const {
  return {{"channel_pair", std::to_string(channel_pair_)},
          {"operation", operation_},
          {"target_pair", target_pair_},
          {"set_description", set_description_},
          {"description", description_}};
}

bool AudioChannelMapStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key == "channel_pair") {
      const auto* v = std::get_if<std::string>(&value);
      if (!v) {
        ORC_LOG_ERROR("AudioChannelMapStage: channel_pair must be a string");
        return false;
      }
      const auto parsed = parse_pair_index(*v);
      if (!parsed) {
        ORC_LOG_ERROR("AudioChannelMapStage: invalid channel_pair '{}'", *v);
        return false;
      }
      channel_pair_ = *parsed;
    } else if (key == "operation") {
      const auto* v = std::get_if<std::string>(&value);
      if (!v || !parse_operation(*v)) {
        ORC_LOG_ERROR("AudioChannelMapStage: unknown operation '{}'",
                      v ? *v : std::string("<non-string>"));
        return false;
      }
      operation_ = *v;
    } else if (key == "target_pair") {
      const auto* v = std::get_if<std::string>(&value);
      if (!v || (*v != kTargetNew && !parse_pair_index(*v))) {
        ORC_LOG_ERROR("AudioChannelMapStage: invalid target_pair '{}'",
                      v ? *v : std::string("<non-string>"));
        return false;
      }
      target_pair_ = *v;
    } else if (key == "set_description") {
      const auto* v = std::get_if<bool>(&value);
      if (!v) {
        ORC_LOG_ERROR("AudioChannelMapStage: set_description must be a bool");
        return false;
      }
      set_description_ = *v;
    } else if (key == "description") {
      const auto* v = std::get_if<std::string>(&value);
      if (!v) {
        ORC_LOG_ERROR("AudioChannelMapStage: description must be a string");
        return false;
      }
      description_ = *v;
    } else {
      ORC_LOG_WARN("AudioChannelMapStage: unknown parameter '{}'", key);
      return false;
    }
  }
  return true;
}

StagePreviewCapability AudioChannelMapStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

}  // namespace orc
