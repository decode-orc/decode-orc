/*
 * File:        observation_scheduler.h
 * Module:      orc-core
 * Purpose:     Background worker that computes missing observations ahead of
 *              demand, priority-ordered and cancellable
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// Host-internal background scheduler. Only orc-core and orc-presenters may
// include this header; GUI/CLI code must go through presenters.
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/observation_scheduler.h. Use a presenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/observation_scheduler.h. Use a presenter instead."
#endif

#include <orc/stage/frame_id.h>
#include <orc/stage/node_id.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "frame_provenance.h"

namespace orc {

class DAG;                  // dag_executor.h
class DAGFrameRenderer;     // dag_frame_renderer.h
class ObservationStore;     // observation_store.h
class IObservationService;  // observation_service_interface.h

/**
 * @brief Priority classes ordering background observation work.
 *
 * Lower numeric value = higher priority. Interactive requests (a frame a user
 * is looking at right now) preempt prefetch (frames near the preview position)
 * which preempts the background sweep of whole nodes. Within one class, work is
 * processed in submission order (FIFO).
 */
enum class ObservationPriority : int {
  kInteractive = 0,
  kPrefetch = 1,
  kSweep = 2,
};

/**
 * @brief One unit of background observation work.
 *
 * Describes a contiguous frame range to observe at a single DAG node with a
 * given observer set. The @ref fingerprint is captured from the node's
 * provenance at enqueue time so the scheduler can drop the item if a later DAG
 * change makes it stale (the node's content identity changed).
 */
struct ObservationWorkItem {
  NodeID node_id;                         ///< Node to render/observe at.
  NodeFingerprint fingerprint;            ///< Node provenance at enqueue time.
  FrameIDRange frames;                    ///< Inclusive frame range to cover.
  std::vector<std::string> observer_ids;  ///< Observer set (advisory; see
                                          ///< IObservationTaskRunner).
  /// True when the observer set includes a cross-frame (stateful) observer, so
  /// the scheduler must observe this item's frames strictly in ascending order.
  bool stateful = false;
  ObservationPriority priority = ObservationPriority::kSweep;
};

/**
 * @brief Per-node progress emitted while a work item is processed.
 *
 * @c frames_observed is cumulative within a single work item and increases
 * monotonically; @c frames_total is that item's frame count.
 */
struct ObservationProgress {
  NodeID node_id;
  std::uint64_t frames_observed = 0;
  std::uint64_t frames_total = 0;
};

/**
 * @brief Terminal report emitted exactly once per dequeued work item.
 *
 * Fires whether the item completed, failed, or was cancelled mid-flight. Items
 * that are dropped from the queue before ever being dequeued (stale-fingerprint
 * purge, or shutdown) never emit a completion.
 */
struct ObservationCompletion {
  NodeID node_id;
  NodeFingerprint fingerprint;
  ObservationPriority priority = ObservationPriority::kSweep;
  std::uint64_t frames_observed = 0;
  std::uint64_t frames_total = 0;
  bool succeeded = false;  ///< True iff every frame was observed without error.
  bool cancelled = false;  ///< True iff a DAG change / shutdown aborted it.
};

/**
 * @brief Snapshot of the scheduler's current outstanding observation workload.
 *
 * Reflects the *current* outstanding work, not a monotonic session total:
 * newly enqueued work raises @c frames_total (which lowers
 * @c percent_complete), and the snapshot returns to idle (@c active == false,
 * every counter zero) the moment the queue drains. @c percent_complete is
 * always clamped to [0, 100]. @c frames_observed never exceeds
 * @c frames_total.
 */
struct ObservationWorkload {
  bool active = false;       ///< True while any work is outstanding.
  int percent_complete = 0;  ///< frames_observed / frames_total, 0..100.
  std::uint64_t frames_observed = 0;  ///< Frames observed in the current batch.
  std::uint64_t frames_total = 0;     ///< Frames enqueued in the current batch.
  /// Frames the runner actually computed (rendered) in the current batch, as
  /// opposed to frames it skipped because their records were already stored.
  /// Lets a consumer distinguish real computation ("Computing…") from a
  /// coverage check over warm data ("Checking…").
  std::uint64_t frames_computed = 0;
  std::size_t outstanding_nodes = 0;  ///< Distinct nodes with pending work.
  /// True while whole-node sweep work is queued but held back by
  /// set_sweep_paused(). The percentage keeps counting down as interactive and
  /// prefetch work continues; this says the bulk of what remains is waiting.
  bool sweep_deferred = false;
};

