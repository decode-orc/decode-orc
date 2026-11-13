/*
 * File:        dropout_analysis_sink_deps.h
 * Module:      orc-core
 * Purpose:     DropoutAnalysisSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_DROPOUT_ANALYSIS_SINK_DEPS_H
#define ORC_CORE_DROPOUT_ANALYSIS_SINK_DEPS_H

#include <orc/support/logging.h>

#include <atomic>
#include <iosfwd>
#include <utility>

#include "dropout_analysis_sink_deps_interface.h"

namespace orc {
class DropoutAnalysisSinkStageDeps : public IDropoutAnalysisSinkStageDeps {
 public:
  DropoutAnalysisSinkStageDeps() = default;

  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* cancel_requested) override;

  DropoutAnalysisComputeResult compute_and_analyze(
      VideoFrameRepresentation* representation,
      IObservationContext& observation_context,
      DropoutAnalysisComputeOptions options) override;

  bool write_csv(const std::string& path,
                 const std::vector<FrameDropoutStats>& frame_stats) override;

  // Filesystem-free formatter. Writes the canonical per-frame CSV to `os`:
  // frame_number,dropout_count,dropout_length_samples — one row per analysed
  // frame, including zero-dropout frames. Not part of the deps interface so it
  // can be unit-tested directly against an std::ostringstream.
  void write_csv(std::ostream& os,
                 const std::vector<FrameDropoutStats>& frame_stats);

  std::unique_ptr<IDropoutReportWriter> open_report(
      const std::string& path, DropoutReportFormat format) override;

  // Filesystem-free per-dropout report formatters. CSV emits one row per run
  // (frame_number,line_number,sample_start,sample_end,length_samples); TEXT is
  // human-readable, grouped by frame. Frames with no dropouts never appear.
  // Not part of the deps interface so they can be unit-tested directly against
  // an std::ostringstream.
  static void write_report_csv(
      std::ostream& os, const std::vector<DropoutDetailRecord>& detail_records);
  static void write_report_text(
      std::ostream& os, const std::vector<DropoutDetailRecord>& detail_records);

  // The CSV header line, and the rows of |detail_records| without it:
  // write_report_csv() is the two together. The streamed report writes the
  // header once and then each frame's rows.
  static void write_report_csv_header(std::ostream& os);
  static void write_report_csv_rows(
      std::ostream& os, const std::vector<DropoutDetailRecord>& detail_records);

 private:
  class SpdlogLoggerAdapter {
   public:
    template <typename... Args>
    void trace(const char* fmt, Args&&... args) const {
      ORC_LOG_TRACE(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void debug(const char* fmt, Args&&... args) const {
      ORC_LOG_DEBUG(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(const char* fmt, Args&&... args) const {
      ORC_LOG_INFO(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(const char* fmt, Args&&... args) const {
      ORC_LOG_WARN(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(const char* fmt, Args&&... args) const {
      ORC_LOG_ERROR(fmt, std::forward<Args>(args)...);
    }
  };

  TriggerProgressCallback progress_callback_;
  std::atomic<bool>* cancel_requested_{nullptr};
  SpdlogLoggerAdapter logger_;
};
}  // namespace orc

#endif  // ORC_CORE_DROPOUT_ANALYSIS_SINK_DEPS_H
