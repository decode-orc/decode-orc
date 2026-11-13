/*
 * File:        audio_sink_stage_deps.h
 * Module:      orc-core
 * Purpose:     AudioSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_AUDIO_SINK_STAGE_DEPS_H
#define ORC_CORE_AUDIO_SINK_STAGE_DEPS_H

#include <orc/plugin/orc_stage_services.h>
#include <orc/stage/triggerable_stage.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "audio_sink_stage_deps_interface.h"

namespace orc {
class AudioSinkStageDeps : public IAudioSinkStageDeps {
 public:
  // stage_services may be null (e.g. direct in-process construction in
  // tests); write_audio_wav() then fails with a diagnostic.
  explicit AudioSinkStageDeps(IStageServices* stage_services)
      : stage_services_(stage_services) {}

  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* is_processing,
            std::atomic<bool>* cancel_requested);

  AudioSinkWriteResult write_audio_wav(
      const VideoFrameRepresentation* representation,
      const std::string& output_path, size_t pair) override;

 private:
  IStageServices* stage_services_{nullptr};
  TriggerProgressCallback progress_callback_;
  std::atomic<bool>* is_processing_{nullptr};
  std::atomic<bool>* cancel_requested_{nullptr};
};
}  // namespace orc

#endif  // ORC_CORE_AUDIO_SINK_STAGE_DEPS_H
