/*
 * File:        observation_store.cpp
 * Module:      orc-core
 * Purpose:     Provenance-keyed, memory-budgeted store of observer output
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "observation_store.h"

#include <utility>
#include <variant>

#include "observation_persistence.h"

namespace orc {

namespace {

// Approximate per-record and per-entry bookkeeping overhead so that many tiny
// records still count against the budget rather than appearing free.
constexpr std::size_t kPerRecordOverhead = 64;
constexpr std::size_t kPerValueOverhead = 32;

// Approximate the in-memory footprint of a single observation value.
std::size_t value_bytes(const ObservationValue& value) {
  return std::visit(
      [](const auto& v) -> std::size_t {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return v.size();
        } else {
          return sizeof(T);
        }
      },
      value);
}

}  // namespace

std::size_t ObservationStore::Hash::operator()(
    const ObservationRecordKey& key) const noexcept {
  std::size_t h = std::hash<std::string>{}(key.fingerprint.value);
  h ^= std::hash<FieldID>{}(key.field_id) + 0x9e3779b97f4a7c15ULL + (h << 6) +
       (h >> 2);
  h ^= std::hash<std::string>{}(key.observer_id) + 0x9e3779b97f4a7c15ULL +
       (h << 6) + (h >> 2);
  h ^= std::hash<std::string>{}(key.observer_version) + 0x9e3779b97f4a7c15ULL +
       (h << 6) + (h >> 2);
  return h;
}

ObservationStore::ObservationStore(std::size_t memory_budget_bytes)
    : budget_bytes_(memory_budget_bytes) {}

ObservationStore::~ObservationStore() { stop_writer(); }

std::size_t ObservationStore::estimate_bytes(const ObservationRecordKey& key,
                                             const ObservationRecord& record) {
  std::size_t bytes = kPerRecordOverhead + key.fingerprint.value.size() +
                      key.observer_id.size() + key.observer_version.size();
  for (const auto& [ns, keys] : record) {
    bytes += ns.size();
    for (const auto& [k, v] : keys) {
      bytes += kPerValueOverhead + k.size() + value_bytes(v);
    }
  }
  return bytes;
}

void ObservationStore::touch(std::list<Entry>::iterator it) const {
  lru_.splice(lru_.begin(), lru_, it);
}

void ObservationStore::evict_to_budget(std::size_t keep_at_least) {
  while (usage_bytes_ > budget_bytes_ && lru_.size() > keep_at_least) {
    Entry& oldest = lru_.back();
    usage_bytes_ -= oldest.bytes;
    index_.erase(oldest.key);
    lru_.pop_back();
  }
}

bool ObservationStore::ensure_resident(const ObservationRecordKey& key) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index_.find(key) != index_.end()) {
      return true;
    }
    if (!persistence_) {
      return false;
    }
  }
  // Sidecar point read with no store lock held, so database I/O never blocks
  // other readers. A concurrent install of the same key is harmless:
  // put_in_memory replaces.
  auto record = persistence_->load_one(key);
  if (!record) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  put_in_memory(key, std::move(*record));
  return true;
}

bool ObservationStore::has(const ObservationRecordKey& key) {
  return ensure_resident(key);
}

bool ObservationStore::has_stored(const ObservationRecordKey& key) {
  std::shared_ptr<IObservationPersistence> persistence;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index_.find(key) != index_.end()) {
      return true;
    }
    persistence = persistence_;
  }
  // Presence probe with no store lock held, so sidecar I/O never blocks other
  // readers. Deliberately does NOT install the record in memory: callers are
  // coverage checks, not readers.
  return persistence && persistence->exists(key);
}

std::optional<ObservationRecord> ObservationStore::get(
    const ObservationRecordKey& key) {
  if (!ensure_resident(key)) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = index_.find(key);
  if (it == index_.end()) {
    return std::nullopt;  // evicted between ensure_resident and here
  }
  touch(it->second);
  return it->second->record;
}

void ObservationStore::put_in_memory(const ObservationRecordKey& key,
                                     ObservationRecord record) {
  const std::size_t bytes = estimate_bytes(key, record);

  auto existing = index_.find(key);
  if (existing != index_.end()) {
    // Replace in place, adjust usage, and refresh LRU position.
    usage_bytes_ -= existing->second->bytes;
    existing->second->record = std::move(record);
    existing->second->bytes = bytes;
    usage_bytes_ += bytes;
    touch(existing->second);
  } else {
    lru_.push_front(Entry{key, std::move(record), bytes});
    index_[key] = lru_.begin();
    usage_bytes_ += bytes;
  }

  // Never evict the record just inserted/updated (it is at the front).
  evict_to_budget(/*keep_at_least=*/1);
}

void ObservationStore::put(const ObservationRecordKey& key,
                           ObservationRecord record) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    put_in_memory(key, record);  // copy retained in memory
  }

  // Queue a copy for durable storage behind the interactive path. Only when
  // persistence is enabled; otherwise the store is a pure in-memory cache.
  std::lock_guard<std::mutex> wb_lock(wb_mutex_);
  if (!persistence_) return;
  pending_keys_.push_back(key);
  pending_records_.push_back(std::move(record));
  ++queued_;
  wb_cv_.notify_one();
}

