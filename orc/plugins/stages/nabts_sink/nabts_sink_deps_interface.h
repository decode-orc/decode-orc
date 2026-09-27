/*
 * File:        nabts_sink_deps_interface.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Interface for NabtsSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_SINK_DEPS_INTERFACE_H
#define ORC_NABTS_SINK_DEPS_INTERFACE_H

#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "nabts_record_catalogue.h"
#include "vbi-services/teletext_slicer.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

/**
 * @brief Options for one NABTS recovery run
 *
 * Field lines are 0-based and identical in both fields (the stage converts
 * from the 1-based UI parameters). The default window 9-20 covers broadcast
 * lines 10-21 and 273-284, which is where ITU-R BT.653 §2 places both
 * 525-line teletext services and where CEA-516 §1.1.1 places NABTS.
 */
struct NabtsSinkOptions {
  // Where the packet stream goes, with the service's extension applied if it
  // is not already there. Empty is the browse-only run: the pass recovers and
  // catalogues exactly as it would and writes nothing. The report is written
  // beside the stream, so it is not available without a path.
  std::string output_path;
  int32_t first_field_line{9};
  int32_t last_field_line{20};
  // Emit a whole zero packet for every candidate line with no data so packet
  // position maps 1:1 to (frame, field, line) — the vhs-decode convention.
  bool keep_empty_packets{false};
  // Accept framing codes with one bit error (TeletextSlicerOptions).
  bool tolerant_framing{false};
  // Drop packets whose five Hamming 8/4 coded prefix bytes (CEA-516 §3.2.1)
  // do not all decode (TeletextSlicerOptions::require_valid_mrag).
  bool require_valid_prefix{true};
  // Bit detector (TeletextSlicerOptions).
  TeletextDetector detector{TeletextDetector::kAuto};
  // Narrow the bit-phase acquisition sweep to where this recording's data
  // lines have already been seen to start (NabtsPhaseTracker). Cannot lose
  // a packet: a pinned attempt that fails is repeated over the full window.
  bool pin_data_phase{true};
  // Learn the bit clock the recording was sampled at from the first packets
  // and read the rest at it (TeletextClockCalibrator).
  bool calibrate_bit_clock{true};
  // Slice only the candidate lines this recording has been seen to carry data
  // on, rechecking the full window periodically (NabtsLineTracker).
  bool learn_active_lines{true};
  // Threads to recover lines on; 0 asks for one per hardware thread. The
  // recovered stream does not depend on this — see NabtsScanSnapshot for
  // what makes that true — so it is a claim on the machine rather than a
  // tuning knob.
  int32_t decode_threads{0};
  // Let the NAPLPS grammar settle the byte positions the vote across a
  // record's copies could not settle on the evidence (see nabts_vote_record).
  // Unlike the viewer's syntax repair this is recovery rather than
  // presentation: it changes the recovered record data, and so what the record
  // files export and what every later reading of the record starts from.
  bool grammar_assisted_vote{true};
  // Write the run's diagnostic report to <output_path>.txt as well as logging
  // it. The report is always built; this only decides whether it is kept
  // somewhere a reader can go back to.
  bool write_report{false};
  // Write each catalogued record's data as a file beside the packet stream,
  // so the presentation code can be examined with external tools. Needs an
  // output path for the same reason the report does.
  bool export_records{false};
  // Write the caption service (CEA-516 §7.3.10) as a SubRip document beside the
  // packet stream. Needs an output path for the same reason the report does.
  bool export_captions{false};
};

struct NabtsSinkResult {
  bool success{false};
  std::string message;
  // Path actually written, with the .t33 extension applied. Empty when the run
  // was browse-only and wrote no stream.
  std::string output_path;
  uint64_t packets_written{0};
  uint64_t fields_with_data{0};
  uint64_t frames_analysed{0};
  // Packet slots that came back empty on a line the recording has been seen to
  // carry data on — an estimate of what was lost.
  uint64_t lost_packets_estimate{0};
  // Human-readable diagnostic report of the run: what was exported and how the
  // recovery went (vbi-services/teletext_recovery_stats.h). Logged by the stage
  // at debug level, and written to report_path when write_report is set.
  std::string report;
  // Path the report was written to, empty when write_report is off or the
  // write failed (which never fails the export — the packet stream is the
  // product).
  std::string report_path;
  // Record files written when export_records is set.
  uint64_t records_exported{0};
  // Caption cues written when export_captions is set, and where they went. The
  // path is empty when the option is off, when the recording carried no
  // captioning, or when the write failed — none of which fails the export.
  uint64_t caption_cues_written{0};
  std::string caption_path;
  // Every record the range carried, which the stage resolves into the
  // catalogue the host browses through ICatalogueResults.
  NabtsAnalysisDataset dataset;
  // What lint-directed repair made of the presentation records, for the
  // report. Aggregate only; the per-page detail rides on each page.
  NabtsLintTotals lint_totals;
};

class INabtsSinkStageDeps {
 public:
  virtual ~INabtsSinkStageDeps() = default;

  virtual void init(TriggerProgressCallback progress_callback,
                    std::atomic<bool>* cancel_requested) = 0;

  // One linear pass over the whole frame range: recovers the packets and
  // writes the stream (and, optionally, the report).
  virtual NabtsSinkResult analyse(
      const VideoFrameRepresentation* representation,
      const NabtsSinkOptions& options) = 0;
};

}  // namespace orc

#endif  // ORC_NABTS_SINK_DEPS_INTERFACE_H
