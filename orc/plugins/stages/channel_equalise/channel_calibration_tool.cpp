/*
 * File:        channel_calibration_tool.cpp
 * Module:      orc-stage-plugin-channel-equalise
 * Purpose:     Channel Calibration and Channel Tracking, the stage's analysis
 *              tools: set the edge smear from the VITS on the stage's input,
 *              for the whole source or frame by frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "channel_calibration_tool.h"

#include <orc/stage/video_frame_representation.h>
#include <orc/support/channel_equalise.h>

#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace orc::channel_calibration_tool {

namespace {

std::string fixed(double value, int digits) {
  std::ostringstream s;
  s << std::fixed << std::setprecision(digits) << value;
  return s.str();
}

int64_t count(size_t value) { return static_cast<int64_t>(value); }

std::string verdict_text(SmearVerdict verdict) {
  switch (verdict) {
    case SmearVerdict::Smear:
      return "smear";
    case SmearVerdict::NoSmear:
      return "no smear";
    case SmearVerdict::OutOfRange:
      return "smear beyond the stage's limits";
    case SmearVerdict::CannotTell:
      break;
  }
  return "cannot tell";
}

}  // namespace

StageAnalysisReport describe(const ChannelCalibration& calibration) {
  StageAnalysisReport report;
  report.status = StageAnalysisReport::Status::Success;
  size_t frames_used = 0;
  for (const auto& window : calibration.windows) {
    frames_used += window.frames_used;
    StageAnalysisItem item;
    item.type = "window";
    item.start_frame = count(window.first);
    item.end_frame = count(window.first + window.frames - 1);
    item.message = verdict_text(window.verdict);
    item.data["framesUsed"] = count(window.frames_used);
    if (window.fit) {
      item.message += ": fraction " + fixed(window.fit->fraction, 3) +
                      ", tau " + fixed(window.fit->tau_samples, 2) + " samples";
      item.data["fraction"] = window.fit->fraction;
      item.data["tauSamples"] = window.fit->tau_samples;
      item.data["residualIre"] = window.fit->residual_ire;
      item.data["residualNoSmearIre"] = window.fit->residual_no_smear_ire;
    }
    report.items.push_back(std::move(item));
  }
  report.statistics["windows"] = count(calibration.windows.size());
  report.statistics["framesUsed"] = count(frames_used);

  switch (calibration.verdict) {
    case SmearVerdict::CannotTell:
      report.summary =
          "No smear could be measured: none of the frames read has a usable "
          "white bar and 2T pulse (CCIR 17) on field 1 line 19. The stage's "
          "values are left as they are.";
      return report;
    case SmearVerdict::OutOfRange:
      report.summary =
          "The edge smear is longer than Channel Equalise can correct: a "
          "time constant over " +
          fixed(channel_equalise::kMaxTauSamples, 0) +
          " samples (see the windows). The stage's values are left as they "
          "are.";
      return report;
    case SmearVerdict::NoSmear:
      report.summary =
          "No smear: the test signals on field 1 line 19 are explained "
          "without one. Leave Channel Equalise out of this project; Apply to "
          "Stage sets smear_fraction to 0, which passes every frame through "
          "unchanged.";
      report.statistics["smearFraction"] = 0.0;
      report.parameter_changes["smear_fraction"] = 0.0;
      return report;
    case SmearVerdict::Smear:
      break;
  }
  report.statistics["smearFraction"] = calibration.fraction;
  report.statistics["smearTauSamples"] = calibration.tau_samples;
  report.summary = "Edge smear: fraction " + fixed(calibration.fraction, 3) +
                   ", time constant " + fixed(calibration.tau_samples, 2) +
                   " samples.\nMeasured on field 1 line 19 of " +
                   std::to_string(frames_used) +
                   " frames.\nApply to Stage "
                   "sets smear_fraction and smear_tau_samples.";
  // As the old tool wrote them: four and three decimals.
  report.parameter_changes["smear_fraction"] =
      std::stod(fixed(calibration.fraction, 4));
  report.parameter_changes["smear_tau_samples"] =
      std::stod(fixed(calibration.tau_samples, 3));
  return report;
}

namespace {

// The node's input when it is PAL composite video; otherwise |report| says
// why not and nullptr is returned.
std::shared_ptr<const VideoFrameRepresentation> pal_input(
    IToolJobContext& context, StageAnalysisReport& report) {
  auto fail = [&report](const std::string& why) {
    report.status = StageAnalysisReport::Status::Failed;
    report.summary = why;
    return nullptr;
  };
  if (context.inputs().empty()) {
    return fail("Connect a source to the Channel Equalise node first.");
  }
  auto source = std::dynamic_pointer_cast<const VideoFrameRepresentation>(
      context.inputs().front());
  if (!source) {
    return fail("The input produces no video.");
  }
  const auto params = source->get_video_parameters();
  if (!params || params->system != VideoSystem::PAL ||
      source->has_separate_channels()) {
    return fail(
        "The edge smear is a PAL LaserDisc effect: the input must be a PAL "
        "composite source.");
  }
  return source;
}

}  // namespace

StageAnalysisReport run(IToolJobContext& context) {
  StageAnalysisReport report;
  const auto source = pal_input(context, report);
  if (!source) return report;

  const auto calibration =
      calibrate_channel(*source, ChannelCalibrationOptions{},
                        [&context](int percent, const std::string& status) {
                          context.report_progress(percent / 100.0, status);
                          return !context.is_cancelled();
                        });
  if (context.is_cancelled()) {
    report.status = StageAnalysisReport::Status::Cancelled;
    return report;
  }
  return describe(calibration);
}

StageAnalysisReport describe_track(const ChannelTrack& track) {
  StageAnalysisReport report;
  report.status = StageAnalysisReport::Status::Success;
  // One item per run of windows with the same choice.
  for (size_t i = 0; i < track.blocks.size();) {
    size_t j = i;
    while (j < track.blocks.size() &&
           track.blocks[j].choice == track.blocks[i].choice) {
      ++j;
    }
    StageAnalysisItem item;
    item.type = "run";
    item.start_frame = count(track.blocks[i].first);
    item.end_frame =
        count(track.blocks[j - 1].first + track.blocks[j - 1].frames - 1);
    item.message = track.blocks[i].choice;
    item.data["windows"] = count(j - i);
    report.items.push_back(std::move(item));
    i = j;
  }
  size_t upstream = 0;
  size_t tracked = 0;
  size_t constrained = 0;
  size_t reverted = 0;
  for (const auto& block : track.blocks) {
    if (block.choice == "tracked") ++tracked;
    if (block.choice == "constrained") ++constrained;
    if (block.choice == "upstream") ++upstream;
    if (block.reverted) ++reverted;
  }
  report.statistics["windows"] = count(track.blocks.size());
  report.statistics["upstreamWindows"] = count(upstream);
  report.statistics["trackedWindows"] = count(tracked);
  report.statistics["constrainedWindows"] = count(constrained);
  report.statistics["revertedWindows"] = count(reverted);
  report.statistics["pictureEdgesField1"] = count(track.picture_edges_found[0]);
  report.statistics["pictureEdgesField2"] = count(track.picture_edges_found[1]);
  report.statistics["fieldsRead"] = count(track.fields_read);
  report.statistics["fieldsMeasured"] = count(track.fields_measured);
  report.statistics["knots"] = count(track.track.knots().size());
  report.statistics["knotsField2"] = count(track.track_field2.knots().size());
  report.statistics["upstreamFraction"] = track.upstream.fraction;

  if (track.blocks.empty()) {
    report.summary =
        "No frames to track: the input is empty. The stage's values are left "
        "as they are.";
    return report;
  }
  if (track.fields_measured == 0) {
    report.summary =
        "No smear could be measured: no field has a usable white bar and 2T "
        "pulse (CCIR 17) on line 19. The stage's values are left as they "
        "are.";
    return report;
  }
  report.parameter_changes["smear_track"] = track.track.to_string();
  report.parameter_changes["smear_track_field2"] =
      track.track_field2.to_string();
  if (track.track.empty() && track.track_field2.empty()) {
    report.summary =
        "No correction anywhere along the source: Channel Calibration finds "
        "no smear, and no window certifies a correction against leaving the "
        "picture as it is. Apply to Stage clears the tracks and sets "
        "smear_fraction to 0, which passes every frame through unchanged.";
    report.parameter_changes["smear_fraction"] = 0.0;
    return report;
  }
  report.summary =
      "Edge smear tracked along the source in " +
      std::to_string(track.blocks.size()) +
      " windows: " + std::to_string(constrained + tracked) +
      " certified better than Channel Calibration's smear on every measure (" +
      std::to_string(constrained) + " by the constrained search, " +
      std::to_string(tracked) + " by the tracked fit), " +
      std::to_string(upstream) + " keep Channel Calibration's smear" +
      (reverted > 0 ? " (" + std::to_string(reverted) +
                          " of them because the track interpolated through "
                          "their own correction was not certified)"
                    : std::string()) +
      ". The track was certified as the stage applies it. Measured on line "
      "19 of " +
      std::to_string(track.fields_measured) +
      " fields.\nApply to Stage sets smear_track and smear_track_field2 (" +
      std::to_string(track.track.knots().size()) + " and " +
      std::to_string(track.track_field2.knots().size()) + " knots).";
  return report;
}

StageAnalysisReport run_tracking(IToolJobContext& context,
                                 const ChannelTrackOptions& options) {
  StageAnalysisReport report;
  const auto source = pal_input(context, report);
  if (!source) return report;

  const auto track = track_channel(
      *source, options, [&context](int percent, const std::string& status) {
        context.report_progress(percent / 100.0, status);
        return !context.is_cancelled();
      });
  if (context.is_cancelled()) {
    report.status = StageAnalysisReport::Status::Cancelled;
    return report;
  }
  return describe_track(track);
}

}  // namespace orc::channel_calibration_tool
