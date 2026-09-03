/*
 * File:        efm_sink_stage_deps_interface.h
 * Module:      orc-core
 * Purpose:     Interface for RawEFMSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_RAW_EFM_SINK_STAGE_DEPS_INTERFACE_H
#define ORC_CORE_RAW_EFM_SINK_STAGE_DEPS_INTERFACE_H

#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace orc {
struct RawEFMSinkWriteResult {
  bool success{false};
  uint64_t tvalues_written{0};
  std::string status_message;
};

class IRawEFMSinkStageDeps {
 public:
  virtual ~IRawEFMSinkStageDeps() = default;

  virtual void init(TriggerProgressCallback progress_callback,
                    std::atomic<bool>* cancel_requested) = 0;

  // |include_confidence| selects what is written per t-value: true keeps the
  // packed byte exactly as the pipeline carries it (t-value in the low nibble,
  // producer doubt in the high nibble); false writes the bare t-value, which
  // is what tools predating the confidence nibble expect.
  virtual RawEFMSinkWriteResult write_raw_efm(
      const VideoFrameRepresentation* representation,
      const std::string& output_path, bool include_confidence) = 0;
};
}  // namespace orc

#endif  // ORC_CORE_RAW_EFM_SINK_STAGE_DEPS_INTERFACE_H
