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
#include <vector>

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
  // Bit detector (TeletextSlicerOptions).
  TeletextDetector detector{TeletextDetector::kAuto};
  // Learn the bit clock the recording was sampled at from the first packets
  // and read the rest at it (TeletextClockCalibrator).
  bool calibrate_bit_clock{true};
  // Threads to recover lines on; 0 asks for one per hardware thread. The
  // recovered stream does not depend on this — see NabtsScanSnapshot for
  // what makes that true — so it is a claim on the machine rather than a
  // tuning knob.
  int32_t decode_threads{0};
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
  // Packet streams (.t33) of other recordings of the service, the record
  // files export_records wrote beside them, or folders of either, whose
  // records the records viewer may rebuild this recording's lost bytes from
  // (NabtsAnalysisDataset::references). Read after the pass; a path that
  // cannot be read is reported and skipped, and never fails the run.
  std::vector<std::string> reference_records;
};

struct NabtsSinkResult {
  bool success{false};
  std::string message;
  // Path actually written, with the .t33 extension applied. Empty when the run
  // was browse-only and wrote no stream.
  std::string output_path;
  uint64_t packets_written{0};
  // Packets refused because every one of their five prefix bytes needed
  // Hamming correction: test-signal lines read as data (see the sink).
  uint64_t packets_refused_unprotected{0};
  // Packets refused from lines that carry no NABTS (NabtsServiceLines): the
  // other 525-line teletext service's, read inside the MLSE detector's framing
  // margin, or another signal's.
  uint64_t packets_refused_other_service{0};
  // Of the packets written, the ones whose five prefix bytes all arrived whole
  // (nabts_prefix_every_byte_whole()).
  uint64_t packets_prefix_whole{0};
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
  // Where the text of the provider's own data groups (NabtsPrivateTextLog)
  // went, written beside the report when write_report is set; empty when there
  // was none or the write failed.
  std::string private_text_path;
  // Datacast payloads written when export_records is set, one file per
  // channel (NabtsDatacastDecoder), and the text files beside them.
  uint64_t datacasts_exported{0};
  uint64_t datacast_texts_exported{0};
  // Runs of packets salvage placed in no record or page (NabtsSalvage::
  // unplaced()), written as text beside the stream when export_records is
  // set, and where they went; the path is empty when there were none or the
  // write failed.
  uint64_t fragments_exported{0};
  std::string fragments_path;
  // Files the datacasts carried, written beside the stream when
  // export_records is set (NabtsAnalysisDataset::carried_files).
  uint64_t carried_files_exported{0};
  // The frame streams of the framed services, written beside the stream as
  // <output_path>.datacast-<first>-<last>.<service>.frames (export_records).
  uint64_t framed_services_exported{0};
  // The framed services' files, laid out as the web sites they came from
  // under <output_path>.mirror/ (nabts_web_mirror(), export_records): its
  // addresses, hosts, addresses sent again changed, files placed back in the
  // folder a misread took them out of, and the files written.
  uint64_t mirror_addresses{0};
  uint64_t mirror_hosts{0};
  uint64_t mirror_replaced{0};
  uint64_t mirror_misread{0};
  uint64_t mirror_files_exported{0};
  // Reference packet streams read for reconstruction, and the named paths that
  // could not be read.
  uint64_t reference_streams{0};
  uint64_t reference_paths_unreadable{0};
  // Every record the range carried, which the stage resolves into the
  // catalogue the host browses through ICatalogueResults.
  NabtsAnalysisDataset dataset;
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
