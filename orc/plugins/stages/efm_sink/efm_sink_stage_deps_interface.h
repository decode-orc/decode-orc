/*
 * File:        efm_sink_stage_deps_interface.h
 * Module:      orc-core
 * Purpose:     Interface for EFMSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_EFM_SINK_STAGE_DEPS_INTERFACE_H
#define ORC_CORE_EFM_SINK_STAGE_DEPS_INTERFACE_H

#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace orc {
struct EFMSinkOptions {
  std::string output_path;
  bool audio_mode{true};
  bool no_timecodes{false};
  bool audacity_labels{false};
  bool no_audio_concealment{false};
  // Q-8: when true, ignore the 50/15 us pre-emphasis flag and skip de-emphasis.
  bool ignore_preemphasis{false};
  bool zero_pad{false};
  bool no_wav_header{false};
  bool output_metadata{false};
  bool report{false};
  // Issue #231: align the head of the decoded audio with the input (video)
  // timeline so the WAV starts in sync with the video. Ignored when zero_pad
  // is set (zero_pad anchors to disc absolute time 00:00:00 instead).
  bool video_sync{true};
  // User sync slip in milliseconds applied on top of the alignment; positive
  // delays the audio relative to the video, negative advances it.
  double offset_ms{0.0};
  // Issue #307: smallest producer doubt (0 trusted - 15 distrusted) that makes
  // an otherwise-clean EFM symbol a C1/C2 erasure candidate. 0 disables
  // doubt-derived erasures (bit-exact legacy output).
  uint8_t doubt_erasure_threshold{0};
};

struct EFMSinkDecodeResult {
  bool success{false};
  std::string status_message;
};

class IEFMSinkStageDeps {
 public:
  virtual ~IEFMSinkStageDeps() = default;

  virtual void init(TriggerProgressCallback progress_callback,
                    std::atomic<bool>* cancel_requested) = 0;

  virtual EFMSinkDecodeResult decode_efm(
      const VideoFrameRepresentation* representation,
      const EFMSinkOptions& options) = 0;
};
}  // namespace orc

#endif  // ORC_CORE_EFM_SINK_STAGE_DEPS_INTERFACE_H