/**
 * @brief Performs the render + observe work for one frame at one node.
 *
 * This is the seam that lets the scheduler run against a mocked backend in unit
 * tests. The production implementation (RendererObservationTaskRunner) owns a
 * DAGFrameRenderer and writes observed values into a shared ObservationStore.
 *
 * Threading: every method is invoked only on the scheduler's single worker
 * thread, honouring DAGFrameRenderer's single-thread contract; implementations
 * need no internal synchronisation. observe_frame() may throw — the scheduler
 * catches at the work-item boundary and marks the item failed, so a stage or
 * decode error never terminates the process.
 */
class IObservationTaskRunner {
 public:
  virtual ~IObservationTaskRunner() = default;

  /**
   * @brief Observe @p frame_id at @p node_id, keyed by @p fingerprint.
   *
   * @param node_id      Node to render and observe.
   * @param fingerprint  Provenance used to key stored records.
   * @param frame_id     Frame to observe.
   * @param observer_ids Requested observer set. Advisory: an implementation
   *                     may observe the full standard set (less observers
   *                     inapplicable to the node's video system, per
   *                     standard_observer_applies) and rely on the store's
   *                     read-through to skip already-present records.
   * @return True when the frame was actually computed (rendered/observed);
   *         false when every requested record was already stored and the call
   *         was a no-op. Drives the workload's computed-vs-checked split so
   *         the GUI can distinguish real computation from coverage checks.
   */
  virtual bool observe_frame(NodeID node_id, const NodeFingerprint& fingerprint,
                             FrameID frame_id,
                             const std::vector<std::string>& observer_ids) = 0;

  /**
   * @brief Adopt a new DAG and fingerprint map (called on the worker thread).
   *
   * Invoked before any work item for the new DAG is processed, so the runner's
   * renderer is always consistent with the fingerprints keying the store.
   */
  virtual void update_dag(
      std::shared_ptr<const DAG> dag,
      std::shared_ptr<const NodeFingerprintMap> fingerprints) = 0;
};

/**
 * @brief Inputs a scheduling policy uses to decide what to enqueue.
 *
 * A snapshot assembled by the caller (presenter) at the moment of an event.
 */
struct ObservationSchedulingContext {
  /// Current fingerprint map; a policy looks up each node's fingerprint here.
  std::shared_ptr<const NodeFingerprintMap> fingerprints;
  /// Preview position (frame) the prefetch window is centred on.
  FrameID preview_position = 0;
  /// Total frame count of the source (for clamping ranges); 0 if unknown.
  std::uint64_t total_frames = 0;
  /// Nodes worth observing: the previewed node plus any with open dialogs.
  std::vector<NodeID> nodes_of_interest;
  /// Nodes whose fingerprint just changed (from an ObservationInvalidation).
  std::vector<NodeID> changed_nodes;
  /// Optional coverage probe: true when every observer record for the frame at
  /// the node is already stored, so a policy can avoid enqueueing work that
  /// would be an immediate no-op (and would still flash progress at the user).
  /// Only consulted for SMALL plans (the prefetch window); whole-node sweeps
  /// must not probe per-frame — on a cold store that is a sidecar query per
  /// frame at enqueue time. Null disables filtering.
  std::function<bool(NodeID, FrameID)> frame_observed;
};

/**
 * @brief Decides which work items to enqueue for each scheduling event.
 *
 * Injected so the enqueue strategy is testable in isolation and swappable. The
 * scheduler owns no policy of its own; the caller drives the on_* event methods
 * with a context snapshot.
 *
 * Threading: called on the caller's thread (not the worker). Implementations
 * must be pure with respect to the passed context (no hidden shared state).
 */
class IObservationSchedulingPolicy {
 public:
  virtual ~IObservationSchedulingPolicy() = default;

