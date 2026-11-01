/*
 * File:        observation_store.h
 * Module:      orc-core
 * Purpose:     Provenance-keyed, memory-budgeted store of observer output
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// Host-internal observation store. Only orc-core and orc-presenters may include
// this header; GUI/CLI code must go through presenters.
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/observation_store.h. Use a presenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/observation_store.h. Use a presenter instead."
#endif

#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context_interface.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "frame_provenance.h"

namespace orc {

// Defined in observation_persistence.h (included only where the store's
// persistence hooks are used) to avoid a circular include.
class IObservationPersistence;

/**
 * @brief Identity of one stored observation record.
 *
 * A record holds everything a single observer wrote for a single field of a
 * single frame-content. Keying on the node's provenance fingerprint (rather
 * than a live DAG node id) is what lets records survive DAG rebuilds and,
 * later, whole sessions: an unchanged pipeline produces the same fingerprint,
 * so the record still matches. The observer version is part of the key so
 * bumping an observer naturally invalidates only that observer's records.
 */
struct ObservationRecordKey {
  NodeFingerprint fingerprint;  ///< Provenance of the field's parent node.
  FieldID field_id;         ///< Derived field id (frame_id * 2 + field_idx).
  std::string observer_id;  ///< Stable observer id (e.g. "white_snr").
  std::string observer_version;  ///< Observer version string.

  bool operator==(const ObservationRecordKey& other) const {
    return field_id == other.field_id && fingerprint == other.fingerprint &&
           observer_id == other.observer_id &&
           observer_version == other.observer_version;
  }
  bool operator!=(const ObservationRecordKey& other) const {
    return !(*this == other);
  }
};

/**
 * @brief The namespaced observation values one observer wrote for one field.
 *
 * Shape mirrors IObservationContext::get_all_observations(): namespace -> key
 * -> value. An empty record is a valid, distinct state ("this observer ran for
 * this field and produced nothing"), so a stored empty record still reports
 * has() == true and suppresses re-running.
 */
using ObservationRecord =
    std::map<std::string, std::map<std::string, ObservationValue>>;

/**
 * @brief Thread-safe, memory-budgeted store of provenance-keyed observations.
 *
 * Records are keyed by ObservationRecordKey so they are tied to frame *content*
 * rather than to a live DAG node. The store is the sole thread-safe handoff
 * point between the (single-threaded) renderer/scheduler and any reader.
 *
 * Eviction: an approximate byte budget bounds memory. put() and retain_only()
 * evict least-recently-used records once usage exceeds the budget. get(),
 * load_into() and put() mark a record most-recently-used.
 *
 * Read-through: when a persistence sidecar is attached, has()/get()/load_into()
 * fall back to it on an in-memory miss and re-install the record — memory is a
 * cache over the durable sidecar, so budget eviction never causes derived data
 * to be recomputed, only reloaded. Without a sidecar (or for records that never
 * reached it) a miss is genuine.
 *
 * Thread safety (coding standards §5.3.3): every public method is safe to call
 * concurrently from any thread; all state is guarded by an internal mutex. The
 * sidecar is consulted outside that mutex so database I/O never blocks
 * in-memory reads on other threads.
 */
class ObservationStore {
 public:
  /// Default memory budget of approximate record payload. Sized so a full
  /// long-play source stays resident: ~54k frames × 2 fields × 9 observers is
  /// roughly one million records (~300 MB), and pass-through aliasing can
  /// double the key count. Read-through to the sidecar makes overflow cheap
  /// (reload, not recompute), so the budget is a soft ceiling, not a cliff.
  static constexpr std::size_t kDefaultMemoryBudgetBytes =
      static_cast<std::size_t>(512) * 1024 * 1024;

  explicit ObservationStore(
      std::size_t memory_budget_bytes = kDefaultMemoryBudgetBytes);

  // Stops the write-behind thread (flushing anything pending) before the store
  // is destroyed.
  ~ObservationStore();

  // Non-copyable, non-movable (holds a mutex).
  ObservationStore(const ObservationStore&) = delete;
  ObservationStore& operator=(const ObservationStore&) = delete;
  ObservationStore(ObservationStore&&) = delete;
  ObservationStore& operator=(ObservationStore&&) = delete;

  /// True if a record is stored for @p key, reading through to the sidecar on
  /// an in-memory miss (the reloaded record is re-installed and marked
  /// most-recently-used).
  bool has(const ObservationRecordKey& key);

  /// Presence-only variant of has(): true if a record exists in memory or in
  /// the sidecar, WITHOUT loading it into memory or touching LRU order. Use for
  /// coverage checks (e.g. "does this frame still need observing?") that probe
  /// many keys they may never read — has() would churn the LRU and pull every
  /// probed record through the sidecar.
  bool has_stored(const ObservationRecordKey& key);

  /// Return the stored record for @p key, or nullopt on miss (after reading
  /// through to the sidecar). Marks the record most-recently-used on a hit.
  std::optional<ObservationRecord> get(const ObservationRecordKey& key);

  /// Store (or replace) the record for @p key and mark it most-recently-used.
  /// Evicts least-recently-used records if the byte budget is exceeded, but
  /// never evicts the record just inserted.
  void put(const ObservationRecordKey& key, ObservationRecord record);

  /// Return the stored record for @p key, computing it with @p compute and
  /// storing it (as put() does) on a miss. Concurrent callers for the same key
  /// wait for the one computing it rather than computing it again. When
  /// @p compute throws, the exception propagates to its caller, nothing is
  /// stored, and a waiting caller computes it in turn. @p compute runs with no
  /// store lock held.
  ObservationRecord get_or_compute(
      const ObservationRecordKey& key,
      const std::function<ObservationRecord()>& compute);

