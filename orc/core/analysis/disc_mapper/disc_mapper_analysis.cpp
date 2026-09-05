/*
 * File:        disc_mapper_analysis.cpp
 * Module:      analysis
 * Purpose:     Disc mapper analysis tool: detects skipped, repeated, and
 * missing frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "disc_mapper_analysis.h"

#include <biphase_observer.h>
#include <black_psnr_observer.h>
#include <burst_level_observer.h>
#include <frame_numbering.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>
#include <white_snr_observer.h>

#include <algorithm>
#include <cstdint>
#include <sstream>

#include "../../include/dag_executor.h"
#include "../../include/project.h"
#include "../analysis_registry.h"
#include "disc_mapper_analyzer.h"

namespace orc {

// Force linker to include this object file (for static registration)
void force_link_DiscMapperAnalysisTool() {}

std::string DiscMapperAnalysisTool::id() const { return "field_mapping"; }

std::string DiscMapperAnalysisTool::name() const { return "Disc Mapper"; }

std::string DiscMapperAnalysisTool::description() const {
  return "Detect and correct skipped, repeated, and missing frames caused by "
         "laserdisc player tracking problems.";
}

std::string DiscMapperAnalysisTool::category() const { return "Diagnostic"; }

std::vector<ParameterDescriptor> DiscMapperAnalysisTool::parameters() const {
  std::vector<ParameterDescriptor> params;

  {
    ParameterDescriptor desc;
    desc.name = "include_lead_in_out";
    desc.display_name = "Include Lead-in/Lead-out";
    desc.description =
        "Keep one frame of lead-in and one frame of lead-out (when the "
        "capture contains them) at the start and end of the mapped output. "
        "Downstream sinks read the disc's lead-in metadata, such as the "
        "user's code, from that frame. Leave unchecked to map the programme "
        "content only.";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    desc.constraints.required = false;
    params.push_back(desc);
  }

  return params;
}

bool DiscMapperAnalysisTool::canAnalyze(AnalysisSourceType source_type) const {
  // Can analyze laserdisc sources
  return source_type == AnalysisSourceType::LaserDisc;
}

bool DiscMapperAnalysisTool::isApplicableToStage(
    const std::string& stage_name) const {
  return stage_name == "frame_map";
}

AnalysisResult DiscMapperAnalysisTool::analyze(const AnalysisContext& ctx,
                                               AnalysisProgress* progress) {
  AnalysisResult result;

  if (progress) {
    progress->setStatus("Initializing disc mapper analysis...");
    progress->setProgress(0);
  }

  // Get the VideoFrameRepresentation from the DAG execution
  // The frame_map node should have exactly one input
  if (!ctx.dag || !ctx.project) {
    result.status = AnalysisResult::Failed;
    result.summary = "No DAG or project provided for analysis";
    ORC_LOG_ERROR("Frame mapping analysis requires DAG and project in context");
    return result;
  }

  // Find the frame_map node in the DAG
  const auto& dag_nodes = ctx.dag->nodes();
  auto node_it = std::find_if(
      dag_nodes.begin(), dag_nodes.end(),
      [&ctx](const DAGNode& node) { return node.node_id == ctx.node_id; });

  if (node_it == dag_nodes.end()) {
    result.status = AnalysisResult::Failed;
    result.summary = "Node not found in DAG";
    ORC_LOG_ERROR("Node '{}' not found in DAG", ctx.node_id);
    return result;
  }

  const auto& node = *node_it;

  // Get the input node ID
  if (node.input_node_ids.empty()) {
    result.status = AnalysisResult::Failed;
    result.summary = "Frame map node has no input connected";
    ORC_LOG_ERROR("Frame map node '{}' has no input", ctx.node_id);
    return result;
  }

  NodeID input_node_id = node.input_node_ids[0];
  ORC_LOG_DEBUG(
      "Node '{}': Frame mapping analysis - getting input from node '{}'",
      ctx.node_id, input_node_id);

  // Execute DAG to get the VideoFrameRepresentation from the input node
  DAGExecutor executor;
  try {
    auto all_outputs = executor.execute_to_node(*ctx.dag, input_node_id);

    // Get the outputs from the input node
    auto output_it = all_outputs.find(input_node_id);
    if (output_it == all_outputs.end() || output_it->second.empty()) {
      result.status = AnalysisResult::Failed;
      result.summary = "Input node produced no outputs";
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
      result.summary = "Input node did not produce VideoFrameRepresentation";
      ORC_LOG_ERROR(
          "Node '{}': Input node '{}' did not produce VideoFrameRepresentation",
          ctx.node_id, input_node_id);
      return result;
    }

    ORC_LOG_DEBUG("Got VideoFrameRepresentation with {} frames ({} fields)",
                  source->frame_count(), source->frame_count() * 2);

    if (progress) {
      progress->setStatus("Extracting VBI and signal quality data...");
      progress->setProgress(0);
    }

    // Run the observers the mapper depends on over all frames, populating the
    // ObservationContext:
    //  - BiphaseObserver     "biphase" vbi_line_16/17/18 (picture numbers)
    //  - BurstLevelObserver  "burst_level" median_burst_10bit
    //  - WhiteSNRObserver    "white_snr" snr_db
    //  - BlackPSNRObserver   "black_psnr" psnr_db
    // The last three supply the signal-quality readings the deduplication
    // stage uses to choose between duplicate copies of a disc picture. They
    // run in the same pass so that each frame is decoded only once.
    BiphaseObserver biphase_observer;
    BurstLevelObserver burst_level_observer;
    WhiteSNRObserver white_snr_observer;
    BlackPSNRObserver black_psnr_observer;
    auto& obs_context = executor.get_observation_context();
    auto frame_range = source->frame_range();

    ORC_LOG_DEBUG("Running VBI and quality observers on {} frames",
                  frame_range.count());

    {
      size_t total_frames = frame_range.count();
      size_t biphase_idx = 0;
      size_t update_interval =
          std::max(static_cast<size_t>(1), total_frames / 100);
      for (FrameID fid = frame_range.first; fid <= frame_range.last; ++fid) {
        biphase_observer.process_frame(*source, fid, obs_context);
        burst_level_observer.process_frame(*source, fid, obs_context);
        white_snr_observer.process_frame(*source, fid, obs_context);
        black_psnr_observer.process_frame(*source, fid, obs_context);
        ++biphase_idx;
        if (progress && biphase_idx % update_interval == 0) {
          int pct = static_cast<int>(biphase_idx * 100 / total_frames);
          progress->setProgress(pct);
          progress->setSubStatus("Frame " + std::to_string(biphase_idx) +
                                 " / " + std::to_string(total_frames));
          if (progress->isCancelled()) {
            result.status = AnalysisResult::Cancelled;
            return result;
          }
        }
      }
      if (progress) {
        progress->setProgress(100);
        progress->setSubStatus("");
      }
    }

    ORC_LOG_DEBUG("Observers complete, ObservationContext populated");

    if (progress && progress->isCancelled()) {
      result.status = AnalysisResult::Cancelled;
      return result;
    }

    // Now run the analyzer on the representation
    DiscMapperAnalyzer analyzer;
    DiscMapperAnalyzer::Options options;

    auto include_lead_it = ctx.parameters.find("include_lead_in_out");
    if (include_lead_it != ctx.parameters.end() &&
        std::holds_alternative<bool>(include_lead_it->second)) {
      options.include_lead_in_out = std::get<bool>(include_lead_it->second);
    }

    // Run frame mapping analysis - each stage reports its own 0-100% progress
    FieldMappingDecision decision =
        analyzer.analyze(*source, obs_context, options, progress);

    if (!decision.success) {
      result.status = AnalysisResult::Failed;
      result.summary = decision.rationale.empty()
                           ? "Disc mapper analysis failed to produce a mapping"
                           : decision.rationale;

      for (const auto& warning : decision.warnings) {
        AnalysisResult::ResultItem item;
        item.type = "warning";
        item.message = warning;
        result.items.push_back(item);
      }
      return result;
    }

    if (progress && progress->isCancelled()) {
      result.status = AnalysisResult::Cancelled;
      return result;
    }

    if (progress) {
      progress->setStatus("Processing results...");
      progress->setProgress(0);
    }

    // Convert warnings to result items
    for (const auto& warning : decision.warnings) {
      AnalysisResult::ResultItem item;
      item.type = "warning";
      item.message = warning;
      result.items.push_back(item);

      if (progress) {
        progress->reportPartialResult(item);
      }
    }

    if (progress) {
      progress->setStatus("Analysis complete");
      progress->setProgress(100);
    }

    // Build detailed summary
    const auto& stats = decision.stats;
    size_t total_frames = stats.total_fields / 2;
    size_t final_frames = total_frames - stats.removed_lead_in_out -
                          stats.removed_invalid_phase -
                          stats.removed_duplicates - stats.removed_unmappable +
                          stats.padding_frames;

    std::ostringstream summary;
    std::string disc_type = decision.is_cav ? "CAV" : "CLV";
    std::string video_format = decision.is_pal ? "PAL" : "NTSC";

    // Start with the detailed rationale report (as required by design doc
    // section 16)
    summary << "=== DISC MAPPING ANALYSIS REPORT ===\n\n";
    summary << decision.rationale;  // Full stage-by-stage pipeline report
    summary << "\n=== SUMMARY ===\n\n";

    summary << "Source: " << video_format << " " << disc_type << " disc\n\n";

    summary << "Input:\n";
    summary << "  " << stats.total_fields << " fields (" << total_frames
            << " field pairs/frames)\n\n";

    summary << "Output:\n";
    summary << "  " << final_frames << " frames (" << (final_frames * 2)
            << " fields)";

    if (stats.removed_duplicates > 0 || stats.gaps_padded > 0 ||
        stats.removed_lead_in_out > 0) {
      summary << " (";
      bool need_sep = false;
      if (stats.removed_duplicates > 0) {
        summary << stats.removed_duplicates << " duplicates removed";
        need_sep = true;
      }
      if (stats.gaps_padded > 0) {
        if (need_sep) summary << ", ";
        summary << stats.gaps_padded << " gaps padded";
        need_sep = true;
      }
      if (stats.removed_lead_in_out > 0) {
        if (need_sep) summary << ", ";
        summary << stats.removed_lead_in_out << " lead-in/out removed";
      }
      summary << ")";
    }

    if (options.include_lead_in_out) {
      summary << "\n  Lead-in frame: "
              << (stats.lead_in_included ? "included" : "none found")
              << "; lead-out frame: "
              << (stats.lead_out_included ? "included" : "none found");
    }

    // Add generated mapping spec to summary. Shown 1-based to match the
    // Frame Map parameter dialog; the applied spec stays 0-based internally.
    const std::string display_spec =
        range_spec_to_presentation(decision.mapping_spec);
    summary << "\n\nGenerated Frame Mapping:\n";
    if (display_spec.empty()) {
      summary << "  (empty - no frames could be mapped)";
    } else if (display_spec.length() <= 200) {
      summary << "  " << display_spec;
    } else {
      summary << "  " << display_spec.substr(0, 200) << "...\n";
      summary << "  (Full spec: " << display_spec.length()
              << " chars - see details below)";
    }

    result.summary = summary.str();

    // Statistics
    result.statistics["discType"] = decision.is_cav ? "CAV" : "CLV";
    result.statistics["videoFormat"] = decision.is_pal ? "PAL" : "NTSC";
    result.statistics["totalFields"] = static_cast<int64_t>(stats.total_fields);
    result.statistics["outputFields"] = static_cast<int64_t>(final_frames * 2);
    result.statistics["outputFrames"] = static_cast<int64_t>(final_frames);
    result.statistics["removedLeadInOut"] =
        static_cast<int64_t>(stats.removed_lead_in_out);
    result.statistics["leadInIncluded"] =
        static_cast<int64_t>(stats.lead_in_included ? 1 : 0);
    result.statistics["leadOutIncluded"] =
        static_cast<int64_t>(stats.lead_out_included ? 1 : 0);
    result.statistics["removedInvalidPhase"] =
        static_cast<int64_t>(stats.removed_invalid_phase);
    result.statistics["removedDuplicates"] =
        static_cast<int64_t>(stats.removed_duplicates);
    result.statistics["removedUnmappable"] =
        static_cast<int64_t>(stats.removed_unmappable);
    result.statistics["correctedVBIErrors"] =
        static_cast<int64_t>(stats.corrected_vbi_errors);
    result.statistics["pulldownFrames"] =
        static_cast<int64_t>(stats.pulldown_frames);
    result.statistics["paddingFrames"] =
        static_cast<int64_t>(stats.padding_frames);
    result.statistics["gapsPadded"] = static_cast<int64_t>(stats.gaps_padded);
    result.statistics["framesWithQualityData"] =
        static_cast<int64_t>(stats.frames_with_quality);
    result.statistics["duplicateGroups"] =
        static_cast<int64_t>(stats.duplicate_groups);
    result.statistics["duplicatesDecidedByQuality"] =
        static_cast<int64_t>(stats.duplicates_decided_by_quality);

    // Store mapping spec for graph application
    result.graphData["mappingSpec"] = decision.mapping_spec;
    result.graphData["rationale"] = decision.rationale;

    // Note: Rationale is already included in summary per design doc section 16
    // "Each stage shall produce a clear description of the decision making
    // process/pipeline"

    ORC_LOG_DEBUG("Frame mapping analysis complete - mapping spec: {} chars",
                  decision.mapping_spec.length());

    result.status = AnalysisResult::Success;
    return result;

  } catch (const std::exception& e) {
    result.status = AnalysisResult::Failed;
    result.summary = std::string("Analysis failed: ") + e.what();
    ORC_LOG_ERROR("Frame mapping analysis failed: {}", e.what());
    return result;
  }
}

bool DiscMapperAnalysisTool::canApplyToGraph() const { return true; }

bool DiscMapperAnalysisTool::applyToGraph(AnalysisResult& result,
                                          const Project& project,
                                          NodeID node_id) {
  // Find the target node in the project
  const auto& nodes = project.get_nodes();
  auto node_it = std::find_if(
      nodes.begin(), nodes.end(),
      [&node_id](const ProjectDAGNode& n) { return n.node_id == node_id; });

  if (node_it == nodes.end()) {
    ORC_LOG_ERROR("DiscMapperAnalysisTool::applyToGraph - node not found: {}",
                  node_id);
    return false;
  }

  // Apply mapping spec to the node's parameters
  auto mapping_it = result.graphData.find("mappingSpec");
  if (mapping_it == result.graphData.end()) {
    ORC_LOG_ERROR(
        "DiscMapperAnalysisTool::applyToGraph - No mapping spec in result");
    return false;
  }
  std::string mappingSpec = mapping_it->second;

  ORC_LOG_DEBUG("Node '{}': Applying frame mapping results", node_id);
  if (node_it->parameters.count("ranges")) {
    auto& old_value = node_it->parameters.at("ranges");
    if ([[maybe_unused]] auto* str_val = std::get_if<std::string>(&old_value)) {
      ORC_LOG_DEBUG("Node '{}':   Old ranges parameter: {}", node_id, *str_val);
    }
  } else {
    ORC_LOG_DEBUG("Node '{}':   Old ranges parameter: (not set)", node_id);
  }
  ORC_LOG_DEBUG("Node '{}':   New mapping spec: {}", node_id, mappingSpec);

  auto rationale_it = result.graphData.find("rationale");
  if (rationale_it != result.graphData.end()) {
    ORC_LOG_DEBUG("Node '{}':   Rationale: {}", node_id, rationale_it->second);
  }

  // Set the FrameMapStage's "ranges" parameter via parameterChanges
  // The presenter will apply these changes properly
  result.parameterChanges["ranges"] = mappingSpec;

  ORC_LOG_DEBUG(
      "Successfully prepared mapping spec for FrameMapStage 'ranges' "
      "parameter");
  return true;
}

int DiscMapperAnalysisTool::estimateDurationSeconds(
    const AnalysisContext& ctx) const {
  (void)ctx;
  // Disc mapper needs to load entire source and run observers
  // Estimate: ~5-10 seconds for a typical source file
  return 5;
}

// Register the tool
REGISTER_ANALYSIS_TOOL(DiscMapperAnalysisTool);

}  // namespace orc
