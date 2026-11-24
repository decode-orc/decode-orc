/*
 * File:        edge_ring_calibration_tool.cpp
 * Module:      orc-stage-plugin-edge-ring-correct
 * Purpose:     Edge Ring Calibration, the stage's analysis tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "edge_ring_calibration_tool.h"

#include <orc/stage/video_frame_representation.h>

#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>

namespace orc::edge_ring_calibration_tool {

namespace {

std::string fixed(double value, int digits) {
  std::ostringstream s;
  s << std::fixed << std::setprecision(digits) << value;
  return s.str();
}

}  // namespace

StageAnalysisReport report(const EdgeRingCalibration& c) {
  StageAnalysisReport r;
  r.status = StageAnalysisReport::Status::Success;
  int64_t fitted = 0, accepted = 0;
  for (const auto& w : c.windows) {
    fitted += w.fitted;
    accepted += w.accepted;
    StageAnalysisItem item;
    item.type = w.accepted ? "corrected" : "identity";
    item.start_frame = w.first_frame;
    item.end_frame = w.end_frame - 1;
    if (w.accepted) {
      item.message = std::string(w.chosen.adapt ? "per-frame" : "fixed") +
                     " gains rise " + fixed(w.chosen.gains.rise, 2) +
                     ", fall " + fixed(w.chosen.gains.fall, 2) + ", line " +
                     fixed(w.chosen.gains.line, 2);
    } else {
      item.message = w.reasons.empty() ? "not certified" : w.reasons.front();
    }
    for (const auto& [name, m] : w.held_out.measures) {
      if (!m.measured) continue;
      item.data[name + "_before"] = m.before;
      item.data[name + "_after"] = m.after;
    }
    r.items.push_back(std::move(item));
  }
  r.statistics["windows"] = static_cast<int64_t>(c.windows.size());
  r.statistics["windowsFitted"] = fitted;
  r.statistics["windowsCorrected"] = accepted;
  if (c.windows.empty()) {
    r.summary = "Nothing was read: the input must be a PAL composite source.";
    return r;
  }
  if (accepted == 0) {
    r.summary =
        "No window's correction was certified better than leaving it "
        "alone (" +
        std::to_string(fitted) + " of " + std::to_string(c.windows.size()) +
        " windows had enough sharp edges or thin lines to fit). Edge "
        "Ring Correct leaves this source as it is.";
    r.parameter_changes["ring_track"] = std::string();
    return r;
  }
  r.summary = std::to_string(accepted) + " of " +
              std::to_string(c.windows.size()) +
              " windows corrected; the others are left as they are.\n"
              "Apply to Stage sets ring_track.";
  r.parameter_changes["ring_track"] = c.track.to_string();
  return r;
}

StageAnalysisReport run(IToolJobContext& context,
                        const EdgeRingCalibrationOptions& options) {
  StageAnalysisReport r;
  auto fail = [&r](const std::string& why) {
    r.status = StageAnalysisReport::Status::Failed;
    r.summary = why;
    return r;
  };
  if (context.inputs().empty()) {
    return fail("Connect a source to the Edge Ring Correct node first.");
  }
  auto source = std::dynamic_pointer_cast<const VideoFrameRepresentation>(
      context.inputs().front());
  if (!source) return fail("The input produces no video.");
  const auto params = source->get_video_parameters();
  if (!params || params->system != VideoSystem::PAL ||
      source->has_separate_channels()) {
    return fail("Edge Ring Correct works on PAL composite sources.");
  }
  const auto c = calibrate_edge_ring(
      *source, options, [&context](int percent, const std::string& status) {
        context.report_progress(percent / 100.0, status);
        return !context.is_cancelled();
      });
  if (context.is_cancelled()) {
    r.status = StageAnalysisReport::Status::Cancelled;
    return r;
  }
  return report(c);
}

}  // namespace orc::edge_ring_calibration_tool