  /// Load the record for @p key into @p context at its field id. Returns false
  /// on miss (context left untouched, after reading through to the sidecar).
  /// Marks the record most-recently-used.
  bool load_into(const ObservationRecordKey& key, IObservationContext& context);

  /// Drop every record whose fingerprint is not in @p keep, then evict
  /// least-recently-used records until usage is within @p budget. Also updates
  /// the active budget to @p budget.
  void retain_only(const std::unordered_set<NodeFingerprint>& keep,
                   std::size_t budget);

  /// Number of stored records.
  std::size_t size() const;

  /// Approximate payload size of all stored records, in bytes.
  std::size_t memory_usage_bytes() const;

  /// Active memory budget in bytes.
  std::size_t memory_budget_bytes() const;

  /// Remove all records (budget unchanged).
  void clear();

  // --- Durable sidecar persistence (Phase 6) --------------------------------
  //
  // Persistence is optional: without it the store is a pure in-memory cache
  // (its default state, unchanged from earlier phases). When enabled, records
  // written by put() are streamed to durable storage behind the interactive
  // path, and a project can start warm from a previous session.

  /// Enable (or, with nullptr, disable) write-behind persistence. Records
  /// passed to put() after this call are queued and flushed to @p persistence
  /// on a background writer thread. Passing nullptr flushes and stops the
  /// writer. Not thread-safe with concurrent put()/warm_start(); call during
  /// store wiring.
  void set_persistence(std::shared_ptr<IObservationPersistence> persistence);

  /// Populate the in-memory store from persistence for every record whose
  /// fingerprint is in @p keep. Loaded records are NOT re-queued for writing.
  /// No-op without persistence.
  void warm_start(const std::unordered_set<NodeFingerprint>& keep);

  /// Block until every queued write-behind record has been handed to
  /// persistence. No-op without persistence.
  void flush();

  /// Forward garbage collection to persistence: drop persisted records whose
  /// fingerprint is not in @p keep. No-op without persistence.
  void gc_persistence(const std::unordered_set<NodeFingerprint>& keep);

  /// Forward an observer-version purge to persistence: drop persisted records
  /// for @p observer_id whose version differs from @p current_version. No-op
  /// without persistence.
  void purge_observer_version(const std::string& observer_id,
                              const std::string& current_version);

 private:
  struct Hash {
    std::size_t operator()(const ObservationRecordKey& key) const noexcept;
  };

  struct Entry {
    ObservationRecordKey key;
    ObservationRecord record;
    std::size_t bytes;
  };

  // Estimate the approximate payload size of a record (used for the budget).
  static std::size_t estimate_bytes(const ObservationRecordKey& key,
                                    const ObservationRecord& record);

  // Move the entry referenced by @p it to the front of the LRU list. Caller
  // holds mutex_.
  void touch(std::list<Entry>::iterator it) const;

  // Evict least-recently-used entries until usage_bytes_ <= budget_bytes_,
  // keeping at least @p keep_at_least entries. Caller holds mutex_.
  void evict_to_budget(std::size_t keep_at_least);

  // Insert or replace @p record for @p key in the in-memory LRU structure
  // without queuing it for persistence. Caller holds mutex_. Shared by put()
  // (which additionally queues) and warm_start() (which must not re-persist).
  void put_in_memory(const ObservationRecordKey& key, ObservationRecord record);

  // Ensure @p key is resident in memory: on an index miss, reload it from the
  // persistence sidecar (if attached) and re-install it. Returns true when the
  // record is resident on return. Takes mutex_ internally; the sidecar query
  // runs with no store lock held so database I/O never blocks other readers.
  bool ensure_resident(const ObservationRecordKey& key);

  // Write-behind worker loop: drains queued records to persistence_ in
  // batches until stopped.
  void writer_loop();

  // Stop the write-behind thread, flushing anything still queued.
  void stop_writer();

  mutable std::mutex mutex_;
  std::size_t budget_bytes_;
  std::size_t usage_bytes_ = 0;

  // LRU list (front = most recently used) plus an index into it. Marked mutable
  // so const probes (get/load_into) can refresh LRU order under the lock.
  mutable std::list<Entry> lru_;
  mutable std::unordered_map<ObservationRecordKey, std::list<Entry>::iterator,
                             Hash>
      index_;

  // Keys get_or_compute() is computing now, guarded by mutex_; in_flight_cv_
  // wakes the callers waiting for one to finish.
  std::unordered_set<ObservationRecordKey, Hash> in_flight_;
  std::condition_variable in_flight_cv_;

  // --- Write-behind persistence state ---------------------------------------
  // persistence_ is set once during wiring and read by the writer thread; it
  // is not re-assigned while the writer runs. The queue and its counters are
  // guarded by wb_mutex_ (a separate lock from mutex_ so database I/O never
  // blocks in-memory reads). queued_ counts every record handed to put();
  // flushed_ counts every record actually written, so flush() waits for
  // flushed_ == queued_.
  std::shared_ptr<IObservationPersistence> persistence_;
  std::mutex wb_mutex_;
  std::condition_variable wb_cv_;
  std::vector<ObservationRecordKey> pending_keys_;
  std::vector<ObservationRecord> pending_records_;
  std::thread writer_;
  bool writer_stop_ = false;
  std::uint64_t queued_ = 0;
  std::uint64_t flushed_ = 0;
};

}  // namespace orc
