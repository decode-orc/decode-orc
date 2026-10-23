/*
 * File:        burst_level_analysis_sink_deps.cpp
 * Module:      orc-core
 * Purpose:     BurstLevelAnalysisSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "burst_level_analysis_sink_deps.h"

#include <orc/stage/field_id.h>
#include <orc/support/logging.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <ostream>
#include <utility>
#include <variant>

namespace {

// Write `path` atomically: stream into a temporary sibling file and rename it
// into place only after a fully successful write. A cancelled or failed run
// therefore never leaves a truncated file at `path`.
template <typename Logger, typename Writer>
bool write_file_atomically(const std::string& path, Logger& logger,
                           Writer&& writer) {
  const std::string tmp_path = path + ".tmp";
  {
    std::ofstream out(tmp_path,
                      std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out.is_open()) {
      logger.error("Failed to open temporary file: {}", tmp_path);
      return false;
    }
    writer(out);
    out.flush();
    if (!out.good()) {
      logger.error("Failed writing temporary file: {}", tmp_path);
      out.close();
      std::remove(tmp_path.c_str());
      return false;
    }
  }
  if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
    logger.error("Failed to move {} into place at {}", tmp_path, path);
    std::remove(tmp_path.c_str());
    return false;
  }
  return true;
}

}  // namespace

namespace orc {
void BurstLevelAnalysisSinkStageDeps::init(
    TriggerProgressCallback progress_callback,
    std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

BurstAnalysisComputeResult BurstLevelAnalysisSinkStageDeps::compute_and_analyze(
    VideoFrameRepresentation* representation,
    IObservationContext& observation_context,
    BurstAnalysisComputeOptions options) {
  if (!representation) {
    return {false, "Input representation is null", {}, 0};
  }

  (void)options.output_path;
  (void)options.write_csv;

  BurstAnalysisComputeResult result;
  result.success = true;
  result.message = "Burst level analysis complete";

  if (representation->has_unbounded_frame_range()) {
    return {false,
            "Input source has an unbounded frame range (a piped/live "
            "source left frame_count at 0) — burst level analysis needs a "
            "known length; set an explicit frame_count on the source.",
            {},
            0};
  }

  const auto frame_rng = representation->frame_range();
  const uint64_t total_frames = frame_rng.count();

  if (total_frames == 0) {
    logger_.warn("BurstLevelAnalysisSinkDeps: No frames available");
    result.total_frames = 0;
    return result;
  }

  // A single observer session is reused across every sampled frame (the burst
  // level observer holds no cross-frame state; its observation field is read
  // and cleared each iteration). A null service — e.g. an older host — leaves
  // the handle null and the per-frame observation is skipped.
  std::unique_ptr<IObserverHandle> burst_level_handle;
  if (observation_service_) {
    burst_level_handle = observation_service_->create_observer("burst_level");
  } else {
    logger_.warn(
        "BurstLevelAnalysisSinkDeps: observation service unavailable; burst "
        "level observations skipped");
  }

  // Canonical per-frame capture: analyse every frame in the range and record
  // each frame's true frame number. Display bucketing is applied downstream by
  // the shared decimation utility
  // (orc/core/analysis/analysis_series_decimator), not here.
  logger_.debug("BurstLevelAnalysisSinkDeps: analysing {} frames",
                total_frames);

  result.frame_stats.reserve(static_cast<size_t>(total_frames));

  uint64_t analysed = 0;
  for (uint64_t offset = 0; offset < total_frames; ++offset) {
    if (cancel_requested_ && cancel_requested_->load()) {
      logger_.warn(
          "BurstLevelAnalysisSinkDeps: Cancel requested at frame offset {}",
          offset);
      result.success = false;
      result.message = "Cancelled by user";
      result.frame_stats.clear();
      result.total_frames = 0;
      return result;
    }

    const FrameID fid = frame_rng.first + offset;
    const FieldID frame_fid(fid * 2U);

    // Phase 5.3: reuse a pre-loaded observation when the host has already
    // supplied this frame's value from the provenance-keyed store. burst_level
    // is stateless, so per-frame skipping is safe.
    if (burst_level_handle && !observation_context.has(frame_fid, "burst_level",
                                                       "median_burst_10bit")) {
      burst_level_handle->process_frame(*representation, fid,
                                        observation_context);
    }

    auto val =
        observation_context.get(frame_fid, "burst_level", "median_burst_10bit");

    FrameBurstLevelStats frame_stat;
    frame_stat.frame_number = static_cast<int32_t>(fid) + 1;
    if (val && std::holds_alternative<double>(*val)) {
      frame_stat.median_burst_10bit = std::get<double>(*val);
      frame_stat.has_data = true;
    }
    result.frame_stats.push_back(frame_stat);

    observation_context.clear_field(frame_fid);

    ++analysed;
    if (progress_callback_ &&
        (analysed % 50 == 0 || analysed == total_frames)) {
      progress_callback_(analysed, total_frames,
                         "Analysing frame " + std::to_string(analysed) + "/" +
                             std::to_string(total_frames));
    }
  }

  result.total_frames = static_cast<int32_t>(total_frames);
  logger_.debug(
      "BurstLevelAnalysisSinkDeps: Complete — {} analysed frames from {}",
      result.frame_stats.size(), total_frames);

  return result;
}

void BurstLevelAnalysisSinkStageDeps::write_csv(
    std::ostream& os, const std::vector<FrameBurstLevelStats>& frame_stats) {
  // Canonical per-frame schema. One row per analysed frame; an absent value is
  // written as an empty field (never the string "nan"). Units live in the
  // header name (10-bit sample units); values are plain numbers.
  os << "frame_number,median_burst_10bit\n";
  for (const auto& fs : frame_stats) {
    os << fs.frame_number << ',';
    if (fs.has_data) os << fs.median_burst_10bit;
    os << '\n';
  }
}

bool BurstLevelAnalysisSinkStageDeps::write_csv(
    const std::string& path,
    const std::vector<FrameBurstLevelStats>& frame_stats) {
  if (frame_stats.empty()) {
    logger_.warn("BurstLevelAnalysisSinkDeps: No data to write");
    return false;
  }

  logger_.debug("BurstLevelAnalysisSinkDeps: Writing CSV to: {}", path);

  const bool ok = write_file_atomically(
      path, logger_, [&](std::ostream& os) { write_csv(os, frame_stats); });
  if (ok) {
    logger_.debug(
        "BurstLevelAnalysisSinkDeps: Successfully wrote {} data rows to: {}",
        frame_stats.size(), path);
  }
  return ok;
}
}  // namespace orc