  /// Full background sweep, e.g. on project load. Lowest priority.
  virtual std::vector<ObservationWorkItem> plan_sweep(
      const ObservationSchedulingContext& context) = 0;

  /// Prefetch window around the current preview position. Medium priority.
  virtual std::vector<ObservationWorkItem> plan_prefetch(
      const ObservationSchedulingContext& context) = 0;

  /// Re-enqueue only the changed nodes' frames after an invalidation.
  virtual std::vector<ObservationWorkItem> plan_invalidation(
      const ObservationSchedulingContext& context) = 0;

  /// The single interactively-requested frame. Highest priority.
  virtual ObservationWorkItem plan_interactive(
      NodeID node_id, FrameID frame_id,
      const ObservationSchedulingContext& context) = 0;
};

/**
 * @brief Background observation scheduler.
 *
 * Owns one worker thread that drains a priority-ordered queue of work items,
 * driving the injected IObservationTaskRunner one frame at a time. Interactive
 * work preempts prefetch preempts sweep; within a class, FIFO. Stateful items
 * are observed in ascending frame order.
 *
 * Enqueued work is deduplicated against what is already queued or in flight for
 * the same node + fingerprint, so overlapping plans (a prefetch window
 * re-planned on every preview move) cannot make several workers render the same
 * frame.
 *
 * DAG changes: on_dag_changed() atomically adopts the new fingerprint map,
 * drops every queued item whose node fingerprint no longer matches, and aborts
 * the in-flight item if it became stale. Requeuing new work against the new map
 * is the policy's job (via on_invalidation()).
 *
 * Progress/completion callbacks fire on the worker thread; a presenter marshals
 * them to the UI thread. No callback fires after stop() has joined the worker.
 *
 * Thread safety (coding standards §5.3.3): submit*, on_dag_changed, the on_*
 * policy events, callback setters, start and stop are all safe to call from any
 * thread. Internal state is guarded by a mutex; the runner and callbacks run
 * only on the worker thread.
 */
class ObservationScheduler {
 public:
  using ProgressCallback = std::function<void(const ObservationProgress&)>;
  using CompletionCallback = std::function<void(const ObservationCompletion&)>;
  using WorkloadCallback = std::function<void(const ObservationWorkload&)>;

  /// Creates one task runner (renderer/executor/observer handles) for a worker
  /// thread. Called once per worker at construction; each returned runner is
  /// used only on its own thread, so implementations need not be thread-safe.
  using TaskRunnerFactory =
      std::function<std::unique_ptr<IObservationTaskRunner>()>;

  /**
   * @param runner        Backend that renders + observes frames (required).
   * @param fingerprints  Initial fingerprint map used for staleness checks.
   * @param policy        Optional enqueue policy driving the on_* events.
   *
   * Single-worker constructor: one runner, one worker thread. Retained for
   * tests and callers that do not parallelise.
   */
  ObservationScheduler(
      std::unique_ptr<IObservationTaskRunner> runner,
      std::shared_ptr<const NodeFingerprintMap> fingerprints,
      std::shared_ptr<IObservationSchedulingPolicy> policy = nullptr);

  /**
   * @brief Multi-worker constructor: a pool of @p worker_count threads, each
   *        with its own runner built by @p runner_factory.
   *
   * Stateless work items are split across the pool; stateful items stay whole
   * on a single worker (their ascending-frame order is preserved). The shared
   * ObservationStore each runner writes to is the sole thread-safe handoff
   * point. @p worker_count is clamped to at least 1.
   *
   * @param runner_factory Builds one runner per worker (called worker_count
   *                       times, on the constructing thread).
   * @param worker_count   Number of worker threads (>= 1).
   * @param fingerprints   Initial fingerprint map used for staleness checks.
   * @param policy         Optional enqueue policy driving the on_* events.
   */
  ObservationScheduler(
      TaskRunnerFactory runner_factory, unsigned worker_count,
      std::shared_ptr<const NodeFingerprintMap> fingerprints,
      std::shared_ptr<IObservationSchedulingPolicy> policy = nullptr);

  ~ObservationScheduler();

  ObservationScheduler(const ObservationScheduler&) = delete;
  ObservationScheduler& operator=(const ObservationScheduler&) = delete;
  ObservationScheduler(ObservationScheduler&&) = delete;
  ObservationScheduler& operator=(ObservationScheduler&&) = delete;

