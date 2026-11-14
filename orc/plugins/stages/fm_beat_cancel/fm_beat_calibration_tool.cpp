/*
 * File:        fm_beat_calibration_tool.cpp
 * Module:      orc-stage-plugin-fm-beat-cancel
 * Purpose:     FM Beat Calibration, the stage's analysis tool: sets the
 *              carrier from the beat on the stage's input
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "fm_beat_calibration_tool.h"

#include <orc/stage/video_frame_representation.h>
#include <orc/support/fm_beat.h>

#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

#include "fm_beat_calibration.h"

namespace orc::fm_beat_calibration_tool {

namespace {

std::string fixed(double value, int digits) {
  std::ostringstream s;
  s << std::fixed << std::setprecision(digits) << value;
  return s.str();
}

std::string mhz(double hz) { return fixed(hz / 1.0e6, 4); }

int64_t count(size_t value) { return static_cast<int64_t>(value); }

}  // namespace

StageAnalysisReport run(IToolJobContext& context) {
  StageAnalysisReport report;
  auto fail = [&report](const std::string& why) {
    report.status = StageAnalysisReport::Status::Failed;
    report.summary = why;
    return report;
  };

  std::shared_ptr<const VideoFrameRepresentation> source;
  if (!context.inputs().empty()) {
    source = std::dynamic_pointer_cast<const VideoFrameRepresentation>(
        context.inputs().front());
  }
  if (context.inputs().empty()) {
    return fail("Connect a source to the FM Beat Cancel node first.");
  }
  if (!source) {
    return fail("The input produces no video.");
  }
  const auto params = source->get_video_parameters();
  if (!params || params->system != VideoSystem::PAL ||
      source->has_separate_channels()) {
    return fail(
        "The FM-beat herringbone is a PAL LaserDisc effect: the input must be "
        "a PAL composite source.");
  }

  const auto calibration =
      calibrate_fm_beat(*source, FmBeatCalibrationOptions{},
                        [&context](int percent, const std::string& status) {
                          context.report_progress(percent / 100.0, status);
                          return !context.is_cancelled();
                        });
  if (context.is_cancelled()) {
    report.status = StageAnalysisReport::Status::Cancelled;
    return report;
  }

  report.status = StageAnalysisReport::Status::Success;
  report.statistics["probedFrames"] = count(calibration.probed_frames);
  report.statistics["fieldsSearched"] = count(calibration.fields);
  if (calibration.fields == 0) {
    report.summary =
        "No field of the " + std::to_string(calibration.probed_frames) +
        " frames read has enough flat saturated colour to measure a beat on. "
        "FM Beat Cancel leaves such pictures as they are, so it has nothing to "
        "do on this source.";
    return report;
  }
  for (const auto& [carrier, energy] : calibration.sweep) {
    StageAnalysisItem item;
    item.type = "sweep";
    item.message = mhz(carrier) + " MHz";
    item.data["carrierMHz"] = carrier / 1.0e6;
    item.data["removedEnergy"] = energy;
    report.items.push_back(std::move(item));
  }
  if (calibration.peak_at_edge) {
    report.summary =
        "No carrier found between " + mhz(fm_beat::kMinCarrierBlackHz) +
        " and " + mhz(fm_beat::kMaxCarrierBlackHz) + " MHz: on the " +
        std::to_string(calibration.fields) +
        " fields with the most flat colour, the beat removed only grows "
        "towards an end of the range, with no peak inside it. The carrier "
        "was left unchanged.";
    return report;
  }
  report.statistics["carrierBlackMHz"] = calibration.carrier_black_hz / 1.0e6;
  report.statistics["linesCorrected"] = count(calibration.lines_corrected);
  report.statistics["pictureLines"] = count(calibration.picture_lines);
  report.statistics["removedRmsIre"] = calibration.removed_rms_ire;
  if (!calibration.found) {
    report.summary =
        "No beat found: on the " + std::to_string(calibration.fields) +
        " fields with the most flat colour, no carrier corrects any line. "
        "This source carries no beat FM Beat Cancel can remove.";
    return report;
  }
  report.summary =
      "Carrier at black: " + mhz(calibration.carrier_black_hz) + " MHz.\n" +
      std::to_string(calibration.lines_corrected) + " of " +
      std::to_string(calibration.picture_lines) +
      " picture lines corrected on " + std::to_string(calibration.fields) +
      " fields with flat colour, " + fixed(calibration.removed_rms_ire, 2) +
      " IRE RMS removed on them.\nApply to Stage sets carrier_black_mhz.";
  // As the report rounds it.
  report.parameter_changes["carrier_black_mhz"] =
      std::stod(mhz(calibration.carrier_black_hz));
  return report;
}

}  // namespace orc::fm_beat_calibration_tool
