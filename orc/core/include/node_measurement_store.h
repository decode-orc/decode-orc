/*
 * File:        node_measurement_store.h
 * Module:      orc-core
 * Purpose:     A stage's measurement store, kept in the observation store
 *              under the node's provenance fingerprint
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// Host-internal (wraps ObservationStore). Only orc-core and orc-presenters may
// include this header; GUI/CLI code must go through presenters.
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/node_measurement_store.h. Use a presenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/node_measurement_store.h. Use a presenter instead."
#endif

#include <orc/stage/observation/stage_measurement_store.h>

#include <memory>
#include <string>

#include "frame_provenance.h"
#include "observation_store.h"

namespace orc {

/**
 * @brief IStageMeasurementStore over the shared ObservationStore, bound to one
 * node's content.
 *
 * A measurement is one record keyed by the node's fingerprint, field 0, the
 * observer id "stage:<measurement_id>" and the measurement's version; its
 * values sit under the namespace <measurement_id>. The record therefore lives,
 * persists to the sidecar and is garbage-collected with the node's
 * observations, and every DAG copy rendering the same content — the preview,
 * the observers' renderer, the background workers, a later session — finds
 * the one measurement.
 *
 * Thread safety: as ObservationStore::get_or_compute().
 */
class NodeMeasurementStore final : public IStageMeasurementStore {
 public:
  /// Observer-id prefix that keeps stage measurements apart from observers'
  /// records.
  static constexpr const char* kObserverIdPrefix = "stage:";

  NodeMeasurementStore(std::shared_ptr<ObservationStore> store,
                       NodeFingerprint fingerprint);

  StageMeasurement get_or_measure(
      const std::string& measurement_id, const std::string& version,
      const std::function<StageMeasurement()>& measure) override;

  /// The record key a measurement is stored under.
  ObservationRecordKey key_for(const std::string& measurement_id,
                               const std::string& version) const;

 private:
  std::shared_ptr<ObservationStore> store_;
  NodeFingerprint fingerprint_;
};

}  // namespace orc
