/*
 * File:        efm_sink_stage_deps.cpp
 * Module:      orc-core
 * Purpose:     EFMSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "efm_sink_stage_deps.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>

#include "efm-decode/efm-lib/efm_exception.h"
#include "efm-decode/efm_processor.h"

namespace orc {
void EFMSinkStageDeps::init(TriggerProgressCallback progress_callback,
                            std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

EFMSinkDecodeResult EFMSinkStageDeps::decode_efm(
    const VideoFrameRepresentation* representation,
    const EFMSinkOptions& options) {
  const auto frame_rng = representation->frame_range();
  const FrameID start_fid = frame_rng.first;
  const FrameID end_fid = frame_rng.last;
  const uint64_t total_frames = frame_rng.count();

  ORC_LOG_DEBUG("EFMSinkDeps: Counting EFM t-values across {} frames",
                total_frames);

  uint64_t total_tvalues = 0;
  for (FrameID fid = start_fid; fid <= end_fid; ++fid) {
    total_tvalues += representation->get_efm_sample_count(fid);
  }

  if (total_tvalues == 0) {
    return {false, "Error: EFMSink: no EFM t-values found in frame range"};
  }
  ORC_LOG_DEBUG("EFMSinkDeps: Total EFM t-values: {}", total_tvalues);

  // The GUI shows a single 0-100 % progress bar for the whole stage. The
  // t-values are streamed straight into the decoder (no full-capture buffer),
  // so there is no separate buffering phase to represent: map the whole
  // frame-push loop onto 0..99 % and reserve 99..100 % for the finalise/flush
  // step so the bar advances monotonically.

  // The EFM decoder throws efm::EfmDecodeError on unrecoverable conditions
  // (invariant violations, corrupt or unsupported data). Catch it here so a
  // bad decode reports a failure instead of terminating the host process.
  try {
    EfmProcessor processor;
    processor.setAudioMode(options.audio_mode);
    processor.setNoTimecodes(options.no_timecodes);
    processor.setAudacityLabels(options.audacity_labels);
    processor.setNoAudioConcealment(options.no_audio_concealment);
    processor.setIgnorePreemphasis(options.ignore_preemphasis);
    processor.setZeroPad(options.zero_pad);
    processor.setNoWavHeader(options.no_wav_header);
    processor.setOutputMetadata(options.output_metadata);
    processor.setReportOutput(options.report);

    // Issue #231: align the decoded audio head with the input (video)
    // timeline, plus the user's millisecond slip. IEC 60908: CD audio is
    // 44.1 kHz, so the slip converts at 44.1 pairs per millisecond.
    processor.setAudioSyncOffset(options.video_sync);
    processor.setAudioSyncSlipPairs(
        static_cast<int64_t>(std::llround(options.offset_ms * 44.1)));

    processor.beginStream(options.output_path,
                          static_cast<int64_t>(total_tvalues));

    // Push each frame's t-values straight into the decoder, accumulating into a
    // bounded staging buffer only to preserve the 1024-byte chunk sizing of the
    // old buffered path (so decoded output stays byte-identical). Peak RSS is
    // now ~one chunk, independent of capture length.
    constexpr size_t CHUNK_SIZE = 1024;
    std::vector<uint8_t> staging;
    staging.reserve(CHUNK_SIZE);

    uint64_t tvalues_pushed = 0;
    int last_pct = -1;
    for (FrameID fid = start_fid; fid <= end_fid; ++fid) {
      if (cancel_requested_ && cancel_requested_->load()) {
        return {false, "Cancelled by user"};
      }

      const auto samples = representation->get_efm_samples(fid);
      size_t pos = 0;
      while (pos < samples.size()) {
        const size_t take =
            std::min(CHUNK_SIZE - staging.size(), samples.size() - pos);
        // The pipeline carries the producer's per-t-value doubt in the high
        // nibble of each byte. It travels the DAG intact, but the decoder
        // consumes t-values, so strip it here at the point of consumption.
        // (The doubt is not yet used; when the C1/C2 stages learn to take
        // erasure hints it should be forwarded rather than discarded.)
        for (size_t i = 0; i < take; ++i) {
          staging.push_back(efm_tvalue(samples[pos + i]));
        }
        pos += take;
        if (staging.size() == CHUNK_SIZE) {
          processor.pushChunk(staging);
          staging.clear();
        }
      }
      tvalues_pushed += samples.size();

      if (progress_callback_) {
        // Map the frame-push loop onto 0..99 % of the bar, and put the live
        // percentage in the label so the modal shows motion.
        const uint64_t pct = (tvalues_pushed * 99) / total_tvalues;
        if (static_cast<int>(pct) != last_pct) {
          last_pct = static_cast<int>(pct);
          progress_callback_(pct, 100,
                             "Decoding EFM... " + std::to_string(pct) + "%");
        }
      }
    }

    // Flush the final partial chunk (fewer than CHUNK_SIZE t-values).
    if (!staging.empty()) {
      processor.pushChunk(staging);
      staging.clear();
    }

    // finishStream() flushes the pipeline tail and writes final output/report
    // without emitting progress of its own; mark the bar so the modal does not
    // look stalled during the finalise step.
    if (progress_callback_) {
      progress_callback_(99, 100, "Finalising EFM decode...");
    }
    const bool ok = processor.finishStream();
    if (!ok) {
      std::string reason = processor.lastError();
      if (reason.empty()) {
        reason = "EFM decoding did not complete successfully";
      }
      return {false, "Error: EFMSink: " + reason};
    }
  } catch (const efm::EfmDecodeError& e) {
    ORC_LOG_ERROR("EFMSinkDeps: {}", e.what());
    return {false, std::string("Error: EFMSink: ") + e.what()};
  } catch (const std::exception& e) {
    // Backstop: any other escape from the decoder must fail this stage rather
    // than propagate out of the worker thread and kill the host.
    ORC_LOG_ERROR("EFMSinkDeps: unexpected exception: {}", e.what());
    return {false, std::string("Error: EFMSink: ") + e.what()};
  }

  if (progress_callback_) {
    progress_callback_(total_tvalues, total_tvalues, "Done");
  }

  return {true, "Success: EFM decoded to " + options.output_path};
}
}  // namespace orc
