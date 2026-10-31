/*
 * File:        node_measurement_store.cpp
 * Module:      orc-core
 * Purpose:     A stage's measurement store, kept in the observation store
 *              under the node's provenance fingerprint
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "node_measurement_store.h"

#include <utility>

namespace orc {

NodeMeasurementStore::NodeMeasurementStore(
    std::shared_ptr<ObservationStore> store, NodeFingerprint fingerprint)
    : store_(std::move(store)), fingerprint_(std::move(fingerprint)) {}

ObservationRecordKey NodeMeasurementStore::key_for(
    const std::string& measurement_id, const std::string& version) const {
  return ObservationRecordKey{fingerprint_, FieldID(0),
                              kObserverIdPrefix + measurement_id, version};
}

StageMeasurement NodeMeasurementStore::get_or_measure(
    const std::string& measurement_id, const std::string& version,
    const std::function<StageMeasurement()>& measure) {
  ObservationRecord record = store_->get_or_compute(
      key_for(measurement_id, version),
      [&]() -> ObservationRecord { return {{measurement_id, measure()}}; });
  auto it = record.find(measurement_id);
  if (it == record.end()) {
    return {};
  }
  return std::move(it->second);
}

}  // namespace orc
