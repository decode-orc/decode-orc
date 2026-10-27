/*
 * File:        dropout_analysis_sink_deps.cpp
 * Module:      orc-core
 * Purpose:     DropoutAnalysisSinkStage dependency implementation (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "dropout_analysis_sink_deps.h"

#include <orc/support/frame_read_ahead.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <ostream>
#include <stdexcept>
#include <utility>
#include <vector>

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

void DropoutAnalysisSinkStageDeps::init(
    TriggerProgressCallback progress_callback,
    std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

DropoutAnalysisComputeResult DropoutAnalysisSinkStageDeps::compute_and_analyze(
    VideoFrameRepresentation* representation,
    IObservationContext& observation_context,
    DropoutAnalysisComputeOptions options) {
  (void)observation_context;

  if (!representation) {
    DropoutAnalysisComputeResult err;
    err.success = false;
    err.message = "Input representation is null";
    return err;
  }

  DropoutAnalysisComputeResult result;
  result.success = true;
  result.message = "Dropout analysis complete";

  auto range = representation->frame_range();
  if (range.count() == 0) {
    logger_.warn("DropoutAnalysisSinkDeps: No frames available");
    result.total_frames = 0;
    return result;
  }

  const size_t total_frames_count = static_cast<size_t>(range.count());
  const auto video_params = representation->get_video_parameters();

  // Nominal samples per line for line-position approximation in visible-area
  // mode. PAL: 1135, NTSC: 910.
  int32_t nominal_spl = 910;
  if (video_params) nominal_spl = video_params->frame_width_nominal;

  // Canonical per-frame capture: one record per analysed frame, carrying that
  // frame's true frame number. Display bucketing is applied downstream by the
  // shared decimation utility (orc/core/analysis/analysis_series_decimator),
  // not here.
  result.frame_stats.resize(total_frames_count);

  // A stacked input builds a frame's dropout hints by stacking the frame, so
  // the hints ahead are built on other threads while this loop runs in order.
  FrameReadAhead read_ahead(range.first, range.last,
                            [representation](FrameID id) {
                              if (representation->get_frame_descriptor(id)) {
                                (void)representation->get_dropout_hints(id);
                              }
                            });

  for (size_t i = 0; i < total_frames_count; ++i) {
    if (cancel_requested_ && cancel_requested_->load()) {
      logger_.warn("DropoutAnalysisSinkDeps: Cancel requested at frame {}", i);
      result.success = false;
      result.message = "Cancelled by user";
      result.frame_stats.clear();
      result.total_frames = 0;
      return result;
    }

    const FrameID fid = range.first + i;
    read_ahead.reached(fid);
    const int32_t frame_num = static_cast<int32_t>(fid) + 1;

    auto& accum = result.frame_stats[i];
    accum.frame_number = frame_num;

    const auto desc = representation->get_frame_descriptor(fid);
    if (!desc) continue;

    const auto runs = representation->get_dropout_hints(fid);

    int64_t frame_dropout_length = 0;
    int32_t frame_dropout_count = 0;

    for (const auto& run : runs) {
      bool include = true;

      // Approximate frame-flat line/sample of this run's start position using
      // the recording's nominal samples-per-line. Used both for visible-area
      // filtering and for per-dropout detail records.
      const int32_t approx_line = static_cast<int32_t>(
          run.sample_start / static_cast<uint64_t>(nominal_spl));
      const int32_t approx_sample = static_cast<int32_t>(
          run.sample_start % static_cast<uint64_t>(nominal_spl));

      if (options.mode == DropoutAnalysisMode::VISIBLE_AREA) {
        // Filter by active frame line range.
        if (video_params && video_params->first_active_frame_line >= 0 &&
            video_params->last_active_frame_line >= 0) {
          if (approx_line < video_params->first_active_frame_line ||
              approx_line > video_params->last_active_frame_line) {
            include = false;
          }
        }

        // Filter by active video sample range within line.
        if (include && video_params && video_params->active_video_start >= 0 &&
            video_params->active_video_end >= 0) {
          if (approx_sample >= video_params->active_video_end ||
              approx_sample + static_cast<int32_t>(run.sample_count) <=
                  video_params->active_video_start) {
            include = false;
          }
        }
      }

      if (include) {
        uint64_t length = run.sample_count;
        int32_t rec_sample_start = approx_sample;

        // Clamp to active_video_start / active_video_end within the line.
        if (options.mode == DropoutAnalysisMode::VISIBLE_AREA && video_params &&
            video_params->active_video_start >= 0 &&
            video_params->active_video_end >= 0) {
          const int32_t clamped_start =
              std::max(approx_sample, video_params->active_video_start);
          const int32_t clamped_end =
              std::min(approx_sample + static_cast<int32_t>(run.sample_count),
                       video_params->active_video_end);
          length =
              static_cast<uint64_t>(std::max(0, clamped_end - clamped_start));
          rec_sample_start = clamped_start;
        }

        frame_dropout_length += static_cast<int64_t>(length);
        frame_dropout_count++;

        // Per-dropout detail record (full-resolution, independent of graph
        // decimation). sample_end is inclusive; a zero-length clamp emits no
        // record.
        if (options.collect_detail && length > 0) {
          DropoutDetailRecord rec;
          rec.frame_number = frame_num;
          rec.line_number = approx_line;
          rec.sample_start = rec_sample_start;
          rec.sample_end = rec_sample_start + static_cast<int32_t>(length) - 1;
          rec.length_samples = static_cast<int64_t>(length);
          result.detail_records.push_back(rec);
        }
      }
    }

    accum.dropout_length_samples = frame_dropout_length;
    accum.dropout_count = frame_dropout_count;
    if (frame_dropout_count > 0) accum.has_data = true;

    if (progress_callback_ && (i % 100 == 0 || i + 1 == total_frames_count)) {
      progress_callback_(i + 1, total_frames_count,
                         "Analysing frame " + std::to_string(i + 1) + "/" +
                             std::to_string(total_frames_count));
    }
  }

  result.total_frames = static_cast<int32_t>(total_frames_count);

  logger_.debug("DropoutAnalysisSinkDeps: captured {} per-frame records",
                result.frame_stats.size());

  return result;
}

void DropoutAnalysisSinkStageDeps::write_csv(
    std::ostream& os, const std::vector<FrameDropoutStats>& frame_stats) {
  // Canonical per-frame schema. One row per analysed frame, including
  // zero-dropout frames: a zero row is genuine data ("analysed, no dropouts"),
  // whereas a missing row means the frame was not analysed. Units live in the
  // header names; values are plain integers.
  os << "frame_number,dropout_count,dropout_length_samples\n";
  for (const auto& fs : frame_stats) {
    os << fs.frame_number << ',' << fs.dropout_count << ','
       << fs.dropout_length_samples << '\n';
  }
}

bool DropoutAnalysisSinkStageDeps::write_csv(
    const std::string& path,
    const std::vector<FrameDropoutStats>& frame_stats) {
  if (frame_stats.empty()) {
    logger_.warn("DropoutAnalysisSinkDeps: No data to write");
    return false;
  }

  logger_.debug("DropoutAnalysisSinkDeps: Writing CSV to: {}", path);

  const bool ok = write_file_atomically(
      path, logger_, [&](std::ostream& os) { write_csv(os, frame_stats); });
  if (ok) {
    logger_.debug("DropoutAnalysisSinkDeps: Wrote {} rows to: {}",
                  frame_stats.size(), path);
  }
  return ok;
}

void DropoutAnalysisSinkStageDeps::write_report_csv(
    std::ostream& os, const std::vector<DropoutDetailRecord>& detail_records) {
  // One row per dropout run. Coordinates are frame-flat 0-based line and
  // sample-within-line; sample_end is inclusive; units live in the header.
  os << "frame_number,line_number,sample_start,sample_end,length_samples\n";
  for (const auto& rec : detail_records) {
    os << rec.frame_number << ',' << rec.line_number << ',' << rec.sample_start
       << ',' << rec.sample_end << ',' << rec.length_samples << '\n';
  }
}

void DropoutAnalysisSinkStageDeps::write_report_text(
    std::ostream& os, const std::vector<DropoutDetailRecord>& detail_records) {
  // Human-readable, grouped by frame. Records are captured in frame order, so a
  // single pass groups consecutive records sharing a frame_number. Each group
  // opens with a heading (dropout count + total length) and lists every run's
  // line/sample extent — the "map of where they are in the frame" from #214.
  size_t i = 0;
  while (i < detail_records.size()) {
    const int32_t frame = detail_records[i].frame_number;
    size_t j = i;
    int64_t total_length = 0;
    while (j < detail_records.size() &&
           detail_records[j].frame_number == frame) {
      total_length += detail_records[j].length_samples;
      ++j;
    }
    const size_t count = j - i;

    os << "Frame " << frame << ": " << count << " dropout"
       << (count == 1 ? "" : "s") << ", " << total_length << " samples total\n";
    for (size_t k = i; k < j; ++k) {
      const auto& rec = detail_records[k];
      os << "  line " << rec.line_number << ", samples " << rec.sample_start
         << "-" << rec.sample_end << " (" << rec.length_samples
         << " samples)\n";
    }
    i = j;
  }
}

bool DropoutAnalysisSinkStageDeps::write_report(
    const std::string& path,
    const std::vector<DropoutDetailRecord>& detail_records,
    DropoutReportFormat format) {
  if (detail_records.empty()) {
    logger_.warn("DropoutAnalysisSinkDeps: No dropout detail to report");
    return false;
  }

  logger_.debug("DropoutAnalysisSinkDeps: Writing dropout report to: {}", path);

  const bool ok = write_file_atomically(path, logger_, [&](std::ostream& os) {
    switch (format) {
      case DropoutReportFormat::TEXT:
        write_report_text(os, detail_records);
        break;
      case DropoutReportFormat::CSV:
        write_report_csv(os, detail_records);
        break;
    }
  });
  if (ok) {
    logger_.debug(
        "DropoutAnalysisSinkDeps: Wrote {} dropout detail rows to: {}",
        detail_records.size(), path);
  }
  return ok;
}

}  // namespace orc