  /// Launch the worker thread. Idempotent; a no-op once running.
  void start();

  /// Signal shutdown, abort in-flight work, and join the worker. Idempotent;
  /// safe to call from the destructor. No callback fires after it returns.
  void stop();

  /// True while the worker threads are running.
  bool is_running() const;

  /// Number of worker threads in the pool (>= 1).
  std::size_t worker_count() const { return workers_.size(); }

  // ---- Direct submission ---------------------------------------------------

  /// Enqueue one work item. Ignored (dropped) if the scheduler is stopping.
  /// Frames already queued or in flight for the same node + fingerprint at an
  /// equal or higher priority are dropped from the item first; an item every
  /// frame of which is already pending enqueues nothing.
  void submit(ObservationWorkItem item);

  /// Enqueue a batch of work items in order, each deduplicated as in submit().
  void submit_batch(std::vector<ObservationWorkItem> items);

  // ---- Sweep throttle ------------------------------------------------------

  /**
   * @brief Hold back whole-node sweep work without discarding it.
   *
   * A sweep saturates the pool for as long as a node has unobserved frames,
   * which is exactly the wrong thing to be doing while a preview is playing:
   * the frames the user is watching are rendered on a different thread that
   * then has to compete with N/2 sweep workers for cores. Pausing keeps the
   * queued sweep intact and stops it being dequeued; interactive and prefetch
   * work is unaffected, so the frame in front of the user still gets its
   * observations.
   *
   * A sweep item already in flight runs to completion - the pause gates the
   * dequeue, not the work. Idempotent, and safe from any thread.
   */
  void set_sweep_paused(bool paused);

  /// True while set_sweep_paused(true) is in force.
  bool sweep_paused() const;

  // ---- Policy-driven events ------------------------------------------------

  /// Project loaded: enqueue the policy's sweep plan.
  void on_project_loaded(const ObservationSchedulingContext& context);

  /// Preview moved: enqueue the policy's prefetch plan.
  void on_preview_moved(const ObservationSchedulingContext& context);

  /// Invalidation: enqueue the policy's re-observation plan for changed nodes.
  void on_invalidation(const ObservationSchedulingContext& context);

  /// Interactive request: enqueue the policy's single-frame plan at top
  /// priority. Returns the fingerprint the item was keyed with (empty if no
  /// policy is set, in which case nothing is enqueued).
  NodeFingerprint request_interactive(
      NodeID node_id, FrameID frame_id,
      const ObservationSchedulingContext& context);

  // ---- DAG change ----------------------------------------------------------

  /**
   * @brief Adopt a new DAG / fingerprint map and drop stale work.
   *
   * Queued items whose node fingerprint no longer matches @p fingerprints are
   * removed; the in-flight item is aborted if stale. The runner adopts the new
   * DAG on the worker thread before the next item runs.
   */
  void on_dag_changed(std::shared_ptr<const DAG> dag,
                      std::shared_ptr<const NodeFingerprintMap> fingerprints);

  // ---- Callbacks -----------------------------------------------------------

  void set_progress_callback(ProgressCallback callback);
  void set_completion_callback(CompletionCallback callback);

  /**
   * @brief Subscribe to overall workload snapshots (Task 5.4).
   *
   * Fires whenever the outstanding workload changes — a batch is enqueued, a
   * frame is observed, stale work is purged, or the queue drains to idle.
   * Invoked on whichever thread drove the change (worker thread for observe /
   * drain, caller thread for enqueue / purge); a presenter marshals to the UI
   * thread. Like the progress/completion callbacks, no workload callback fires
   * after stop() has joined the worker.
   */
  void set_workload_callback(WorkloadCallback callback);

  /// Current workload snapshot (introspection / testing).
  ObservationWorkload workload() const;

  // ---- Introspection / testing --------------------------------------------

  /// Total number of items currently queued across all priority classes.
  std::size_t queued_count() const;

  /**
   * @brief Process at most one pending action synchronously, for tests.
   *
   * Must only be used when the worker thread is NOT running (i.e. start() was
   * not called). Applies a pending DAG update or processes the next work item
   * exactly as the worker loop would, then returns. Returns true if it did
   * work, false if there was nothing pending.
   */
  bool process_one_for_testing();