ObservationRecord ObservationStore::get_or_compute(
    const ObservationRecordKey& key,
    const std::function<ObservationRecord()>& compute) {
  for (;;) {
    if (auto stored = get(key)) {
      return *std::move(stored);
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (index_.find(key) != index_.end()) {
      continue;  // stored between the miss and taking the lock
    }
    if (in_flight_.insert(key).second) {
      break;  // this caller computes it
    }
    // Another caller is computing it: wait for that, then read what it stored
    // (or, if it failed, take a turn at computing it).
    in_flight_cv_.wait(lock, [&] { return in_flight_.count(key) == 0; });
  }

  ObservationRecord record;
  try {
    record = compute();
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      in_flight_.erase(key);
    }
    in_flight_cv_.notify_all();
    throw;
  }
  put(key, record);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    in_flight_.erase(key);
  }
  in_flight_cv_.notify_all();
  return record;
}

bool ObservationStore::load_into(const ObservationRecordKey& key,
                                 IObservationContext& context) {
  if (!ensure_resident(key)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = index_.find(key);
  if (it == index_.end()) {
    return false;  // evicted between ensure_resident and here
  }
  touch(it->second);
  for (const auto& [ns, keys] : it->second->record) {
    for (const auto& [k, v] : keys) {
      context.set(key.field_id, ns, k, v);
    }
  }
  return true;
}

void ObservationStore::retain_only(
    const std::unordered_set<NodeFingerprint>& keep, std::size_t budget) {
  std::lock_guard<std::mutex> lock(mutex_);
  budget_bytes_ = budget;

  for (auto it = lru_.begin(); it != lru_.end();) {
    if (keep.find(it->key.fingerprint) == keep.end()) {
      usage_bytes_ -= it->bytes;
      index_.erase(it->key);
      it = lru_.erase(it);
    } else {
      ++it;
    }
  }

  evict_to_budget(/*keep_at_least=*/0);
}

std::size_t ObservationStore::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lru_.size();
}

std::size_t ObservationStore::memory_usage_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return usage_bytes_;
}

std::size_t ObservationStore::memory_budget_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return budget_bytes_;
}

void ObservationStore::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  lru_.clear();
  index_.clear();
  usage_bytes_ = 0;
}

// --- Write-behind persistence ------------------------------------------------

void ObservationStore::set_persistence(
    std::shared_ptr<IObservationPersistence> persistence) {
  // Tear down any existing writer (flushing pending work) before switching.
  stop_writer();

  std::lock_guard<std::mutex> wb_lock(wb_mutex_);
  persistence_ = std::move(persistence);
  pending_keys_.clear();
  pending_records_.clear();
  queued_ = 0;
  flushed_ = 0;
  writer_stop_ = false;
  if (persistence_) {
    writer_ = std::thread(&ObservationStore::writer_loop, this);
  }
}

void ObservationStore::writer_loop() {
  for (;;) {
    std::vector<ObservationRecordKey> keys;
    std::vector<ObservationRecord> records;
    {
      std::unique_lock<std::mutex> wb_lock(wb_mutex_);
      wb_cv_.wait(wb_lock,
                  [this] { return writer_stop_ || !pending_keys_.empty(); });
      if (pending_keys_.empty()) {
        if (writer_stop_) return;
        continue;
      }
      keys.swap(pending_keys_);
      records.swap(pending_records_);
    }

    // Build the batch and hand it to persistence outside every lock.
    std::vector<PersistedObservation> batch;
    batch.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
      batch.push_back(
          PersistedObservation{std::move(keys[i]), std::move(records[i])});
    }
    if (persistence_) {
      persistence_->save(batch);
    }

    {
      std::lock_guard<std::mutex> wb_lock(wb_mutex_);
      flushed_ += batch.size();
    }
    wb_cv_.notify_all();  // wake any flush() waiters
  }
}

void ObservationStore::stop_writer() {
  {
    std::lock_guard<std::mutex> wb_lock(wb_mutex_);
    if (!writer_.joinable()) return;
    writer_stop_ = true;
  }
  wb_cv_.notify_all();
  writer_.join();
}

void ObservationStore::flush() {
  std::unique_lock<std::mutex> wb_lock(wb_mutex_);
  if (!persistence_) return;
  wb_cv_.wait(wb_lock, [this] { return flushed_ == queued_; });
}

void ObservationStore::warm_start(
    const std::unordered_set<NodeFingerprint>& keep) {
  std::shared_ptr<IObservationPersistence> persistence;
  {
    std::lock_guard<std::mutex> wb_lock(wb_mutex_);
    persistence = persistence_;
  }
  if (!persistence) return;

  // Loaded records are inserted straight into memory and must NOT be re-queued
  // for writing (they are already durable).
  persistence->load_matching(
      keep, [this](ObservationRecordKey key, ObservationRecord record) {
        std::lock_guard<std::mutex> lock(mutex_);
        put_in_memory(key, std::move(record));
      });
}

void ObservationStore::gc_persistence(
    const std::unordered_set<NodeFingerprint>& keep) {
  std::shared_ptr<IObservationPersistence> persistence;
  {
    std::lock_guard<std::mutex> wb_lock(wb_mutex_);
    persistence = persistence_;
  }
  if (persistence) persistence->retain_only(keep);
}

void ObservationStore::purge_observer_version(
    const std::string& observer_id, const std::string& current_version) {
  std::shared_ptr<IObservationPersistence> persistence;
  {
    std::lock_guard<std::mutex> wb_lock(wb_mutex_);
    persistence = persistence_;
  }
  if (persistence) {
    persistence->purge_observer_version(observer_id, current_version);
  }
}

}  // namespace orc
