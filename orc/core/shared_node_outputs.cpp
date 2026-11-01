/*
 * File:        shared_node_outputs.cpp
 * Module:      orc-core
 * Purpose:     Node outputs produced once and shared by several private
 *              copies of one DAG
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "shared_node_outputs.h"

namespace orc {

SharedNodeOutputs::Entry SharedNodeOutputs::get_or_produce(
    const NodeID& node_id, const std::function<Entry()>& produce) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      auto it = entries_.find(node_id);
      if (it != entries_.end()) {
        return it->second;
      }
      if (in_flight_.insert(node_id).second) {
        break;  // this caller produces it
      }
      produced_.wait(lock, [&] { return in_flight_.count(node_id) == 0; });
    }
  }

  Entry entry;
  try {
    entry = produce();
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      in_flight_.erase(node_id);
    }
    produced_.notify_all();
    throw;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[node_id] = entry;
    in_flight_.erase(node_id);
  }
  produced_.notify_all();
  return entry;
}

size_t SharedNodeOutputs::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

std::shared_ptr<SharedNodeOutputs> SharedNodeOutputsPool::for_dag(
    const std::shared_ptr<const DAG>& dag) {
  if (!dag) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!outputs_ || dag_.lock() != dag) {
    dag_ = dag;
    outputs_ = std::make_shared<SharedNodeOutputs>();
  }
  return outputs_;
}

}  // namespace orc
