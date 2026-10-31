/*
 * File:        stage_measurement_store.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Host-kept store for a stage's own measurements of its input
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// SDK TIER: stage/observation — stage contract type crossing the plugin
// boundary. A layout change here bumps the host ABI version.

#include <orc/stage/observation/observation_context_interface.h>

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace orc {

/// One measurement: named values, as an observer writes them for a field.
using StageMeasurement = std::map<std::string, ObservationValue>;

/**
 * @brief Where a stage keeps an expensive measurement of its whole input.
 *
 * Some stages measure their input before they can produce a frame — the
 * stacker, for example, groups its inputs by pressing from frames spread
 * across the whole stack. The host runs several copies of a DAG (the preview,
 * the observers' renderer, a pool of background observation workers), each
 * with its own stage instances, and every copy would otherwise repeat the
 * measurement. The host's store keeps it with the node's observations, keyed
 * by the node's provenance, so it is measured once per content: across the
 * copies, and across sessions when the project's observations are kept.
 *
 * A store is bound to one node's content. A measurement must depend only on
 * that content (the node's inputs and parameters), never on which copy asked.
 *
 * Thread safety: get_or_measure() may be called from any thread. A caller that
 * finds the measurement already being taken waits for it rather than taking
 * it again.
 */
class IStageMeasurementStore {
 public:
  virtual ~IStageMeasurementStore() = default;

  /**
   * @brief The stored measurement, taken with @p measure when there is none.
   *
   * @param measurement_id Stable id of the measurement within the stage (e.g.
   *                       "stacker.pressings").
   * @param version        Version of the measurement; change it whenever what
   *                       @p measure computes or how it encodes the result
   *                       changes, so a stored older result is not reused.
   * @param measure        Takes the measurement. Called at most once per
   *                       content while the result stays stored; an exception
   *                       propagates to the caller and nothing is stored.
   * @return The measurement, stored or freshly taken.
   */
  virtual StageMeasurement get_or_measure(
      const std::string& measurement_id, const std::string& version,
      const std::function<StageMeasurement()>& measure) = 0;
};

/**
 * @brief Optional capability: a stage that keeps measurements in the host's
 * store.
 *
 * A stage opts in by additionally inheriting from IStageMeasurementConsumer.
 * Before each execute() the host hands it the store bound to the executing
 * node's content, or nullptr when the host keeps no store for it (the CLI, or
 * a node without provenance); a stage given nullptr measures for itself as it
 * did before.
 *
 * Thread safety: the host may call set_measurement_store() from several
 * threads executing the same stage instance; the stage must guard the handle.
 */
class IStageMeasurementConsumer {
 public:
  virtual ~IStageMeasurementConsumer() = default;

  virtual void set_measurement_store(
      std::shared_ptr<IStageMeasurementStore> store) = 0;
};

}  // namespace orc
