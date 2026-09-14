/*
 * File:        ac3rf_sink_stage_deps.h
 * Module:      orc-core
 * Purpose:     AC3RFSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_AC3RF_SINK_STAGE_DEPS_H
#define ORC_CORE_AC3RF_SINK_STAGE_DEPS_H

#include <orc/stage/triggerable_stage.h>

#include <atomic>
#include <fstream>
#include <ostream>

#include "ac3rf_sink_stage_deps_interface.h"

namespace orc {
class AC3RFSinkStageDeps : public IAC3RFSinkStageDeps {
 public:
  AC3RFSinkStageDeps() = default;
  ~AC3RFSinkStageDeps() override = default;

  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* cancel_requested);

  AC3RFSinkDecodeResult decode_and_write_ac3(
      const VideoFrameRepresentation* representation,
      const std::string& output_path) override;

 protected:
  // Seam for tests: returns the stream the decoded AC3 frames are written to,
  // or nullptr on failure to open a real file. The default implementation
  // opens |output_path| as a real file (backed by |file_storage|) or, when
  // |piping| is set, returns pipe_io::stdout_binary_stream() — real stdout,
  // which a test cannot safely let a production instance write to. A test
  // subclass overrides this to capture into an in-memory stream instead.
  virtual std::ostream* open_output(const std::string& output_path, bool piping,
                                    std::ofstream& file_storage);

 private:
  TriggerProgressCallback progress_callback_;
  std::atomic<bool>* cancel_requested_{nullptr};
};
}  // namespace orc

#endif  // ORC_CORE_AC3RF_SINK_STAGE_DEPS_H
