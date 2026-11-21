/*
 * File:        dropout_analysis_sink_deps_interface.h
 * Module:      orc-core
 * Purpose:     Interface for DropoutAnalysisSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_DROPOUT_ANALYSIS_SINK_DEPS_INTERFACE_H
#define ORC_CORE_DROPOUT_ANALYSIS_SINK_DEPS_INTERFACE_H

#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dropout_analysis_types.h"

namespace orc {

// Output format for the per-dropout detail report (issue #214, plan Phase 4).
enum class DropoutReportFormat {
  CSV,  ///< One row per dropout run, machine-readable
  TEXT  ///< Human-readable, grouped by frame
};

// One record per dropout run, describing its location within the frame.
// Coordinates are frame-flat 0-based line and sample-within-line, derived from
// the run's flat offset using the recording's nominal samples-per-line (the
// same approximation used for visible-area filtering). sample_end is inclusive.
struct DropoutDetailRecord {
  int32_t frame_number = 0;    ///< Analysed frame number (1-based)
  int32_t line_number = 0;     ///< Frame-flat 0-based line of the run's start
  int32_t sample_start = 0;    ///< Sample-within-line of the run's start
  int32_t sample_end = 0;      ///< Sample-within-line of the run's end (incl.)
  int64_t length_samples = 0;  ///< Run length after any visible-area clamping
};

struct DropoutAnalysisComputeOptions {
  std::string output_path;
  bool write_csv{false};
  DropoutAnalysisMode mode{DropoutAnalysisMode::FULL_FIELD};
  // When true, per-dropout detail records are produced alongside the per-frame
  // stats. Gated to avoid the cost when the detail report is not needed.
  bool collect_detail{false};
  // Where the detail records go, in frame order, as they are produced. When
  // unset they are gathered in DropoutAnalysisComputeResult::detail_records
  // instead (tests); a run that writes a report streams them to the report
  // rather than holding one record per dropout of the capture.
  std::function<void(const DropoutDetailRecord&)> detail_sink;
};

// The per-dropout report, written as the analysis produces its records. The
// file appears at its path only on commit(); a writer destroyed without one
// leaves no file (a cancelled or failed run).
class IDropoutReportWriter {
 public:
  virtual ~IDropoutReportWriter() = default;
  // Records arrive in frame order.
  virtual void add(const DropoutDetailRecord& record) = 0;
  // Finish the file and move it into place. False when nothing was recorded
  // (no file is written) or the write failed.
  virtual bool commit() = 0;
};

struct DropoutAnalysisComputeResult {
  bool success{false};
  std::string message;
  std::vector<FrameDropoutStats> frame_stats;
  std::vector<DropoutDetailRecord> detail_records;
  int32_t total_frames{0};
};

class IDropoutAnalysisSinkStageDeps {
 public:
  virtual ~IDropoutAnalysisSinkStageDeps() = default;

  virtual void init(TriggerProgressCallback progress_callback,
                    std::atomic<bool>* cancel_requested) = 0;

  virtual DropoutAnalysisComputeResult compute_and_analyze(
      VideoFrameRepresentation* representation,
      IObservationContext& observation_context,
      DropoutAnalysisComputeOptions options) = 0;

  // Thin path-opening wrapper: opens `path` for truncation and writes the
  // canonical per-frame CSV. Returns false if there is no data or the file
  // cannot be opened. The concrete deps expose a filesystem-free
  // write_csv(std::ostream&, …) formatter for unit testing.
  virtual bool write_csv(const std::string& path,
                         const std::vector<FrameDropoutStats>& frame_stats) = 0;

  // Start the per-dropout detail report at `path` in the requested format.
  // Returns nullptr if the file cannot be opened. The concrete deps expose
  // filesystem-free write_report_csv/text(std::ostream&, …) formatters for
  // unit testing.
  virtual std::unique_ptr<IDropoutReportWriter> open_report(
      const std::string& path, DropoutReportFormat format) = 0;
};
}  // namespace orc

#endif  // ORC_CORE_DROPOUT_ANALYSIS_SINK_DEPS_INTERFACE_H
