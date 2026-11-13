/*
 * File:        dropout_analysis_sink_deps.cpp
 * Module:      orc-core
 * Purpose:     DropoutAnalysisSinkStage dependency implementation (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "dropout_analysis_sink_deps.h"

#include <orc/support/frame_line_util.h>
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

  // Nominal samples per line, used to turn a run's frame-flat start offset
  // into a frame line and sample. PAL: 1135, NTSC: 910.
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

      // Frame line and sample of this run's start position. PAL frame lines
      // 312 and 624 carry 2 extra samples, so rows from 313 on start 2 samples
      // after k × 1135 (issue #370). Used both for visible-area filtering and
      // for per-dropout detail records.
      const VideoSystem system =
          video_params ? video_params->system : desc->system;
      const auto [flat_line, flat_sample] = frame_flat_offset_to_line_sample(
          system, static_cast<size_t>(nominal_spl), run.sample_start);
      const auto line = static_cast<int32_t>(flat_line);
      const auto sample = static_cast<int32_t>(flat_sample);

      if (options.mode == DropoutAnalysisMode::VISIBLE_AREA) {
        // Filter by active frame line range.
        if (video_params && video_params->first_active_frame_line >= 0 &&
            video_params->last_active_frame_line >= 0) {
          if (line < video_params->first_active_frame_line ||
              line > video_params->last_active_frame_line) {
            include = false;
          }
        }

        // Filter by active video sample range within line.
        if (include && video_params && video_params->active_video_start >= 0 &&
            video_params->active_video_end >= 0) {
          if (sample >= video_params->active_video_end ||
              sample + static_cast<int32_t>(run.sample_count) <=
                  video_params->active_video_start) {
            include = false;
          }
        }
      }

      if (include) {
        uint64_t length = run.sample_count;
        int32_t rec_sample_start = sample;

        // Clamp to active_video_start / active_video_end within the line.
        if (options.mode == DropoutAnalysisMode::VISIBLE_AREA && video_params &&
            video_params->active_video_start >= 0 &&
            video_params->active_video_end >= 0) {
          const int32_t clamped_start =
              std::max(sample, video_params->active_video_start);
          const int32_t clamped_end =
              std::min(sample + static_cast<int32_t>(run.sample_count),
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
          rec.line_number = line;
          rec.sample_start = rec_sample_start;
          rec.sample_end = rec_sample_start + static_cast<int32_t>(length) - 1;
          rec.length_samples = static_cast<int64_t>(length);
          if (options.detail_sink) {
            options.detail_sink(rec);
          } else {
            result.detail_records.push_back(rec);
          }
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
  write_report_csv_header(os);
  write_report_csv_rows(os, detail_records);
}

void DropoutAnalysisSinkStageDeps::write_report_csv_header(std::ostream& os) {
  // One row per dropout run. Coordinates are frame-flat 0-based line and
  // sample-within-line; sample_end is inclusive; units live in the header.
  os << "frame_number,line_number,sample_start,sample_end,length_samples\n";
}

void DropoutAnalysisSinkStageDeps::write_report_csv_rows(
    std::ostream& os, const std::vector<DropoutDetailRecord>& detail_records) {
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

namespace {

// Streams the report into a temporary sibling of its path, one frame's
// records at a time (the TEXT form groups by frame), and renames it into
// place on commit(), as write_file_atomically() does for a whole file.
class StreamedDropoutReport final : public IDropoutReportWriter {
 public:
  StreamedDropoutReport(std::string path, DropoutReportFormat format)
      : path_(std::move(path)), tmp_path_(path_ + ".tmp"), format_(format) {}

  ~StreamedDropoutReport() override {
    if (!committed_) {
      if (out_.is_open()) out_.close();
      std::remove(tmp_path_.c_str());
    }
  }

  bool open() {
    out_.open(tmp_path_, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out_.is_open()) {
      ORC_LOG_ERROR("Failed to open temporary file: {}", tmp_path_);
      return false;
    }
    if (format_ == DropoutReportFormat::CSV) {
      DropoutAnalysisSinkStageDeps::write_report_csv_header(out_);
    }
    return true;
  }

  void add(const DropoutDetailRecord& record) override {
    if (!frame_.empty() && frame_.front().frame_number != record.frame_number) {
      flush_frame();
    }
    frame_.push_back(record);
    ++records_;
  }

  bool commit() override {
    flush_frame();
    if (records_ == 0) {
      ORC_LOG_WARN("DropoutAnalysisSinkDeps: No dropout detail to report");
      return false;
    }
    out_.flush();
    if (!out_.good()) {
      ORC_LOG_ERROR("Failed writing temporary file: {}", tmp_path_);
      return false;
    }
    out_.close();
    if (std::rename(tmp_path_.c_str(), path_.c_str()) != 0) {
      ORC_LOG_ERROR("Failed to move {} into place at {}", tmp_path_, path_);
      return false;
    }
    committed_ = true;
    ORC_LOG_DEBUG(
        "DropoutAnalysisSinkDeps: Wrote {} dropout detail rows to: {}",
        records_, path_);
    return true;
  }

 private:
  void flush_frame() {
    if (frame_.empty()) return;
    if (format_ == DropoutReportFormat::CSV) {
      DropoutAnalysisSinkStageDeps::write_report_csv_rows(out_, frame_);
    } else {
      DropoutAnalysisSinkStageDeps::write_report_text(out_, frame_);
    }
    frame_.clear();
  }

  std::string path_;
  std::string tmp_path_;
  DropoutReportFormat format_;
  std::ofstream out_;
  std::vector<DropoutDetailRecord> frame_;  // the frame being gathered
  uint64_t records_ = 0;
  bool committed_ = false;
};

}  // namespace

std::unique_ptr<IDropoutReportWriter> DropoutAnalysisSinkStageDeps::open_report(
    const std::string& path, DropoutReportFormat format) {
  logger_.debug("DropoutAnalysisSinkDeps: Writing dropout report to: {}", path);
  auto report = std::make_unique<StreamedDropoutReport>(path, format);
  if (!report->open()) return nullptr;
  return report;
}

}  // namespace orc
