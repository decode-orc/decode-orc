/*
 * File:        cc_sink_stage_deps_interface.h
 * Module:      orc-core
 * Purpose:     Interface for CCSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_CC_SINK_STAGE_DEPS_INTERFACE_H
#define ORC_CORE_CC_SINK_STAGE_DEPS_INTERFACE_H

#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/eia608_service_demux.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace orc {
/**
 * @brief Closed Caption output format
 */
enum class CCExportFormat {
  SCC,         ///< Scenarist SCC V1.0 format (industry standard)
  PLAIN_TEXT,  ///< Plain text, one screen line per line, with timestamps
  SRT,         ///< SubRip subtitles
  HTML         ///< Monospaced HTML transcript (keeps the column layout)
};

struct CCExportOptions {
  std::string output_path;
  CCExportFormat export_format{CCExportFormat::SCC};
  /// Which of the eight services multiplexed onto line 21 to export
  EIA608Service service{EIA608Service::CC1};
  /// Export the second field's Extended Data Service packets instead of a
  /// caption or text service (plain text or HTML only)
  bool export_xds{false};
  /// Export every service in one pass into the folder output_path names:
  /// CC1-CC4 as SubRip (CC1.srt), TEXT1-TEXT4 and XDS as plain text
  /// (TEXT1.txt, XDS.txt), a file for each service that carried anything.
  /// export_format, service and export_xds do not apply.
  bool all_services{false};
  /// Write the line 21 report beside the output, as <output_path>.report.txt
  /// (line21.report.txt in the folder, for all_services)
  bool write_report{false};
};

struct CCExportResult {
  bool success{false};
  std::string message;
  int32_t cc_frames_exported{0};
  /// What line 21 carried over the run: every service, the XDS, the ATVEF
  /// triggers (see CCLine21Report). Empty when the export failed early.
  std::string report;
};

class ICCSinkStageDeps {
 public:
  virtual ~ICCSinkStageDeps() = default;

  virtual void init(TriggerProgressCallback progress_callback,
                    std::atomic<bool>* cancel_requested) = 0;

  virtual CCExportResult export_cc(VideoFrameRepresentation* representation,
                                   IObservationContext& observation_context,
                                   CCExportOptions options) = 0;
};
}  // namespace orc

#endif  // ORC_CORE_CC_SINK_STAGE_DEPS_INTERFACE_H
