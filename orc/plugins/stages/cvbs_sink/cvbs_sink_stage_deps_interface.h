/*
 * File:        cvbs_sink_stage_deps_interface.h
 * Module:      orc-core
 * Purpose:     Interface for CVBSSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_CVBS_SINK_STAGE_DEPS_INTERFACE_H
#define ORC_CORE_CVBS_SINK_STAGE_DEPS_INTERFACE_H

#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <string>

#include "cvbs_sink_encode.h"

namespace orc {

// Validated write request built by CVBSSinkStage from its parameters.
struct CVBSSinkWriteConfig {
  // Base path for all output files (payload extension already stripped);
  // the writer appends .cvbs/.cvbsy/.cvbsc, .meta, and sidecar extensions.
  std::string output_base_path;

  // Output sample encoding; recorded as sample_encoding_preset in the .meta.
  CVBSSampleEncoding sample_encoding{CVBSSampleEncoding::U10_4FSC};

  // Derived from the input representation (never user-selected):
  // "composite" writes <base>.cvbs; "yc" writes <base>.cvbsy + <base>.cvbsc.
  std::string signal_type{"composite"};

  // User-selected for a Y/C input (issue #331): write a single composite
  // <base>.cvbs whose samples are Y + C, instead of the .cvbsy/.cvbsc pair.
  // Requires signal_type "composite" and an input with separate channels.
  bool combine_yc{false};

  // Free-text capture_notes for the .meta; omitted when empty.
  std::string capture_notes;
};

struct CVBSSinkWriteResult {
  bool success{false};
  uint64_t frames_written{0};
  std::string status_message;
};

class ICVBSSinkStageDeps {
 public:
  virtual ~ICVBSSinkStageDeps() = default;

  virtual void init(TriggerProgressCallback progress_callback,
                    std::atomic<bool>* cancel_requested) = 0;

  virtual CVBSSinkWriteResult write_cvbs(
      const VideoFrameRepresentation* representation,
      const CVBSSinkWriteConfig& config) = 0;
};

}  // namespace orc

#endif  // ORC_CORE_CVBS_SINK_STAGE_DEPS_INTERFACE_H
