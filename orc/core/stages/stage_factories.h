/*
 * File:        stage_factories.h
 * Module:      orc-core
 * Purpose:     Implement abstract factory design pattern (
 * https://en.wikipedia.org/wiki/Abstract_factory_pattern ) to a) encourage
 * encapsulation in the architecture and b) make mocking in unit tests easier
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Matt Ownby
 */

#ifndef DECODE_ORC_ROOT_STAGE_FACTORIES_H
#define DECODE_ORC_ROOT_STAGE_FACTORIES_H

#include "../factories_interface.h"
#include "stage_factories_interface.h"

namespace orc {
class StageFactories : public IStageFactories {
 public:
  // We intentionally pass in the parent factory (IFactories) to the constructor
  // of this child factory (StageFactories)
  //  because the parent factory will already be instantiated, so it's natural
  //  to pass in 'this' to StageFactories constructor.
  // Normally, we'd prefer to use std::shared_ptr for this to avoid potential
  // memory management issues, but in this case, we are confident that the
  // parent factory will outlive the child factory, so we can safely use a raw
  // pointer.
  StageFactories(IFactories& factories) : factories_(factories) {}

  std::shared_ptr<IDaphneVBISinkStageDeps> CreateInstanceDaphneVBISinkStageDeps(
      TriggerProgressCallback& progress_callback,
      std::atomic<bool>& is_processing,
      std::atomic<bool>& cancel_requested) override;

  std::shared_ptr<IDaphneVBIWriterUtil> CreateInstanceDaphneVBIWriterUtil(
      IFileWriter<uint8_t>& writer) override;

  std::shared_ptr<ITBCSinkStageDeps> CreateInstanceTBCSinkStageDeps(
      TriggerProgressCallback& progress_callback,
      std::atomic<bool>& is_processing,
      std::atomic<bool>& cancel_requested) override;

 private:
  // The parent factory, kept per the constructor note above so this child can
  // reach back into it. No member needs it yet, hence [[maybe_unused]].
  [[maybe_unused]] IFactories& factories_;
};
}  // namespace orc

#endif  // DECODE_ORC_ROOT_STAGE_FACTORIES_H