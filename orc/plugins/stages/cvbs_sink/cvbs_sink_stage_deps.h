/*
 * File:        cvbs_sink_stage_deps.h
 * Module:      orc-core
 * Purpose:     CVBSSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_CVBS_SINK_STAGE_DEPS_H
#define ORC_CORE_CVBS_SINK_STAGE_DEPS_H

#include <atomic>
#include <fstream>
#include <ostream>
#include <string>
#include <utility>

#include "cvbs_sink_stage_deps_interface.h"

namespace orc {

// Production writer for the CVBS file-format family: payload file(s), the
// .meta SQLite sidecar, and the dropout/audio/EFM/AC3 extension sidecars.
class CVBSSinkStageDeps : public ICVBSSinkStageDeps {
 public:
  CVBSSinkStageDeps() = default;
  ~CVBSSinkStageDeps() override = default;

  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* cancel_requested) override;

  CVBSSinkWriteResult write_cvbs(const VideoFrameRepresentation* representation,
                                 const CVBSSinkWriteConfig& config) override;

 protected:
  // Seam for tests: returns the stream the primary payload is written to, or
  // nullptr on failure to open a real file. The default implementation opens
  // |primary_path| as a real file (backed by |file_storage|, left unopened on
  // failure or when piping) or, when |piping| is set, returns
  // pipe_io::stdout_binary_stream() — real stdout, which a test cannot safely
  // let a production instance write to. A test subclass overrides this to
  // capture into an in-memory stream instead, without touching the filesystem
  // or the process's real stdout.
  virtual std::ostream* open_primary_output(const std::string& primary_path,
                                            bool piping,
                                            std::ofstream& file_storage);

 private:
  TriggerProgressCallback progress_callback_;
  std::atomic<bool>* cancel_requested_{nullptr};
};

}  // namespace orc

#endif  // ORC_CORE_CVBS_SINK_STAGE_DEPS_H
