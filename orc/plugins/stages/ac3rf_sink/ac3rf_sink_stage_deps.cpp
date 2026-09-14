/*
 * File:        ac3rf_sink_stage_deps.cpp
 * Module:      orc-core
 * Purpose:     AC3RFSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "ac3rf_sink_stage_deps.h"

#include <ac3/Ac3Decoder.h>
#include <orc/support/logging.h>
#include <orc/support/pipe_io.h>

#include <algorithm>
#include <fstream>
#include <ostream>
#include <utility>

// Adapter: forwards ac3rf Logger calls to the orc spdlog logger.
class SpdlogLogger : public Logger {
 public:
  void log(LogPriority priority, LogCategoryFlags,
           const std::string& message) override {
    auto l = orc::get_logger();
    switch (priority) {
      case eDebug:
        l->debug("[ac3rf] {}", message);
        break;
      case eInfo:
        l->info("[ac3rf] {}", message);
        break;
      case eWarn:
        l->warn("[ac3rf] {}", message);
        break;
      default:
        l->error("[ac3rf] {}", message);
        break;
    }
  }
  [[nodiscard]] bool isEnabled(LogPriority priority,
                               LogCategoryFlags) const override {
    auto l = orc::get_logger();
    const spdlog::level::level_enum lvl = l->level();
    if (priority >= eError) return lvl <= spdlog::level::err;
    if (priority >= eWarn) return lvl <= spdlog::level::warn;
    if (priority >= eInfo) return lvl <= spdlog::level::info;
    return lvl <= spdlog::level::debug;
  }
  void sync() override { orc::get_logger()->flush(); }
};

namespace orc {
void AC3RFSinkStageDeps::init(TriggerProgressCallback progress_callback,
                              std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

std::ostream* AC3RFSinkStageDeps::open_output(const std::string& output_path,
                                              bool piping,
                                              std::ofstream& file_storage) {
  if (piping) {
    return &orc::pipe_io::stdout_binary_stream();
  }
  file_storage.open(output_path, std::ios::binary | std::ios::trunc);
  return file_storage ? &file_storage : nullptr;
}

AC3RFSinkDecodeResult AC3RFSinkStageDeps::decode_and_write_ac3(
    const VideoFrameRepresentation* representation,
    const std::string& output_path) {
  const bool piping = orc::pipe_io::is_pipe_path(output_path);

  std::ofstream out_file;
  std::ostream* out = open_output(output_path, piping, out_file);
  if (!out) {
    return {false, 0, "Failed to open output file: " + output_path};
  }

  const auto frame_rng = representation->frame_range();
  const FrameID start_frame = frame_rng.first;
  const FrameID end_frame = frame_rng.last;
  const uint64_t total_frames = frame_rng.count();

  ORC_LOG_DEBUG("AC3RFSinkDeps: frame range [{}, {}], total_frames={}",
                start_frame, end_frame, total_frames);

  SpdlogLogger ac3_log;
  Ac3Decoder decoder(ac3_log);

  uint64_t frames_written = 0;

  for (FrameID fid = start_frame; fid <= end_frame; ++fid) {
    if (cancel_requested_ && cancel_requested_->load()) {
      if (!piping) out_file.close();
      return {false, 0, "Cancelled by user"};
    }

    auto symbols = representation->get_ac3_symbols(fid);
    auto frames = decoder.decodeSymbols(symbols);
    for (const auto& frame : frames) {
      out->write(reinterpret_cast<const char*>(frame.data()),
                 static_cast<std::streamsize>(frame.size()));
      ++frames_written;
    }

    const uint64_t current = fid - start_frame + 1;
    if (progress_callback_) {
      progress_callback_(current, total_frames,
                         "Decoding AC3 RF: frame " + std::to_string(current) +
                             "/" + std::to_string(total_frames));
    }
  }

  if (!piping) out_file.close();
  ORC_LOG_INFO("AC3RFSinkDeps: {}", decoder.reedSolomonStatistics());
  return {true, frames_written,
          "Success: " + std::to_string(frames_written) + " frames written"};
}
}  // namespace orc
