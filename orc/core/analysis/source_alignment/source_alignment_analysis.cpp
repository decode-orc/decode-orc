/*
 * File:        source_alignment_analysis.cpp
 * Module:      orc-core
 * Purpose:     Source alignment analysis tool implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "source_alignment_analysis.h"

#include <biphase_observer.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>

#include "../../../plugins/stages/source_align/source_align_stage.h"
#include "../../include/dag_executor.h"
#include "../../include/project.h"
#include "../analysis_registry.h"

namespace orc {

// Register the tool
REGISTER_ANALYSIS_TOOL(SourceAlignmentAnalysisTool)

void force_link_SourceAlignmentAnalysisTool() {}

std::string SourceAlignmentAnalysisTool::id() const {
  return "source_alignment";
}

std::string SourceAlignmentAnalysisTool::name() const {
  return "Source Alignment Analysis";
}

std::string SourceAlignmentAnalysisTool::description() const {
  return "Analyzes multiple sources to determine optimal alignment based on "
         "VBI frame numbers or CLV timecodes";
}

std::string SourceAlignmentAnalysisTool::category() const {
  return "Source Processing";
}

std::vector<ParameterDescriptor> SourceAlignmentAnalysisTool::parameters()
    const {
  return {ParameterDescriptor{
      "alignmentMode", "Alignment Mode",
      "How to align sources: 'pad_for_alignment' prepends synthetic padding "
      "frames so all sources start from the earliest VBI frame available "
      "across all sources, and pads every output at the tail to the longest "
      "one; 'first_common_frame' aligns all sources to the "
      "first VBI frame that is common to every source (trims leading frames).",
      ParameterType::STRING,
      ParameterConstraints{std::nullopt,
                           std::nullopt,
                           ParameterValue{std::string("pad_for_alignment")},
                           {"first_common_frame", "pad_for_alignment"},
                           false,
                           std::nullopt}}};
}

bool SourceAlignmentAnalysisTool::canAnalyze(
    AnalysisSourceType source_type) const {
  // Can analyze laserdisc sources
  return source_type == AnalysisSourceType::LaserDisc;
}

bool SourceAlignmentAnalysisTool::isApplicableToStage(
    const std::string& stage_name) const {
  // Source alignment analysis is only applicable to source_align stages
  return stage_name == "source_align";
}

/**
 * @brief Get VBI frame number or CLV timecode frame equivalent for a field
 *
 * ARCHITECTURAL NOTE: This function queries the ObservationContext for VBI
 * data. Observations must be populated by running BiphaseObserver on the field
 * first.
 *
 * @param observation_context The observation context containing VBI data
 * @param field_id The field to query
 * @param is_pal Whether the source is PAL (used for CLV timecode conversion)
 * @return Frame number if found, -1 otherwise
 */
static int32_t get_frame_number_from_vbi(
    const ObservationContext& observation_context, FieldID field_id,
    bool is_pal) {
  // Check for CAV picture number (preferred)
  auto picture_num = observation_context.get(field_id, "vbi", "picture_number");
  if (picture_num && std::holds_alternative<int32_t>(*picture_num)) {
    return std::get<int32_t>(*picture_num);
  }

  // Check for CLV timecode (need to query all components)
  auto hours_opt =
      observation_context.get(field_id, "vbi", "clv_timecode_hours");
  auto minutes_opt =
      observation_context.get(field_id, "vbi", "clv_timecode_minutes");
  auto seconds_opt =
      observation_context.get(field_id, "vbi", "clv_timecode_seconds");
  auto picture_opt =
      observation_context.get(field_id, "vbi", "clv_timecode_picture");

  if (hours_opt && minutes_opt && seconds_opt && picture_opt) {
    if (std::holds_alternative<int32_t>(*hours_opt) &&
        std::holds_alternative<int32_t>(*minutes_opt) &&
        std::holds_alternative<int32_t>(*seconds_opt) &&
        std::holds_alternative<int32_t>(*picture_opt)) {
      int32_t hours = std::get<int32_t>(*hours_opt);
      int32_t minutes = std::get<int32_t>(*minutes_opt);
      int32_t seconds = std::get<int32_t>(*seconds_opt);
      int32_t picture_number = std::get<int32_t>(*picture_opt);

      // Convert CLV timecode to frame number
      int32_t fps = is_pal ? 25 : 30;
      int32_t frame_num = hours * 3600 * fps + minutes * 60 * fps +
                          seconds * fps + picture_number;
      return frame_num;
    }
  }

  return -1;  // No VBI frame number found
}

