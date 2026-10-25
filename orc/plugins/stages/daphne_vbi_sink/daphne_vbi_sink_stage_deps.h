/*
 * File:        daphne_vbi_sink_stage_deps.h
 * Module:      orc-core
 * Purpose:     Generate .VBI binary files, dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Matt Ownby
 */

#ifndef DECODE_ORC_ROOT_DAPHNE_VBI_SINK_STAGE_DEPS_H
#define DECODE_ORC_ROOT_DAPHNE_VBI_SINK_STAGE_DEPS_H

#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/observation/observation_service_interface.h>
#include <orc/stage/triggerable_stage.h>

#include <atomic>
#include <utility>

#include "daphne_vbi_sink_stage_deps_interface.h"
#include "daphne_vbi_writer_util_interface.h"

namespace orc {
class IStageServices;

class DaphneVBISinkStageDeps : public IDaphneVBISinkStageDeps {
 public:
  // This class needs IStageServices to create file writers and other runtime
  // dependencies, and the host IObservationService to decode the biphase and
  // white flag VBI of each field (the stage passes
  // orc::plugin::get_observation_service()). A null service (older host /
  // direct construction) still writes one record per field, with every line
  // reported as not decoded.
  DaphneVBISinkStageDeps(IStageServices* stage_services,
                         IObservationService* observation_service)
      : stage_services_(stage_services),
        observation_service_(observation_service) {}

  /**
   * @brief Sets dependencies that aren't interfaces.
   *
   * @param progress_callback The progress callback
   * @param pIsProcessing Pointer to is_processing bool
   * @param pCancelRequested Pointer to cancel_requested bool
   */
  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* pIsProcessing,
            std::atomic<bool>* pCancelRequested);

  bool write_vbi(const VideoFrameRepresentation* representation,
                 const std::string& vbi_path,
                 IObservationContext& observation_context) override;

 private:
  TriggerProgressCallback
      progress_callback_;  // Progress callback for trigger operations
  std::atomic<bool>* pIsProcessing_{};
  std::atomic<bool>* pCancelRequested_{};
  IStageServices* stage_services_{};
  IObservationService* observation_service_{};
};
}  // namespace orc

#endif  // DECODE_ORC_ROOT_DAPHNE_VBI_SINK_STAGE_DEPS_H