 private:
  static constexpr int kPriorityClasses = 3;

  // One background worker: its own task runner (renderer/executor/handles), its
  // thread, a per-item cancel flag polled inside long observe loops, and
  // in-flight bookkeeping for staleness aborts. `runner` is touched only on
  // this worker's own thread; the remaining fields are guarded by mutex_ except
  // `cancel`, which is atomic. Held by unique_ptr because atomic/thread members
  // make Worker non-movable.
  struct Worker {
    std::unique_ptr<IObservationTaskRunner> runner;
    std::thread thread;
    std::atomic<bool> cancel{false};
    bool has_in_flight = false;
    NodeID in_flight_node;
    NodeFingerprint in_flight_fingerprint;
    // Frame range and priority of the in-flight item, so enqueue-time
    // deduplication can see work a worker has already taken off the queue.
    FrameIDRange in_flight_frames;
    ObservationPriority in_flight_priority = ObservationPriority::kSweep;
    // DAG generation this worker's runner has adopted; lags dag_generation_
    // until the worker applies the pending update before its next item.
    std::uint64_t applied_generation = 0;
  };

  // A control action taken by a worker (or the test driver): either a DAG
  // update to forward to that worker's runner, or a work item to process.
  struct NextAction {
    enum class Kind { kNone, kDagUpdate, kItem } kind = Kind::kNone;
    ObservationWorkItem item;
    std::shared_ptr<const DAG> dag;
    std::shared_ptr<const NodeFingerprintMap> fingerprints;
    std::uint64_t target_generation = 0;
  };

  // True if @p item's fingerprint still matches the node in @p map.
  static bool fingerprint_current(const ObservationWorkItem& item,
                                  const NodeFingerprintMap& map);

  void worker_loop(Worker* worker);

  // Pop the next action for @p worker under the lock. When @p blocking, waits
  // until there is work, a DAG update, or a stop request; returns kNone only on
  // stop. When not blocking, returns kNone immediately if nothing is pending.
  NextAction take_next(Worker* worker, bool blocking);

  // Execute one dequeued work item on @p worker: observe its frames in order,
  // honouring cancellation, and emit progress + a single completion. Never
  // throws.
  void process_item(Worker* worker, const ObservationWorkItem& item);

  // True (caller holds mutex_) if any worker's runner has not yet adopted the
  // current DAG generation.
  bool any_dag_update_pending_locked() const;

  void enqueue_locked(ObservationWorkItem item);
  void purge_stale_locked();

  /**
   * @brief Sub-ranges of @p item's frames that no pending work already covers.
   *
   * Overlapping plans are the norm: a playing preview re-plans its prefetch
   * window on every frame advance, so the same not-yet-observed frames would
   * otherwise be enqueued again on each move and rendered several times over by
   * different workers. Coverage is counted from queued items and from items a
   * worker has already taken in flight, matched on node + fingerprint (a
   * different fingerprint is different content, never a duplicate).
   *
   * Only work at an equal or higher priority suppresses: a queued sweep must
   * never swallow a prefetch or interactive request for the same frame, or that
   * frame would inherit the sweep's latency.
   *
   * Returns the whole range when nothing overlaps, and an empty vector when the
   * item is fully covered. Caller holds mutex_.
   */
  std::vector<FrameIDRange> uncovered_ranges_locked(
      const ObservationWorkItem& item) const;

  // Drop frames already pending, then chunk and enqueue what remains. Caller
  // holds mutex_.
  void submit_item_locked(const ObservationWorkItem& item);

  ProgressCallback progress_callback() const;
  CompletionCallback completion_callback() const;
  WorkloadCallback workload_callback() const;

  // Build a workload snapshot from the current counters and queue state. Caller
  // holds mutex_.
  ObservationWorkload build_workload_locked() const;

  // Compute a snapshot under the lock and deliver it to the workload callback
  // outside the lock (so a callback may re-enter the scheduler safely). A no-op
  // when no callback is set or a stop has been requested.
  void emit_workload();

