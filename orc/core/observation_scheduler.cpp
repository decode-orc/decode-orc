/*
 * File:        observation_scheduler.cpp
 * Module:      orc-core
 * Purpose:     Background worker that computes missing observations ahead of
 *              demand, priority-ordered and cancellable
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "observation_scheduler.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <utility>

#include "core_observation_service.h"
#include "dag_executor.h"
#include "dag_frame_renderer.h"
#include "observation_store.h"

namespace orc {

// ---------------------------------------------------------------------------
// ObservationScheduler
// ---------------------------------------------------------------------------

ObservationScheduler::ObservationScheduler(
    std::unique_ptr<IObservationTaskRunner> runner,
    std::shared_ptr<const NodeFingerprintMap> fingerprints,
    std::shared_ptr<IObservationSchedulingPolicy> policy)
    : policy_(std::move(policy)), fingerprints_(std::move(fingerprints)) {
  auto worker = std::make_unique<Worker>();
  worker->runner = std::move(runner);
  workers_.push_back(std::move(worker));
}

ObservationScheduler::ObservationScheduler(
    TaskRunnerFactory runner_factory, unsigned worker_count,
    std::shared_ptr<const NodeFingerprintMap> fingerprints,
    std::shared_ptr<IObservationSchedulingPolicy> policy)
    : policy_(std::move(policy)), fingerprints_(std::move(fingerprints)) {
  const unsigned n = worker_count == 0 ? 1 : worker_count;
  workers_.reserve(n);
  for (unsigned i = 0; i < n; ++i) {
    auto worker = std::make_unique<Worker>();
    worker->runner = runner_factory ? runner_factory() : nullptr;
    workers_.push_back(std::move(worker));
  }
}

ObservationScheduler::~ObservationScheduler() { stop(); }

void ObservationScheduler::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (running_) {
    return;
  }
  stop_requested_ = false;
  running_ = true;
  for (auto& worker : workers_) {
    worker->cancel.store(false);
    worker->thread =
        std::thread(&ObservationScheduler::worker_loop, this, worker.get());
  }
}

void ObservationScheduler::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
    for (auto& worker : workers_) {
      worker->cancel.store(true);
    }
  }
  cv_.notify_all();
  for (auto& worker : workers_) {
    if (worker->thread.joinable()) {
      worker->thread.join();
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  running_ = false;
  // Abandon any work never dequeued; a stopped scheduler holds no queue. Items
  // dropped here were never started, so no completion is owed for them.
  for (auto& queue : queues_) {
    queue.clear();
  }
  for (auto& worker : workers_) {
    worker->has_in_flight = false;
  }
  in_flight_count_ = 0;
  // Clear the workload so a later start() begins from idle. No callback is
  // fired here: no workload snapshot is delivered after shutdown.
  workload_total_frames_ = 0;
  workload_observed_frames_ = 0;
  workload_computed_frames_ = 0;
}

bool ObservationScheduler::is_running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return running_;
}

namespace {

// Upper bound on frames per work item. Workers check the priority queues only
// between items, so this cap bounds how long a queued interactive request can
// wait behind in-flight sweep work (a whole-node sweep split merely per-worker
// would produce multi-thousand-frame items that block preemption for minutes).
constexpr std::uint64_t kMaxChunkFrames = 128;

// Split a work item into disjoint, contiguous frame sub-ranges so a pool can
// observe them in parallel and preempt between them: enough chunks to occupy
// every worker, and never more than kMaxChunkFrames per chunk. Stateful items
// are never split (their ascending-frame contract is per-item) and neither are
// single-frame items; in those cases the original item is returned unchanged.
// Priority, node, and fingerprint are preserved on every chunk.
std::vector<ObservationWorkItem> chunk_item(const ObservationWorkItem& item,
                                            unsigned parts) {
  const std::uint64_t total = item.frames.count();
  if (item.stateful || total <= 1) {
    return {item};
  }
  const std::uint64_t by_cap = (total + kMaxChunkFrames - 1) / kMaxChunkFrames;
  const std::uint64_t wanted =
      std::max<std::uint64_t>(parts == 0 ? 1 : parts, by_cap);
  const unsigned n =
      static_cast<unsigned>(std::min<std::uint64_t>(wanted, total));
  if (n <= 1) {
    return {item};
  }
  std::vector<ObservationWorkItem> out;
  out.reserve(n);
  const std::uint64_t base = total / n;
  const std::uint64_t remainder = total % n;
  FrameID start = item.frames.first;
  for (unsigned i = 0; i < n; ++i) {
    // Spread the remainder over the first `remainder` chunks so sizes differ by
    // at most one frame.
    const std::uint64_t len = base + (i < remainder ? 1 : 0);
    ObservationWorkItem chunk = item;
    chunk.frames = FrameIDRange{start, start + len - 1};
    out.push_back(std::move(chunk));
    start += len;
  }
  return out;
}

}  // namespace

void ObservationScheduler::submit(ObservationWorkItem item) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_) {
      return;
    }
    submit_item_locked(item);
  }
  cv_.notify_all();
  emit_workload();
}

void ObservationScheduler::submit_batch(
    std::vector<ObservationWorkItem> items) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_) {
      return;
    }
    // Enqueued as we go, so a later item in the same batch is deduplicated
    // against the earlier ones too.
    for (const auto& item : items) {
      submit_item_locked(item);
    }
  }
  cv_.notify_all();
  emit_workload();
}

std::vector<FrameIDRange> ObservationScheduler::uncovered_ranges_locked(
    const ObservationWorkItem& item) const {
  if (item.frames.empty()) {
    return {};
  }

  // Collect the pending ranges that overlap this item at an equal or higher
  // priority (numerically <=, see ObservationPriority).
  const int limit = static_cast<int>(item.priority);
  std::vector<FrameIDRange> covering;
  const auto note = [&](const FrameIDRange& range) {
    if (range.empty() || range.last < item.frames.first ||
        range.first > item.frames.last) {
      return;  // disjoint
    }
    covering.push_back(range);
  };

  for (int p = 0; p <= limit && p < kPriorityClasses; ++p) {
    for (const auto& queued : queues_[p]) {
      if (queued.node_id != item.node_id ||
          queued.fingerprint != item.fingerprint) {
        continue;
      }
      note(queued.frames);
    }
  }
  for (const auto& worker : workers_) {
    if (!worker->has_in_flight ||
        static_cast<int>(worker->in_flight_priority) > limit ||
        worker->in_flight_node != item.node_id ||
        worker->in_flight_fingerprint != item.fingerprint) {
      continue;
    }
    // The whole in-flight range counts: frames it has already observed are
    // done, and the rest this worker will reach without being re-enqueued.
    note(worker->in_flight_frames);
  }

  if (covering.empty()) {
    return {item.frames};
  }

  std::sort(covering.begin(), covering.end(),
            [](const FrameIDRange& a, const FrameIDRange& b) {
              return a.first < b.first;
            });

  // Walk the sorted cover and emit the gaps left inside the item's range.
  std::vector<FrameIDRange> gaps;
  FrameID cursor = item.frames.first;
  for (const auto& range : covering) {
    if (range.first > cursor) {
      gaps.push_back(FrameIDRange{cursor, range.first - 1});
    }
    if (range.last >= item.frames.last) {
      return gaps;  // covered to the end; no wrap-around on range.last + 1
    }
    cursor = std::max(cursor, range.last + 1);
  }
  gaps.push_back(FrameIDRange{cursor, item.frames.last});
  return gaps;
}

void ObservationScheduler::submit_item_locked(const ObservationWorkItem& item) {
  for (const FrameIDRange& range : uncovered_ranges_locked(item)) {
    ObservationWorkItem part = item;
    part.frames = range;
    for (auto& chunk :
         chunk_item(part, static_cast<unsigned>(workers_.size()))) {
      enqueue_locked(std::move(chunk));
    }
  }
}

void ObservationScheduler::enqueue_locked(ObservationWorkItem item) {
  workload_total_frames_ += item.frames.count();
  const int idx = static_cast<int>(item.priority);
  queues_[idx].push_back(std::move(item));
}

void ObservationScheduler::on_project_loaded(
    const ObservationSchedulingContext& context) {
  if (!policy_) {
    return;
  }
  submit_batch(policy_->plan_sweep(context));
}

void ObservationScheduler::on_preview_moved(
    const ObservationSchedulingContext& context) {
  if (!policy_) {
    return;
  }
  submit_batch(policy_->plan_prefetch(context));
}

void ObservationScheduler::on_invalidation(
    const ObservationSchedulingContext& context) {
  if (!policy_) {
    return;
  }
  submit_batch(policy_->plan_invalidation(context));
}

NodeFingerprint ObservationScheduler::request_interactive(
    NodeID node_id, FrameID frame_id,
    const ObservationSchedulingContext& context) {
  if (!policy_) {
    return {};
  }
  ObservationWorkItem item =
      policy_->plan_interactive(node_id, frame_id, context);
  NodeFingerprint fingerprint = item.fingerprint;
  submit(std::move(item));
  return fingerprint;
}

void ObservationScheduler::on_dag_changed(
    std::shared_ptr<const DAG> dag,
    std::shared_ptr<const NodeFingerprintMap> fingerprints) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fingerprints_ = fingerprints;
    // Record the new DAG and bump the generation; each worker adopts it into
    // its own runner before processing its next item.
    current_dag_ = std::move(dag);
    ++dag_generation_;

    // Abort any worker whose in-flight item's content identity is gone in the
    // new map; workers whose item is still current keep running.
    if (fingerprints_) {
      for (auto& worker : workers_) {
        if (!worker->has_in_flight) {
          continue;
        }
        const auto it = fingerprints_->find(worker->in_flight_node);
        const bool stale = (it == fingerprints_->end()) ||
                           (it->second != worker->in_flight_fingerprint);
        if (stale) {
          worker->cancel.store(true);
        }
      }
    }

    purge_stale_locked();
  }
  cv_.notify_all();
  // A purge shrinks the outstanding workload; publish it, then drop to idle if
  // nothing remains (invalidation that clears the queue resets the aggregate).
  emit_workload();
  reset_workload_if_drained();
}

void ObservationScheduler::purge_stale_locked() {
  if (!fingerprints_) {
    return;
  }
  for (auto& queue : queues_) {
    std::deque<ObservationWorkItem> kept;
    for (auto& item : queue) {
      if (fingerprint_current(item, *fingerprints_)) {
        kept.push_back(std::move(item));
      } else {
        // Purged queued items never ran, so remove their whole frame count from
        // the outstanding total (clamped so it can never underflow).
        const std::uint64_t count = item.frames.count();
        workload_total_frames_ -= std::min(workload_total_frames_, count);
      }
    }
    queue = std::move(kept);
  }
}

bool ObservationScheduler::fingerprint_current(const ObservationWorkItem& item,
                                               const NodeFingerprintMap& map) {
  const auto it = map.find(item.node_id);
  return it != map.end() && it->second == item.fingerprint;
}

void ObservationScheduler::set_progress_callback(ProgressCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  progress_cb_ = std::move(callback);
}

void ObservationScheduler::set_completion_callback(
    CompletionCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  completion_cb_ = std::move(callback);
}

ObservationScheduler::ProgressCallback ObservationScheduler::progress_callback()
    const {
  std::lock_guard<std::mutex> lock(mutex_);
  return progress_cb_;
}

ObservationScheduler::CompletionCallback
ObservationScheduler::completion_callback() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return completion_cb_;
}

void ObservationScheduler::set_workload_callback(WorkloadCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  workload_cb_ = std::move(callback);
}

ObservationScheduler::WorkloadCallback ObservationScheduler::workload_callback()
    const {
  std::lock_guard<std::mutex> lock(mutex_);
  return workload_cb_;
}

ObservationWorkload ObservationScheduler::workload() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return build_workload_locked();
}

ObservationWorkload ObservationScheduler::build_workload_locked() const {
  ObservationWorkload w;
  w.frames_total = workload_total_frames_;
  w.frames_observed =
      std::min(workload_observed_frames_, workload_total_frames_);
  w.frames_computed = workload_computed_frames_;
  w.active = workload_total_frames_ > 0;
  if (workload_total_frames_ > 0) {
    // Rounded percentage, clamped to 100 (observed never exceeds total).
    const std::uint64_t pct =
        (w.frames_observed * 100 + workload_total_frames_ / 2) /
        workload_total_frames_;
    w.percent_complete = static_cast<int>(pct > 100 ? 100 : pct);
  }
  // Distinct nodes with pending work: queued items plus every in-flight item.
  std::unordered_set<NodeID> nodes;
  for (const auto& queue : queues_) {
    for (const auto& item : queue) {
      nodes.insert(item.node_id);
    }
  }
  for (const auto& worker : workers_) {
    if (worker->has_in_flight) {
      nodes.insert(worker->in_flight_node);
    }
  }
  w.outstanding_nodes = nodes.size();
  w.sweep_deferred =
      sweep_paused_ &&
      !queues_[static_cast<int>(ObservationPriority::kSweep)].empty();
  return w;
}

void ObservationScheduler::set_sweep_paused(bool paused) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sweep_paused_ == paused) {
      return;
    }
    sweep_paused_ = paused;
  }
  // Resuming has to wake every worker that parked because the only work left
  // was sweep work.
  cv_.notify_all();
  emit_workload();
}

bool ObservationScheduler::sweep_paused() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sweep_paused_;
}

void ObservationScheduler::emit_workload() {
  WorkloadCallback cb;
  ObservationWorkload snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_ || !workload_cb_) {
      return;
    }
    snapshot = build_workload_locked();
    // Deliver only when a consumer-visible field changed. This is called per
    // observed frame across the whole pool; without the gate a fast sweep
    // (thousands of frames/second) floods the GUI event queue with queued
    // status updates and starves painting. The forwarded view payload is
    // exactly {active, percent, outstanding_nodes, computed-anything,
    // sweep-deferred}, so gating on those fields is lossless for every
    // consumer.
    if (workload_emitted_once_ && snapshot.active == last_workload_.active &&
        snapshot.percent_complete == last_workload_.percent_complete &&
        snapshot.outstanding_nodes == last_workload_.outstanding_nodes &&
        snapshot.sweep_deferred == last_workload_.sweep_deferred &&
        (snapshot.frames_computed > 0) ==
            (last_workload_.frames_computed > 0)) {
      return;
    }
    last_workload_ = snapshot;
    workload_emitted_once_ = true;
    cb = workload_cb_;
  }
  cb(snapshot);
}

bool ObservationScheduler::any_dag_update_pending_locked() const {
  for (const auto& worker : workers_) {
    if (worker->applied_generation < dag_generation_) {
      return true;
    }
  }
  return false;
}

void ObservationScheduler::reset_workload_if_drained() {
  WorkloadCallback cb;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_) {
      return;
    }
    if (in_flight_count_ > 0 || any_dag_update_pending_locked()) {
      return;
    }
    for (const auto& queue : queues_) {
      if (!queue.empty()) {
        return;
      }
    }
    // Nothing outstanding: return to idle. Record the idle snapshot as the
    // last delivery so emit_workload()'s change gate stays consistent.
    workload_total_frames_ = 0;
    workload_observed_frames_ = 0;
    workload_computed_frames_ = 0;
    last_workload_ = ObservationWorkload{};
    workload_emitted_once_ = true;
    cb = workload_cb_;
  }
  if (cb) {
    cb(ObservationWorkload{});
  }
}

std::size_t ObservationScheduler::queued_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t total = 0;
  for (const auto& queue : queues_) {
    total += queue.size();
  }
  return total;
}

ObservationScheduler::NextAction ObservationScheduler::take_next(
    Worker* worker, bool blocking) {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    if (stop_requested_) {
      return {};
    }

    // Bring this worker's runner up to the current DAG generation before it
    // processes any (necessarily current-generation) work item.
    if (worker->applied_generation < dag_generation_) {
      NextAction action;
      action.kind = NextAction::Kind::kDagUpdate;
      action.dag = current_dag_;
      action.fingerprints = fingerprints_;
      action.target_generation = dag_generation_;
      return action;
    }

    for (int priority = 0; priority < kPriorityClasses; ++priority) {
      if (sweep_paused_ &&
          priority == static_cast<int>(ObservationPriority::kSweep)) {
        // Held, not dropped: the item stays at the head of its queue and is
        // taken as soon as the pause lifts.
        continue;
      }
      auto& queue = queues_[priority];
      if (!queue.empty()) {
        NextAction action;
        action.kind = NextAction::Kind::kItem;
        action.item = std::move(queue.front());
        queue.pop_front();

        worker->has_in_flight = true;
        worker->in_flight_node = action.item.node_id;
        worker->in_flight_fingerprint = action.item.fingerprint;
        worker->in_flight_frames = action.item.frames;
        worker->in_flight_priority = action.item.priority;
        worker->cancel.store(false);
        ++in_flight_count_;
        return action;
      }
    }

    if (!blocking) {
      return {};
    }
    cv_.wait(lock);
  }
}

void ObservationScheduler::process_item(Worker* worker,
                                        const ObservationWorkItem& item) {
  const std::uint64_t total = item.frames.count();
  std::uint64_t observed = 0;
  bool cancelled = false;
  bool failed = false;

  const auto item_start = std::chrono::steady_clock::now();
  const ProgressCallback prog_cb = progress_callback();

  if (!item.frames.empty()) {
    for (FrameID frame = item.frames.first;; ++frame) {
      if (worker->cancel.load()) {
        cancelled = true;
        break;
      }
      bool computed = false;
      try {
        computed = worker->runner->observe_frame(item.node_id, item.fingerprint,
                                                 frame, item.observer_ids);
      } catch (...) {
        // A stage/decode error fails the item but never the worker.
        failed = true;
        break;
      }
      ++observed;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++workload_observed_frames_;
        if (computed) {
          ++workload_computed_frames_;
        }
      }
      emit_workload();
      if (prog_cb) {
        prog_cb(ObservationProgress{item.node_id, observed, total});
      }
      if (frame == item.frames.last) {
        break;  // guards against wrap-around when last == UINT64_MAX
      }
    }
  }

  // Timing instrumentation: how fast this item actually processed. Frames a
  // runner short-circuits (already stored / passthrough-copied) still count as
  // observed, so fps here reflects the effective end-to-end rate.
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - item_start)
                              .count();
  if (observed > 0 && elapsed_ms > 0) {
    ORC_LOG_DEBUG(
        "ObservationScheduler: node '{}' frames {}-{}: {}/{} observed in {} ms "
        "({:.0f} frames/s){}{}",
        item.node_id.to_string(), item.frames.first, item.frames.last, observed,
        total, elapsed_ms, observed * 1000.0 / elapsed_ms,
        cancelled ? " [cancelled]" : "", failed ? " [failed]" : "");
  }

  const CompletionCallback comp_cb = completion_callback();
  if (comp_cb) {
    ObservationCompletion completion;
    completion.node_id = item.node_id;
    completion.fingerprint = item.fingerprint;
    completion.priority = item.priority;
    completion.frames_observed = observed;
    completion.frames_total = total;
    completion.succeeded = !failed && !cancelled && observed == total;
    completion.cancelled = cancelled;
    comp_cb(completion);
  }
}

void ObservationScheduler::worker_loop(Worker* worker) {
  for (;;) {
    NextAction action = take_next(worker, /*blocking=*/true);
    switch (action.kind) {
      case NextAction::Kind::kNone:
        return;  // stop requested
      case NextAction::Kind::kDagUpdate:
        worker->runner->update_dag(std::move(action.dag),
                                   std::move(action.fingerprints));
        {
          std::lock_guard<std::mutex> lock(mutex_);
          worker->applied_generation = action.target_generation;
        }
        break;
      case NextAction::Kind::kItem:
        process_item(worker, action.item);
        {
          std::lock_guard<std::mutex> lock(mutex_);
          worker->has_in_flight = false;
          --in_flight_count_;
        }
        reset_workload_if_drained();
        break;
    }
  }
}

