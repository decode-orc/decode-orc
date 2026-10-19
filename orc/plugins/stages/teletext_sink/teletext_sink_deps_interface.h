/*
 * File:        teletext_sink_deps_interface.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Interface for TeletextSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_SINK_DEPS_INTERFACE_H
#define ORC_TELETEXT_SINK_DEPS_INTERFACE_H

#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/teletext_slicer.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

/**
 * @brief Options for one teletext analysis run.
 *
 * Field lines are 0-based and identical in both fields (the stage converts
 * from the 1-based UI parameters). The default window 5-21 covers broadcast
 * lines 6-22 / 318-335 (ETSI EN 300 706 §4.1); a 525-line service uses 9-20
 * (ITU-R BT.653 §2).
 */
struct TeletextSinkOptions {
  // Where the packet stream goes, with the service's extension applied if it
  // is not already there. Empty is the browse-only run: the pass recovers,
  // squashes and catalogues exactly as it would and writes nothing. Both
  // file-side extras below are written beside the stream, so neither is
  // available without a path.
  std::string output_path;
  int32_t first_field_line{5};
  int32_t last_field_line{21};
  // Emit a whole zero packet for every candidate line with no data so packet
  // position maps 1:1 to (frame, field, line) — the vhs-decode convention.
  bool keep_empty_packets{false};
  // Restore odd parity on damaged display bytes by flipping the bit the MLSE
  // detector was least sure of (TeletextSlicerOptions::parity_repair).
  bool parity_repair{true};
  // Bit detector (TeletextSlicerOptions).
  TeletextDetector detector{TeletextDetector::kAuto};
  // Learn the bit clock the recording was sampled at from the first packets
  // and read the rest at it (TeletextClockCalibrator).
  bool calibrate_bit_clock{true};
  // Threads to recover lines on; 0 asks for one per hardware thread. The
  // recovered stream does not depend on this — see TeletextScanSnapshot for
  // what makes that true — so it is a claim on the machine rather than a
  // tuning knob.
  int32_t decode_threads{0};
  // G0 primary character set pages are displayed in when the service
  // designates none of its own (TeletextPageDecoder::set_default_g0_set).
  // Affects rendered pages and subtitle text only: the exported packet stream
  // is the transmitted bytes and carries no character set.
  TeletextG0Set character_set{TeletextG0Set::Latin};
  // Second G0 set the ESC control character switches into, for the services
  // that mix two alphabets on a page (EN 300 706 §15.3,
  // TeletextPageDecoder::set_default_second_g0_set). Unset — the default —
  // leaves ESC inert and every page in one alphabet. Overridden by a service
  // that designates its own pair, and affects rendered pages and subtitle text
  // only, as the set above does.
  std::optional<TeletextG0Designation> second_character_set{};
  // Combine repeated transmissions of each page row and write the combined
  // form ("squashing", see vbi-services/teletext_row_squasher.h). Costs a
  // second pass over the recovered packets, held in memory (~50 bytes each).
  bool squash_repeated_rows{true};
  // How the transmissions of a page number are filed into pages
  // (TeletextPageDecoder::set_page_separation()). kTransmission is for
  // VirText, which sent a rotation of screens under one page number: each
  // screen becomes a version of the page, and pages in Zenith's own packet
  // layout are read in it. Needs squash_repeated_rows for the screens to be
  // told apart.
  TeletextPageSeparation page_separation{TeletextPageSeparation::kPageNumber};
  // Decode the subtitle page alongside the packet export and write SubRip cues
  // next to the packet stream. 625-line services only: the cue timing derives
  // from the 50 fields/s of ITU-R BT.1700 Annex 1 Part B Table 1 item 2.
  bool export_subtitles{false};
  // Watched subtitle page in the conventional magazine + two-hex-digit form
  // (validated by the stage via TeletextPageDecoder::parse_page_number).
  std::string subtitle_page{"888"};
  // Write the run's diagnostic report to <output_path>.txt as well as logging
  // it. The report is always built; this only decides whether it is kept
  // somewhere a reader can go back to.
  bool write_report{false};
};

struct TeletextSinkResult {
  bool success{false};
  std::string message;
  // Path actually written, with the service's extension applied: .t42 for the
  // 42-byte 625-line packet stream, .t34 for the 34-byte 525-line one. Empty
  // when the run was browse-only and wrote no stream.
  std::string output_path;
  uint64_t packets_written{0};
  uint64_t fields_with_data{0};
  // Row packets whose bytes were changed by squashing (0 when disabled, or
  // when every row was only ever transmitted once).
  uint64_t packets_corrected{0};
  // Display bytes whose parity was restored by flipping the detector's
  // least-confident bit (0 unless parity_repair is set).
  uint64_t bytes_repaired{0};
  // Subtitle export results (export_subtitles only).
  std::string subtitle_path;
  uint64_t subtitle_cues_written{0};
  // The run's headline, for a caller that wants the result without the
  // report: display characters written, and how many of those are known
  // damaged because they fail the odd parity of ETSI EN 300 706 §8.1. A floor
  // rather than an exact count — a byte damaged in two bits passes parity —
  // and silent about rows that never arrived. Zero characters means no display
  // row was written.
  uint64_t characters_written{0};
  uint64_t characters_damaged{0};
  // Human-readable diagnostic report of the run: what was exported, how the
  // recovery went (vbi-services/teletext_recovery_stats.h) and what combining
  // repeated rows changed (teletext_squash_stats.h). Logged by the stage at
  // debug level, and written to report_path when write_report is set.
  std::string report;
  // Path the report was written to, empty when write_report is off or the
  // write failed (which never fails the export — the packet stream is the
  // product).
  std::string report_path;
  // The display packets that reached no page (TeletextUnplacedRows), and the
  // text of them written beside the report as <output_path>.unplaced.txt —
  // empty when write_report is off, none reached no page, or the write failed.
  uint64_t unplaced_rows{0};
  std::string unplaced_path;
  // The packets numbered 30 or 31 that carried nothing believable and those
  // on data channels no format is defined for (TeletextDataChannels), and the
  // payload of each independent data line that was read, written beside the
  // report as <output_path>.data-<tag>.bin — none when write_report is off,
  // the service sent none, or the write failed.
  uint64_t data_packets_refused{0};
  uint64_t data_packets_unassigned{0};
  // Transmissions of scrambled pages (Page Format - CA), and the pages
  // (TeletextPageFormatClear).
  uint64_t scrambled_page_transmissions{0};
  uint64_t scrambled_pages{0};
  std::vector<std::string> data_line_paths;
  // The viewer's half of the run: every page the range carried, plus the
  // aggregate recovery figures. Populated for a cancelled run too — what it
  // got to is exactly what a cancelled run leaves a reader asking about.
  TeletextAnalysisDataset dataset;
};

class ITeletextSinkStageDeps {
 public:
  virtual ~ITeletextSinkStageDeps() = default;

  virtual void init(TriggerProgressCallback progress_callback,
                    std::atomic<bool>* cancel_requested) = 0;

  // One linear pass over the whole frame range: recovers the packets, writes
  // the stream (and, optionally, the subtitle document and the report), and
  // builds the page catalogue the stage tool displays.
  virtual TeletextSinkResult analyse(
      const VideoFrameRepresentation* representation,
      const TeletextSinkOptions& options) = 0;
};

}  // namespace orc

#endif  // ORC_TELETEXT_SINK_DEPS_INTERFACE_H
