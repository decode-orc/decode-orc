/*
 * File:        dropout_map_stage.cpp
 * Module:      orc-core
 * Purpose:     Dropout map stage implementation (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dropout_map_stage.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/error_types.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>

namespace orc {

// ============================================================================
// DropoutMappedFrameRepresentation
// ============================================================================

DropoutMappedFrameRepresentation::DropoutMappedFrameRepresentation(
    std::shared_ptr<const VideoFrameRepresentation> source,
    const std::map<uint64_t, FrameDropoutMapEntry>& dropout_map)
    : VideoFrameRepresentationWrapper(std::move(source)),
      Artifact(ArtifactID("dropout_map"), Provenance{}),
      dropout_map_(dropout_map) {}

std::optional<DropoutRun> DropoutMappedFrameRepresentation::entry_to_run(
    VideoSystem sys, int32_t nominal_spl, FrameID frame_id,
    const DropoutEntrySpec& entry) {
  if (entry.end_sample < entry.start_sample) return std::nullopt;
  const uint64_t start =
      static_cast<uint64_t>(frame_line_sample_offset(
          sys, static_cast<size_t>(nominal_spl), entry.line)) +
      entry.start_sample;
  const uint64_t count =
      static_cast<uint64_t>(entry.end_sample - entry.start_sample) + 1;
  return DropoutRun{frame_id, start, static_cast<uint32_t>(count),
                    uint8_t{128}};
}

std::vector<DropoutRun> DropoutMappedFrameRepresentation::get_dropout_hints(
    FrameID id) const {
  std::vector<DropoutRun> result;
  if (source_) result = source_->get_dropout_hints(id);

  auto it = dropout_map_.find(id);
  if (it == dropout_map_.end()) return result;

  const FrameDropoutMapEntry& entry = it->second;

  // Determine VideoSystem and nominal samples per line for coordinate
  // conversion.
  VideoSystem sys = VideoSystem::PAL;
  int32_t nominal_spl = kPalSamplesPerLineNominal;  // 1135 for PAL
  if (source_) {
    auto params = source_->get_video_parameters();
    if (params) {
      sys = params->system;
      nominal_spl = params->frame_width_nominal;
    }
  }

  ORC_LOG_DEBUG(
      "DropoutMappedFrameRepresentation::get_dropout_hints - frame {} has "
      "{} source hints, {} additions, {} removals",
      id, result.size(), entry.additions.size(), entry.removals.size());

  // Apply removals: remove any source run whose range overlaps a removal spec.
  for (const auto& rem : entry.removals) {
    const uint64_t rem_start =
        static_cast<uint64_t>(frame_line_sample_offset(
            sys, static_cast<size_t>(nominal_spl), rem.line)) +
        rem.start_sample;
    const uint64_t rem_end =
        static_cast<uint64_t>(frame_line_sample_offset(
            sys, static_cast<size_t>(nominal_spl), rem.line)) +
        rem.end_sample;

    // Note: run.frame_id is deliberately not checked — the runs were fetched
    // for this frame, and upstream wrappers (frame_map, stacker) do not all
    // preserve the frame_id field.
    result.erase(std::remove_if(result.begin(), result.end(),
                                [&](const DropoutRun& run) {
                                  const uint64_t run_end =
                                      run.sample_start + run.sample_count - 1;
                                  return !(run_end < rem_start ||
                                           run.sample_start > rem_end);
                                }),
                 result.end());
  }

  // Apply additions.
  for (const auto& add : entry.additions) {
    auto run = entry_to_run(sys, nominal_spl, id, add);
    if (run) result.push_back(*run);
  }

  // Sort by sample_start for consistency.
  std::sort(result.begin(), result.end(),
            [](const DropoutRun& a, const DropoutRun& b) {
              return a.sample_start < b.sample_start;
            });

  return result;
}

// ============================================================================
// DropoutMapStage
// ============================================================================

DropoutMapStage::DropoutMapStage() {
  // Yellow, not Red: the stage runs fine with an empty map (pass-through).
  set_configuration_status(orc::ConfigurationStatus::Yellow);
}

std::vector<ArtifactPtr> DropoutMapStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  (void)observation_context;
  if (inputs.size() != 1) {
    throw DAGExecutionError("DropoutMapStage requires exactly one input");
  }

  auto source =
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  if (!source) {
    throw DAGExecutionError(
        "DropoutMapStage input must be VideoFrameRepresentation");
  }

  if (!parameters.empty()) set_parameters(parameters);

  std::map<uint64_t, FrameDropoutMapEntry> dropout_map =
      parse_dropout_map(dropout_map_str_);
  ORC_LOG_DEBUG("DropoutMapStage: {} frame mappings loaded",
                dropout_map.size());

  auto mapped =
      std::make_shared<DropoutMappedFrameRepresentation>(source, dropout_map);
  cached_output_ = mapped;

  std::vector<ArtifactPtr> out;
  out.push_back(std::const_pointer_cast<DropoutMappedFrameRepresentation>(
      std::dynamic_pointer_cast<const DropoutMappedFrameRepresentation>(
          mapped)));
  return out;
}

// ============================================================================
// Parameter Interface
// ============================================================================

std::vector<ParameterDescriptor> DropoutMapStage::get_parameter_descriptors(
    VideoSystem, SourceType) const {
  ParameterDescriptor desc;
  desc.name = "dropout_map";
  desc.display_name = "Dropout Map";
  desc.description =
      "Per-frame dropout overrides: "
      "[{frame:N,add:[{line:L,start:S,end:E}],remove:[...]}] "
      "(frame = 1-based frame number as shown in the preview; stored 0-based "
      "in the project file. line/start/end are frame-flat 0-based)";
  desc.type = ParameterType::STRING;
  desc.constraints.default_value = std::string("[]");
  desc.constraints.required = false;
  return {desc};
}

std::map<std::string, ParameterValue> DropoutMapStage::get_parameters() const {
  return {{"dropout_map", ParameterValue{dropout_map_str_}}};
}

bool DropoutMapStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  if (params.count("dropout_map")) {
    const auto* v = std::get_if<std::string>(&params.at("dropout_map"));
    if (v) dropout_map_str_ = *v;
  }
  const bool has_map = !dropout_map_str_.empty() && dropout_map_str_ != "[]";
  set_configuration_status(has_map ? orc::ConfigurationStatus::Green
                                   : orc::ConfigurationStatus::Yellow);
  return true;
}

// ============================================================================
// Preview
// ============================================================================

StagePreviewCapability DropoutMapStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

// ============================================================================
// The "dropout_map" parameter text
// ============================================================================

std::map<uint64_t, FrameDropoutMapEntry> DropoutMapStage::parse_dropout_map(
    const std::string& map_str) {
  if (dropout_map_text::is_malformed(map_str)) {
    ORC_LOG_ERROR("DropoutMapStage: map must start with '['");
    return {};
  }
  return dropout_map_text::parse(map_str);
}

std::string DropoutMapStage::encode_dropout_map(
    const std::map<uint64_t, FrameDropoutMapEntry>& map) {
  return dropout_map_text::encode(map);
}

// ============================================================================
// Tools
// ============================================================================

std::vector<StageToolDescriptor> DropoutMapStage::get_stage_tools() const {
  return {
      StageToolDescriptor{"dropout_editor", "Dropout Editor",
                          "Mark and remove dropouts by hand, frame by frame",
                          StageToolKind::NonModalEditor, true,
                          dropout_map_text::kDropoutEditorContractId}};
}

}  // namespace orc