bool ObservationScheduler::process_one_for_testing() {
  // The single-threaded test driver always uses the first worker's runner.
  Worker* worker = workers_.front().get();
  NextAction action = take_next(worker, /*blocking=*/false);
  switch (action.kind) {
    case NextAction::Kind::kNone:
      return false;
    case NextAction::Kind::kDagUpdate:
      worker->runner->update_dag(std::move(action.dag),
                                 std::move(action.fingerprints));
      {
        std::lock_guard<std::mutex> lock(mutex_);
        worker->applied_generation = action.target_generation;
      }
      return true;
    case NextAction::Kind::kItem:
      process_item(worker, action.item);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        worker->has_in_flight = false;
        --in_flight_count_;
      }
      reset_workload_if_drained();
      return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// RendererObservationTaskRunner
// ---------------------------------------------------------------------------

namespace {

// The DAG this runner will render against, privately owned.
//
// Every runner in the pool is handed the same DAG, and a DAG's stages are
// stateful: several re-apply the parameter map from execute(), so two workers
// inside one stage at once is a data race — one that has been observed to
// corrupt a stage's parsed configuration mid-execute. Each runner therefore
// clones the DAG and owns the stages behind it; the store they all write into
// stays the single point of contact between workers.
//
// Falls back to the shared DAG if the clone fails (an unregistered stage), so
// observation still runs where it used to. Logged loudly because that path is
// the racy one.
std::shared_ptr<const DAG> private_dag(std::shared_ptr<const DAG> dag) {
  if (!dag) {
    return dag;
  }
  if (auto clone = clone_dag_with_fresh_stages(*dag)) {
    return clone;
  }
  ORC_LOG_WARN(
      "ObservationTaskRunner: could not clone the DAG; sharing stage instances "
      "with the other workers, which is not thread-safe");
  return dag;
}

}  // namespace

RendererObservationTaskRunner::RendererObservationTaskRunner(
    std::shared_ptr<const DAG> dag,
    std::shared_ptr<const NodeFingerprintMap> fingerprints,
    std::shared_ptr<ObservationStore> store,
    std::shared_ptr<IObservationService> service)
    : store_(std::move(store)), service_(std::move(service)) {
  renderer_ = std::make_unique<DAGFrameRenderer>(private_dag(std::move(dag)));
  if (service_) {
    renderer_->set_observation_service(service_);
  }
  renderer_->set_observation_store(store_, std::move(fingerprints));

  // Cache the standard observer set (id + version) so observe_frame() can
  // pre-check the store before rendering. Use the injected service when
  // present, otherwise the host CoreObservationService (same inventory the
  // renderer's observer pass runs).
  const IObservationService* svc = service_.get();
  CoreObservationService fallback;
  if (svc == nullptr) {
    svc = &fallback;
  }
  for (const auto& info : svc->available_observers()) {
    observer_keys_.emplace_back(info.id, info.version);
  }
}

RendererObservationTaskRunner::~RendererObservationTaskRunner() = default;

bool RendererObservationTaskRunner::observe_frame(
    NodeID node_id, const NodeFingerprint& fingerprint, FrameID frame_id,
    const std::vector<std::string>& /*observer_ids*/) {
  // Fast path: if every applicable observer's records for both fields of this
  // frame are already stored, there is nothing to compute — skip the expensive
  // render. Observation is per-frame independent (a fresh observer runs each
  // frame), so a store hit is authoritative regardless of which chunk/worker
  // produced it. has_stored() is presence-only: probing a warm sweep must not
  // drag every record through the sidecar into the memory LRU just to prove it
  // exists. The key set is filtered to the node's video system because the
  // renderer's observer pass writes no records for inapplicable observers
  // (standard_observer_applies) — demanding those keys here would defeat the
  // fast path on every frame.
  if (store_ && !fingerprint.value.empty() && !observer_keys_.empty()) {
    const FieldID field_top(frame_id * 2);
    const FieldID field_bottom(frame_id * 2 + 1);
    bool all_present = true;
    for (const auto& [id, version] : observer_keys_for(node_id)) {
      if (!store_->has_stored({fingerprint, field_top, id, version}) ||
          !store_->has_stored({fingerprint, field_bottom, id, version})) {
        all_present = false;
        break;
      }
    }
    if (all_present) {
      return false;  // already covered — nothing computed
    }
  }

  // The renderer's store-backed observer pass writes every standard observer's
  // records for this frame, keyed by the node fingerprint the renderer holds
  // (kept in sync via update_dag). observer_ids is advisory — see the header.
  const FrameRenderResult result =
      renderer_->render_frame_at_node(node_id, frame_id);
  if (!result.is_valid) {
    // Surface as a work-item failure so the scheduler reports it and moves on.
    throw DAGFrameRenderError(result.error_message);
  }
  return true;
}

const std::vector<std::pair<std::string, std::string>>&
RendererObservationTaskRunner::observer_keys_for(NodeID node_id) {
  const auto it = node_observer_keys_.find(node_id);
  if (it != node_observer_keys_.end()) {
    return it->second;
  }
  auto keys = observer_keys_;
  // Applicability fails open: without video parameters the full set is kept,
  // matching the observer pass's own fallback.
  if (const auto params = renderer_->get_video_parameters_at_node(node_id)) {
    keys.erase(std::remove_if(keys.begin(), keys.end(),
                              [&params](const auto& key) {
                                return !standard_observer_applies(key.first,
                                                                  *params);
                              }),
               keys.end());
  }
  return node_observer_keys_.emplace(node_id, std::move(keys)).first->second;
}

void RendererObservationTaskRunner::update_dag(
    std::shared_ptr<const DAG> dag,
    std::shared_ptr<const NodeFingerprintMap> fingerprints) {
  renderer_->update_dag(private_dag(std::move(dag)));
  renderer_->set_observation_store(store_, std::move(fingerprints));
  node_observer_keys_.clear();
}

// ---------------------------------------------------------------------------
// DefaultObservationSchedulingPolicy
// ---------------------------------------------------------------------------

DefaultObservationSchedulingPolicy::DefaultObservationSchedulingPolicy(
    std::vector<std::string> observer_ids,
    std::vector<std::string> stateful_ids, FrameID prefetch_radius)
    : observer_ids_(std::move(observer_ids)),
      prefetch_radius_(prefetch_radius) {
  // stateful_ids is accepted for the ObserverInfo contract but no longer
  // splits the plan: the runner observes the full set per frame with a fresh
  // observer instance, so processing order cannot affect stored records and a
  // second ascending-order item over the same range would only duplicate work.
  (void)stateful_ids;
}

NodeFingerprint DefaultObservationSchedulingPolicy::fingerprint_of(
    NodeID node_id, const ObservationSchedulingContext& context) {
  if (!context.fingerprints) {
    return {};
  }
  const auto it = context.fingerprints->find(node_id);
  return it != context.fingerprints->end() ? it->second : NodeFingerprint{};
}

void DefaultObservationSchedulingPolicy::emit_node_items(
    NodeID node_id, const NodeFingerprint& fingerprint, FrameIDRange frames,
    ObservationPriority priority, std::vector<ObservationWorkItem>& out) const {
  if (frames.empty() || observer_ids_.empty()) {
    return;
  }
  // One item, full observer set, chunkable (stateful = false): see the class
  // comment for why a separate ascending-order stateful item is not emitted.
  ObservationWorkItem item;
  item.node_id = node_id;
  item.fingerprint = fingerprint;
  item.frames = frames;
  item.observer_ids = observer_ids_;
  item.stateful = false;
  item.priority = priority;
  out.push_back(std::move(item));
}

std::vector<ObservationWorkItem> DefaultObservationSchedulingPolicy::plan_sweep(
    const ObservationSchedulingContext& context) {
  std::vector<ObservationWorkItem> out;
  if (context.total_frames == 0 || context.total_frames > kMaxWholeNodeFrames) {
    return out;
  }
  const FrameIDRange whole{0, context.total_frames - 1};
  for (const NodeID node : context.nodes_of_interest) {
    emit_node_items(node, fingerprint_of(node, context), whole,
                    ObservationPriority::kSweep, out);
  }
  return out;
}

std::vector<ObservationWorkItem>
DefaultObservationSchedulingPolicy::plan_prefetch(
    const ObservationSchedulingContext& context) {
  std::vector<ObservationWorkItem> out;
  const FrameID centre = context.preview_position;
  const FrameID first =
      centre > prefetch_radius_ ? centre - prefetch_radius_ : 0;
  FrameID last = centre + prefetch_radius_;
  if (context.total_frames > 0) {
    last = std::min<FrameID>(last, context.total_frames - 1);
  }
  const FrameIDRange window{first, last};
  for (const NodeID node : context.nodes_of_interest) {
    const NodeFingerprint fingerprint = fingerprint_of(node, context);
    if (!context.frame_observed) {
      emit_node_items(node, fingerprint, window, ObservationPriority::kPrefetch,
                      out);
      continue;
    }
    // Coverage-filtered prefetch: enqueue only the contiguous runs of frames
    // that are not already fully stored, so revisiting an observed position
    // enqueues nothing (and the GUI reports no phantom work). The window is
    // small (2 * radius + 1 frames), so probing per frame is cheap.
    bool in_run = false;
    FrameID run_start = window.first;
    for (FrameID frame = window.first;; ++frame) {
      const bool needed = !context.frame_observed(node, frame);
      if (needed && !in_run) {
        in_run = true;
        run_start = frame;
      } else if (!needed && in_run) {
        in_run = false;
        emit_node_items(node, fingerprint, FrameIDRange{run_start, frame - 1},
                        ObservationPriority::kPrefetch, out);
      }
      if (frame == window.last) {
        break;  // guards against wrap-around when last == UINT64_MAX
      }
    }
    if (in_run) {
      emit_node_items(node, fingerprint, FrameIDRange{run_start, window.last},
                      ObservationPriority::kPrefetch, out);
    }
  }
  return out;
}

std::vector<ObservationWorkItem>
DefaultObservationSchedulingPolicy::plan_invalidation(
    const ObservationSchedulingContext& context) {
  std::vector<ObservationWorkItem> out;
  // Re-observing a changed node covers its whole range, so it is bounded by
  // the same limit as the sweep that first observed it.
  if (context.total_frames == 0 || context.total_frames > kMaxWholeNodeFrames) {
    return out;
  }
  const FrameIDRange whole{0, context.total_frames - 1};
  for (const NodeID node : context.changed_nodes) {
    emit_node_items(node, fingerprint_of(node, context), whole,
                    ObservationPriority::kSweep, out);
  }
  return out;
}

ObservationWorkItem DefaultObservationSchedulingPolicy::plan_interactive(
    NodeID node_id, FrameID frame_id,
    const ObservationSchedulingContext& context) {
  ObservationWorkItem item;
  item.node_id = node_id;
  item.fingerprint = fingerprint_of(node_id, context);
  item.frames = FrameIDRange{frame_id, frame_id};
  item.observer_ids = observer_ids_;
  item.stateful = false;  // single frame — ordering is irrelevant
  item.priority = ObservationPriority::kInteractive;
  return item;
}

}  // namespace orc
