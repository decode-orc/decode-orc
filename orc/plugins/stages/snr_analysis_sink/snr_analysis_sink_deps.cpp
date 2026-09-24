/*
 * File:        snr_analysis_sink_deps.cpp
 * Module:      orc-core
 * Purpose:     SNRAnalysisSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "snr_analysis_sink_deps.h"

#include <orc/stage/field_id.h>

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
void SNRAnalysisSinkStageDeps::init(TriggerProgressCallback progress_callback,
                                    std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

SNRAnalysisComputeResult SNRAnalysisSinkStageDeps::compute_and_analyze(
    VideoFrameRepresentation* representation,
    IObservationContext& observation_context,
    SNRAnalysisComputeOptions options) {
  if (!representation) {
    return {false, "Input representation is null", {}, 0};
  }

  (void)options.output_path;
  (void)options.write_csv;

  SNRAnalysisComputeResult result;
  result.success = true;
  result.message = "SNR analysis complete";

  if (representation->has_unbounded_frame_range()) {
    return {false,
            "Input source has an unbounded frame range (a piped/live "
            "source left frame_count at 0) — SNR analysis needs a known "
            "length; set an explicit frame_count on the source.",
            {},
            0};
  }

  const auto frame_rng = representation->frame_range();
  const uint64_t total_frames = frame_rng.count();

  if (total_frames == 0) {
    logger_.warn("SNRAnalysisSinkDeps: No frames available");
    result.total_frames = 0;
    return result;
  }

  // Observer sessions are created once and reused across every sampled frame
  // (these observers hold no cross-frame state; the observation field is read
  // and cleared each iteration). A null service — e.g. an older host — leaves
  // the handles null and the per-frame observation is skipped.
  std::unique_ptr<IObserverHandle> white_snr_handle;
  std::unique_ptr<IObserverHandle> black_psnr_handle;
  if (observation_service_) {
    white_snr_handle = observation_service_->create_observer("white_snr");
    black_psnr_handle = observation_service_->create_observer("black_psnr");
  } else {
    logger_.warn(
        "SNRAnalysisSinkDeps: observation service unavailable; SNR "
        "observations skipped");
  }

  // Canonical per-frame capture: analyse every frame in the range and record
  // each frame's true frame number. Display bucketing is applied downstream by
  // the shared decimation utility
  // (orc/core/analysis/analysis_series_decimator), not here.
  logger_.debug("SNRAnalysisSinkDeps: analysing {} frames", total_frames);

  result.frame_stats.reserve(static_cast<size_t>(total_frames));

  uint64_t analysed = 0;
  for (uint64_t offset = 0; offset < total_frames; ++offset) {
    if (cancel_requested_ && cancel_requested_->load()) {
      logger_.warn("SNRAnalysisSinkDeps: Cancel requested at frame offset {}",
                   offset);
      result.success = false;
      result.message = "Cancelled by user";
      result.frame_stats.clear();
      result.total_frames = 0;
      return result;
    }

    const FrameID fid = frame_rng.first + offset;
    const FieldID frame_fid(fid * 2U);

    // Phase 5.3: reuse already-computed observations. When the host has
    // pre-loaded this frame's value into the context (from the provenance-keyed
    // store), skip re-running the observer. white_snr/black_psnr are stateless,
    // so a per-frame skip is safe.
    if (white_snr_handle &&
        (options.snr_mode == SNRAnalysisMode::WHITE ||
         options.snr_mode == SNRAnalysisMode::BOTH) &&
        !observation_context.has(frame_fid, "white_snr", "snr_db")) {
      white_snr_handle->process_frame(*representation, fid,
                                      observation_context);
    }
    if (black_psnr_handle &&
        (options.snr_mode == SNRAnalysisMode::BLACK ||
         options.snr_mode == SNRAnalysisMode::BOTH) &&
        !observation_context.has(frame_fid, "black_psnr", "psnr_db")) {
      black_psnr_handle->process_frame(*representation, fid,
                                       observation_context);
    }

    FrameSNRStats frame_stat;
    frame_stat.frame_number = static_cast<int32_t>(fid) + 1;

    auto white_val = observation_context.get(frame_fid, "white_snr", "snr_db");
    if (white_val && std::holds_alternative<double>(*white_val)) {
      frame_stat.white_snr = std::get<double>(*white_val);
      frame_stat.has_white_snr = true;
    }

    auto black_val =
        observation_context.get(frame_fid, "black_psnr", "psnr_db");
    if (black_val && std::holds_alternative<double>(*black_val)) {
      frame_stat.black_psnr = std::get<double>(*black_val);
      frame_stat.has_black_psnr = true;
    }

    observation_context.clear_field(frame_fid);

    frame_stat.has_data = frame_stat.has_white_snr || frame_stat.has_black_psnr;
    result.frame_stats.push_back(frame_stat);

    ++analysed;
    if (progress_callback_ &&
        (analysed % 50 == 0 || analysed == total_frames)) {
      progress_callback_(analysed, total_frames,
                         "Analysing frame " + std::to_string(analysed) + "/" +
                             std::to_string(total_frames));
    }
  }

  result.total_frames = static_cast<int32_t>(total_frames);
  logger_.debug("SNRAnalysisSinkDeps: Complete — {} analysed frames from {}",
                result.frame_stats.size(), total_frames);

  return result;
}

void SNRAnalysisSinkStageDeps::write_csv(
    std::ostream& os, const std::vector<FrameSNRStats>& frame_stats) {
  // Canonical per-frame schema. One row per analysed frame; an absent metric is
  // written as an empty field (never the string "nan"). Units live in the
  // header names (dB); values are plain numbers.
  os << "frame_number,white_snr_db,black_psnr_db\n";
  for (const auto& fs : frame_stats) {
    os << fs.frame_number << ',';
    if (fs.has_white_snr) os << fs.white_snr;
    os << ',';
    if (fs.has_black_psnr) os << fs.black_psnr;
    os << '\n';
  }
}

bool SNRAnalysisSinkStageDeps::write_csv(
    const std::string& path, const std::vector<FrameSNRStats>& frame_stats) {
  if (frame_stats.empty()) {
    logger_.warn("SNRAnalysisSinkDeps: No data to write");
    return false;
  }

  logger_.debug("SNRAnalysisSinkDeps: Writing CSV to: {}", path);

  const bool ok = write_file_atomically(
      path, logger_, [&](std::ostream& os) { write_csv(os, frame_stats); });
  if (ok) {
    logger_.debug("SNRAnalysisSinkDeps: Successfully wrote {} data rows to: {}",
                  frame_stats.size(), path);
  }
  return ok;
}
}  // namespace orc
