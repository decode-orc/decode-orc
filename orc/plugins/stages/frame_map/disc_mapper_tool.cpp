/*
 * File:        disc_mapper_tool.cpp
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     The Frame Map's Disc Mapper: map a whole side's frames by
 *              their picture numbers, as the Frame Map's ranges
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "disc_mapper_tool.h"

#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>

#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "disc-metadata/disc_metadata_collect.h"
#include "disc-metadata/disc_metadata_document.h"
#include "frame_map_spec_text.h"

namespace orc::disc_mapper_tool {

namespace {

constexpr char kIncludeLeadInOut[] = "include_lead_in_out";

StageAnalysisReport failed(std::string why) {
  StageAnalysisReport report;
  report.status = StageAnalysisReport::Status::Failed;
  report.summary = std::move(why);
  return report;
}

StageAnalysisReport cancelled() {
  StageAnalysisReport report;
  report.status = StageAnalysisReport::Status::Cancelled;
  return report;
}

int64_t count(size_t n) { return static_cast<int64_t>(n); }

/// Passes the analyzer's steps on as the job's progress: each step reports
/// its own 0-100% under its own status line.
class JobProgress final : public DiscMapperProgress {
 public:
  explicit JobProgress(IToolJobContext& context) : context_(context) {}

  void setProgress(int percentage) override {
    fraction_ = percentage / 100.0;
    report();
  }
  void setStatus(const std::string& status) override {
    status_ = status;
    report();
  }
  void setSubStatus(const std::string& sub_status) {
    sub_status_ = sub_status;
    report();
  }
  bool isCancelled() const override { return context_.is_cancelled(); }

 private:
  void report() {
    if (status_.empty() || sub_status_.empty()) {
      context_.report_progress(fraction_, status_ + sub_status_);
    } else {
      context_.report_progress(fraction_, status_ + " - " + sub_status_);
    }
  }

  IToolJobContext& context_;
  double fraction_ = 0.0;
  std::string status_;
  std::string sub_status_;
};

// Write the analyzer's warnings into the report text.
//
// They are also published as items, but the report the user reads is the
// summary, so a warning that lives only in items is never seen. The analyzer
// raises at most one warning per class of problem (each already a count
// across the whole disc), so the whole list belongs in the report rather
// than a truncated sample of it.
void append_warnings(std::ostringstream& out,
                     const std::vector<std::string>& warnings) {
  if (warnings.empty()) {
    return;
  }

  out << "\n\nWarnings (" << warnings.size() << "):";
  for (const auto& warning : warnings) {
    out << "\n  - " << warning;
  }
}

void add_warning_items(const std::vector<std::string>& warnings,
                       StageAnalysisReport& report) {
  for (const auto& warning : warnings) {
    StageAnalysisItem item;
    item.type = "warning";
    item.message = warning;
    report.items.push_back(std::move(item));
  }
}

std::string summary_of(const FieldMappingDecision& decision,
                       const DiscMapperAnalyzer::Options& options) {
  const auto& stats = decision.stats;
  const size_t total_frames = stats.total_fields / 2;
  // The count the analyzer actually emitted, not one recomputed from the
  // removal tallies: a frame can be removed for more than one reason, and
  // a recomputed figure silently drifts from the mapping it describes.
  const size_t final_frames = stats.final_frames;

  std::ostringstream summary;
  const std::string disc_type = decision.is_cav ? "CAV" : "CLV";
  const std::string video_format = decision.is_pal ? "PAL" : "NTSC";

  // The stage-by-stage report of the decisions comes first.
  summary << "=== DISC MAPPING ANALYSIS REPORT ===\n\n";
  summary << decision.rationale;
  summary << "\n=== SUMMARY ===\n\n";

  summary << "Source: " << video_format << " " << disc_type << " disc\n\n";

  summary << "Input:\n";
  summary << "  " << stats.total_fields << " fields (" << total_frames
          << " field pairs/frames)\n\n";

  summary << "Output:\n";
  summary << "  " << final_frames << " frames (" << (final_frames * 2)
          << " fields)";

  if (stats.removed_duplicates > 0 || stats.gaps_padded > 0 ||
      stats.removed_lead_in_out > 0 || stats.removed_unmappable > 0) {
    summary << " (";
    bool need_sep = false;
    if (stats.removed_duplicates > 0) {
      summary << stats.removed_duplicates << " duplicates removed";
      need_sep = true;
    }
    if (stats.removed_unmappable > 0) {
      if (need_sep) summary << ", ";
      summary << stats.removed_unmappable
              << " without a picture number removed";
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

  append_warnings(summary, decision.warnings);

  // Shown 1-based to match the Frame Map parameter dialog; the applied spec
  // stays 0-based.
  const std::string display_spec =
      frame_map_spec_text::to_presentation(decision.mapping_spec);
  summary << "\n\nGenerated Frame Mapping:\n";
  if (display_spec.empty()) {
    summary << "  (empty - no frames could be mapped, so there is nothing "
               "to apply)";
  } else if (display_spec.length() <= 200) {
    summary << "  " << display_spec;
  } else {
    summary << "  " << display_spec.substr(0, 200) << "...\n";
    summary << "  (Truncated for display; the full spec is "
            << display_spec.length()
            << " chars and is written to the stage's ranges parameter by "
               "'Apply to Stage')";
  }
  return summary.str();
}

void add_statistics(const FieldMappingDecision& decision,
                    StageAnalysisReport& report) {
  const auto& stats = decision.stats;
  auto& out = report.statistics;
  out["discType"] = std::string(decision.is_cav ? "CAV" : "CLV");
  out["videoFormat"] = std::string(decision.is_pal ? "PAL" : "NTSC");
  out["totalFields"] = count(stats.total_fields);
  out["outputFields"] = count(stats.final_frames * 2);
  out["outputFrames"] = count(stats.final_frames);
  out["removedLeadInOut"] = count(stats.removed_lead_in_out);
  out["leadInIncluded"] = int64_t{stats.lead_in_included ? 1 : 0};
  out["leadOutIncluded"] = int64_t{stats.lead_out_included ? 1 : 0};
  out["removedInvalidPhase"] = count(stats.removed_invalid_phase);
  out["removedDuplicates"] = count(stats.removed_duplicates);
  out["removedUnmappable"] = count(stats.removed_unmappable);
  out["rejectedImplausiblePictureNumbers"] =
      count(stats.rejected_implausible_pn);
  out["rejectedDisagreeingPictureNumbers"] =
      count(stats.rejected_disagreeing_pn);
  out["splitFieldFrames"] = count(stats.split_field_frames);
  out["gapsTooWideToPad"] = count(stats.gaps_too_wide_to_pad);
  out["finalFrames"] = count(stats.final_frames);
  out["correctedVBIErrors"] = count(stats.corrected_vbi_errors);
  out["pulldownFrames"] = count(stats.pulldown_frames);
  out["pulldownFramesPadded"] = count(stats.pulldown_frames_padded);
  out["paddingFrames"] = count(stats.padding_frames);
  out["gapsPadded"] = count(stats.gaps_padded);
  out["framesWithQualityData"] = count(stats.frames_with_quality);
  out["duplicateGroups"] = count(stats.duplicate_groups);
  out["duplicatesDecidedByQuality"] =
      count(stats.duplicates_decided_by_quality);
}

}  // namespace

std::vector<ParameterDescriptor> parameters() {
  ParameterDescriptor desc;
  desc.name = kIncludeLeadInOut;
  desc.display_name = "Include Lead-in/Lead-out";
  desc.description =
      "Keep one frame of lead-in and one frame of lead-out (when the "
      "capture contains them) at the start and end of the mapped output. "
      "Downstream sinks read the disc's lead-in metadata, such as the "
      "user's code, from that frame. Uncheck to map the programme content "
      "only.";
  desc.type = ParameterType::BOOL;
  desc.constraints.default_value = true;
  desc.constraints.required = false;
  return {desc};
}

std::map<std::string, StageAnalysisValue> disc_identity_statistics(
    const IObservationContext& observations, FrameIDRange frames,
    VideoSystem system, const FieldMappingDecision& decision) {
  std::map<std::string, StageAnalysisValue> out;
  if (frames.count() > 0) {
    const DiscMetadataDocument doc = build_disc_metadata_document(
        collect_disc_metadata_frames(observations, frames.first * 2,
                                     frames.count()),
        system, /*is_tff=*/true, frames.first, /*generator_version=*/"");
    const DiscStatusSummary& s = doc.status;
    if (s.user_code) out["userCode"] = std::string(*s.user_code);
    if (s.is_side_1) out["discSide"] = int64_t{*s.is_side_1 ? 1 : 2};
    if (s.is_12_inch) out["discSizeInches"] = int64_t{*s.is_12_inch ? 12 : 8};
    if (s.cx_enabled) out["cx"] = bool{*s.cx_enabled};
    if (s.has_teletext) out["teletext"] = bool{*s.has_teletext};
    if (s.is_digital) out["digitalVideo"] = bool{*s.is_digital};
    if (s.sound_mode) {
      out["soundMode"] = std::string(disc_sound_mode_name(*s.sound_mode));
    }
    if (s.fm_multiplex) out["fmMultiplex"] = bool{*s.fm_multiplex};
    if (s.programme_dump) out["programmeDump"] = bool{*s.programme_dump};
    out["amendment2"] = bool{s.amendment2_present};
    if (s.a2_copy_permitted) {
      out["amendment2CopyPermitted"] = bool{*s.a2_copy_permitted};
    }
    if (s.a2_video_standard) {
      out["amendment2VideoStandard"] = bool{*s.a2_video_standard};
    }
    if (s.a2_sound_mode) {
      out["amendment2SoundMode"] =
          std::string(disc_sound_mode_name(*s.a2_sound_mode));
    }
    // Rounded as the document writes it.
    if (s.fields_with_status > 0) {
      out["statusAgreement"] = std::round(s.agreement * 10000.0) / 10000.0;
    }
    out["leadInSeen"] = !doc.events.lead_in.empty();
    out["leadOutSeen"] = !doc.events.lead_out.empty();
  }
  // From the mapping rather than the document's address runs, which over a
  // whole side include the odd stray number a frame misread.
  if (decision.is_cav && decision.stats.first_picture &&
      decision.stats.last_picture) {
    out["firstPicture"] = static_cast<int64_t>(*decision.stats.first_picture);
    out["lastPicture"] = static_cast<int64_t>(*decision.stats.last_picture);
  }
  return out;
}

