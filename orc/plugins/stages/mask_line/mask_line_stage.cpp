/*
 * File:        mask_line_stage.cpp
 * Module:      orc-core
 * Purpose:     Line masking stage (VFrameR, frame-flat line addressing)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "mask_line_stage.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/error_types.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <sstream>

#include "../common/config-tools/stage_config_wire.h"

namespace orc {

// ============================================================================
// MaskedFrameRepresentation
// ============================================================================

MaskedFrameRepresentation::MaskedFrameRepresentation(
    std::shared_ptr<const VideoFrameRepresentation> source,
    const std::string& line_spec, int32_t mask_sample_level)
    : VideoFrameRepresentationWrapper(std::move(source)),
      Artifact(ArtifactID("masked_frame"), Provenance{}),
      mask_sample_level_(mask_sample_level) {
  parse_line_spec(line_spec);
  // The longest frame line any system has: PAL's nominal 1135 samples plus
  // the extra samples a few of its lines carry.
  size_t longest = 0;
  for (const VideoSystem system :
       {VideoSystem::PAL, VideoSystem::NTSC, VideoSystem::PAL_M}) {
    const auto spl = static_cast<size_t>(samples_per_line_from_system(system));
    for (size_t line = 0; line < 625; ++line) {
      longest = std::max(longest, frame_line_sample_count(system, spl, line));
    }
  }
  mask_line_samples_.assign(longest, static_cast<sample_type>(std::clamp(
                                         mask_sample_level_, 0, 1023)));
}

void MaskedFrameRepresentation::parse_line_spec(const std::string& spec) {
  line_ranges_.clear();
  if (spec.empty()) return;

  std::istringstream ss(spec);
  std::string token;
  while (std::getline(ss, token, ',')) {
    token.erase(0, token.find_first_not_of(" \t"));
    const size_t last = token.find_last_not_of(" \t");
    if (last == std::string::npos) continue;
    token = token.substr(0, last + 1);

    const size_t dash = token.find('-');
    if (dash != std::string::npos) {
      try {
        const size_t start = std::stoul(token.substr(0, dash));
        const size_t end = std::stoul(token.substr(dash + 1));
        line_ranges_.push_back({start, end});
      } catch (...) {
        ORC_LOG_WARN("MaskLine: invalid range '{}'", token);
      }
    } else {
      try {
        const size_t line = std::stoul(token);
        line_ranges_.push_back({line, line});
      } catch (...) {
        ORC_LOG_WARN("MaskLine: invalid line '{}'", token);
      }
    }
  }
}

bool MaskedFrameRepresentation::should_mask_line(size_t frame_line) const {
  for (const auto& r : line_ranges_) {
    if (frame_line >= r.start && frame_line <= r.end) return true;
  }
  return false;
}

const MaskedFrameRepresentation::sample_type*
MaskedFrameRepresentation::get_line(FrameID id, size_t line) const {
  if (!source_) return nullptr;
  if (!should_mask_line(line)) return source_->get_line(id, line);
  if (!source_->get_video_parameters().has_value()) return nullptr;
  // Every masked line is the mask level throughout; the shared buffer is at
  // least as long as any line.
  return mask_line_samples_.data();
}

std::vector<MaskedFrameRepresentation::sample_type>
MaskedFrameRepresentation::get_frame_copy(FrameID id) const {
  if (!source_) return {};
  auto desc = source_->get_frame_descriptor(id);
  auto params = source_->get_video_parameters();
  if (!desc || !params) return {};

  std::vector<sample_type> result;
  result.reserve(desc->samples_total);
  const size_t spl =
      static_cast<size_t>(samples_per_line_from_system(params->system));
  for (size_t line = 0; line < desc->height; ++line) {
    const size_t w = frame_line_sample_count(params->system, spl, line);
    const sample_type* ptr = get_line(id, line);
    if (ptr) {
      result.insert(result.end(), ptr, ptr + w);
    } else {
      result.insert(result.end(), w, sample_type{0});
    }
  }
  return result;
}

const MaskedFrameRepresentation::sample_type*
MaskedFrameRepresentation::get_line_luma(FrameID id, size_t line) const {
  if (!source_ || !source_->has_separate_channels()) return nullptr;
  if (!should_mask_line(line)) return source_->get_line_luma(id, line);
  if (!source_->get_video_parameters().has_value()) return nullptr;
  return mask_line_samples_.data();
}

const MaskedFrameRepresentation::sample_type*
MaskedFrameRepresentation::get_line_chroma(FrameID id, size_t line) const {
  if (!source_ || !source_->has_separate_channels()) return nullptr;
  if (!should_mask_line(line)) return source_->get_line_chroma(id, line);
  if (!source_->get_video_parameters().has_value()) return nullptr;
  return mask_line_samples_.data();
}

const MaskedFrameRepresentation::sample_type*
MaskedFrameRepresentation::source_plane(FrameID id, Plane plane) const {
  return plane == Plane::kComposite ? source_->get_frame(id)
         : plane == Plane::kLuma    ? source_->get_frame_luma(id)
                                    : source_->get_frame_chroma(id);
}

const MaskedFrameRepresentation::sample_type*
MaskedFrameRepresentation::masked_plane(FrameID id, Plane plane,
                                        PlaneCache& cache) const {
  Frame frame = cache.get(id, [&]() -> Frame {
    auto desc = source_->get_frame_descriptor(id);
    auto params = source_->get_video_parameters();
    // Fetched last: the source's pointer is valid until this thread's next
    // call on it.
    const sample_type* src = source_plane(id, plane);
    if (!desc || !params || !src) return nullptr;

    const size_t spl =
        static_cast<size_t>(samples_per_line_from_system(params->system));
    auto result = std::make_shared<std::vector<sample_type>>();
    result->reserve(desc->samples_total);
    size_t offset = 0;
    for (size_t line = 0; line < desc->height; ++line) {
      const size_t w = frame_line_sample_count(params->system, spl, line);
      if (should_mask_line(line)) {
        result->insert(result->end(), w, mask_line_samples_.front());
      } else {
        result->insert(result->end(), src + offset, src + offset + w);
      }
      offset += w;
    }
    return result;
  });
  // A frame that could not be masked is the source's own.
  if (!frame) return source_plane(id, plane);
  const sample_type* data = frame->data();
  return cache.pin(std::move(frame), data);
}

// Each plane is pinned under its own owner key, so a caller holding the
// luma and chroma pointers of one frame keeps both.
const MaskedFrameRepresentation::sample_type*
MaskedFrameRepresentation::get_frame(FrameID id) const {
  if (!source_) return nullptr;
  if (line_ranges_.empty()) return source_->get_frame(id);
  return masked_plane(id, Plane::kComposite, masked_frames_);
}

const MaskedFrameRepresentation::sample_type*
MaskedFrameRepresentation::get_frame_luma(FrameID id) const {
  if (!source_ || !source_->has_separate_channels()) return nullptr;
  if (line_ranges_.empty()) return source_->get_frame_luma(id);
  return masked_plane(id, Plane::kLuma, masked_luma_frames_);
}

const MaskedFrameRepresentation::sample_type*
MaskedFrameRepresentation::get_frame_chroma(FrameID id) const {
  if (!source_ || !source_->has_separate_channels()) return nullptr;
  if (line_ranges_.empty()) return source_->get_frame_chroma(id);
  return masked_plane(id, Plane::kChroma, masked_chroma_frames_);
}

// ============================================================================
// MaskLineStage
// ============================================================================

MaskLineStage::MaskLineStage() {
  set_configuration_status(orc::ConfigurationStatus::Yellow);
}

std::vector<ArtifactPtr> MaskLineStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  (void)observation_context;
  if (inputs.empty()) {
    throw DAGExecutionError("MaskLineStage requires one input");
  }
  auto input = inputs[0];
  if (!input) {
    throw DAGExecutionError("MaskLineStage received null input");
  }

  auto vfr = std::dynamic_pointer_cast<const VideoFrameRepresentation>(input);
  if (!vfr) {
    throw DAGExecutionError(
        "MaskLineStage input must be VideoFrameRepresentation");
  }

  if (!parameters.empty()) {
    const_cast<MaskLineStage*>(this)->set_parameters(parameters);
  }

  auto output = process(vfr);
  cached_output_ = output;

  std::vector<ArtifactPtr> outputs;
  outputs.push_back(std::dynamic_pointer_cast<Artifact>(
      std::const_pointer_cast<VideoFrameRepresentation>(output)));
  return outputs;
}

std::shared_ptr<const VideoFrameRepresentation> MaskLineStage::process(
    std::shared_ptr<const VideoFrameRepresentation> source) const {
  if (!source) return nullptr;
  if (line_spec_.empty()) return source;  // pass-through
  return std::make_shared<MaskedFrameRepresentation>(source, line_spec_,
                                                     mask_sample_level_);
}

std::vector<ParameterDescriptor> MaskLineStage::get_parameter_descriptors(
    VideoSystem, SourceType) const {
  return {
      ParameterDescriptor{
          "lineSpec", "Line Specification",
          "Frame-flat line numbers to mask, 1-based as shown in the Mask Line "
          "tool (stored 0-based in the project file). Format: LINE or "
          "START-END, comma-separated. Examples: '22' (NTSC CC line), "
          "'101-116', '22,335'.",
          ParameterType::STRING,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue{std::string("")},
                               {},
                               false,
                               std::nullopt}},
      ParameterDescriptor{
          "maskSampleLevel", "Mask Sample Level",
          "10-bit sample level for masked pixels (0=sync tip; PAL "
          "256=blanking, "
          "844=white; NTSC/PAL-M 240=blanking, 282=black, 800=white).",
          ParameterType::INT32,
          ParameterConstraints{ParameterValue{int32_t{0}},
                               ParameterValue{int32_t{1023}},
                               ParameterValue{int32_t{0}},
                               {},
                               false,
                               std::nullopt}}};
}

std::map<std::string, ParameterValue> MaskLineStage::get_parameters() const {
  return {{"lineSpec", line_spec_}, {"maskSampleLevel", mask_sample_level_}};
}

bool MaskLineStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key == "lineSpec" && std::holds_alternative<std::string>(value)) {
      line_spec_ = std::get<std::string>(value);
    } else if (key == "maskSampleLevel" &&
               std::holds_alternative<int32_t>(value)) {
      mask_sample_level_ =
          std::clamp(std::get<int32_t>(value), int32_t{0}, int32_t{1023});
    }
  }
  set_configuration_status(line_spec_.empty()
                               ? orc::ConfigurationStatus::Yellow
                               : orc::ConfigurationStatus::Green);
  return true;
}

StagePreviewCapability MaskLineStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

std::vector<StageToolDescriptor> MaskLineStage::get_stage_tools() const {
  return {StageToolDescriptor{"mask_line_config", "Mask Line Config",
                              "Open mask-line helper dialog for line presets",
                              StageToolKind::ConfigDialog, false,
                              config_wire::kMaskLineContractId}};
}

namespace {

config_wire::System wire_system(VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
      return config_wire::System::PAL;
    case VideoSystem::NTSC:
      return config_wire::System::NTSC;
    case VideoSystem::PAL_M:
      return config_wire::System::PAL_M;
    default:
      return config_wire::System::Unknown;
  }
}

}  // namespace

StageToolReply MaskLineStage::run_tool_job(const std::string& contract_id,
                                           const std::string& verb,
                                           const std::vector<uint8_t>& payload,
                                           IToolJobContext& context) {
  (void)payload;
  StageToolReply reply;
  if (contract_id != config_wire::kMaskLineContractId ||
      verb != config_wire::kSignalVerb) {
    reply.error = "Mask Line does not run '" + verb + "' for " + contract_id;
    return reply;
  }
  const auto& inputs = context.inputs();
  const auto vfr =
      inputs.empty()
          ? nullptr
          : std::dynamic_pointer_cast<const VideoFrameRepresentation>(
                inputs.front());
  if (!vfr) {
    reply.error = "Mask Line has no video input";
    return reply;
  }
  config_wire::Signal signal;
  if (const auto params = vfr->get_video_parameters()) {
    signal.system = wire_system(params->system);
    signal.blanking_level = params->blanking_level;
    signal.white_level = params->white_level;
  }
  reply.data = config_wire::encode(signal);
  reply.ok = true;
  return reply;
}

}  // namespace orc