AnalysisResult SourceAlignmentAnalysisTool::analyze(
    const AnalysisContext& ctx, AnalysisProgress* progress) {
  AnalysisResult result;

  if (progress) {
    progress->setStatus("Initializing source alignment analysis...");
    progress->setProgress(0);
  }

  // Get the source_align node's inputs from the DAG
  if (!ctx.dag || !ctx.project) {
    result.status = AnalysisResult::Failed;
    result.summary = "No DAG or project provided for analysis";
    ORC_LOG_ERROR(
        "Source alignment analysis requires DAG and project in context");
    return result;
  }

  // Find the source_align node in the DAG
  const auto& dag_nodes = ctx.dag->nodes();
  auto node_it = std::find_if(
      dag_nodes.begin(), dag_nodes.end(),
      [&ctx](const DAGNode& node) { return node.node_id == ctx.node_id; });

  if (node_it == dag_nodes.end()) {
    result.status = AnalysisResult::Failed;
    result.summary = "Source align node not found in DAG";
    ORC_LOG_ERROR("Node '{}': Not found in DAG", ctx.node_id);
    return result;
  }

  // Get all input node IDs
  const auto& input_node_ids = node_it->input_node_ids;

  if (input_node_ids.empty()) {
    result.status = AnalysisResult::Failed;
    result.summary = "Source align node has no inputs";
    ORC_LOG_ERROR("Node '{}': No input nodes", ctx.node_id);
    return result;
  }

  if (progress) {
    progress->setStatus("Executing DAG to get input sources...");
    progress->setProgress(10);
  }

  // Execute the DAG to get all input sources
  DAGExecutor executor;
  std::vector<std::shared_ptr<VideoFrameRepresentation>> input_sources;

  try {
    for (size_t i = 0; i < input_node_ids.size(); ++i) {
      const auto& input_node_id = input_node_ids[i];
      auto all_outputs = executor.execute_to_node(*ctx.dag, input_node_id);

      // Get the outputs from this input node
      auto output_it = all_outputs.find(input_node_id);
      if (output_it == all_outputs.end() || output_it->second.empty()) {
        result.status = AnalysisResult::Failed;
        result.summary =
            "Input node " + std::to_string(i + 1) + " produced no outputs";
        ORC_LOG_ERROR("Node '{}': Input node '{}' produced no outputs",
                      ctx.node_id, input_node_id);
        return result;
      }

      // Find the VideoFrameRepresentation output
      std::shared_ptr<VideoFrameRepresentation> source;
      for (const auto& artifact : output_it->second) {
        source = std::dynamic_pointer_cast<VideoFrameRepresentation>(artifact);
        if (source) {
          break;
        }
      }

      if (!source) {
        result.status = AnalysisResult::Failed;
        result.summary = "Input node " + std::to_string(i + 1) +
                         " did not produce VideoFrameRepresentation";
        ORC_LOG_ERROR(
            "Node '{}': Input node '{}' did not produce "
            "VideoFrameRepresentation",
            ctx.node_id, input_node_id);
        return result;
      }

      ORC_LOG_DEBUG("Input {}: node_id='{}', frame_count={}, ptr={}", i + 1,
                    input_node_id, source->frame_count(),
                    static_cast<const void*>(source.get()));

      input_sources.push_back(source);

      if (progress && progress->isCancelled()) {
        result.status = AnalysisResult::Cancelled;
        return result;
      }
    }

    ORC_LOG_DEBUG("Got {} input sources for alignment analysis",
                  input_sources.size());

    // Check if all sources are the same object (pointer equality)
    if (input_sources.size() > 1) {
      bool all_same_ptr = true;
      const void* first_ptr = input_sources[0].get();
      for (size_t i = 1; i < input_sources.size(); ++i) {
        if (input_sources[i].get() != first_ptr) {
          all_same_ptr = false;
          break;
        }
      }

      if (all_same_ptr) {
        result.status = AnalysisResult::Failed;
        result.summary = "ERROR: All " + std::to_string(input_sources.size()) +
                         " inputs are the SAME source object";

        AnalysisResult::ResultItem error_item;
        error_item.type = "error";
        error_item.message =
            "All inputs to the source_align node point to the same source "
            "object. "
            "This indicates a configuration problem:\n\n"
            "• Each input should come from a DIFFERENT source (different "
            "captures)\n"
            "• Check that your upstream nodes (field_map stages) are connected "
            "to different sources\n"
            "• The source_align stage is meant to align multiple captures of "
            "the same disc,\n"
            "  not the same capture duplicated multiple times";
        result.items.push_back(error_item);

        ORC_LOG_ERROR(
            "All {} inputs are the same object - this is a configuration "
            "error!",
            input_sources.size());
        return result;
      }
    }

    if (progress) {
      progress->setStatus("Finding first common VBI frame across sources...");
      progress->setProgress(30);
    }

    // Structure to track VBI frames found in each source
    struct SourceVBIInfo {
      FrameIDRange range;
      std::set<int32_t> vbi_frames;
      std::map<int32_t, FrameID>
          frame_to_frame;  // Map VBI frame number to source FrameID
      int32_t first_vbi = -1;
      int32_t last_vbi = -1;
      size_t total_vbi_count = 0;
      bool fully_scanned = false;
    };

    std::vector<SourceVBIInfo> source_info(input_sources.size());

    // Phase 1: Quick scan - find the first few VBI frames from each source
    constexpr size_t MAX_SCAN_FRAMES = 500;

    ORC_LOG_DEBUG(
        "Phase 1: Quick scan for initial VBI frames (up to {} frames per "
        "source)",
        MAX_SCAN_FRAMES);

    // Create observation context and biphase observer for VBI scanning
    ObservationContext observation_context;
    BiphaseObserver biphase_observer;

    for (size_t src_idx = 0; src_idx < input_sources.size(); ++src_idx) {
      const auto& source = input_sources[src_idx];
      if (!source) {
        continue;
      }

      auto& info = source_info[src_idx];
      info.range = source->frame_range();

      // Determine if source is PAL
      bool is_pal = false;
      if (auto first_desc = source->get_frame_descriptor(info.range.first)) {
        is_pal = (first_desc->system == VideoSystem::PAL);
      }

      ORC_LOG_DEBUG("  Source {}: quick scan (frame range {}-{})", src_idx + 1,
                    info.range.first, info.range.last);

      size_t scanned = 0;
      for (uint64_t i = 0; i < info.range.count() && scanned < MAX_SCAN_FRAMES;
           ++i) {
        FrameID frame_id = info.range.first + i;
        if (!source->has_frame(frame_id)) {
          continue;
        }

        ++scanned;

        biphase_observer.process_frame(*source, frame_id, observation_context);

        // Check both fields of this frame for VBI data.
        int32_t frame_num = -1;
        for (int field = 0; field < 2 && frame_num < 0; ++field) {
          FieldID field_id =
              FieldID(frame_id * 2 + static_cast<uint64_t>(field));
          frame_num =
              get_frame_number_from_vbi(observation_context, field_id, is_pal);
        }

        if (frame_num >= 0) {
          info.vbi_frames.insert(frame_num);
          info.frame_to_frame[frame_num] = frame_id;

          if (info.first_vbi < 0) {
            info.first_vbi = frame_num;
            ORC_LOG_DEBUG(
                "    Source {}: first VBI frame {} found at frame_id {}",
                src_idx + 1, frame_num, frame_id);
          }
          info.last_vbi = frame_num;
        }
      }

      ORC_LOG_DEBUG(
          "    Source {}: found {} unique VBI frames in first {} frames",
          src_idx + 1, info.vbi_frames.size(), scanned);

      if (progress && progress->isCancelled()) {
        result.status = AnalysisResult::Cancelled;
        return result;
      }
    }

    if (progress) {
      progress->setProgress(50);
    }

    // Read alignment mode from parameters (default: pad_for_alignment)
    std::string alignment_mode = "pad_for_alignment";
    {
      auto mode_it = ctx.parameters.find("alignmentMode");
      if (mode_it != ctx.parameters.end()) {
        if (const auto* s = std::get_if<std::string>(&mode_it->second)) {
          alignment_mode = *s;
        }
      }
    }
    ORC_LOG_DEBUG("Source alignment mode: {}", alignment_mode);

    // Collect per-source summary data (common to both modes)
    std::vector<int32_t> first_vbi_frames;
    std::vector<int32_t> last_vbi_frames;
    std::vector<size_t> vbi_counts;
    std::vector<FrameIDRange> source_ranges;
    for (const auto& info : source_info) {
      first_vbi_frames.push_back(info.first_vbi);
      last_vbi_frames.push_back(info.last_vbi);
      vbi_counts.push_back(info.vbi_frames.size());
      source_ranges.push_back(info.range);
    }

    // =========================================================================
    // PAD FOR ALIGNMENT MODE
    // =========================================================================
    if (alignment_mode == "pad_for_alignment") {
      // Phase 2 (pad): if any source has no first_vbi from the quick scan,
      // run a targeted full scan until we find the first VBI for that source.
      bool any_missing = false;
      for (const auto& info : source_info) {
        if (info.first_vbi < 0) {
          any_missing = true;
          break;
        }
      }

      if (any_missing) {
        ORC_LOG_DEBUG(
            "Pad mode: some sources have no VBI in quick scan – scanning");
        for (size_t src_idx = 0; src_idx < input_sources.size(); ++src_idx) {
          auto& info = source_info[src_idx];
          if (info.first_vbi >= 0) continue;

          const auto& source = input_sources[src_idx];
          bool is_pal = false;
          if (auto fd = source->get_frame_descriptor(info.range.first)) {
            is_pal = (fd->system == VideoSystem::PAL);
          }
          for (uint64_t i = 0; i < info.range.count(); ++i) {
            FrameID frame_id = info.range.first + i;
            if (!source->has_frame(frame_id)) continue;
            biphase_observer.process_frame(*source, frame_id,
                                           observation_context);
            int32_t frame_num = -1;
            for (int field = 0; field < 2 && frame_num < 0; ++field) {
              FieldID fid =
                  FieldID(frame_id * 2 + static_cast<uint64_t>(field));
              frame_num =
                  get_frame_number_from_vbi(observation_context, fid, is_pal);
            }
            if (frame_num >= 0) {
              info.first_vbi = frame_num;
              info.vbi_frames.insert(frame_num);
              info.frame_to_frame[frame_num] = frame_id;
              ORC_LOG_DEBUG("  Pad mode: source {} first VBI = {}", src_idx + 1,
                            frame_num);
              break;
            }
          }
          first_vbi_frames[src_idx] = source_info[src_idx].first_vbi;
          if (progress && progress->isCancelled()) {
            result.status = AnalysisResult::Cancelled;
            return result;
          }
        }
      }

      // Phase 3 (pad): find the globally earliest VBI frame number
      int32_t global_first_vbi = -1;
      for (const auto& info : source_info) {
        if (info.first_vbi >= 0) {
          if (global_first_vbi < 0 || info.first_vbi < global_first_vbi) {
            global_first_vbi = info.first_vbi;
          }
        }
      }

      if (global_first_vbi < 0) {
        result.status = AnalysisResult::Failed;
        result.summary = "No VBI frames found in any sources";
        AnalysisResult::ResultItem error_item;
        error_item.type = "error";
        error_item.message =
            "Could not find any VBI frame numbers in the input sources. "
            "This may indicate sources have no VBI data or are corrupted.";
        result.items.push_back(error_item);
        for (size_t i = 0; i < input_sources.size(); ++i) {
          AnalysisResult::ResultItem item;
          item.type = "info";
          std::ostringstream msg;
          msg << "Source " << (i + 1) << ": frames " << source_ranges[i].first
              << "-" << source_ranges[i].last << " ("
              << source_ranges[i].count() << " total), " << vbi_counts[i]
              << " with VBI";
          item.message = msg.str();
          result.items.push_back(item);
        }
        return result;
      }

      ORC_LOG_DEBUG("Pad mode: global_first_vbi = {}", global_first_vbi);

      // Compute per-source padding and build alignment map
      std::vector<size_t> pad_counts(input_sources.size(), 0);
      std::ostringstream alignment_map;
      bool am_first = true;
      for (size_t i = 0; i < input_sources.size(); ++i) {
        size_t pad =
            (first_vbi_frames[i] >= 0 && first_vbi_frames[i] > global_first_vbi)
                ? static_cast<size_t>(first_vbi_frames[i] - global_first_vbi)
                : 0;
        pad_counts[i] = pad;
        if (!am_first) alignment_map << ", ";
        am_first = false;
        alignment_map << (i + 1) << "+" << pad;
      }

      // Frame coverage: each source contributes real frames from
      // [pad_count[i], pad_count[i] + source_count[i])
      size_t max_output_frame = 0;
      for (size_t i = 0; i < input_sources.size(); ++i) {
        if (first_vbi_frames[i] >= 0) {
          // Cast needed: size_t and uint64_t are distinct types on macOS,
          // which breaks std::max template deduction.
          max_output_frame = std::max(
              max_output_frame,
              static_cast<size_t>(pad_counts[i] + source_ranges[i].count()));
        }
      }
      // Source Align pads every output at the tail to the longest one,
      // including sources without VBI, which pass through unpadded at the
      // head.
      size_t padded_length = 0;
      for (size_t i = 0; i < input_sources.size(); ++i) {
        padded_length = std::max(
            padded_length,
            static_cast<size_t>(pad_counts[i] + source_ranges[i].count()));
      }
      std::map<size_t, int> cov_events;
      for (size_t i = 0; i < input_sources.size(); ++i) {
        if (first_vbi_frames[i] < 0) continue;
        cov_events[pad_counts[i]] += 1;
        cov_events[pad_counts[i] + source_ranges[i].count()] -= 1;
      }
      size_t frames_one = 0, frames_two = 0, frames_three_plus = 0;
      {
        size_t prev = 0;
        int cnt = 0;
        for (const auto& [pos, delta] : cov_events) {
          size_t span = pos - prev;
          if (cnt == 1) {
            frames_one += span;
          } else if (cnt == 2) {
            frames_two += span;
          } else if (cnt >= 3) {
            frames_three_plus += span;
          }
          cnt += delta;
          prev = pos;
        }
        if (max_output_frame > prev) {
          size_t tail = max_output_frame - prev;
          if (cnt == 1) {
            frames_one += tail;
          } else if (cnt == 2) {
            frames_two += tail;
          } else if (cnt >= 3) {
            frames_three_plus += tail;
          }
        }
      }

      if (progress) {
        progress->setStatus("Generating alignment map...");
        progress->setProgress(90);
      }

      // Build summary
      std::ostringstream summary;
      summary << "Pad for frame alignment based on global first VBI frame "
              << global_first_vbi << "\n\n";
      summary << "Alignment Map: " << alignment_map.str() << "\n\n";

      summary << "Source Details:\n";
      for (size_t i = 0; i < input_sources.size(); ++i) {
        summary << "  Source " << (i + 1) << ":\n";
        // Frame numbers are presented 1-based, matching the preview.
        summary << "    Frame range: " << (source_ranges[i].first + 1) << "-"
                << (source_ranges[i].last + 1) << " ("
                << source_ranges[i].count() << " frames)\n";
        if (first_vbi_frames[i] >= 0) {
          summary << "    VBI range: frame " << first_vbi_frames[i] << "-"
                  << last_vbi_frames[i] << " (" << vbi_counts[i]
                  << " frames with VBI)\n";
          summary << "    Padding prepended: " << pad_counts[i] << " frames";
          if (pad_counts[i] == 0) {
            summary << " (source starts at global first VBI)";
          }
          summary << "\n";
          const size_t tail_count =
              padded_length - pad_counts[i] -
              static_cast<size_t>(source_ranges[i].count());
          summary << "    Output: " << padded_length << " frames total ("
                  << pad_counts[i] << " leading padding + "
                  << source_ranges[i].count() << " real + " << tail_count
                  << " trailing padding)\n";
        } else {
          summary << "    VBI data: none found — source included without "
                     "leading padding\n";
        }
        if (i < input_sources.size() - 1) summary << "\n";
      }

      summary << "\nFrame Coverage (after alignment, " << max_output_frame
              << " total frames):\n";
      summary << "  1 source:   " << frames_one << " frames"
              << (frames_one > 0 ? " (not stackable)" : "") << "\n";
      summary << "  2 sources:  " << frames_two << " frames"
              << (frames_two > 0 ? " (stackable)" : "") << "\n";
      summary << "  3+ sources: " << frames_three_plus << " frames"
              << (frames_three_plus > 0 ? " (stackable)" : "") << "\n";

      result.status = AnalysisResult::Success;
      result.summary = summary.str();
      result.graphData["alignmentMap"] = alignment_map.str();
      result.graphData["alignmentMode"] = alignment_mode;
      result.graphData["globalFirstVBIFrame"] =
          std::to_string(global_first_vbi);

      result.statistics["sourceCount"] =
          static_cast<int64_t>(input_sources.size());
      result.statistics["globalFirstVBIFrame"] =
          static_cast<int64_t>(global_first_vbi);
      result.statistics["framesWith1Source"] = static_cast<int64_t>(frames_one);
      result.statistics["framesWith2Sources"] =
          static_cast<int64_t>(frames_two);
      result.statistics["framesWith3PlusSources"] =
          static_cast<int64_t>(frames_three_plus);

      for (size_t i = 0; i < input_sources.size(); ++i) {
        AnalysisResult::ResultItem item;
        item.type = "info";
        item.message = "Source " + std::to_string(i + 1) + ": prepend " +
                       std::to_string(pad_counts[i]) + " padding frames, VBI " +
                       (first_vbi_frames[i] >= 0
                            ? std::to_string(first_vbi_frames[i]) + "-" +
                                  std::to_string(last_vbi_frames[i])
                            : "none");
        result.items.push_back(item);
      }

      if (progress) {
        progress->setStatus("Analysis complete");
        progress->setProgress(100);
      }
    }

    // =========================================================================
    // FIRST COMMON FRAME MODE
    // =========================================================================
    else {
      // Phase 2: Find first common VBI frame from the quick scan
      std::set<int32_t> common_frames;
      bool first_src = true;
      for (size_t src_idx = 0; src_idx < source_info.size(); ++src_idx) {
        const auto& info = source_info[src_idx];
        if (info.vbi_frames.empty()) {
          ORC_LOG_WARN("Source {} has no VBI frames in quick scan",
                       src_idx + 1);
          common_frames.clear();
          break;
        }
        if (first_src) {
          common_frames = info.vbi_frames;
          first_src = false;
        } else {
          std::set<int32_t> intersection;
          std::set_intersection(
              common_frames.begin(), common_frames.end(),
              info.vbi_frames.begin(), info.vbi_frames.end(),
              std::inserter(intersection, intersection.begin()));
          common_frames = std::move(intersection);
        }
        if (common_frames.empty()) {
          ORC_LOG_WARN(
              "No VBI frame overlap found between sources up to source {}",
              src_idx + 1);
          break;
        }
      }

      int32_t first_common_frame = -1;
      if (!common_frames.empty()) {
        first_common_frame = *common_frames.begin();
        ORC_LOG_DEBUG(
            "Found first common VBI frame {} in quick scan (all {} sources)",
            first_common_frame, input_sources.size());
      } else {
        ORC_LOG_WARN("No common VBI frame in quick scan – will need full scan");
      }

      if (progress) progress->setProgress(60);

      // Phase 3: Full scan if no common frame found in the quick scan
      if (first_common_frame < 0) {
        ORC_LOG_DEBUG("Phase 3: Full scan to find best alignment");
        for (size_t src_idx = 0; src_idx < input_sources.size(); ++src_idx) {
          const auto& source = input_sources[src_idx];
          if (!source) continue;
          auto& info = source_info[src_idx];
          bool is_pal = false;
          if (auto fd = source->get_frame_descriptor(info.range.first)) {
            is_pal = (fd->system == VideoSystem::PAL);
          }
          ORC_LOG_DEBUG("  Source {}: full scan of {} frames", src_idx + 1,
                        info.range.count());
          for (uint64_t i = 0; i < info.range.count(); ++i) {
            FrameID frame_id = info.range.first + i;
            if (!source->has_frame(frame_id)) continue;
            biphase_observer.process_frame(*source, frame_id,
                                           observation_context);
            int32_t frame_num = -1;
            for (int field = 0; field < 2 && frame_num < 0; ++field) {
              FieldID fid =
                  FieldID(frame_id * 2 + static_cast<uint64_t>(field));
              frame_num =
                  get_frame_number_from_vbi(observation_context, fid, is_pal);
            }
            if (frame_num >= 0) {
              if (info.vbi_frames.find(frame_num) == info.vbi_frames.end()) {
                info.vbi_frames.insert(frame_num);
                info.frame_to_frame[frame_num] = frame_id;
              }
              ++info.total_vbi_count;
              info.last_vbi = frame_num;
            }
          }
          info.fully_scanned = true;
          ORC_LOG_DEBUG(
              "    Source {}: {} unique VBI frames, {} total VBI observations",
              src_idx + 1, info.vbi_frames.size(), info.total_vbi_count);
          if (progress && progress->isCancelled()) {
            result.status = AnalysisResult::Cancelled;
            return result;
          }
        }
        // Refresh per-source summary data after full scan
        for (size_t i = 0; i < source_info.size(); ++i) {
          last_vbi_frames[i] = source_info[i].last_vbi;
          vbi_counts[i] = source_info[i].fully_scanned
                              ? source_info[i].total_vbi_count
                              : source_info[i].vbi_frames.size();
        }
      }

      if (progress) {
        progress->setStatus("Computing optimal alignment...");
        progress->setProgress(70);
      }

      // Phase 4: Determine the best alignment based on available data
      std::vector<FrameID> alignment_offsets(input_sources.size(), FrameID{0});
      std::vector<size_t> participating_sources;
      size_t max_sources_found = 0;

      if (first_common_frame >= 0) {
        max_sources_found = input_sources.size();
        for (size_t src_idx = 0; src_idx < input_sources.size(); ++src_idx) {
          const auto& info = source_info[src_idx];
          auto it = info.frame_to_frame.find(first_common_frame);
          if (it != info.frame_to_frame.end()) {
            alignment_offsets[src_idx] = it->second;
            participating_sources.push_back(src_idx);
          }
        }
      } else {
        std::map<int32_t, std::vector<size_t>> frame_to_sources;
        for (size_t src_idx = 0; src_idx < source_info.size(); ++src_idx) {
          for (int32_t frame_num : source_info[src_idx].vbi_frames) {
            frame_to_sources[frame_num].push_back(src_idx);
          }
        }
        for (const auto& [frame_num, sources] : frame_to_sources) {
          if (sources.size() > max_sources_found) {
            max_sources_found = sources.size();
            first_common_frame = frame_num;
            participating_sources.clear();
            for (size_t src_idx : sources) {
              participating_sources.push_back(src_idx);
              alignment_offsets[src_idx] =
                  source_info[src_idx].frame_to_frame[frame_num];
            }
            if (max_sources_found == input_sources.size()) break;
          }
        }
      }

      if (first_common_frame < 0 || max_sources_found == 0) {
        result.status = AnalysisResult::Failed;
        result.summary = "No VBI frames found in any sources";
        AnalysisResult::ResultItem warning_item;
        warning_item.type = "error";
        warning_item.message =
            "Could not find any VBI frame numbers in the input sources. "
            "This may indicate sources have no VBI data or are corrupted.";
        result.items.push_back(warning_item);
        for (size_t i = 0; i < input_sources.size(); ++i) {
          AnalysisResult::ResultItem info_item;
          info_item.type = "info";
          std::ostringstream msg;
          msg << "Source " << (i + 1) << ": frames " << source_ranges[i].first
              << "-" << source_ranges[i].last << " ("
              << source_ranges[i].count() << " total), " << vbi_counts[i]
              << " with VBI";
          if (first_vbi_frames[i] >= 0) {
            msg << ", VBI frames " << first_vbi_frames[i] << "-"
                << last_vbi_frames[i];
          }
          info_item.message = msg.str();
          result.items.push_back(info_item);
        }
        return result;
      }

      ORC_LOG_DEBUG("  Best common VBI frame {} found in {} of {} sources:",
                    first_common_frame, max_sources_found,
                    input_sources.size());
      for ([[maybe_unused]] size_t src_idx : participating_sources) {
        ORC_LOG_DEBUG("    Source {}: at frame_id {} (offset = {})",
                      src_idx + 1, alignment_offsets[src_idx],
                      alignment_offsets[src_idx]);
      }

      if (max_sources_found < input_sources.size()) {
        std::vector<size_t> excluded_sources;
        for (size_t i = 0; i < input_sources.size(); ++i) {
          if (std::find(participating_sources.begin(),
                        participating_sources.end(),
                        i) == participating_sources.end()) {
            excluded_sources.push_back(i);
          }
        }
        ORC_LOG_WARN(
            "Not all sources have overlapping VBI frames – {} excluded",
            excluded_sources.size());
        AnalysisResult::ResultItem warning_item;
        warning_item.type = "warning";
        std::ostringstream msg;
        msg << "Only " << max_sources_found << " of " << input_sources.size()
            << " sources have overlapping VBI frames.\n\n";
        msg << "Excluded sources (from different disc sections):\n";
        for (size_t src_idx : excluded_sources) {
          msg << "  • Source " << (src_idx + 1) << ": VBI frames ";
          if (first_vbi_frames[src_idx] >= 0) {
            msg << first_vbi_frames[src_idx] << "-" << last_vbi_frames[src_idx];
          } else {
            msg << "none";
          }
          msg << "\n";
        }
        msg << "\nThe alignment map will only include the " << max_sources_found
            << " overlapping sources.";
        warning_item.message = msg.str();
        result.items.push_back(warning_item);
      }

      if (progress) {
        progress->setStatus("Generating alignment map...");
        progress->setProgress(90);
      }

      // Build alignment map (format: source_id+skip_offset)
      std::ostringstream alignment_map;
      bool am_first = true;
      for (size_t src_idx : participating_sources) {
        if (!am_first) alignment_map << ", ";
        am_first = false;
        alignment_map << (src_idx + 1) << "+" << alignment_offsets[src_idx];
      }

      // Frame coverage: participating sources all start at frame 0 after trim
      size_t max_output_frame = 0;
      for (size_t src_idx : participating_sources) {
        const FrameID off = alignment_offsets[src_idx];
        size_t cnt =
            (off < source_ranges[src_idx].count())
                ? static_cast<size_t>(source_ranges[src_idx].count() - off)
                : 0;
        max_output_frame = std::max(max_output_frame, cnt);
      }
      std::map<size_t, int> cov_events;
      for (size_t src_idx : participating_sources) {
        const FrameID off = alignment_offsets[src_idx];
        size_t cnt =
            (off < source_ranges[src_idx].count())
                ? static_cast<size_t>(source_ranges[src_idx].count() - off)
                : 0;
        if (cnt > 0) {
          cov_events[0] += 1;
          cov_events[cnt] -= 1;
        }
      }
      size_t frames_one = 0, frames_two = 0, frames_three_plus = 0;
      {
        size_t prev = 0;
        int cnt = 0;
        for (const auto& [pos, delta] : cov_events) {
          size_t span = pos - prev;
          if (cnt == 1) {
            frames_one += span;
          } else if (cnt == 2) {
            frames_two += span;
          } else if (cnt >= 3) {
            frames_three_plus += span;
          }
          cnt += delta;
          prev = pos;
        }
        if (max_output_frame > prev) {
          size_t tail = max_output_frame - prev;
          if (cnt == 1) {
            frames_one += tail;
          } else if (cnt == 2) {
            frames_two += tail;
          } else if (cnt >= 3) {
            frames_three_plus += tail;
          }
        }
      }

      // Build summary
      std::ostringstream summary;
      if (max_sources_found < input_sources.size()) {
        summary << "⚠ Partial alignment: " << max_sources_found << " of "
                << input_sources.size()
                << " sources have overlapping VBI frames\n";
      }
      summary << "First common frame alignment based on VBI frame "
              << first_common_frame << "\n\n";
      summary << "Alignment Map: " << alignment_map.str() << "\n\n";

      summary << "Source Details:\n";
      for (size_t i = 0; i < input_sources.size(); ++i) {
        bool is_participating = std::find(participating_sources.begin(),
                                          participating_sources.end(),
                                          i) != participating_sources.end();
        summary << "  Source " << (i + 1);
        if (!is_participating) {
          summary << " [EXCLUDED - no overlapping VBI frames]";
        }
        summary << ":\n";
        // Frame numbers are presented 1-based, matching the preview.
        summary << "    Frame range: " << (source_ranges[i].first + 1) << "-"
                << (source_ranges[i].last + 1) << " ("
                << source_ranges[i].count() << " frames)\n";
        if (first_vbi_frames[i] >= 0) {
          summary << "    VBI range: frame " << first_vbi_frames[i] << "-"
                  << last_vbi_frames[i] << " (" << vbi_counts[i]
                  << " frames with VBI)\n";
          if (is_participating) {
            const FrameID frame_offset = alignment_offsets[i];
            summary << "    First common VBI frame (" << first_common_frame
                    << ") at frame: " << (frame_offset + 1) << "\n";
            summary << "    Alignment offset: " << frame_offset << " frames";
            if (frame_offset > FrameID{0}) {
              summary << " (skip first " << frame_offset << ")";
            }
            summary << "\n";
            size_t output_frames =
                (frame_offset < source_ranges[i].count())
                    ? static_cast<size_t>(source_ranges[i].count() -
                                          frame_offset)
                    : 0;
            summary << "    Output: " << output_frames
                    << " frames after alignment\n";
          } else {
            summary << "    Status: VBI range does not overlap with other "
                       "sources\n";
          }
        } else {
          summary << "    VBI data: none found\n";
          if (!is_participating) {
            summary << "    Status: Cannot align without VBI data\n";
          }
        }
        if (i < input_sources.size() - 1) summary << "\n";
      }

      summary << "\nFrame Coverage (after alignment, " << max_output_frame
              << " total frames):\n";
      summary << "  1 source:    " << frames_one << " frames"
              << (frames_one > 0 ? " (single-source, not stackable)" : "")
              << "\n";
      summary << "  2 sources:   " << frames_two << " frames"
              << (frames_two > 0 ? " (stackable)" : "") << "\n";
      summary << "  3+ sources:  " << frames_three_plus << " frames"
              << (frames_three_plus > 0 ? " (stackable)" : "") << "\n";

      result.status = AnalysisResult::Success;
      result.summary = summary.str();
      result.graphData["alignmentMap"] = alignment_map.str();
      result.graphData["alignmentMode"] = alignment_mode;
      result.graphData["firstCommonFrame"] = std::to_string(first_common_frame);

      result.statistics["sourceCount"] =
          static_cast<int64_t>(input_sources.size());
      result.statistics["participatingSourceCount"] =
          static_cast<int64_t>(max_sources_found);
      result.statistics["excludedSourceCount"] =
          static_cast<int64_t>(input_sources.size() - max_sources_found);
      result.statistics["firstCommonVBIFrame"] =
          static_cast<int64_t>(first_common_frame);
      result.statistics["framesWith1Source"] = static_cast<int64_t>(frames_one);
      result.statistics["framesWith2Sources"] =
          static_cast<int64_t>(frames_two);
      result.statistics["framesWith3PlusSources"] =
          static_cast<int64_t>(frames_three_plus);

      size_t total_output_frames = 0;
      size_t total_dropped_frames = 0;
      for (size_t src_idx : participating_sources) {
        const FrameID frame_offset = alignment_offsets[src_idx];
        size_t output_frames =
            (frame_offset < source_ranges[src_idx].count())
                ? static_cast<size_t>(source_ranges[src_idx].count() -
                                      frame_offset)
                : 0;
        total_output_frames += output_frames;
        total_dropped_frames += static_cast<size_t>(frame_offset);
      }
      result.statistics["totalOutputFrames"] =
          static_cast<int64_t>(total_output_frames);
      result.statistics["totalDroppedFrames"] =
          static_cast<int64_t>(total_dropped_frames);

      for (size_t i = 0; i < input_sources.size(); ++i) {
        bool is_participating = std::find(participating_sources.begin(),
                                          participating_sources.end(),
                                          i) != participating_sources.end();
        AnalysisResult::ResultItem source_item;
        if (is_participating) {
          source_item.type = "info";
          source_item.message =
              "Source " + std::to_string(i + 1) + ": offset +" +
              std::to_string(alignment_offsets[i]) + " frames, VBI frames " +
              std::to_string(first_vbi_frames[i]) + "-" +
              std::to_string(last_vbi_frames[i]);
        } else {
          source_item.type = "warning";
          source_item.message = "Source " + std::to_string(i + 1) +
                                " [EXCLUDED]: VBI frames " +
                                std::to_string(first_vbi_frames[i]) + "-" +
                                std::to_string(last_vbi_frames[i]) +
                                " (no overlap with other sources)";
        }
        result.items.push_back(source_item);
      }

      if (progress) {
        progress->setStatus("Analysis complete");
        progress->setProgress(100);
      }
    }  // end first_common_frame mode

  } catch (const std::exception& e) {
    result.status = AnalysisResult::Failed;
    result.summary = "Analysis failed: " + std::string(e.what());
    ORC_LOG_ERROR("Source alignment analysis failed: {}", e.what());
  }

  return result;
}

