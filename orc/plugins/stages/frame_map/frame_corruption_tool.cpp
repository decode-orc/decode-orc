/*
 * File:        frame_corruption_tool.cpp
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     The Frame Map's Frame Corruption Generator: a frame map with
 *              skips, repeats and gaps, to test the Disc Mapper against
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "frame_corruption_tool.h"

#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "frame_corruption_analyzer.h"
#include "frame_map_spec_text.h"

namespace orc::frame_corruption_tool {

namespace {

constexpr char kPattern[] = "pattern";
constexpr char kRegenerateSeed[] = "regenerate_seed";

// Longer specs are cut short in the report; Apply writes the whole spec.
constexpr size_t kSpecPreviewChars = 500;

StageAnalysisReport failed(std::string why) {
  StageAnalysisReport report;
  report.status = StageAnalysisReport::Status::Failed;
  report.summary = std::move(why);
  return report;
}

std::string text_parameter(const std::map<std::string, ParameterValue>& params,
                           const char* name, std::string fallback) {
  const auto it = params.find(name);
  if (it != params.end()) {
    if (const auto* value = std::get_if<std::string>(&it->second)) {
      return *value;
    }
  }
  return fallback;
}

bool bool_parameter(const std::map<std::string, ParameterValue>& params,
                    const char* name) {
  const auto it = params.find(name);
  if (it != params.end()) {
    if (const auto* value = std::get_if<bool>(&it->second)) {
      return *value;
    }
  }
  return false;
}

/// The spec as the report shows it: whole when short, else its start.
void write_spec_preview(std::ostringstream& out,
                        const std::string& display_spec,
                        const char* apply_hint) {
  if (display_spec.length() > kSpecPreviewChars) {
    out << display_spec.substr(0, kSpecPreviewChars) << "...\n\n";
    out << "(Full specification: " << display_spec.length() << " characters)\n";
    out << apply_hint << "\n";
  } else {
    out << display_spec << "\n";
  }
}

}  // namespace

std::vector<ParameterDescriptor> parameters() {
  ParameterDescriptor pattern;
  pattern.name = kPattern;
  pattern.display_name = "Corruption Pattern";
  pattern.description = "Type of corruption pattern to generate";
  pattern.type = ParameterType::STRING;
  pattern.constraints.allowed_strings = {
      "simple-skip",  "simple-repeat", "skip-with-gap", "heavy-skip",
      "heavy-repeat", "mixed-light",   "mixed-heavy"};
  pattern.constraints.default_value = std::string("simple-skip");
  pattern.constraints.required = true;

  ParameterDescriptor regenerate;
  regenerate.name = kRegenerateSeed;
  regenerate.display_name = "Regenerate Seed";
  regenerate.description =
      "Generate a new random seed (unchecked = reuse the seed of the pattern "
      "last applied to the node, for reproducibility; a node with none gets "
      "a new one either way)";
  regenerate.type = ParameterType::BOOL;
  regenerate.constraints.default_value = false;
  regenerate.constraints.required = false;

  return {pattern, regenerate};
}

StageAnalysisReport run(const std::map<std::string, ParameterValue>& params,
                        IToolJobContext& context, int32_t stored_seed,
                        const std::function<uint32_t()>& new_seed) {
  context.report_progress(0.0, "Initializing corruption generator...");

  uint64_t frame_count = 0;
  const auto& inputs = context.inputs();
  if (!inputs.empty()) {
    if (auto source = std::dynamic_pointer_cast<const VideoFrameRepresentation>(
            inputs[0])) {
      frame_count = source->frame_count();
    }
  }
  if (frame_count == 0) {
    return failed(
        "Cannot determine input frame count.\n\n"
        "Frame Corruption Generator requires a VideoFrameRepresentation "
        "input.\n"
        "Ensure this frame_map stage has an input connection.");
  }

  const std::string pattern_name =
      text_parameter(params, kPattern, "simple-skip");
  const bool regenerate_seed = bool_parameter(params, kRegenerateSeed);

  const uint32_t seed = (stored_seed != 0 && !regenerate_seed)
                            ? static_cast<uint32_t>(stored_seed)
                            : new_seed();

  const auto patterns = FrameCorruptionAnalyzer::get_all_patterns();
  const auto pattern_it =
      std::find_if(patterns.begin(), patterns.end(),
                   [&](const FrameCorruptionAnalyzer::PatternConfig& p) {
                     return p.name == pattern_name;
                   });
  if (pattern_it == patterns.end()) {
    return failed("Unknown corruption pattern: " + pattern_name);
  }

  context.report_progress(0.1, "Generating corruption pattern...");
  FrameCorruptionAnalyzer analyzer(frame_count, *pattern_it, seed);
  const auto generated = analyzer.analyze();
  context.report_progress(0.9, "Formatting results...");

  StageAnalysisReport report;
  report.status = generated.success ? StageAnalysisReport::Status::Success
                                    : StageAnalysisReport::Status::Failed;

  std::ostringstream summary;
  summary << "Pattern: " << pattern_it->name << "\n";
  summary << "Description: " << pattern_it->description << "\n";
  summary << "Input frames: " << frame_count << "\n";
  summary << "Seed: " << seed;
  if (stored_seed != 0) {
    summary << (regenerate_seed ? " (regenerated)" : " (reused)");
  } else {
    summary << " (new)";
  }
  summary << "\n\n";

  // Shown 1-based to match the Frame Map parameter dialog; the applied spec
  // stays 0-based.
  const std::string display_spec =
      frame_map_spec_text::to_presentation(generated.mapping_spec);
  summary << "==================================================\n";
  summary << "Generated Frame Mapping Specification:\n";
  summary << "==================================================\n";
  write_spec_preview(summary, display_spec,
                     "This will be applied to the 'ranges' parameter when you "
                     "click 'Apply to Node'");
  report.summary = summary.str();

  const auto& stats = generated.stats;
  report.statistics["normalFrames"] = static_cast<int64_t>(stats.normal_frames);
  report.statistics["repeatedFrames"] =
      static_cast<int64_t>(stats.repeated_frames);
  report.statistics["skippedFrames"] =
      static_cast<int64_t>(stats.skipped_frames);
  report.statistics["gapMarkers"] = static_cast<int64_t>(stats.gap_markers);
  report.statistics["totalOutputFrames"] =
      static_cast<int64_t>(stats.total_output_frames);
  report.statistics["patternName"] = pattern_it->name;
  report.statistics["seed"] = static_cast<int64_t>(seed);

  // The spec first, so it is at the top of the list.
  StageAnalysisItem spec_item;
  spec_item.type = "info";
  std::ostringstream spec_message;
  spec_message << "==================================================\n";
  spec_message << "Generated Frame Mapping Specification\n";
  spec_message << "==================================================\n\n";
  write_spec_preview(spec_message, display_spec,
                     "Click 'Apply to Node' to use this specification");
  spec_item.message = spec_message.str();
  report.items.push_back(std::move(spec_item));

  if (!generated.events.empty()) {
    StageAnalysisItem separator;
    separator.type = "info";
    separator.message =
        "\n==================================================\nCorruption "
        "Events Applied\n==================================================\n";
    report.items.push_back(std::move(separator));
  }
  for (const auto& event : generated.events) {
    StageAnalysisItem item;
    item.type =
        (event.type == FrameCorruptionAnalyzer::CorruptionEvent::SKIP) ? "skip"
        : (event.type == FrameCorruptionAnalyzer::CorruptionEvent::REPEAT)
            ? "repeat"
            : "gap";
    item.message = event.to_string();
    item.start_frame = static_cast<int64_t>(event.start_frame);
    item.end_frame = static_cast<int64_t>(event.end_frame);
    item.data["count"] = static_cast<int64_t>(event.count);
    report.items.push_back(std::move(item));
  }

  if (report.status == StageAnalysisReport::Status::Success) {
    report.parameter_changes["ranges"] = generated.mapping_spec;
    report.parameter_changes[kSeedParameter] = static_cast<int32_t>(seed);
  }

  context.report_progress(1.0, "Complete");
  ORC_LOG_DEBUG("Frame Corruption Generator: {} events, {} output frames",
                generated.events.size(), stats.total_output_frames);
  return report;
}

}  // namespace orc::frame_corruption_tool