  // If no work remains queued or in flight, zero the workload counters and emit
  // a single idle snapshot. Called after an item finishes and after a purge.
  void reset_workload_if_drained();

  // The worker pool (>= 1). Each worker owns a runner and, once started, a
  // thread. Populated at construction; threads are spawned in start().
  std::vector<std::unique_ptr<Worker>> workers_;
  std::shared_ptr<IObservationSchedulingPolicy> policy_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;

  // FIFO queue per priority class (index == ObservationPriority value).
  std::deque<ObservationWorkItem> queues_[kPriorityClasses];

  std::shared_ptr<const NodeFingerprintMap> fingerprints_;

  // Latest DAG to adopt and its monotonic generation. on_dag_changed() bumps
  // the generation and records the DAG; each worker lazily brings its runner up
  // to the current generation before its next item (a per-worker barrier).
  std::uint64_t dag_generation_ = 0;
  std::shared_ptr<const DAG> current_dag_;

  // Number of items currently being processed across all workers (guarded by
  // mutex_); the workload returns to idle only when this reaches zero.
  int in_flight_count_ = 0;

  bool stop_requested_ = false;
  bool running_ = false;

  // While set, take_next() skips the sweep queue. Guarded by mutex_ so a
  // worker blocked in cv_.wait() sees the change when it is notified.
  bool sweep_paused_ = false;

  ProgressCallback progress_cb_;
  CompletionCallback completion_cb_;
  WorkloadCallback workload_cb_;

  // Outstanding-workload counters for the current batch (guarded by mutex_).
  // frames_total grows on enqueue and shrinks only when never-started queued
  // items are purged; frames_observed grows per observed frame (computed or
  // skipped-as-stored), and frames_computed only for frames the runner really
  // rendered. All reset to zero when the queue drains, so the counters always
  // reflect the *current* outstanding workload and frames_observed never
  // exceeds frames_total.
  std::uint64_t workload_total_frames_ = 0;
  std::uint64_t workload_observed_frames_ = 0;
  std::uint64_t workload_computed_frames_ = 0;

  // Last snapshot delivered to the workload callback (guarded by mutex_).
  // emit_workload() suppresses deliveries whose consumer-visible fields
  // (active / percent / outstanding nodes) are unchanged, so per-frame calls
  // from a fast sweep cannot flood the subscriber (typically a GUI queue).
  ObservationWorkload last_workload_;
  bool workload_emitted_once_ = false;
};

/**
 * @brief Production IObservationTaskRunner backed by a DAGFrameRenderer.
 *
 * Renders each requested frame at its node; the renderer's built-in
 * store-backed observer pass writes every standard observer's output for that
 * frame into the shared ObservationStore (read-through skips already-present
 * records). The observer_ids on a work item are therefore advisory — coverage
 * is the full standard set.
 *
 * Threading: not thread-safe; used only on the scheduler's worker thread.
 */
class RendererObservationTaskRunner final : public IObservationTaskRunner {
 public:
  /**
   * @param dag           Initial DAG to render against.
   * @param fingerprints  Fingerprint map matching @p dag (keys the store).
   * @param store         Shared, provenance-keyed observation store.
   * @param service       Optional observation service override (nullptr uses
   *                      the host CoreObservationService).
   */
  RendererObservationTaskRunner(
      std::shared_ptr<const DAG> dag,
      std::shared_ptr<const NodeFingerprintMap> fingerprints,
      std::shared_ptr<ObservationStore> store,
      std::shared_ptr<IObservationService> service = nullptr);

  ~RendererObservationTaskRunner() override;

  bool observe_frame(NodeID node_id, const NodeFingerprint& fingerprint,
                     FrameID frame_id,
                     const std::vector<std::string>& observer_ids) override;

  void update_dag(
      std::shared_ptr<const DAG> dag,
      std::shared_ptr<const NodeFingerprintMap> fingerprints) override;