bool SourceAlignmentAnalysisTool::canApplyToGraph() const {
  return true;  // Can apply alignment map to source_align node
}

bool SourceAlignmentAnalysisTool::applyToGraph(
    AnalysisResult& result, const Project& /*project*/,
    [[maybe_unused]] NodeID node_id) {
  if (result.status != AnalysisResult::Success) {
    ORC_LOG_ERROR("Cannot apply failed analysis result");
    return false;
  }

  // Get the alignment map from the result
  auto it = result.graphData.find("alignmentMap");
  if (it == result.graphData.end()) {
    ORC_LOG_ERROR("Analysis result does not contain alignment map");
    return false;
  }

  const std::string& alignment_map = it->second;

  // Populate parameterChanges instead of modifying project directly
  try {
    result.parameterChanges["alignmentMap"] = alignment_map;

    auto mode_it = result.graphData.find("alignmentMode");
    if (mode_it != result.graphData.end()) {
      result.parameterChanges["alignmentMode"] = mode_it->second;
    }

    ORC_LOG_DEBUG(
        "Prepared alignment map '{}' (mode '{}') for node '{}'", alignment_map,
        mode_it != result.graphData.end() ? mode_it->second : "unknown",
        node_id);
    return true;
  } catch (const std::exception& e) {
    ORC_LOG_ERROR("Failed to prepare alignment map: {}", e.what());
    return false;
  }
}

int SourceAlignmentAnalysisTool::estimateDurationSeconds(
    const AnalysisContext& ctx) const {
  (void)ctx;
  // Alignment analysis is relatively fast - just scanning VBI data
  return 10;
}

}  // namespace orc
