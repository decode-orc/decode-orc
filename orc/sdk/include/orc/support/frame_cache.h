/*
 * File:        frame_cache.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     The frame cache a stage keeps of the frames it builds: shared
 *              entries, pointers pinned for the calling thread, one build per
 *              frame, and a capacity sized from the span of its readers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/stage/frame_id.h>
#include <orc/support/lru_cache.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace orc {

// ============================================================================
// Capacity
// ============================================================================
// A frame cache must hold every frame its readers can want at once, or a
// frame is evicted while still wanted and built again. The readers are, at
// most:
//
// - a sink's decode workers, each of which may claim a frame while fewer
//   than twice the worker count are claimed and unwritten;
// - the frames either side of each that its chroma decoder reads;
// - a FrameReadAhead running ahead of a serial loop;
// - a stacker downstream, which reads each input up to its still-frame
//   search either side of the frames it stacks, and builds signatures
//   further ahead.
//
// A sink caps its workers so that this span fits the smallest cache between
// it and its sources (max_concurrent_frame_requests()), so the span is
// defined however many threads the host has.

// The most decode workers a sink starts when its thread count is automatic.
inline constexpr unsigned kMaxAutoFrameWorkers = 32;

// The widest look-behind plus look-ahead of any chroma decoder, in frames
// (Transform PAL 3D reads 1 behind and 4 ahead).
inline constexpr size_t kMaxDecoderLookAroundFrames = 8;

// The widest FrameReadAhead window (FrameReadAhead::default_window()).
inline constexpr size_t kMaxReadAheadFrames = 32;

// What a stacker adds to its inputs' readers: its default still-frame search
// either side of a frame (24) and the signatures it builds ahead of the
// search (8). A wider search than the default still works; frames past the
// allowance are read again.
inline constexpr size_t kStackerReachFrames = 2 * 24 + 8;

// Decode workers a sink starts when its thread count is automatic: the
// host's hardware threads, at most kMaxAutoFrameWorkers.
inline unsigned default_frame_workers() {
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  return std::min(threads, kMaxAutoFrameWorkers);
}

// Frames a sink with |workers| decode workers can want at once: its claim
// window and the decoders' look-around.
inline size_t sink_frame_span(unsigned workers) {
  return 2 * static_cast<size_t>(std::max(workers, 1u)) +
         kMaxDecoderLookAroundFrames;
}

// The capacity a frame cache needs for a sink of |workers| decode workers
// downstream of it: that sink's span, a read-ahead, and a stacker's reach.
inline size_t frame_cache_capacity_for(unsigned workers) {
  return sink_frame_span(workers) + kMaxReadAheadFrames + kStackerReachFrames;
}

// The most decode workers whose frame_cache_capacity_for() fits |bound|
// frames; at least 1.
inline unsigned frame_workers_within(size_t bound) {
  const size_t reserve = frame_cache_capacity_for(0) - sink_frame_span(0);
  if (bound <= reserve + sink_frame_span(1)) return 1;
  const size_t workers = (bound - reserve - kMaxDecoderLookAroundFrames) / 2;
  return static_cast<unsigned>(
      std::min<size_t>(workers, std::numeric_limits<unsigned>::max()));
}

// The capacity of a stage's frame cache on this host, which is also what the
// stage reports from max_concurrent_frame_requests().
inline size_t nominal_frame_cache_capacity() {
  return frame_cache_capacity_for(default_frame_workers());
}

// The capacity a stage's frame cache is created with:
// nominal_frame_cache_capacity(), unless ORC_FRAME_CACHE_FRAMES sets another
// (at least 1). The variable is a diagnostic: a capacity of 2 under a
// sanitizer shows any reader that keeps a pointer the cache has evicted.
// Read once per plugin.
inline size_t frame_cache_capacity() {
  static const size_t capacity = [] {
    if (const char* value = std::getenv("ORC_FRAME_CACHE_FRAMES")) {
      char* end = nullptr;
      const unsigned long long frames = std::strtoull(value, &end, 10);
      if (end != value && *end == '\0' && frames > 0) {
        return static_cast<size_t>(frames);
      }
    }
    return nominal_frame_cache_capacity();
  }();
  return capacity;
}

// ============================================================================
// FrameCache
// ============================================================================
// The frames (or any per-frame values) a stage builds, least recently used
// evicted first.
//
// - Entries are shared: a frame evicted while a thread still reads it stays
//   alive until that thread lets go.
// - get() builds a missing frame once. A thread wanting a frame another
//   thread is building waits for that build. A build that throws, or returns
//   nothing, wakes its waiters, and the next of them builds it itself.
// - pin() hands out a raw pointer into an entry with
//   VideoFrameRepresentation's contract: it stays valid until the calling
//   thread pins through the same cache and slot again. Use a slot per plane
//   a reader may hold at once (luma and chroma).
//
// A build must not get() another key of the same cache: two threads could
// then wait on each other.
//
// Thread-safe: every method may be called from many threads at once.
template <typename Value, typename Key = FrameID,
          typename Hash = std::hash<Key>>
class FrameCache {
 public:
  using Entry = std::shared_ptr<const Value>;

  explicit FrameCache(size_t capacity = frame_cache_capacity())
      : capacity_(std::max<size_t>(capacity, 1)) {}

  FrameCache(const FrameCache&) = delete;
  FrameCache& operator=(const FrameCache&) = delete;

  // The cached entry for |key|, or nullptr. Marks it most recently used.
  Entry find(const Key& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return find_locked(key);
  }

  // The cached entry for |key|, built with |build| first if missing.
  // |build| returns an Entry, or a Value to be shared; a null Entry is
  // returned and not cached.
  template <typename Build>
  Entry get(const Key& key, Build&& build) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      for (;;) {
        if (Entry found = find_locked(key)) return found;
        if (building_.insert(key).second) break;
        built_.wait(lock);
      }
    }
    Entry entry;
    try {
      entry = make_entry(build());
    } catch (...) {
      finish(key, nullptr);
      throw;
    }
    return finish(key, std::move(entry));
  }

  // Caches |entry| for |key| unless one is cached already, and returns the
  // cached one. A cached entry is never replaced: a thread may be reading it.
  Entry insert(const Key& key, Entry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    return insert_locked(key, std::move(entry));
  }

  // |data|, which points into |entry|, kept alive for the calling thread
  // until it next pins through this cache and |slot|.
  template <typename T>
  const T* pin(Entry entry, const T* data, unsigned slot = 0) const {
    pin_for_thread<Value>(owner(slot), std::move(entry));
    return data;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lru_.size();
  }

  size_t capacity() const { return capacity_; }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    index_.clear();
    lru_.clear();
  }

 private:
  // Slots stay inside this object, so no two caches share an owner.
  static constexpr unsigned kMaxSlots = 4;

  const void* owner(unsigned slot) const {
    return reinterpret_cast<const char*>(this) + std::min(slot, kMaxSlots - 1);
  }

  template <typename Built>
  static Entry make_entry(Built&& built) {
    if constexpr (std::is_convertible_v<Built, Entry>) {
      return Entry(std::forward<Built>(built));
    } else {
      return std::make_shared<const Value>(std::forward<Built>(built));
    }
  }

  Entry find_locked(const Key& key) {
    const auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->second;
  }

  Entry insert_locked(const Key& key, Entry entry) {
    if (!entry) return nullptr;
    if (Entry found = find_locked(key)) return found;
    while (lru_.size() >= capacity_) {
      index_.erase(lru_.back().first);
      lru_.pop_back();
    }
    lru_.emplace_front(key, entry);
    index_[key] = lru_.begin();
    return entry;
  }

  Entry finish(const Key& key, Entry entry) {
    Entry cached;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      building_.erase(key);
      cached = insert_locked(key, std::move(entry));
    }
    built_.notify_all();
    return cached;
  }

  const size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable built_;
  std::list<std::pair<Key, Entry>> lru_;
  std::unordered_map<Key, typename std::list<std::pair<Key, Entry>>::iterator,
                     Hash>
      index_;
  std::unordered_set<Key, Hash> building_;
};

}  // namespace orc
