/*
 * File:        efm_sink_stage_deps.cpp
 * Module:      orc-core
 * Purpose:     RawEFMSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "efm_sink_stage_deps.h"

#include <orc/support/logging.h>

#include <cstddef>

namespace orc {
void RawEFMSinkStageDeps::init(TriggerProgressCallback progress_callback,
                               std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

RawEFMSinkWriteResult RawEFMSinkStageDeps::write_raw_efm(
    const VideoFrameRepresentation* representation,
    const std::string& output_path, bool include_confidence) {
  auto frame_rng = representation->frame_range();
  FrameID start_frame = frame_rng.first;
  FrameID end_frame = frame_rng.last;

  uint64_t total_frames = frame_rng.count();
  ORC_LOG_DEBUG("RawEFMSinkDeps: Processing {} frames", total_frames);

  uint64_t total_tvalues = 0;
  for (FrameID fid = start_frame; fid <= end_frame; ++fid) {
    total_tvalues += representation->get_efm_sample_count(fid);
  }

  ORC_LOG_DEBUG("RawEFMSinkDeps: Total EFM t-values: {}", total_tvalues);

  if (total_tvalues == 0) {
    return {false, 0, "Error: No EFM t-values found in frame range"};
  }

  std::shared_ptr<IFileWriterUint8> writer;
  if (stage_services_) {
    writer = stage_services_->create_buffered_file_writer_uint8(
        static_cast<size_t>(4 * 1024 * 1024));
  }
  if (!writer) {
    return {false, 0, "Error: File writer service unavailable"};
  }
  if (!writer->open(output_path)) {
    return {false, 0, "Error: Failed to open output file: " + output_path};
  }

  uint64_t tvalues_written = 0;
  uint64_t invalid_tvalue_count = 0;

  for (FrameID fid = start_frame; fid <= end_frame; ++fid) {
    if (cancel_requested_ && cancel_requested_->load()) {
      writer->close();
      return {false, 0, "Cancelled by user"};
    }

    auto tvalues = representation->get_efm_samples(fid);
    if (!tvalues.empty()) {
      // The pipeline byte packs the t-value into the low nibble and the
      // producer's doubt into the high one, so the conventional T3-T11 range
      // check applies to the extracted t-value, never to the raw byte.
      for (const auto& packed : tvalues) {
        const uint8_t tval = efm_tvalue(packed);
        if (tval < 3 || tval > 11) {
          invalid_tvalue_count++;
        }
      }

      // Writing the packed byte is the lossless export; stripping the doubt
      // nibble yields the plain t-value stream that tools predating the
      // confidence field expect. A stream with nothing doubted is byte
      // identical either way.
      if (!include_confidence) {
        for (auto& value : tvalues) {
          value = efm_tvalue(value);
        }
      }

      writer->write(tvalues);
      tvalues_written += tvalues.size();
    }

    uint64_t current_frame = fid - start_frame + 1;
    if (current_frame % 10 == 0 && progress_callback_) {
      progress_callback_(current_frame, total_frames,
                         "Writing EFM: frame " + std::to_string(current_frame) +
                             "/" + std::to_string(total_frames));
    }
  }

  writer->close();

  ORC_LOG_INFO(
      "RawEFMSinkDeps: Successfully wrote {} t-values ({} confidence nibble) "
      "to {}",
      tvalues_written, include_confidence ? "with" : "without", output_path);
  ORC_LOG_DEBUG(
      "RawEFMSinkDeps: Expected t-values: {}, Actual t-values: {}, Match: {}",
      total_tvalues, tvalues_written,
      total_tvalues == tvalues_written ? "YES" : "NO");

  if (invalid_tvalue_count > 0) {
    ORC_LOG_WARN(
        "RawEFMSinkDeps: Found {} invalid t-values (outside range [3, 11])",
        invalid_tvalue_count);
  }

  return {true, tvalues_written,
          "Success: " + std::to_string(tvalues_written) + " t-values written"};
}
}  // namespace orc