StageAnalysisReport run(const std::map<std::string, ParameterValue>& params,
                        IToolJobContext& context,
                        const ObservationPassOptions& options) {
  JobProgress progress(context);
  progress.setStatus("Initializing disc mapper analysis...");
  progress.setProgress(0);

  const auto& inputs = context.inputs();
  if (inputs.empty() || !inputs[0]) {
    return failed("Frame map node has no input connected");
  }
  auto source = std::dynamic_pointer_cast<VideoFrameRepresentation>(inputs[0]);
  if (!source) {
    return failed("Input node did not produce VideoFrameRepresentation");
  }

  try {
    ORC_LOG_DEBUG("Disc Mapper: {} frames ({} fields)", source->frame_count(),
                  source->frame_count() * 2);

    progress.setStatus("Extracting VBI and signal quality data...");
    progress.setProgress(0);

    // Sweep the observers the mapper depends on over every frame. The sweep
    // reads the whole recording, so it dominates the analysis; it runs across
    // several workers, each on a private copy of the inputs, where it can.
    // Sharing one source would race: stages keep configuration in members,
    // and a source's cache hands out pointers into storage another thread
    // can evict.
    ObservationContext observations;
    const auto frame_range = source->frame_range();
    const auto make_worker_source = [&context]() -> ObservationWorkerSource {
      ObservationWorkerSource worker;
      auto copy =
          std::make_shared<ToolJobPrivateInputs>(context.open_private_inputs());
      if (copy->inputs.empty()) return worker;
      auto vfr =
          std::dynamic_pointer_cast<VideoFrameRepresentation>(copy->inputs[0]);
      if (!vfr) return worker;
      worker.source = std::move(vfr);
      worker.owner = std::move(copy);
      return worker;
    };
    ObservationPassMonitor monitor;
    monitor.progress = [&progress](uint64_t observed, uint64_t total) {
      progress.setProgress(static_cast<int>(observed * 100 / total));
      progress.setSubStatus("Frame " + std::to_string(observed) + " / " +
                            std::to_string(total));
    };
    monitor.cancelled = [&context] { return context.is_cancelled(); };

    const ObservationPassOutcome outcome =
        run_disc_analysis_observers(*source, frame_range, observations, monitor,
                                    make_worker_source, options);
    if (outcome.cancelled) {
      return cancelled();
    }
    progress.setProgress(100);
    progress.setSubStatus("");
    ORC_LOG_DEBUG("Disc Mapper: observers complete ({} worker(s))",
                  outcome.workers_used);

    if (context.is_cancelled()) {
      return cancelled();
    }

    DiscMapperAnalyzer analyzer;
    DiscMapperAnalyzer::Options analyzer_options;
    const auto include_it = params.find(kIncludeLeadInOut);
    if (include_it != params.end() &&
        std::holds_alternative<bool>(include_it->second)) {
      analyzer_options.include_lead_in_out = std::get<bool>(include_it->second);
    }

    // Each step reports its own 0-100% progress.
    const FieldMappingDecision decision =
        analyzer.analyze(*source, observations, analyzer_options, &progress);

    if (!decision.success) {
      std::ostringstream failure_summary;
      failure_summary << (decision.rationale.empty()
                              ? "Disc mapper analysis failed to produce a "
                                "mapping"
                              : decision.rationale);
      append_warnings(failure_summary, decision.warnings);
      auto report = failed(failure_summary.str());
      add_warning_items(decision.warnings, report);
      return report;
    }

    if (context.is_cancelled()) {
      return cancelled();
    }

    progress.setStatus("Processing results...");
    progress.setProgress(0);

    StageAnalysisReport report;
    add_warning_items(decision.warnings, report);
    report.summary = summary_of(decision, analyzer_options);
    add_statistics(decision, report);

    // What the disc says of itself, read over every field of the side.
    const auto video = source->get_video_parameters();
    const VideoSystem system =
        video ? video->system
              : (decision.is_pal ? VideoSystem::PAL : VideoSystem::NTSC);
    for (auto& [key, value] : disc_identity_statistics(
             observations, frame_range, system, decision)) {
      report.statistics[key] = std::move(value);
    }

    // An empty spec means no frame could be mapped (no readable picture
    // numbers or time codes). Writing it would empty the Frame Map's ranges,
    // so the disc would pass through unmapped; there is nothing to apply.
    if (!decision.mapping_spec.empty()) {
      report.parameter_changes["ranges"] = decision.mapping_spec;
    }
    ORC_LOG_DEBUG("Disc Mapper: mapping spec of {} chars",
                  decision.mapping_spec.length());

    progress.setStatus("Analysis complete");
    progress.setProgress(100);
    report.status = StageAnalysisReport::Status::Success;
    return report;
  } catch (const std::exception& e) {
    ORC_LOG_ERROR("Disc Mapper failed: {}", e.what());
    return failed(std::string("Analysis failed: ") + e.what());
  }
}

}  // namespace orc::disc_mapper_tool
