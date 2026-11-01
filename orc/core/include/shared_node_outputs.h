/*
 * File:        shared_node_outputs.h
 * Module:      orc-core
 * Purpose:     Node outputs produced once and shared by several private
 *              copies of one DAG
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/artifact.h>
#include <orc/stage/node_id.h>

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "dag_executor.h"

namespace orc {

/**
 * @brief The outputs of one DAG generation's nodes, shared by the private
 * copies of that DAG.
 *
 * The background observation workers each clone the DAG, because two threads
 * executing one stage instance race on its configuration. Each copy then
 * produced its own representations too, and with them its own frame caches —
 * a source keeps 150 frames, FM Beat Cancel and the stacker more — so a stack
 * of eleven discs held all of that once per worker. A representation's const
 * methods are safe to call from several threads (see VideoFrameRepresentation),
 * so the copies need only one of each: the first copy to execute a node
 * produces its outputs, and every other copy uses those.
 *
 * Keyed by node id, so one instance must only ever serve copies of one DAG
 * (see SharedNodeOutputsPool). Artifact ids are no key: several stages give
 * every output the same id.
 *
 * A stored entry keeps the stage that produced it alive, because a
 * representation may refer back to its stage; the producing copy can be
 * dropped while another copy still reads the outputs.
 *
 * Thread safety: get_or_produce() may be called from any thread. A caller that
 * finds the node being produced waits for it rather than producing it again.
 */
class SharedNodeOutputs {
 public:
  struct Entry {
    std::vector<ArtifactPtr> outputs;
    DAGStagePtr producer;  ///< Kept alive with the outputs.
  };

  /// The outputs stored for @p node_id, produced with @p produce (and stored)
  /// when there are none. When @p produce throws, the exception propagates to
  /// its caller, nothing is stored, and a waiting caller produces it in turn.
  /// @p produce runs with no lock held.
  Entry get_or_produce(const NodeID& node_id,
                       const std::function<Entry()>& produce);

  /// Number of nodes whose outputs are stored.
  size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable produced_;
  std::map<NodeID, Entry> entries_;
  std::set<NodeID> in_flight_;
};

/**
 * @brief Hands each copy of a DAG the SharedNodeOutputs of its generation.
 *
 * The background workers adopt a new DAG one by one, each on its own thread,
 * so the first worker to ask with a new DAG starts a new generation and the
 * rest join it. The previous generation's outputs are released once the last
 * worker has moved on.
 *
 * Thread safety: for_dag() may be called from any thread.
 */
class SharedNodeOutputsPool {
 public:
  /// The shared outputs for copies of @p dag; nullptr for a null @p dag.
  std::shared_ptr<SharedNodeOutputs> for_dag(
      const std::shared_ptr<const DAG>& dag);

 private:
  std::mutex mutex_;
  // Weak, so a dropped DAG whose address is reused by a new one can never
  // be mistaken for it.
  std::weak_ptr<const DAG> dag_;
  std::shared_ptr<SharedNodeOutputs> outputs_;
};

}  // namespace orc