 private:
  std::shared_ptr<ObservationStore> store_;
  std::shared_ptr<IObservationService> service_;
  std::unique_ptr<DAGFrameRenderer> renderer_;
  // (id, version) of every standard observer, cached so observe_frame() can
  // pre-check the store and skip rendering a frame whose observations are all
  // already present (e.g. computed by a parallel chunk covering the same node).
  std::vector<std::pair<std::string, std::string>> observer_keys_;
  // Per-node subsets of observer_keys_ applicable to the node's video system
  // (standard_observer_applies), computed on first use — the observer pass
  // writes no records for inapplicable observers, so the fast path must not
  // demand their keys. Single worker thread by contract (no locking); cleared
  // on update_dag().
  std::unordered_map<NodeID, std::vector<std::pair<std::string, std::string>>>
      node_observer_keys_;

  // Cached lookup into node_observer_keys_, filtering on first use.
  const std::vector<std::pair<std::string, std::string>>& observer_keys_for(
      NodeID node_id);
};

/**
 * @brief Default scheduling policy: whole-node sweeps, a prefetch window, and
 *        changed-node re-observation.
 *
 * Emits ONE work item per (node, range) covering the full observer set. The
 * production runner observes the complete standard set applicable to the
 * node's video system per frame regardless of a work item's observer_ids
 * (each observer is constructed fresh per frame, so
 * stored records never depend on processing order); a separate ascending-order
 * item for the stateful observers would double the enqueued workload and race
 * the chunked items into rendering the same frames twice.
 *
 * Prefetch plans consult the context's frame_observed probe (when set) and
 * skip frames that are already fully stored, so revisiting a stage does not
 * enqueue — or report — work that would be a no-op.
 *
 * Thread safety: immutable after construction; safe to share across threads.
 */
class DefaultObservationSchedulingPolicy final
    : public IObservationSchedulingPolicy {
 public:
  /// Default half-width (frames) of the prefetch window around the preview.
  static constexpr FrameID kDefaultPrefetchRadius = 24;

  /**
   * Longest source a whole-node plan (sweep or invalidation) is emitted for.
   *
   * A whole-node plan is unbounded background work: every frame of the node,
   * through the full observer set. That is a fair trade for a disc — a
   * LaserDisc side runs to some 80 000 frames, which the pool clears while the
   * user is still looking at the first one. It is not a fair trade for a tape.
   * A VBI capture of an E180 holds around 270 000 PAL frames, and each one is
   * synthesised from its line records before an observer can look at it, so
   * the plan runs for tens of hours and starves the interactive render for its
   * whole duration.
   *
   * Beyond this, whole-node plans are empty and the node is left to the
   * prefetch window: observations accumulate around wherever the user actually
   * scrubs, at the cost of not having the whole source ready up front. The
   * limit sits above every disc-length source, so nothing that swept before
   * stops sweeping.
   */
  static constexpr FrameID kMaxWholeNodeFrames = 100000;

  /**
   * @param observer_ids   Stable ids of every standard observer.
   * @param stateful_ids   Subset of @p observer_ids that are stateful. Retained
   *                       for the ObserverInfo contract, but no longer drives a
   *                       separate ascending-order item (see class comment).
   * @param prefetch_radius Half-width of the prefetch window in frames.
   */
  DefaultObservationSchedulingPolicy(
      std::vector<std::string> observer_ids,
      std::vector<std::string> stateful_ids,
      FrameID prefetch_radius = kDefaultPrefetchRadius);

  std::vector<ObservationWorkItem> plan_sweep(
      const ObservationSchedulingContext& context) override;
  std::vector<ObservationWorkItem> plan_prefetch(
      const ObservationSchedulingContext& context) override;
  std::vector<ObservationWorkItem> plan_invalidation(
      const ObservationSchedulingContext& context) override;
  ObservationWorkItem plan_interactive(
      NodeID node_id, FrameID frame_id,
      const ObservationSchedulingContext& context) override;

 private:
  // Look up a node's fingerprint in the context (empty if absent).
  static NodeFingerprint fingerprint_of(
      NodeID node_id, const ObservationSchedulingContext& context);

  // Emit the single full-observer-set work item covering @p frames at @p node.
  void emit_node_items(NodeID node_id, const NodeFingerprint& fingerprint,
                       FrameIDRange frames, ObservationPriority priority,
                       std::vector<ObservationWorkItem>& out) const;

  std::vector<std::string> observer_ids_;
  FrameID prefetch_radius_;
};

}  // namespace orc
