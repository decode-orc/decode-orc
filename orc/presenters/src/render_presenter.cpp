/*
 * File:        render_presenter.cpp
 * Module:      orc-presenters
 * Purpose:     Rendering presenter implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "render_presenter.h"

#include <orc/stage/analysis_sink_results.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/preview/colour_preview_provider.h>
#include <orc/stage/preview/stage_preview_capability.h>
#include <orc/stage/stage.h>
#include <orc/stage/tooling/catalogue_results.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/include/carrier_scope_extraction.h"
#include "../core/include/core_observation_service.h"
#include "../core/include/dag_executor.h"
#include "../core/include/dag_frame_renderer.h"
#include "../core/include/frame_provenance.h"
#include "../core/include/observation_cache.h"
#include "../core/include/observation_cache_retention.h"
#include "../core/include/observation_invalidation.h"
#include "../core/include/observation_scheduler.h"
#include "../core/include/observation_store.h"
#include "../core/include/preview_renderer.h"
#include "../core/include/preview_view_registry.h"
#include "../core/include/project.h"
#include "../core/include/sqlite_observation_persistence.h"
#include "../core/include/store_backed_observation_context.h"
#include "analysis_series_decimator.h"
#include "metrics_presenter.h"
#include "project_presenter.h"
#include "representation_audio_stream_reader.h"

namespace orc::presenters {

namespace {

using orc::analysis::decimate_series;
using orc::analysis::SeriesPoint;

// Representative frame number for a display bucket: its centre frame, matching
// the previous SNR/burst graph labelling.
int32_t bucket_centre(int32_t frame_start, int32_t frame_end) {
  return frame_start + (frame_end - frame_start) / 2;
}

// Fill the frame-range/provenance fields shared by every display point from a
// decimated bucket. `frame_label` is the x-axis position for the point.
orc::presenters::AnalysisDisplayBucket make_bucket(
    const orc::analysis::DecimatedBucket& b, int32_t frame_label) {
  orc::presenters::AnalysisDisplayBucket out;
  out.frame_start = b.frame_start;
  out.frame_end = b.frame_end;
  out.frame_label = frame_label;
  // Bucket width: all analysed frames spanned, including any without a value.
  out.contributing_frames = static_cast<int32_t>(b.record_count);
  out.has_data = b.value_count > 0;
  return out;
}

// True when at least one point aggregates more than one analysed frame, i.e.
// the display is bucketed rather than one-point-per-frame.
template <typename PointT>
bool series_is_decimated(const std::vector<PointT>& points) {
  for (const auto& p : points) {
    if (p.bucket.frame_end > p.bucket.frame_start) {
      return true;
    }
  }
  return false;
}

// Reduce the full-resolution per-frame dropout series to at most `max_points`
// display points. Counts and lengths are summed within each bucket (the graph
// shows dropout activity per bucket); the bucket is labelled with its last
// frame, matching the previous graph labelling.
std::vector<orc::presenters::DropoutDisplayPoint> decimate_dropout_series(
    const std::vector<orc::FrameDropoutStats>& full, std::size_t max_points) {
  std::vector<SeriesPoint> length_series;
  std::vector<SeriesPoint> count_series;
  length_series.reserve(full.size());
  count_series.reserve(full.size());
  for (const auto& fs : full) {
    length_series.push_back(SeriesPoint{
        fs.frame_number, static_cast<double>(fs.dropout_length_samples),
        fs.has_data});
    count_series.push_back(SeriesPoint{
        fs.frame_number, static_cast<double>(fs.dropout_count), fs.has_data});
  }

  const auto len_buckets = decimate_series(length_series, max_points);
  const auto cnt_buckets = decimate_series(count_series, max_points);

  std::vector<orc::presenters::DropoutDisplayPoint> out;
  out.reserve(len_buckets.size());
  for (std::size_t i = 0; i < len_buckets.size(); ++i) {
    orc::presenters::DropoutDisplayPoint d;
    d.bucket = make_bucket(len_buckets[i], len_buckets[i].frame_end);
    d.dropout_length_samples =
        static_cast<int64_t>(std::llround(len_buckets[i].sum));
    d.dropout_count = static_cast<int32_t>(std::llround(cnt_buckets[i].sum));
    out.push_back(d);
  }
  return out;
}

// Reduce the full-resolution per-frame SNR series to at most `max_points`
// display points, averaging each metric within a bucket and labelling the
// bucket with its centre frame.
std::vector<orc::presenters::SNRDisplayPoint> decimate_snr_series(
    const std::vector<orc::FrameSNRStats>& full, std::size_t max_points) {
  std::vector<SeriesPoint> white_series;
  std::vector<SeriesPoint> black_series;
  white_series.reserve(full.size());
  black_series.reserve(full.size());
  for (const auto& fs : full) {
    white_series.push_back(
        SeriesPoint{fs.frame_number, fs.white_snr, fs.has_white_snr});
    black_series.push_back(
        SeriesPoint{fs.frame_number, fs.black_psnr, fs.has_black_psnr});
  }

  const auto white_buckets = decimate_series(white_series, max_points);
  const auto black_buckets = decimate_series(black_series, max_points);

  std::vector<orc::presenters::SNRDisplayPoint> out;
  out.reserve(white_buckets.size());
  for (std::size_t i = 0; i < white_buckets.size(); ++i) {
    orc::presenters::SNRDisplayPoint d;
    const int32_t label =
        bucket_centre(white_buckets[i].frame_start, white_buckets[i].frame_end);
    d.bucket = make_bucket(white_buckets[i], label);
    d.has_white_snr = white_buckets[i].value_count > 0;
    d.white_snr = white_buckets[i].mean;
    d.has_black_psnr = black_buckets[i].value_count > 0;
    d.black_psnr = black_buckets[i].mean;
    // A point carries data if either metric contributed a value.
    d.bucket.has_data = d.has_white_snr || d.has_black_psnr;
    out.push_back(d);
  }
  return out;
}

// Reduce the full-resolution per-frame burst-level series to at most
// `max_points` display points, averaging within a bucket.
std::vector<orc::presenters::BurstLevelDisplayPoint> decimate_burst_series(
    const std::vector<orc::FrameBurstLevelStats>& full,
    std::size_t max_points) {
  std::vector<SeriesPoint> series;
  series.reserve(full.size());
  for (const auto& fs : full) {
    series.push_back(
        SeriesPoint{fs.frame_number, fs.median_burst_10bit, fs.has_data});
  }

  const auto buckets = decimate_series(series, max_points);

  std::vector<orc::presenters::BurstLevelDisplayPoint> out;
  out.reserve(buckets.size());
  for (const auto& b : buckets) {
    orc::presenters::BurstLevelDisplayPoint d;
    d.bucket = make_bucket(b, bucket_centre(b.frame_start, b.frame_end));
    d.median_burst_10bit = b.mean;
    out.push_back(d);
  }
  return out;
}

// Derived field ids for a frame: top = frame*2, bottom = frame*2+1. Mirrors
// Observer::process_frame() and DAGFrameRenderer's store keying.
constexpr orc::FieldID::value_type kFieldsPerFrame = 2;

// The two phases a sink trigger reports before the sink itself runs. Both are
// metered against the same frame total, so the progress bar moves continuously
// from the first phase into the second.
constexpr const char* kCheckingMessage = "Checking cached observations…";
constexpr const char* kComputingMessage = "Computing observations…";

// User config directory (same resolution rules as the plugin registry:
// XDG_CONFIG_HOME / ~/.config on POSIX, APPDATA on Windows, cwd fallback).
std::filesystem::path resolveUserConfigDir() {
  const auto env_or_empty = [](const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
  };
#if defined(_WIN32)
  const std::string appdata = env_or_empty("APPDATA");
  if (!appdata.empty()) {
    return std::filesystem::path(appdata) / "decode-orc";
  }
  const std::string userprofile = env_or_empty("USERPROFILE");
  if (!userprofile.empty()) {
    return std::filesystem::path(userprofile) / "AppData" / "Roaming" /
           "decode-orc";
  }
#else
  const std::string xdg_config_home = env_or_empty("XDG_CONFIG_HOME");
  if (!xdg_config_home.empty()) {
    return std::filesystem::path(xdg_config_home) / "decode-orc";
  }
  const std::string home = env_or_empty("HOME");
  if (!home.empty()) {
    return std::filesystem::path(home) / ".config" / "decode-orc";
  }
#endif
  return std::filesystem::current_path() / ".decode-orc";
}

// Observation sidecar path for a DAG: a per-source database in the user
// config dir, named by a stable digest of the DAG's SOURCE nodes (stage,
// version, parameters — which include the source file path). Used for every
// project, saved or not: content-addressed records make one cache per source
// the natural unit, shared across quick sessions and saved projects. File
// identity (mtime/size) is deliberately excluded so the file name survives
// touches; staleness is already handled by the fingerprint record keys
// inside. The legacy "quick-" filename prefix is kept so caches created
// before saved projects switched to this path remain warm. A hash collision
// merely shares a database file — content-addressed keys keep that correct.
// Returns "" when the DAG has no sources or the directory cannot be created
// (the store then runs in-memory only).
std::string perSourceSidecarPath(const orc::DAG& dag) {
  std::string serial;
  for (const auto& node : dag.nodes()) {
    if (!node.stage) {
      continue;
    }
    const auto info = node.stage->get_node_type_info();
    if (info.type != orc::NodeType::SOURCE) {
      continue;
    }
    serial +=
        orc::serialize_node_provenance(info.stage_name, node.stage->version(),
                                       /*input_tokens=*/{}, node.parameters,
                                       /*source_identity=*/"");
  }
  if (serial.empty()) {
    return {};
  }
  char digest[17];
  std::snprintf(digest, sizeof(digest), "%016zx",
                std::hash<std::string>{}(serial));

  try {
    const std::filesystem::path dir =
        resolveUserConfigDir() / "observation-cache";
    std::filesystem::create_directories(dir);
    return (dir / (std::string("quick-") + digest + orc::kSidecarSuffix))
        .string();
  } catch (const std::exception& e) {
    ORC_LOG_WARN(
        "RenderPresenter: cannot create observation cache directory ({}); "
        "continuing in-memory only",
        e.what());
    return {};
  }
}

// True iff the store already holds every observer's record for both fields of
// @p frame at @p fingerprint (an empty record still counts as present).
bool store_has_frame(orc::ObservationStore& store,
                     const std::vector<orc::ObserverInfo>& observers,
                     const orc::NodeFingerprint& fingerprint,
                     orc::FrameID frame) {
  const orc::FieldID field_top(frame * kFieldsPerFrame);
  const orc::FieldID field_bottom(frame * kFieldsPerFrame + 1);
  for (const auto& observer : observers) {
    const orc::ObservationRecordKey key_top{fingerprint, field_top, observer.id,
                                            observer.version};
    const orc::ObservationRecordKey key_bottom{fingerprint, field_bottom,
                                               observer.id, observer.version};
    if (!store.has(key_top) || !store.has(key_bottom)) {
      return false;
    }
  }
  return true;
}

// Presence-only variant of store_has_frame(): consults memory and the sidecar
// without loading records into the memory LRU. Used by coverage checks (the
// prefetch probe and sweep-marker validation) that may test many frames they
// never read.
bool store_frame_is_stored(orc::ObservationStore& store,
                           const std::vector<orc::ObserverInfo>& observers,
                           const orc::NodeFingerprint& fingerprint,
                           orc::FrameID frame) {
  const orc::FieldID field_top(frame * kFieldsPerFrame);
  const orc::FieldID field_bottom(frame * kFieldsPerFrame + 1);
  for (const auto& observer : observers) {
    if (!store.has_stored(
            {fingerprint, field_top, observer.id, observer.version}) ||
        !store.has_stored(
            {fingerprint, field_bottom, observer.id, observer.version})) {
      return false;
    }
  }
  return true;
}

// Load every observer's stored record for both fields of @p frame into @p out.
void load_frame_from_store(orc::ObservationStore& store,
                           const std::vector<orc::ObserverInfo>& observers,
                           const orc::NodeFingerprint& fingerprint,
                           orc::FrameID frame, orc::ObservationContext& out) {
  const orc::FieldID field_top(frame * kFieldsPerFrame);
  const orc::FieldID field_bottom(frame * kFieldsPerFrame + 1);
  for (const auto& observer : observers) {
    store.load_into({fingerprint, field_top, observer.id, observer.version},
                    out);
    store.load_into({fingerprint, field_bottom, observer.id, observer.version},
                    out);
  }
}

}  // namespace

class RenderPresenter::Impl {
 public:
  explicit Impl(void* project_handle)
      : project_(static_cast<orc::Project*>(project_handle)),
        trigger_cancel_requested_(false),
        trigger_active_(false),
        next_request_id_(1) {
    if (!project_) {
      throw std::invalid_argument("Project cannot be null");
    }
  }

  // Helper to get concrete DAG from opaque handle
  std::shared_ptr<const orc::DAG> getConcreteDAG() const {
    return std::static_pointer_cast<const orc::DAG>(dag_void_);
  }

  orc::Project* project_;
  std::shared_ptr<void> dag_void_;  // Opaque DAG handle
  std::unique_ptr<orc::PreviewRenderer> preview_renderer_;
  orc::PreviewViewRegistry preview_view_registry_;
  std::unique_ptr<orc::DAGFrameRenderer> field_renderer_;
  std::shared_ptr<orc::ObservationCache> obs_cache_;

  // Provenance-keyed observation store, owned here so it survives DAG rebuilds
  // (editing a parameter on one branch must not discard observations for
  // unaffected branches/frames). Created lazily on first rebuild and shared
  // into the ObservationCache and field renderer. The file-identity provider
  // folds source-file identity into SOURCE-node fingerprints.
  std::shared_ptr<orc::ObservationStore> obs_store_;
  orc::FilesystemFileIdentityProvider file_identity_provider_;

  // Phase 6: durable SQLite sidecar stored beside the project. Created lazily
  // once alongside the store when the project has an on-disk location, so
  // observations survive application restarts. Absent for unsaved projects.
  std::shared_ptr<orc::IObservationPersistence> obs_persistence_;
  bool sidecar_initialized_ = false;

  // False for auxiliary presenters (e.g. the dropout editor's) that only
  // render frames: they skip the background scheduler, sweeps, and prefetch
  // entirely — constructing one must stay cheap enough for the GUI thread,
  // and one background pipeline per process is enough.
  bool background_observation_enabled_ = true;

  // What the last renderPreview() cost on the render worker. Written and read
  // on that worker alone - the coordinator collects it immediately after the
  // render it belongs to.
  orc::presenters::PreviewRenderCostView last_render_cost_;

  // True while a preview is playing. Held here rather than only on the
  // scheduler because playback can start before the first DAG build creates
  // one, and each rebuild re-applies it.
  std::atomic<bool> playback_active_{false};

  // Optional observer of on-demand preview execution, reinstalled on the
  // renderer after every rebuild so it survives DAG changes. Set and fired on
  // the thread driving preview queries (the coordinator's worker), so it needs
  // no locking of its own.
  orc::presenters::DagExecutionProgressCallback execution_progress_;

  // Phase 3: previous fingerprint map + undo retention window drive change
  // propagation (invalidation notifications) and store garbage collection.
  orc::NodeFingerprintMap prev_fingerprints_;
  bool have_prev_fingerprints_ = false;
  orc::ObservationRetentionWindow retention_window_;

  // Invalidation subscribers. Guarded so subscribe/unsubscribe and firing (from
  // the DAG-rebuild thread) are safe to interleave.
  std::mutex subscribers_mutex_;
  std::map<uint64_t, orc::presenters::ObservationInvalidationCallback>
      invalidation_subscribers_;
  uint64_t next_subscription_id_ = 1;

  // Copy subscribers out under the lock, then invoke without holding it so a
  // callback may re-enter unsubscribeInvalidation() without deadlocking.
  void notifyInvalidation(const std::vector<NodeID>& changed_nodes) {
    std::vector<orc::presenters::ObservationInvalidationCallback> callbacks;
    {
      std::lock_guard<std::mutex> lock(subscribers_mutex_);
      callbacks.reserve(invalidation_subscribers_.size());
      for (const auto& [id, cb] : invalidation_subscribers_) {
        callbacks.push_back(cb);
      }
    }
    if (callbacks.empty()) {
      return;
    }
    orc::presenters::ObservationInvalidationEvent event;
    event.changed_nodes = changed_nodes;
    for (const auto& cb : callbacks) {
      if (cb) {
        cb(event);
      }
    }
  }

  // === Phase 5: background scheduler + async observation requests ===

  // Standard observer service (host inventory) and its cached enumeration. Used
  // to key the store for async requests and to split the scheduling policy's
  // stateless/stateful observer sets.
  orc::CoreObservationService obs_service_;
  std::vector<orc::ObserverInfo> observers_;

  // Per-fingerprint subsets of observers_ applicable to the node's video
  // system (standard_observer_applies): the observer pass writes no records
  // for inapplicable observers, so every coverage probe must demand the same
  // filtered set or frames would look permanently uncovered. A fingerprint
  // covers the source's identity and parameters, so an entry can never go
  // stale; the map is cleared on rebuild only to drop unreachable keys.
  // Touched only on the coordinator worker thread (like sched_swept_);
  // other threads receive snapshots (PendingObservationRequest::observers,
  // the makeSchedContext capture).
  std::unordered_map<std::string, std::vector<orc::ObserverInfo>>
      applicable_observers_by_fp_;

  // Background scheduler with a dedicated renderer/store on its own worker
  // thread; created on the first DAG build and re-pointed on each rebuild. The
  // shared fingerprint map keys interactive work items and store lookups.
  std::shared_ptr<orc::IObservationSchedulingPolicy> obs_policy_;
  std::unique_ptr<orc::ObservationScheduler> scheduler_;
  std::shared_ptr<const orc::NodeFingerprintMap> fingerprints_shared_;
  std::atomic<uint64_t> next_obs_request_id_{1};

  // Current preview focus that drives background prefetch + whole-node sweeps.
  // Touched only on the coordinator worker thread (renderPreview /
  // rebuildRenderersFromDAG run there), so no extra locking is needed.
  NodeID sched_preview_node_{0};
  orc::FrameID sched_preview_frame_ = 0;
  bool sched_have_preview_ = false;
  // Nodes already swept under their current fingerprint, so a sweep is enqueued
  // once per provenance rather than on every preview step. Cleared/updated when
  // a node's fingerprint changes.
  std::unordered_map<NodeID, orc::NodeFingerprint> sched_swept_;

  // Observer inventory stamp ("id:version;..." for every standard observer).
  // Computed once when observers_ is first populated (the inventory is fixed
  // for the process lifetime) and used both for the sidecar's version-purge
  // stamp and to validate persisted sweep-complete markers.
  std::string observer_versions_stamp_;

  // Whole-node sweeps in flight, keyed by fingerprint value with the number of
  // frames still unaccounted for. When a fingerprint's count reaches zero the
  // sweep completed in full and a durable "swept:<fingerprint>" marker is
  // stamped in the sidecar, so the next session skips re-enqueueing the sweep
  // entirely. Guarded by sweep_mutex_: registered on the coordinator worker
  // thread, decremented from scheduler-worker completion callbacks.
  std::mutex sweep_mutex_;
  std::unordered_map<std::string, std::uint64_t> pending_sweeps_;

  // Meta key prefix for durable sweep-complete markers.
  static constexpr const char* kSweptMetaPrefix = "swept:";

  // Whether a node of @p total frames is short enough to sweep in the
  // background. The policy would decline to plan the sweep anyway (see
  // DefaultObservationSchedulingPolicy::kMaxWholeNodeFrames for why); asking
  // the same question here keeps this class's sweep bookkeeping honest, since
  // registerPendingSweep would otherwise be left waiting forever on
  // completions for frames nobody enqueued. Reported once per node, so a
  // dialog showing only the scrubbed neighbourhood has a stated reason.
  bool sweepableLength(NodeID node_id, std::uint64_t total) {
    if (total <= orc::DefaultObservationSchedulingPolicy::kMaxWholeNodeFrames) {
      return true;
    }
    ORC_LOG_INFO(
        "RenderPresenter: node '{}' holds {} frames, beyond the {}-frame limit "
        "on whole-node background observation; its observations will be "
        "gathered around the preview position as you scrub rather than over "
        "the whole source",
        node_id.to_string(), total,
        orc::DefaultObservationSchedulingPolicy::kMaxWholeNodeFrames);
    return false;
  }

  // A frame observation awaited by an async requestObservations() caller.
  struct PendingObservationRequest {
    uint64_t request_id = 0;
    NodeID node_id{0};
    orc::FrameID frame_id = 0;
    orc::NodeFingerprint fingerprint;
    // Applicable observer set snapshotted at request time, so the
    // scheduler-thread resolution probes the same coverage the observer pass
    // writes without touching the coordinator-thread applicability cache.
    std::vector<orc::ObserverInfo> observers;
    orc::presenters::ObservationDataReadyCallback callback;
  };
  std::mutex pending_mutex_;
  std::vector<PendingObservationRequest> pending_requests_;

  // Workload-progress subscribers (Task 5.4). Guarded like the invalidation
  // subscribers so subscribe/unsubscribe and worker-thread firing interleave
  // safely.
  std::mutex progress_subscribers_mutex_;
  std::map<uint64_t, orc::presenters::ObservationProgressCallback>
      progress_subscribers_;
  uint64_t next_progress_subscription_id_ = 1;

  ~Impl() {
    // Join the worker before any state it may touch (store, subscribers) is
    // torn down; no callback runs after this returns.
    if (scheduler_) {
      scheduler_->stop();
    }
  }

  orc::NodeFingerprint fingerprintOf(NodeID node) const {
    if (!fingerprints_shared_) {
      return {};
    }
    const auto it = fingerprints_shared_->find(node);
    return it != fingerprints_shared_->end() ? it->second
                                             : orc::NodeFingerprint{};
  }

  // The subset of observers_ applicable to @p node_id's video parameters,
  // cached in applicable_observers_by_fp_ (see the member comment for the
  // consistency and threading contract). Applicability fails open: without a
  // renderer or video parameters the full set is returned, matching the
  // observer pass's own fallback.
  const std::vector<orc::ObserverInfo>& observersForNode(
      NodeID node_id, const orc::NodeFingerprint& fp) {
    const auto it = applicable_observers_by_fp_.find(fp.value);
    if (it != applicable_observers_by_fp_.end()) {
      return it->second;
    }
    std::optional<orc::SourceParameters> params;
    if (preview_renderer_) {
      try {
        if (const auto repr =
                preview_renderer_->get_representation_at_node(node_id)) {
          params = repr->get_video_parameters();
        }
      } catch (const std::exception&) {
        params.reset();  // fail open: the full observer set stays in force
      }
    }
    return applicable_observers_by_fp_
        .emplace(fp.value, orc::filter_applicable_observers(observers_, params))
        .first->second;
  }

  // Point the current renderer's executor at execution_progress_ (or clear it
  // when nobody is observing). Called whenever either side changes: the
  // subscriber via setExecutionProgressCallback(), the renderer on rebuild.
  void installExecutionProgress() {
    if (!preview_renderer_) {
      return;
    }
    if (!execution_progress_) {
      preview_renderer_->set_execution_progress_callback(nullptr);
      return;
    }
    preview_renderer_->set_execution_progress_callback(
        [this](NodeID node_id, std::size_t current, std::size_t total) {
          if (!execution_progress_) {
            return;
          }
          orc::presenters::DagExecutionProgressEvent event;
          event.node_id = node_id.value();
          event.current = static_cast<std::uint64_t>(current);
          event.total = static_cast<std::uint64_t>(total);
          execution_progress_(event);
        });
  }

  // Number of frames (a frame is a field pair) available at a node's output, or
  // 0 if unknown. The scheduler works in FrameID, so this bounds sweep/prefetch
  // ranges. Uses the field count (the most fundamental output) halved, falling
  // back to the interlaced-frame count for nodes without a plain field output.
  std::uint64_t frameCountForNode(NodeID node) {
    if (!preview_renderer_) {
      return 0;
    }
    const std::uint64_t fields = preview_renderer_->get_output_count(
        node, orc::PreviewOutputType::Frame_Field1);
    if (fields >= 2) {
      return fields / 2;
    }
    return preview_renderer_->get_output_count(
        node, orc::PreviewOutputType::Frame_Field1_First);
  }

  // Assemble a scheduling context snapshot from the current fingerprint map.
  orc::ObservationSchedulingContext makeSchedContext(
      std::vector<NodeID> nodes_of_interest, orc::FrameID preview_pos,
      std::uint64_t total_frames, std::vector<NodeID> changed_nodes) {
    orc::ObservationSchedulingContext ctx;
    ctx.fingerprints = fingerprints_shared_;
    ctx.preview_position = preview_pos;
    ctx.total_frames = total_frames;
    ctx.nodes_of_interest = std::move(nodes_of_interest);
    ctx.changed_nodes = std::move(changed_nodes);
    // Coverage probe for small plans (prefetch): presence-only, so probing a
    // warm window costs hash lookups / sidecar EXISTS queries, not record
    // loads. The context is consumed synchronously inside the scheduler's
    // policy call on this thread, so capturing `this` is safe; the per-node
    // applicable observer sets are still snapshotted so the lambda never
    // touches the coordinator-thread applicability cache.
    if (obs_store_) {
      auto store = obs_store_;
      auto fingerprints = fingerprints_shared_;
      // Applicable observer set per node the policy may probe. The probe must
      // demand exactly the records the observer pass writes (inapplicable
      // observers store nothing), or covered frames would look uncovered.
      std::unordered_map<NodeID, std::vector<orc::ObserverInfo>> node_observers;
      const auto collect = [this,
                            &node_observers](const std::vector<NodeID>& nodes) {
        for (const NodeID node : nodes) {
          if (node_observers.count(node) != 0) {
            continue;
          }
          const orc::NodeFingerprint fp = fingerprintOf(node);
          if (!fp.value.empty()) {
            node_observers.emplace(node, observersForNode(node, fp));
          }
        }
      };
      collect(ctx.nodes_of_interest);
      collect(ctx.changed_nodes);
      auto observers = observers_;
      ctx.frame_observed = [store, fingerprints, observers, node_observers](
                               NodeID node, orc::FrameID frame) {
        if (!fingerprints) {
          return false;
        }
        const auto it = fingerprints->find(node);
        if (it == fingerprints->end() || it->second.value.empty()) {
          return false;
        }
        const auto obs_it = node_observers.find(node);
        const auto& node_obs =
            obs_it != node_observers.end() ? obs_it->second : observers;
        return store_frame_is_stored(*store, node_obs, it->second, frame);
      };
    }
    return ctx;
  }

  // Prefetch a window of observations around the current preview position.
  // Runs on the coordinator worker thread on every preview render. Deliberately
  // does NOT sweep the whole node: merely clicking through stages must stay
  // cheap. Because a node's provenance fingerprint is distinct per node (even
  // when two nodes' output content is identical), a per-stage whole-node sweep
  // would re-observe every frame for every stage visited. Whole-node sweeps are
  // reserved for nodes an observer dialog is actually reading
  // (sweepNodeForObservation, driven by requestObservations). No-op until a
  // scheduler exists (i.e. a DAG has been built).
  void scheduleObservationsForPreview(NodeID node_id, orc::FrameID frame_id) {
    if (!scheduler_) {
      return;
    }
    if (sched_have_preview_ && node_id == sched_preview_node_ &&
        frame_id == sched_preview_frame_) {
      return;  // no movement: nothing new to prefetch
    }
    const std::uint64_t total = frameCountForNode(node_id);
    if (total == 0) {
      return;
    }
    sched_preview_node_ = node_id;
    sched_preview_frame_ = frame_id;
    sched_have_preview_ = true;

    // Prefetch a window around the new preview position (medium priority).
    scheduler_->on_preview_moved(makeSchedContext({node_id}, frame_id, total,
                                                  /*changed_nodes=*/{}));
  }

  // True when the sidecar carries a valid sweep-complete marker for @p fp: the
  // marker's observer-version stamp matches the current inventory AND a spot
  // probe of the first and last frames still finds records (detects a sidecar
  // GC that removed the fingerprint's records after the marker was written; GC
  // removes whole fingerprints, so any frame probe suffices). @p observers is
  // the node's applicable set — the sweep stored exactly those records, so the
  // probe must demand no more (and a probe against a sidecar swept by an older
  // build, which stored the full set, still hits on the subset).
  bool sweepMarkerValid(const orc::NodeFingerprint& fp, std::uint64_t total,
                        const std::vector<orc::ObserverInfo>& observers) {
    if (!obs_persistence_ || !obs_store_ || total == 0) {
      return false;
    }
    if (obs_persistence_->get_meta(std::string(kSweptMetaPrefix) + fp.value) !=
        observer_versions_stamp_) {
      return false;
    }
    return store_frame_is_stored(*obs_store_, observers, fp, 0) &&
           store_frame_is_stored(*obs_store_, observers, fp, total - 1);
  }

  // Register a whole-node sweep of @p total frames at @p fp so its completions
  // can be counted down and, when it finishes in full, a durable marker
  // written (see noteSweepCompletion). Adding to an existing entry keeps the
  // count correct when two nodes share a fingerprint and both sweep.
  void registerPendingSweep(const orc::NodeFingerprint& fp,
                            std::uint64_t total) {
    if (fp.value.empty() || total == 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(sweep_mutex_);
    pending_sweeps_[fp.value] += total;
  }

  // Count a scheduler completion against any pending sweep of its fingerprint.
  // Invoked on scheduler worker threads from the completion callback. A failed
  // or cancelled chunk voids the sweep (the marker must only ever assert full
  // coverage); when the outstanding count reaches zero the sweep completed in
  // full and the durable marker is stamped so later sessions skip the sweep.
  void noteSweepCompletion(const orc::ObservationCompletion& completion) {
    if (completion.priority != orc::ObservationPriority::kSweep ||
        completion.fingerprint.value.empty()) {
      return;
    }
    bool completed = false;
    {
      std::lock_guard<std::mutex> lock(sweep_mutex_);
      auto it = pending_sweeps_.find(completion.fingerprint.value);
      if (it == pending_sweeps_.end()) {
        return;
      }
      if (!completion.succeeded) {
        pending_sweeps_.erase(it);
        return;
      }
      it->second -= std::min(it->second, completion.frames_observed);
      if (it->second == 0) {
        pending_sweeps_.erase(it);
        completed = true;
      }
    }
    if (completed && obs_persistence_) {
      obs_persistence_->set_meta(
          std::string(kSweptMetaPrefix) + completion.fingerprint.value,
          observer_versions_stamp_);
    }
  }

  // Enqueue a whole-node background sweep so an observer dialog reading this
  // node has every frame observed ahead of scrubbing. Called from
  // requestObservations() — i.e. only when a dialog actually wants the node —
  // and only once per node provenance, so re-requests and preview navigation
  // within the node do not re-sweep. A durable sweep-complete marker from a
  // previous session short-circuits the enqueue entirely: the store already
  // holds every frame, so re-checking each one would only flash progress at
  // the user. Runs on the coordinator worker thread.
  void sweepNodeForObservation(NodeID node_id) {
    if (!scheduler_) {
      return;
    }
    const orc::NodeFingerprint fp = fingerprintOf(node_id);
    if (fp.value.empty()) {
      return;
    }
    const auto it = sched_swept_.find(node_id);
    if (it != sched_swept_.end() && it->second == fp) {
      return;  // already swept under this provenance
    }
    const std::uint64_t total = frameCountForNode(node_id);
    if (total == 0) {
      return;
    }
    if (!sweepableLength(node_id, total)) {
      // Recorded as swept so the refusal is decided (and reported) once per
      // provenance rather than on every dialog request for the node.
      sched_swept_[node_id] = fp;
      return;
    }
    if (sweepMarkerValid(fp, total, observersForNode(node_id, fp))) {
      sched_swept_[node_id] = fp;  // completed in an earlier session
      return;
    }
    sched_swept_[node_id] = fp;
    registerPendingSweep(fp, total);
    scheduler_->on_project_loaded(makeSchedContext({node_id}, /*preview_pos=*/0,
                                                   total,
                                                   /*changed_nodes=*/{}));
  }

  // Deliver every pending request the store can now satisfy, and fail those
  // whose interactive observation attempt has finished without producing
  // records. Invoked on the scheduler's worker thread from the completion
  // callback. Delivery callbacks run outside the lock.
  void resolvePendingRequests(const orc::ObservationCompletion& completion) {
    struct Delivery {
      orc::presenters::ObservationDataReadyCallback callback;
      uint64_t request_id;
      bool available;
      orc::NodeFingerprint fingerprint;
      orc::FrameID frame_id;
      std::vector<orc::ObserverInfo> observers;
    };
    std::vector<Delivery> deliveries;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      auto it = pending_requests_.begin();
      while (it != pending_requests_.end()) {
        const bool same_target = it->node_id == completion.node_id &&
                                 it->fingerprint == completion.fingerprint;
        if (!same_target) {
          ++it;
          continue;
        }
        // Probe against the request's applicable observer snapshot: the
        // observer pass stores nothing for inapplicable observers, so
        // demanding the full set would leave the request waiting forever.
        const bool present =
            obs_store_ && store_has_frame(*obs_store_, it->observers,
                                          it->fingerprint, it->frame_id);
        if (present) {
          deliveries.push_back({std::move(it->callback), it->request_id, true,
                                it->fingerprint, it->frame_id,
                                std::move(it->observers)});
          it = pending_requests_.erase(it);
        } else if (!completion.succeeded) {
          // The observation attempt for this target ended (failed/cancelled)
          // without records: report the miss so the caller stops waiting.
          deliveries.push_back({std::move(it->callback), it->request_id, false,
                                it->fingerprint, it->frame_id,
                                std::move(it->observers)});
          it = pending_requests_.erase(it);
        } else {
          // A successful completion for a different frame of the same node:
          // keep waiting for this frame's own completion.
          ++it;
        }
      }
    }
    for (auto& d : deliveries) {
      if (!d.callback) {
        continue;
      }
      if (d.available && obs_store_) {
        orc::ObservationContext ctx;
        load_frame_from_store(*obs_store_, d.observers, d.fingerprint,
                              d.frame_id, ctx);
        d.callback(d.request_id, true, &ctx);
      } else {
        d.callback(d.request_id, false, nullptr);
      }
    }
  }

  // Fail every outstanding pending request (used when the scheduler is torn
  // down or the DAG changes out from under awaited work).
  void failAllPendingRequests() {
    std::vector<PendingObservationRequest> drained;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      drained.swap(pending_requests_);
    }
    for (auto& p : drained) {
      if (p.callback) {
        p.callback(p.request_id, false, nullptr);
      }
    }
  }

  // Forward a scheduler workload snapshot to the GUI as a view payload. Copies
  // subscribers under the lock, then fires outside it so a callback may
  // re-enter unsubscribe without deadlocking.
  void notifyObservationProgress(const orc::ObservationWorkload& workload) {
    std::vector<orc::presenters::ObservationProgressCallback> callbacks;
    {
      std::lock_guard<std::mutex> lock(progress_subscribers_mutex_);
      callbacks.reserve(progress_subscribers_.size());
      for (const auto& [id, cb] : progress_subscribers_) {
        callbacks.push_back(cb);
      }
    }
    if (callbacks.empty()) {
      return;
    }
    orc::presenters::ObservationProgressEvent event;
    event.active = workload.active;
    event.percent_complete = workload.percent_complete;
    event.computing = workload.frames_computed > 0;
    event.outstanding_nodes = workload.outstanding_nodes;
    event.sweep_paused = workload.sweep_deferred;
    for (const auto& cb : callbacks) {
      if (cb) {
        cb(event);
      }
    }
  }

  std::atomic<bool> trigger_cancel_requested_;
  std::atomic<bool> trigger_active_;
  uint64_t next_request_id_;
  std::atomic<orc::TriggerableStage*> current_trigger_stage_{nullptr};

  // Display point budget for the analysis-graph data path. The sinks expose the
  // full-resolution per-frame series; the getXAnalysisData accessors decimate
  // it to at most this many points and return a typed view series by value.
  static constexpr std::size_t kDisplayPoints = 1000;

  void rebuildRenderersFromDAG() {
    auto dag = getConcreteDAG();
    if (!dag) {
      // Clear all renderers for null DAG. Tear the background scheduler down
      // first and fail any awaited requests so callers stop waiting.
      if (scheduler_) {
        scheduler_->stop();
        scheduler_.reset();
      }
      sched_have_preview_ = false;
      sched_swept_.clear();
      {
        // No scheduler ⇒ no completions: in-flight sweeps can never finish.
        std::lock_guard<std::mutex> lock(sweep_mutex_);
        pending_sweeps_.clear();
      }
      failAllPendingRequests();
      fingerprints_shared_.reset();
      preview_renderer_.reset();
      field_renderer_.reset();
      obs_cache_.reset();
      preview_view_registry_ = orc::PreviewViewRegistry{};
      return;
    }

    // Save dropout state if exists
    bool show_dropouts = false;
    if (preview_renderer_) {
      show_dropouts = preview_renderer_->get_show_dropouts();
    }

    // Compute the static provenance fingerprint map for this DAG. Shared into
    // the observation cache and field renderer so observer output is keyed to
    // frame content, not the live DAG instance.
    auto fingerprints = std::make_shared<const orc::NodeFingerprintMap>(
        orc::compute_node_fingerprints(*dag, file_identity_provider_));

    // Create the shared observation store once; it persists across rebuilds.
    if (!obs_store_) {
      obs_store_ = std::make_shared<orc::ObservationStore>();

      // Attach a durable SQLite sidecar so observations survive eviction and
      // application restarts. EVERY project — saved or quick — uses the
      // per-source cache sidecar in the user config dir, keyed by a digest of
      // the DAG's source stages: records are content-addressed, so the same
      // source warms up instantly across quick sessions, saved projects, and
      // save-as copies alike. (A sidecar beside the project file was tried and
      // abandoned: projects saved into the same directory shared one database,
      // and each open's GC deleted the other projects' records.) A sidecar
      // problem must never fail project open, so recover by logging and
      // continuing in-memory only.
      // Auxiliary presenters (background observation disabled) skip the
      // sidecar entirely: they exist to render a few frames or read
      // parameters, and attaching a potentially multi-GB database makes their
      // construction far too heavy for the GUI thread that typically builds
      // them.
      std::string db_path;
      if (background_observation_enabled_) {
        db_path = perSourceSidecarPath(*dag);
      }
      if (!db_path.empty()) {
        try {
          // Retire sidecars for sources the user has moved on from before
          // adding another. Nothing else prunes this directory: the store's
          // garbage collection only ever works *inside* the open project's own
          // database, so without a pass here every source ever opened leaves a
          // file behind for good — and a feature-length one runs to several
          // gigabytes. The sidecar this session is about to use is spared, and
          // stamping it as used keeps a sidecar another instance has open out
          // of reach of the limits (an open sidecar is always among the most
          // recently used).
          orc::enforceSidecarRetention(
              std::filesystem::path(db_path).parent_path().string(),
              orc::SidecarRetentionPolicy{}, db_path);
          orc::touchSidecar(db_path);

          obs_persistence_ =
              std::make_shared<orc::SqliteObservationPersistence>(db_path);
          obs_store_->set_persistence(obs_persistence_);
          ORC_LOG_INFO("RenderPresenter: observation sidecar at {}", db_path);
        } catch (const std::exception& e) {
          ORC_LOG_WARN(
              "RenderPresenter: observation sidecar unavailable ({}); "
              "continuing in-memory only",
              e.what());
          obs_persistence_.reset();
        }
      }
    }

    // Diff against the previous fingerprint map to drive change propagation and
    // store garbage collection. The first build has no predecessor: seed the
    // state without emitting a spurious full-graph invalidation.
    const orc::NodeFingerprintMap& new_fingerprints = *fingerprints;
    // Nodes whose provenance changed on this build; drives GUI invalidation
    // (below) and background re-observation (after the scheduler is repointed).
    std::vector<NodeID> changed_nodes;
    if (have_prev_fingerprints_) {
      const orc::ObservationInvalidation invalidation =
          orc::diff_node_fingerprints(prev_fingerprints_, new_fingerprints);
      retention_window_.record_unreachable(invalidation.removed_fingerprints);
      if (!invalidation.changed_nodes.empty()) {
        notifyInvalidation(invalidation.changed_nodes);
      }
      changed_nodes = invalidation.changed_nodes;
    }
    prev_fingerprints_ = new_fingerprints;
    have_prev_fingerprints_ = true;

    // On the first build with a durable sidecar, purge records for observers
    // whose version changed (their old records share a still-reachable
    // fingerprint, so fingerprint GC alone would not remove them). Stamped:
    // when the sidecar was last purged with the identical observer-version
    // set — the overwhelmingly common case — the nine whole-table scans are
    // skipped entirely. No bulk warm-start either: the store's read-through
    // reloads any persisted record on demand, so memory fills organically
    // with what is actually used.
    if (observer_versions_stamp_.empty()) {
      // Fixed for the process lifetime; also validates sweep-complete markers
      // (a version bump changes the stamp, silently invalidating them).
      for (const auto& observer : obs_service_.available_observers()) {
        observer_versions_stamp_ += observer.id + ":" + observer.version + ";";
      }
    }
    if (obs_persistence_ && !sidecar_initialized_) {
      if (obs_persistence_->get_meta("observer_versions") !=
          observer_versions_stamp_) {
        for (const auto& observer : obs_service_.available_observers()) {
          obs_store_->purge_observer_version(observer.id, observer.version);
        }
        obs_persistence_->set_meta("observer_versions",
                                   observer_versions_stamp_);
      }
      sidecar_initialized_ = true;
    }

    // Drop pending-sweep bookkeeping for fingerprints no longer in the DAG:
    // their queued work is purged by on_dag_changed() below and never
    // completes, so the entries would otherwise linger forever.
    {
      std::unordered_set<std::string> current_values;
      current_values.reserve(new_fingerprints.size());
      for (const auto& [node_id, fp] : new_fingerprints) {
        current_values.insert(fp.value);
      }
      std::lock_guard<std::mutex> lock(sweep_mutex_);
      for (auto it = pending_sweeps_.begin(); it != pending_sweeps_.end();) {
        it = current_values.count(it->first) ? std::next(it)
                                             : pending_sweeps_.erase(it);
      }
    }

    // Garbage-collect the store: retain everything reachable now, plus the
    // bounded window of recently-unreachable fingerprints so an undo reuses
    // stored observations. Records outside the retain set are evicted by
    // budget. The persistent sidecar is GC'd against the same set so undo
    // within the retention window still finds durable records.
    const std::unordered_set<orc::NodeFingerprint> retain_set =
        retention_window_.retain_set(new_fingerprints);
    obs_store_->retain_only(retain_set, obs_store_->memory_budget_bytes());
    // Sidecar GC scans the whole database, so it only runs when it can matter:
    // within a session, when provenance actually changed (a topology-only
    // rebuild cannot make any fingerprint unreachable); across sessions, when
    // the current fingerprint set differs from the stamped set the last GC
    // retained — an unchanged reopen skips the scan entirely. Skipping GC is
    // always safe: unreachable records waste space but can never serve wrong
    // data (content-addressed keys).
    if (obs_persistence_) {
      std::string fp_stamp;
      {
        std::vector<std::string> values;
        values.reserve(new_fingerprints.size());
        for (const auto& [node_id, fp] : new_fingerprints) {
          values.push_back(fp.value);
        }
        std::sort(values.begin(), values.end());
        std::size_t h = 0;
        for (const auto& v : values) {
          h ^= std::hash<std::string>{}(v) + 0x9e3779b97f4a7c15ULL + (h << 6) +
               (h >> 2);
        }
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016zx", h);
        fp_stamp.assign(buf);
      }
      const bool stamp_stale =
          obs_persistence_->get_meta("gc_fingerprints") != fp_stamp;
      if (stamp_stale || !changed_nodes.empty()) {
        obs_store_->gc_persistence(retain_set);
        obs_persistence_->set_meta("gc_fingerprints", fp_stamp);
      }
    }

    // Rebuild renderers
    obs_cache_ =
        std::make_shared<orc::ObservationCache>(dag, obs_store_, fingerprints);
    preview_renderer_ = std::make_unique<orc::PreviewRenderer>(dag);
    field_renderer_ = std::make_unique<orc::DAGFrameRenderer>(dag);
    field_renderer_->set_observation_store(obs_store_, fingerprints);

    // A fingerprint's applicable observer set can never go stale (provenance
    // covers the source parameters), but unreachable fingerprints would
    // accumulate — drop them with the rebuild.
    applicable_observers_by_fp_.clear();

    preview_view_registry_ = orc::PreviewViewRegistry{};
    orc::PreviewViewRegistry::register_default_views(
        preview_view_registry_, dag, preview_renderer_.get());

    // The renderer is new, so re-arm the execution observer on it.
    installExecutionProgress();

    // Restore dropout state
    if (preview_renderer_) {
      preview_renderer_->set_show_dropouts(show_dropouts);
    }

    // Wire the background observation scheduler (Phase 5). It owns a dedicated
    // renderer/store on its worker thread and writes into the same shared
    // ObservationStore, so interactive requests and background sweeps stay
    // consistent with the fingerprints keying the store.
    fingerprints_shared_ = fingerprints;
    if (observers_.empty()) {
      observers_ = obs_service_.available_observers();
    }
    if (!obs_policy_) {
      std::vector<std::string> observer_ids;
      std::vector<std::string> stateful_ids;
      observer_ids.reserve(observers_.size());
      for (const auto& observer : observers_) {
        observer_ids.push_back(observer.id);
        if (!observer.stateless) {
          stateful_ids.push_back(observer.id);
        }
      }
      obs_policy_ = std::make_shared<orc::DefaultObservationSchedulingPolicy>(
          std::move(observer_ids), std::move(stateful_ids));
    }
    if (!scheduler_ && background_observation_enabled_) {
      // Build a pool of independent runners (each its own renderer/handles) so
      // frame observation runs in parallel. They all write into the shared,
      // thread-safe ObservationStore. Pool size comes from the CLI-configurable
      // resolver (default: half the cores). Auxiliary presenters (background
      // observation disabled) never create a scheduler, which also disables
      // every downstream sweep/prefetch/re-observe path — they all no-op
      // without one.
      const unsigned worker_count =
          orc::presenters::resolveBackgroundObservationWorkerCount();
      orc::ObservationScheduler::TaskRunnerFactory runner_factory =
          [dag, fingerprints, store = obs_store_]() {
            return std::make_unique<orc::RendererObservationTaskRunner>(
                dag, fingerprints, store);
          };
      scheduler_ = std::make_unique<orc::ObservationScheduler>(
          std::move(runner_factory), worker_count, fingerprints, obs_policy_);
      ORC_LOG_INFO(
          "RenderPresenter: background observation scheduler using {} worker "
          "thread(s)",
          worker_count);
      scheduler_->set_completion_callback(
          [this](const orc::ObservationCompletion& completion) {
            noteSweepCompletion(completion);
            resolvePendingRequests(completion);
          });
      scheduler_->set_workload_callback(
          [this](const orc::ObservationWorkload& workload) {
            notifyObservationProgress(workload);
          });
      scheduler_->set_sweep_paused(playback_active_);
      scheduler_->start();
    } else if (scheduler_) {
      // Adopt the new DAG/fingerprints; stale queued work is purged. Requests
      // awaiting a frame whose provenance just changed can never be satisfied
      // by their captured fingerprint, so fail them — the caller re-requests.
      scheduler_->on_dag_changed(dag, fingerprints);
      failStalePendingRequests();
    }

    // Re-observe changed nodes in the background against the new provenance —
    // but only nodes that were already of interest (an observer dialog had
    // swept them). Other changed nodes are recomputed on demand, so a parameter
    // edit does not kick off whole-node sweeps for stages nobody is observing.
    // Done after on_dag_changed() has adopted the new map and purged stale work
    // so the freshly enqueued items are keyed to the fingerprints the scheduler
    // now holds; their new fingerprints are recorded to keep a later request
    // from redundantly re-sweeping them.
    if (scheduler_ && !changed_nodes.empty()) {
      std::vector<NodeID> reobserve;
      for (const NodeID node : changed_nodes) {
        if (sched_swept_.find(node) != sched_swept_.end()) {
          reobserve.push_back(node);
        }
      }
      if (!reobserve.empty()) {
        std::uint64_t total = frameCountForNode(sched_preview_node_);
        if (total == 0) {
          for (const NodeID node : reobserve) {
            total = frameCountForNode(node);
            if (total != 0) {
              break;
            }
          }
        }
        // The re-observation covers the whole node, so it is subject to the
        // same length limit as the sweep that first observed it. Refusing
        // leaves the stale sched_swept_ fingerprints in place, which is what
        // makes a later dialog request re-decide (and re-report) through
        // sweepNodeForObservation.
        if (total != 0 && sweepableLength(reobserve.front(), total)) {
          for (const NodeID node : reobserve) {
            const orc::NodeFingerprint fp = fingerprintOf(node);
            sched_swept_[node] = fp;
            // The re-observation is a full-range sweep of the new provenance;
            // track it so its completion stamps a fresh sweep marker.
            registerPendingSweep(fp, total);
          }
          scheduler_->on_invalidation(makeSchedContext(
              /*nodes_of_interest=*/{}, sched_preview_frame_, total,
              reobserve));
        }
      }
    }

    // Pre-warm analysis-sink inputs: every observation an analysis sink will
    // read on trigger is keyed to its input node's provenance, so sweep those
    // nodes in the background (lowest priority, once per provenance) as soon as
    // the DAG is built. By the time the user triggers, the store is warm and
    // the sink runs zero observer frames. Newly added sinks are picked up here
    // automatically because every project mutation rebuilds the DAG.
    for (const auto& node : dag->nodes()) {
      if (!node.stage || node.input_node_ids.empty()) {
        continue;
      }
      if (node.stage->get_node_type_info().type !=
          orc::NodeType::ANALYSIS_SINK) {
        continue;
      }
      sweepNodeForObservation(node.input_node_ids[0]);
    }
  }

  // Fail pending requests whose captured fingerprint no longer matches the
  // node's current provenance (their awaited work was purged on a DAG change).
  void failStalePendingRequests() {
    std::vector<PendingObservationRequest> stale;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      auto it = pending_requests_.begin();
      while (it != pending_requests_.end()) {
        if (fingerprintOf(it->node_id) != it->fingerprint) {
          stale.push_back(std::move(*it));
          it = pending_requests_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& p : stale) {
      if (p.callback) {
        p.callback(p.request_id, false, nullptr);
      }
    }
  }

  // Phase 5.3 (lazy read-through): wrap a triggered sink's ObservationContext
  // in a store-backed context that materialises each field's stored records on
  // the first access, so the sink can skip re-observing covered frames without
  // the whole recording being loaded into memory up front (the eager pre-load
  // this replaces peaked at GBs on a 600k-frame source). @p input_node is the
  // node whose output VFR the sink observes (its content keys the stored
  // records). Stateless observers load per field; stateful observers
  // all-or-nothing, so a sink's per-frame skip can never break a cross-frame
  // stream's continuity. Returns nullopt when there is nothing to read
  // through, in which case the caller passes the inner context unchanged.
  std::optional<orc::StoreBackedObservationContext> makeStoreBackedContext(
      NodeID input_node, const orc::VideoFrameRepresentation* vfr,
      orc::ObservationContext& inner) {
    if (!obs_store_ || vfr == nullptr) {
      return std::nullopt;
    }
    const orc::NodeFingerprint fingerprint = fingerprintOf(input_node);
    if (fingerprint.value.empty()) {
      return std::nullopt;
    }
    const orc::FrameIDRange range = vfr->frame_range();
    if (range.empty()) {
      return std::nullopt;
    }
    return std::optional<orc::StoreBackedObservationContext>(
        std::in_place, inner, *obs_store_, fingerprint,
        observersForNode(input_node, fingerprint), range);
  }

  // Phase 5.3 (write-back): after a sink trigger computes observations, persist
  // them into the provenance-keyed store so a later trigger, preview, or
  // session reuses them instead of recomputing. @p input_node is the node whose
  // content keys the records (the same key preloadStoredObservations reads).
  // Only observers that actually produced data for a field are written (an
  // absent namespace means the observer never ran — persisting an empty record
  // would wrongly suppress its future computation), and records already present
  // are left untouched so this adds no redundant persistence writes.
  void persistTriggerObservations(NodeID input_node,
                                  const orc::VideoFrameRepresentation* vfr,
                                  const orc::ObservationContext& ctx) {
    if (!obs_store_ || vfr == nullptr) {
      return;
    }
    const orc::NodeFingerprint fingerprint = fingerprintOf(input_node);
    if (fingerprint.value.empty()) {
      return;
    }
    const orc::FrameIDRange range = vfr->frame_range();
    if (range.empty()) {
      return;
    }

    // Iterate the node's applicable set only: an inapplicable observer never
    // produces data here, and its records must stay absent so coverage
    // probes (filtered by the same predicate) stay coherent.
    const auto& observers = observersForNode(input_node, fingerprint);
    const auto persist_field = [&](orc::FieldID field) {
      const auto all_obs = ctx.get_all_observations(field);
      if (all_obs.empty()) {
        return;
      }
      for (const auto& observer : observers) {
        const orc::ObservationRecordKey key{fingerprint, field, observer.id,
                                            observer.version};
        if (obs_store_->has(key)) {
          continue;  // already stored (e.g. preloaded) — leave it be
        }
        // Collect just this observer's namespace(s) from the merged context.
        orc::ObservationRecord record;
        for (const auto& provided : observer.provided_observations) {
          const auto ns_it = all_obs.find(provided.namespace_);
          if (ns_it != all_obs.end()) {
            record[provided.namespace_] = ns_it->second;
          }
        }
        if (!record.empty()) {
          obs_store_->put(key, std::move(record));
        }
      }
    };

    for (orc::FrameID frame = range.first;; ++frame) {
      persist_field(orc::FieldID(frame * kFieldsPerFrame));
      persist_field(orc::FieldID(frame * kFieldsPerFrame + 1));
      if (frame == range.last) {
        break;
      }
    }
  }

  // Fill @p missing with every frame of @p range that is not already covered
  // by a stored record for all of @p observers at @p fingerprint.
  //
  // The obvious implementation — probe the store once per frame — is what this
  // replaces, and it is why triggering a sink on a feature-length node used to
  // sit silently for minutes. Coverage is a question about record *identity*,
  // never their values, but the store's has() is a read-through: each probe
  // that misses memory materialises the record's values, installs it in the
  // LRU and evicts something else. At two fields times a dozen observers per
  // frame that is millions of point queries, millions of nested-map builds,
  // and a cache thrashed by the very scan meant to exploit it.
  //
  // So the sidecar is asked for its record keys in one ordered, index-only
  // walk (load_stored_keys) and coverage is accumulated into a bit per
  // observer per field — 8 bytes a field, and no value is ever read. Because
  // keys arrive in field order, the same pass yields honest progress. A
  // backend that cannot answer in bulk falls back to presence-only probes,
  // which are metered the same way.
  //
  // Records written this session may not have reached the sidecar yet, so
  // anything the bulk pass reports as uncovered is confirmed against the store
  // (memory first) before being called missing.
  //
  // Returns false if the scan was cancelled, in which case @p missing is not
  // meaningful. Runs on the coordinator worker thread.
  bool scanStoredCoverage(const orc::NodeFingerprint& fingerprint,
                          const std::vector<orc::ObserverInfo>& observers,
                          const orc::FrameIDRange& range,
                          const ProgressCallback& callback,
                          std::vector<orc::FrameID>& missing) {
    // One bit per observer, so the bulk path needs the applicable set to fit a
    // mask word. Real sets are ~a dozen; anything larger takes the probe path.
    constexpr std::size_t kMaxMaskedObservers = 64;
    const std::uint64_t total_frames = range.count();

    const auto report = [&](std::uint64_t frames_done) {
      if (callback) {
        callback(static_cast<std::size_t>(frames_done),
                 static_cast<std::size_t>(total_frames), kCheckingMessage);
      }
    };
    report(0);

    bool covered_by_bulk = false;
    std::vector<std::uint64_t> field_mask;
    if (obs_persistence_ && !observers.empty() &&
        observers.size() <= kMaxMaskedObservers) {
      // Observer sets are a dozen entries at most, so the sink below matches
      // by linear scan: hashing would mean building a composite key string per
      // record, and there are millions of records.
      const auto bit_of_observer =
          [&observers](const std::string& id,
                       const std::string& version) -> std::size_t {
        for (std::size_t i = 0; i < observers.size(); ++i) {
          if (observers[i].id == id && observers[i].version == version) {
            return i;
          }
        }
        return observers.size();  // not applicable here; contributes no bit
      };

      const std::uint64_t first_field = range.first * kFieldsPerFrame;
      field_mask.assign(total_frames * kFieldsPerFrame, 0);

      bool cancelled = false;
      std::uint64_t keys_seen = 0;
      covered_by_bulk = obs_persistence_->load_stored_keys(
          fingerprint, [&](orc::FieldID field, const std::string& observer_id,
                           const std::string& observer_version) {
            // Cancellation and progress are checked on a coarse key stride:
            // this lambda runs millions of times and holds the sidecar lock.
            if ((++keys_seen & 0xFFFFU) == 0) {
              if (trigger_cancel_requested_.load()) {
                cancelled = true;
                return false;
              }
              const std::uint64_t value = field.value();
              report(value > first_field
                         ? std::min((value - first_field) / kFieldsPerFrame,
                                    total_frames)
                         : 0);
            }
            const std::uint64_t value = field.value();
            if (value < first_field ||
                value - first_field >= field_mask.size()) {
              return true;  // outside the range this trigger covers
            }
            const std::size_t bit =
                bit_of_observer(observer_id, observer_version);
            if (bit < observers.size()) {
              field_mask[value - first_field] |= std::uint64_t{1} << bit;
            }
            return true;
          });
      if (cancelled) {
        return false;
      }
      if (!covered_by_bulk) {
        field_mask.clear();
        field_mask.shrink_to_fit();
      }
    }

    const std::uint64_t full_mask =
        observers.size() >= kMaxMaskedObservers
            ? ~std::uint64_t{0}
            : (std::uint64_t{1} << observers.size()) - 1;

    // Cancellation is checked on a frame stride; progress is reported on the
    // same stride but only once a probe has actually been paid for, so the
    // fast all-covered walk-through does not drag the bar back from the 100%
    // the key walk just reached.
    constexpr std::uint64_t kConfirmStride = 512;
    std::uint64_t probes = 0;
    std::uint64_t index = 0;
    for (orc::FrameID frame = range.first;; ++frame, ++index) {
      if (index % kConfirmStride == 0) {
        if (trigger_cancel_requested_.load()) {
          return false;
        }
        if (probes > 0 || !covered_by_bulk) {
          report(index);
        }
      }
      bool needs_probe = true;
      if (covered_by_bulk) {
        const std::size_t top =
            static_cast<std::size_t>(index) * kFieldsPerFrame;
        needs_probe =
            field_mask[top] != full_mask || field_mask[top + 1] != full_mask;
      }
      probes += needs_probe ? 1 : 0;
      // The sidecar's answer is only half the picture: a record put() this
      // session may still be queued for the write-behind thread, so confirm
      // an apparent miss against the store (which checks memory first) rather
      // than re-observing a frame that is already in hand.
      if (needs_probe &&
          !store_frame_is_stored(*obs_store_, observers, fingerprint, frame)) {
        missing.push_back(frame);
      }
      if (frame == range.last) {
        break;
      }
    }
    report(total_frames);
    return true;
  }

  // Fill the store with every observation a sink trigger will read, in
  // parallel across a temporary pool of task runners, before the sink runs.
  // The sink's serial loop then finds every frame pre-observed (it reads
  // through a store-backed context) and computes nothing — turning a cold
  // first trigger from a single-threaded pass into an N-way parallel one, and
  // a warm trigger into a pure store read. Frames already stored (background
  // sweep, previous trigger, pass-through alias) are skipped by the runner's
  // store fast-path, so this never repeats completed work. Runs on the
  // coordinator worker thread; progress is reported through @p callback and
  // cancellation honours trigger_cancel_requested_.
  //
  // Nothing is pre-loaded into memory here. Deciding coverage needs record
  // identities, not values (see scanStoredCoverage), and the sink's
  // store-backed context loads each field on first access — an eager warm of
  // the whole node would only fill an LRU that cannot hold it, in the reverse
  // of the order the sink then reads it.
  //
  // Returns false when cancelled mid-computation. Per-frame failures are
  // logged and left for the sink itself to surface.
  bool precomputeTriggerObservations(NodeID input_node,
                                     const orc::VideoFrameRepresentation* vfr,
                                     const ProgressCallback& callback) {
    if (!obs_store_ || !fingerprints_shared_ || vfr == nullptr) {
      return true;
    }
    const orc::NodeFingerprint fingerprint = fingerprintOf(input_node);
    if (fingerprint.value.empty()) {
      return true;
    }
    const orc::FrameIDRange range = vfr->frame_range();
    if (range.empty()) {
      return true;
    }
    auto dag = getConcreteDAG();
    if (!dag) {
      return true;
    }

    // Frames the store does not fully cover yet, judged against the node's
    // applicable observer set (inapplicable observers store no records).
    const auto& observers = observersForNode(input_node, fingerprint);
    std::vector<orc::FrameID> missing;
    const auto scan_start = std::chrono::steady_clock::now();
    if (!scanStoredCoverage(fingerprint, observers, range, callback, missing)) {
      return false;  // cancelled during the coverage scan
    }
    ORC_LOG_INFO(
        "RenderPresenter: trigger precompute — coverage scan of {} frames at "
        "node '{}' took {} ms, {} frame(s) uncovered",
        range.count(), input_node.to_string(),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - scan_start)
            .count(),
        missing.size());
    if (missing.empty()) {
      return true;
    }

    const auto start_time = std::chrono::steady_clock::now();
    const std::size_t worker_count = std::max<std::size_t>(
        1, std::min<std::size_t>(
               orc::presenters::resolveBackgroundObservationWorkerCount(),
               missing.size()));
    ORC_LOG_INFO(
        "RenderPresenter: trigger precompute — observing {} of {} frames at "
        "node '{}' on {} worker(s)",
        missing.size(), range.count(), input_node.to_string(), worker_count);

    std::atomic<std::uint64_t> done{0};
    std::atomic<std::uint64_t> failed{0};
    std::vector<std::thread> threads;
    threads.reserve(worker_count);
    const std::size_t per_worker =
        (missing.size() + worker_count - 1) / worker_count;
    for (std::size_t w = 0; w < worker_count; ++w) {
      const std::size_t begin = w * per_worker;
      const std::size_t end = std::min(begin + per_worker, missing.size());
      if (begin >= end) {
        break;
      }
      // References into locals are safe: every thread is joined before this
      // scope exits. By-reference (not by-value) capture of the string-holding
      // fingerprint/dag also keeps the closure nothrow-copyable, which the
      // thread entry point requires (bugprone-exception-escape).
      threads.emplace_back([this, &missing, &done, &failed, begin, end, &dag,
                            &fingerprint, input_node]() {
        // Nothing may escape a thread entry point, and a catch handler at this
        // level may only perform noexcept work (atomics) — failures are
        // counted here and logged by the metering thread after the join. Every
        // frame of the slice is counted as done regardless of outcome so the
        // metering loop always terminates.
        std::size_t processed = 0;
        try {
          // Each thread owns its renderer (single-threaded by contract); the
          // shared store is the only cross-thread handoff.
          orc::RendererObservationTaskRunner runner(dag, fingerprints_shared_,
                                                    obs_store_);
          for (std::size_t i = begin; i < end; ++i) {
            if (trigger_cancel_requested_.load()) {
              break;
            }
            try {
              runner.observe_frame(input_node, fingerprint, missing[i], {});
            } catch (...) {
              failed.fetch_add(1);
            }
            ++processed;
            done.fetch_add(1);
          }
        } catch (...) {
          // Runner construction (or another non-frame step) failed: fail the
          // remainder of the slice.
          failed.fetch_add(end - begin - processed);
          done.fetch_add(end - begin - processed);
        }
      });
    }

    // Meter progress from this (coordinator) thread so the callback fires on
    // the same thread a stage trigger loop would use.
    const std::size_t total = missing.size();
    while (done.load() < missing.size() && !trigger_cancel_requested_.load()) {
      if (callback) {
        callback(static_cast<std::size_t>(done.load()), total,
                 kComputingMessage);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (auto& t : threads) {
      t.join();
    }

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time)
            .count();
    const std::uint64_t completed = done.load();
    const std::uint64_t failures = failed.load();
    ORC_LOG_INFO(
        "RenderPresenter: trigger precompute finished — {}/{} frames in {} ms"
        "{}{}",
        completed, missing.size(), elapsed_ms,
        trigger_cancel_requested_.load() ? " [cancelled]" : "",
        failures > 0 ? fmt::format(" [{} failed — left for the sink]", failures)
                     : "");
    if (trigger_cancel_requested_.load()) {
      return false;
    }
    if (callback) {
      callback(total, total, kComputingMessage);
    }
    return true;
  }
};

RenderPresenter::RenderPresenter(void* project_handle)
    : impl_(std::make_unique<Impl>(project_handle)) {}

RenderPresenter::~RenderPresenter() = default;

RenderPresenter::RenderPresenter(RenderPresenter&&) noexcept = default;
RenderPresenter& RenderPresenter::operator=(RenderPresenter&&) noexcept =
    default;

void RenderPresenter::setDAG(std::shared_ptr<void> dag_handle) {
  impl_->dag_void_ = std::move(dag_handle);
  impl_->rebuildRenderersFromDAG();
}

uint64_t RenderPresenter::subscribeInvalidation(
    orc::presenters::ObservationInvalidationCallback callback) {
  std::lock_guard<std::mutex> lock(impl_->subscribers_mutex_);
  const uint64_t id = impl_->next_subscription_id_++;
  impl_->invalidation_subscribers_.emplace(id, std::move(callback));
  return id;
}

void RenderPresenter::unsubscribeInvalidation(uint64_t subscription_id) {
  std::lock_guard<std::mutex> lock(impl_->subscribers_mutex_);
  impl_->invalidation_subscribers_.erase(subscription_id);
}

uint64_t RenderPresenter::requestObservations(
    NodeID node_id, FieldID field_id,
    orc::presenters::ObservationDataReadyCallback callback) {
  const uint64_t request_id = impl_->next_obs_request_id_++;

  // Without a store/scheduler (no DAG built), there is nothing to observe.
  if (!impl_->obs_store_ || !impl_->scheduler_) {
    if (callback) {
      callback(request_id, false, nullptr);
    }
    return request_id;
  }

  const orc::FrameID frame_id = static_cast<orc::FrameID>(field_id.value() / 2);
  const orc::NodeFingerprint fingerprint = impl_->fingerprintOf(node_id);
  if (fingerprint.value.empty()) {
    // Node absent from the fingerprint map: cannot provenance-key it.
    if (callback) {
      callback(request_id, false, nullptr);
    }
    return request_id;
  }

  // An observer dialog is reading this node: sweep the whole node in the
  // background (once per provenance) so scrubbing through it is instant. This
  // is the "node of interest" signal — plain preview navigation never reaches
  // here, so clicking through stages does not trigger whole-node sweeps.
  impl_->sweepNodeForObservation(node_id);

  const std::vector<orc::ObserverInfo>& observers =
      impl_->observersForNode(node_id, fingerprint);

  // Store hit: answer synchronously, no render.
  if (store_has_frame(*impl_->obs_store_, observers, fingerprint, frame_id)) {
    orc::ObservationContext ctx;
    load_frame_from_store(*impl_->obs_store_, observers, fingerprint, frame_id,
                          ctx);
    if (callback) {
      callback(request_id, true, &ctx);
    }
    return request_id;
  }

  // Miss: register the request and enqueue the frame at interactive priority.
  {
    std::lock_guard<std::mutex> lock(impl_->pending_mutex_);
    Impl::PendingObservationRequest pending;
    pending.request_id = request_id;
    pending.node_id = node_id;
    pending.frame_id = frame_id;
    pending.fingerprint = fingerprint;
    pending.observers = observers;
    pending.callback = std::move(callback);
    impl_->pending_requests_.push_back(std::move(pending));
  }
  orc::ObservationSchedulingContext context;
  context.fingerprints = impl_->fingerprints_shared_;
  impl_->scheduler_->request_interactive(node_id, frame_id, context);
  return request_id;
}

void RenderPresenter::setBackgroundObservationEnabled(bool enabled) {
  impl_->background_observation_enabled_ = enabled;
}

orc::presenters::PreviewRenderCostView RenderPresenter::lastPreviewRenderCost()
    const {
  return impl_->last_render_cost_;
}

void RenderPresenter::setPlaybackActive(bool active) {
  // Remembered whether or not a scheduler exists yet: playback can start
  // before the first DAG build, and setDAG() re-applies it below.
  impl_->playback_active_ = active;
  if (impl_->scheduler_) {
    impl_->scheduler_->set_sweep_paused(active);
  }
}

void RenderPresenter::setExecutionProgressCallback(
    orc::presenters::DagExecutionProgressCallback callback) {
  impl_->execution_progress_ = std::move(callback);
  impl_->installExecutionProgress();
}

uint64_t RenderPresenter::subscribeObservationProgress(
    orc::presenters::ObservationProgressCallback callback) {
  std::lock_guard<std::mutex> lock(impl_->progress_subscribers_mutex_);
  const uint64_t id = impl_->next_progress_subscription_id_++;
  impl_->progress_subscribers_.emplace(id, std::move(callback));
  return id;
}

void RenderPresenter::unsubscribeObservationProgress(uint64_t subscription_id) {
  std::lock_guard<std::mutex> lock(impl_->progress_subscribers_mutex_);
  impl_->progress_subscribers_.erase(subscription_id);
}

orc::PreviewRenderResult RenderPresenter::renderPreview(
    NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
    const std::string& option_id, orc::PreviewNavigationHint hint) {
  if (!impl_->preview_renderer_) {
    return orc::PreviewRenderResult{
        {},      false,       "Preview renderer not initialized",
        node_id, output_type, output_index};
  }

  try {
    // Call core preview renderer
    auto core_result = impl_->preview_renderer_->render_output(
        node_id, output_type, output_index, option_id, hint);

    impl_->last_render_cost_ = orc::presenters::PreviewRenderCostView{};
    impl_->last_render_cost_.dag_execution_us =
        impl_->preview_renderer_->last_execution_us();

    // Populate observation cache for the rendered field(s). Timed: this
    // materialises the frame a second time through the cache's own renderer,
    // and nothing has yet established what that costs per frame.
    if (impl_->obs_cache_) {
      const auto fill_started = std::chrono::steady_clock::now();
      if (output_type == orc::PreviewOutputType::Frame_Field1 ||
          output_type == orc::PreviewOutputType::Frame_Field2 ||
          output_type == orc::PreviewOutputType::Luma) {
        impl_->obs_cache_->get_field(node_id, orc::FieldID(output_index));
      } else if (output_type == orc::PreviewOutputType::Frame_Field1_First ||
                 output_type == orc::PreviewOutputType::Frame_Reversed ||
                 output_type == orc::PreviewOutputType::Split) {
        uint64_t first_field = output_index * 2;
        impl_->obs_cache_->get_field(node_id, orc::FieldID(first_field));
        impl_->obs_cache_->get_field(node_id, orc::FieldID(first_field + 1));
      }
      impl_->last_render_cost_.observation_fill_us =
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - fill_started)
              .count();
    }

    // Follow the preview with background observation work: prefetch a window
    // around this position and sweep the whole node once per provenance. For
    // field-indexed outputs the index is a field; frame-indexed outputs already
    // count frames.
    const bool field_indexed =
        output_type == orc::PreviewOutputType::Frame_Field1 ||
        output_type == orc::PreviewOutputType::Frame_Field2 ||
        output_type == orc::PreviewOutputType::Luma ||
        output_type == orc::PreviewOutputType::Chroma;
    const orc::FrameID preview_frame =
        field_indexed ? static_cast<orc::FrameID>(output_index / 2)
                      : static_cast<orc::FrameID>(output_index);
    impl_->scheduleObservationsForPreview(node_id, preview_frame);

    // Convert core result to public API result
    orc::PreviewRenderResult result;
    result.image.width = core_result.image.width;
    result.image.height = core_result.image.height;
    result.image.rgb_data = std::move(core_result.image.rgb_data);
    result.image.dropout_regions = std::move(core_result.image.dropout_regions);
    result.success = core_result.success;
    result.error_message = std::move(core_result.error_message);
    result.node_id = core_result.node_id;
    result.output_type = core_result.output_type;
    result.output_index = core_result.output_index;

    return result;

  } catch (const std::exception& e) {
    return orc::PreviewRenderResult{{},      false,       e.what(),
                                    node_id, output_type, output_index};
  }
}

std::vector<orc::PreviewOutputInfo> RenderPresenter::getAvailableOutputs(
    NodeID node_id) {
  if (!impl_->preview_renderer_) {
    return {};
  }

  auto core_outputs = impl_->preview_renderer_->get_available_outputs(node_id);

  // Convert to public API types
  std::vector<orc::PreviewOutputInfo> result;
  result.reserve(core_outputs.size());

  for (const auto& core_out : core_outputs) {
    orc::PreviewOutputInfo info;
    info.type = core_out.type;
    info.display_name = core_out.display_name;
    info.count = core_out.count;
    info.is_available = core_out.is_available;
    info.dar_aspect_correction = core_out.dar_aspect_correction;
    info.option_id = core_out.option_id;
    info.dropouts_available = core_out.dropouts_available;
    info.has_separate_channels = core_out.has_separate_channels;
    info.first_field_offset = core_out.first_field_offset;
    result.push_back(std::move(info));
  }

  return result;
}

bool RenderPresenter::savePNG(NodeID node_id,
                              orc::PreviewOutputType output_type,
                              uint64_t output_index,
                              const std::string& filename,
                              const std::string& option_id,
                              double aspect_correction) {
  if (!impl_->preview_renderer_) {
    return false;
  }

  try {
    return impl_->preview_renderer_->save_png(node_id, output_type,
                                              output_index, filename, option_id,
                                              aspect_correction);
  } catch (const std::exception&) {
    return false;
  }
}

std::vector<orc::PreviewViewDescriptor>
RenderPresenter::getAvailablePreviewViews(NodeID node_id,
                                          orc::VideoDataType data_type) {
  auto dag = impl_->getConcreteDAG();
  if (!dag) {
    return {};
  }

  return impl_->preview_view_registry_.get_applicable_views(*dag, node_id,
                                                            data_type);
}

std::vector<orc::VideoDataType> RenderPresenter::getStageDataTypes(
    NodeID node_id) {
  auto dag = impl_->getConcreteDAG();
  if (!dag) {
    return {};
  }

  const auto& nodes = dag->nodes();
  auto it = std::find_if(
      nodes.begin(), nodes.end(),
      [node_id](const orc::DAGNode& node) { return node.node_id == node_id; });
  if (it == nodes.end() || !it->stage) {
    return {};
  }

  auto* capability_stage =
      dynamic_cast<const orc::IStagePreviewCapability*>(it->stage.get());
  if (!capability_stage) {
    return {};
  }

  return capability_stage->get_preview_capability().supported_data_types;
}

orc::PreviewViewDataResult RenderPresenter::requestPreviewViewData(
    NodeID node_id, const std::string& view_id, orc::VideoDataType data_type,
    const orc::PreviewCoordinate& coordinate) {
  auto dag = impl_->getConcreteDAG();
  if (!dag) {
    return {false, "DAG not initialized", orc::PreviewViewPayloadKind::None,
            std::nullopt, std::nullopt};
  }

  return impl_->preview_view_registry_.request_data(*dag, node_id, view_id,
                                                    data_type, coordinate);
}

orc::PreviewScopePayloads RenderPresenter::getPreviewScopes(
    NodeID node_id, const orc::PreviewScopeRequest& request) {
  orc::PreviewScopePayloads payloads;
  if (request.wantsNothing() || !request.coordinate.is_valid()) {
    return payloads;
  }

  auto dag = impl_->getConcreteDAG();
  if (!dag) {
    return payloads;
  }

  const bool colour_domain =
      request.data_type == orc::VideoDataType::ColourNTSC ||
      request.data_type == orc::VideoDataType::ColourPAL;

  // Signal-domain vectorscope: the acquisition demodulates a carrier rather
  // than reading decoder planes, and there is no histogram for it. It has no
  // fetch to share, so it goes through the registry unchanged — the gain here
  // is only that it now runs on this thread instead of the GUI's.
  if (!colour_domain) {
    if (request.want_vectorscope) {
      const auto result = impl_->preview_view_registry_.request_data(
          *dag, node_id, request.vectorscope_view_id, request.data_type,
          request.coordinate);
      if (result.success &&
          result.payload_kind == orc::PreviewViewPayloadKind::Vectorscope) {
        payloads.vectorscope = result.vectorscope;
      }
    }
    return payloads;
  }

  const orc::DAGNode* node = nullptr;
  for (const auto& candidate : dag->nodes()) {
    if (candidate.node_id == node_id) {
      node = &candidate;
      break;
    }
  }
  if (!node || !node->stage) {
    return payloads;
  }

  const auto* provider =
      dynamic_cast<const orc::IColourPreviewProvider*>(node->stage.get());
  if (!provider) {
    return payloads;
  }

  // The one decode. Both extractions read this carrier; fetching it per scope
  // is what made two open dialogues cost two chroma decodes of one frame.
  const std::uint64_t frame_index = request.coordinate.field_index;
  const auto carrier = provider->get_colour_preview_carrier(frame_index);
  if (!carrier.has_value() || !carrier->is_valid()) {
    return payloads;
  }

  if (request.want_vectorscope) {
    payloads.vectorscope = orc::extract_vectorscope_from_carrier(
        *carrier,
        orc::PreviewScopeSelection{
            request.coordinate.vectorscope_active_area_only,
            request.coordinate.vectorscope_first_line,
            request.coordinate.vectorscope_last_line},
        frame_index);
  }
  if (request.want_histogram) {
    payloads.histogram =
        orc::extract_histogram_from_carrier(*carrier, frame_index);
  }
  return payloads;
}

bool RenderPresenter::requestDropoutData(
    NodeID node_id, uint64_t request_id,
    std::function<void(uint64_t, bool, const std::string&)> callback) {
  // This is a synchronous operation - find the node and return data immediately
  if (!impl_->getConcreteDAG()) {
    if (callback) callback(request_id, false, "DAG not initialized");
    return false;
  }

  const orc::DAGNode* target_node = nullptr;
  for (const auto& node : impl_->getConcreteDAG()->nodes()) {
    if (node.node_id == node_id) {
      target_node = &node;
      break;
    }
  }

  if (!target_node) {
    if (callback) callback(request_id, false, "Node not found");
    return false;
  }

  auto* sink =
      dynamic_cast<orc::IDropoutAnalysisResults*>(target_node->stage.get());
  if (!sink) {
    if (callback) {
      callback(request_id, false, "Node is not a DropoutAnalysisSinkStage");
    }
    return false;
  }

  if (!sink->has_results()) {
    if (callback) {
      callback(request_id, false, "No data available - trigger the sink first");
    }
    return false;
  }

  // Data is available - signal success
  if (callback) callback(request_id, true, "");
  return true;
}

bool RenderPresenter::requestSNRData(
    NodeID node_id, uint64_t request_id,
    std::function<void(uint64_t, bool, const std::string&)> callback) {
  if (!impl_->getConcreteDAG()) {
    if (callback) callback(request_id, false, "DAG not initialized");
    return false;
  }

  const orc::DAGNode* target_node = nullptr;
  for (const auto& node : impl_->getConcreteDAG()->nodes()) {
    if (node.node_id == node_id) {
      target_node = &node;
      break;
    }
  }

  if (!target_node) {
    if (callback) callback(request_id, false, "Node not found");
    return false;
  }

  auto* sink =
      dynamic_cast<orc::ISNRAnalysisResults*>(target_node->stage.get());
  if (!sink) {
    if (callback) {
      callback(request_id, false, "Node is not a SNRAnalysisSinkStage");
    }
    return false;
  }

  if (!sink->has_results()) {
    if (callback) {
      callback(request_id, false, "No data available - trigger the sink first");
    }
    return false;
  }

  if (callback) callback(request_id, true, "");
  return true;
}

bool RenderPresenter::requestBurstLevelData(
    NodeID node_id, uint64_t request_id,
    std::function<void(uint64_t, bool, const std::string&)> callback) {
  if (!impl_->getConcreteDAG()) {
    if (callback) callback(request_id, false, "DAG not initialized");
    return false;
  }

  const orc::DAGNode* target_node = nullptr;
  for (const auto& node : impl_->getConcreteDAG()->nodes()) {
    if (node.node_id == node_id) {
      target_node = &node;
      break;
    }
  }

  if (!target_node) {
    if (callback) callback(request_id, false, "Node not found");
    return false;
  }

  auto* sink =
      dynamic_cast<orc::IBurstLevelAnalysisResults*>(target_node->stage.get());
  if (!sink) {
    if (callback) {
      callback(request_id, false, "Node is not a BurstLevelAnalysisSinkStage");
    }
    return false;
  }

  if (!sink->has_results()) {
    if (callback) {
      callback(request_id, false, "No data available - trigger the sink first");
    }
    return false;
  }

  if (callback) callback(request_id, true, "");
  return true;
}

uint64_t RenderPresenter::triggerStage(NodeID node_id,
                                       ProgressCallback callback) {
  if (!impl_->getConcreteDAG()) {
    throw std::runtime_error("DAG not initialized");
  }

  impl_->trigger_cancel_requested_.store(false);
  impl_->trigger_active_.store(true);
  uint64_t request_id = impl_->next_request_id_++;

  try {
    // Find the target node in the DAG
    const orc::DAGNode* target_node = nullptr;
    for (const auto& node : impl_->getConcreteDAG()->nodes()) {
      if (node.node_id == node_id) {
        target_node = &node;
        break;
      }
    }

    if (!target_node) {
      impl_->trigger_active_.store(false);
      throw std::runtime_error("Node '" + node_id.to_string() +
                               "' not found in DAG");
    }

    auto trigger_stage =
        dynamic_cast<orc::TriggerableStage*>(target_node->stage.get());
    if (!trigger_stage) {
      impl_->trigger_active_.store(false);
      throw std::runtime_error("Stage '" + node_id.to_string() +
                               "' is not triggerable");
    }

    // Build executor to get inputs for this node
    auto executor = std::make_shared<orc::DAGExecutor>();

    // Execute DAG up to (but not including) the target node to get its inputs
    std::vector<orc::ArtifactPtr> inputs;

    if (!target_node->input_node_ids.empty()) {
      // Execute predecessor nodes to get inputs
      auto node_outputs = executor->execute_to_node(
          *impl_->getConcreteDAG(), target_node->input_node_ids[0]);

      // Collect inputs from predecessor nodes
      for (size_t i = 0; i < target_node->input_node_ids.size(); ++i) {
        const auto& input_node_id = target_node->input_node_ids[i];
        size_t input_index = (i < target_node->input_indices.size())
                                 ? target_node->input_indices[i]
                                 : 0;

        auto it = node_outputs.find(input_node_id);
        if (it != node_outputs.end() && input_index < it->second.size()) {
          inputs.push_back(it->second[input_index]);
        }
      }
    }

    // Store pointer to current trigger stage for cancellation
    impl_->current_trigger_stage_.store(trigger_stage);

    // Set up progress callback
    trigger_stage->set_progress_callback([this, trigger_stage, callback](
                                             size_t current, size_t total,
                                             const std::string& message) {
      // Check for cancellation
      if (impl_->trigger_cancel_requested_.load()) {
        trigger_stage->cancel_trigger();
      }

      // Call user callback
      if (callback) {
        callback(static_cast<int>(current), static_cast<int>(total), message);
      }
    });

    // Execute trigger using the observation context populated during
    // execute_to_node
    orc::ObservationContext& obs_context = executor->get_observation_context();
    // Fill the store in parallel with every observation the sink will read
    // (skipping frames the background already covered); the sink then reads
    // through a store-backed context that loads each field on first access,
    // so its own loop computes nothing and peak memory follows the sink's
    // working set rather than the recording length.
    std::optional<orc::StoreBackedObservationContext> store_context;
    if (!target_node->input_node_ids.empty() && !inputs.empty()) {
      auto vfr =
          std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(inputs[0]);
      if (!impl_->precomputeTriggerObservations(target_node->input_node_ids[0],
                                                vfr.get(), callback)) {
        throw std::runtime_error("Trigger failed: Cancelled by user");
      }
      store_context = impl_->makeStoreBackedContext(
          target_node->input_node_ids[0], vfr.get(), obs_context);
    }
    orc::IObservationContext& trigger_context =
        store_context.has_value()
            ? static_cast<orc::IObservationContext&>(*store_context)
            : obs_context;
    bool success = trigger_stage->trigger(inputs, target_node->parameters,
                                          trigger_context);

    // Clear current trigger stage pointer
    impl_->current_trigger_stage_.store(nullptr);
    impl_->trigger_active_.store(false);

    // Phase 5.3 (write-back): persist the freshly-computed observations so a
    // later trigger/preview/session reuses them instead of recomputing.
    if (success && !target_node->input_node_ids.empty() && !inputs.empty()) {
      auto vfr =
          std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(inputs[0]);
      impl_->persistTriggerObservations(target_node->input_node_ids[0],
                                        vfr.get(), obs_context);
    }

    if (!success) {
      std::string status = trigger_stage->get_trigger_status();
      throw std::runtime_error("Trigger failed: " + status);
    }

  } catch (...) {
    impl_->current_trigger_stage_.store(nullptr);
    impl_->trigger_active_.store(false);
    throw;
  }

  return request_id;
}

uint64_t RenderPresenter::triggerStage(
    NodeID node_id, ProgressCallback callback,
    std::map<std::string, ParameterValue> parameter_overrides) {
  if (!impl_->getConcreteDAG()) {
    throw std::runtime_error("DAG not initialized");
  }

  impl_->trigger_cancel_requested_.store(false);
  impl_->trigger_active_.store(true);
  uint64_t request_id = impl_->next_request_id_++;

  try {
    const orc::DAGNode* target_node = nullptr;
    for (const auto& node : impl_->getConcreteDAG()->nodes()) {
      if (node.node_id == node_id) {
        target_node = &node;
        break;
      }
    }

    if (!target_node) {
      impl_->trigger_active_.store(false);
      throw std::runtime_error("Node '" + node_id.to_string() +
                               "' not found in DAG");
    }

    auto trigger_stage =
        dynamic_cast<orc::TriggerableStage*>(target_node->stage.get());
    if (!trigger_stage) {
      impl_->trigger_active_.store(false);
      throw std::runtime_error("Stage '" + node_id.to_string() +
                               "' is not triggerable");
    }

    auto executor = std::make_shared<orc::DAGExecutor>();
    std::vector<orc::ArtifactPtr> inputs;
    if (!target_node->input_node_ids.empty()) {
      auto node_outputs = executor->execute_to_node(
          *impl_->getConcreteDAG(), target_node->input_node_ids[0]);
      for (size_t i = 0; i < target_node->input_node_ids.size(); ++i) {
        const auto& input_node_id = target_node->input_node_ids[i];
        size_t input_index = (i < target_node->input_indices.size())
                                 ? target_node->input_indices[i]
                                 : 0;
        auto it = node_outputs.find(input_node_id);
        if (it != node_outputs.end() && input_index < it->second.size()) {
          inputs.push_back(it->second[input_index]);
        }
      }
    }

    impl_->current_trigger_stage_.store(trigger_stage);
    trigger_stage->set_progress_callback([this, trigger_stage, callback](
                                             size_t current, size_t total,
                                             const std::string& message) {
      if (impl_->trigger_cancel_requested_.load()) {
        trigger_stage->cancel_trigger();
      }
      if (callback) {
        callback(static_cast<int>(current), static_cast<int>(total), message);
      }
    });

    // Merge overrides into a copy of the node's stored parameters so the
    // project file is not modified.
    auto merged_params = target_node->parameters;
    for (const auto& [key, value] : parameter_overrides) {
      merged_params[key] = value;
    }

    orc::ObservationContext& obs_context = executor->get_observation_context();
    // Phase 5.3: read cached observations through a store-backed context (see
    // the primary triggerStage overload).
    std::optional<orc::StoreBackedObservationContext> store_context;
    if (!target_node->input_node_ids.empty() && !inputs.empty()) {
      auto vfr =
          std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(inputs[0]);
      if (!impl_->precomputeTriggerObservations(target_node->input_node_ids[0],
                                                vfr.get(), callback)) {
        throw std::runtime_error("Trigger failed: Cancelled by user");
      }
      store_context = impl_->makeStoreBackedContext(
          target_node->input_node_ids[0], vfr.get(), obs_context);
    }
    orc::IObservationContext& trigger_context =
        store_context.has_value()
            ? static_cast<orc::IObservationContext&>(*store_context)
            : obs_context;
    bool success =
        trigger_stage->trigger(inputs, merged_params, trigger_context);

    impl_->current_trigger_stage_.store(nullptr);
    impl_->trigger_active_.store(false);

    // Phase 5.3 (write-back): persist freshly-computed observations for reuse.
    if (success && !target_node->input_node_ids.empty() && !inputs.empty()) {
      auto vfr =
          std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(inputs[0]);
      impl_->persistTriggerObservations(target_node->input_node_ids[0],
                                        vfr.get(), obs_context);
    }

    if (!success) {
      std::string status = trigger_stage->get_trigger_status();
      throw std::runtime_error("Trigger failed: " + status);
    }

  } catch (...) {
    impl_->current_trigger_stage_.store(nullptr);
    impl_->trigger_active_.store(false);
    throw;
  }

  return request_id;
}

void RenderPresenter::cancelTrigger() {
  impl_->trigger_cancel_requested_.store(true);
  // Load the pointer atomically so the read is safe from the GUI thread
  auto* stage = impl_->current_trigger_stage_.load();
  if (stage) {
    stage->cancel_trigger();
  }
}

void RenderPresenter::setShowDropouts(bool show) {
  if (impl_->preview_renderer_) {
    impl_->preview_renderer_->set_show_dropouts(show);
  }
}

bool RenderPresenter::getShowDropouts() const {
  if (impl_->preview_renderer_) {
    return impl_->preview_renderer_->get_show_dropouts();
  }
  return false;
}

RenderPresenter::ImageToFieldMapping RenderPresenter::mapImageToField(
    NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
    int image_y, int image_height, const std::string& option_id) {
  if (!impl_->preview_renderer_) {
    return {false, 0, 0};
  }

  auto result = impl_->preview_renderer_->map_image_to_field(
      node_id, output_type, output_index, image_y, image_height, option_id);

  return {result.is_valid, result.field_index, result.field_line};
}

RenderPresenter::FieldToImageMapping RenderPresenter::mapFieldToImage(
    NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
    uint64_t field_index, int field_line, int image_height,
    const std::string& option_id) {
  if (!impl_->preview_renderer_) {
    return {false, 0};
  }

  auto result = impl_->preview_renderer_->map_field_to_image(
      node_id, output_type, output_index, field_index, field_line, image_height,
      option_id);

  return {result.is_valid, result.image_y};
}

RenderPresenter::FrameFields RenderPresenter::getFrameFields(
    NodeID node_id, uint64_t frame_index) {
  if (!impl_->preview_renderer_) {
    return {false, 0, 0};
  }

  auto result =
      impl_->preview_renderer_->get_frame_fields(node_id, frame_index);
  return {result.is_valid, result.first_field, result.second_field};
}

RenderPresenter::FrameLineNavigation RenderPresenter::navigateFrameLine(
    NodeID node_id, orc::PreviewOutputType output_type, uint64_t current_field,
    int current_line, int direction, int field_height) {
  if (!impl_->preview_renderer_) {
    return {false, 0, 0};
  }

  auto result = impl_->preview_renderer_->navigate_frame_line(
      node_id, output_type, current_field, current_line, direction,
      field_height);

  return {result.is_valid, result.new_field_index, result.new_line_number};
}

RenderPresenter::LineSampleData RenderPresenter::getLineSamplesWithYC(
    NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
    int line_number, int /*sample_x*/, int /*preview_width*/) {
  LineSampleData result;
  result.has_separate_channels = false;

  if (!impl_->preview_renderer_) {
    return result;
  }

  if (output_type != orc::PreviewOutputType::Frame_Field1 &&
      output_type != orc::PreviewOutputType::Frame_Field2) {
    return result;
  }

  try {
    auto repr = impl_->preview_renderer_->get_representation_at_node(node_id);
    if (!repr) {
      return result;
    }

    orc::FrameID frame_id = static_cast<orc::FrameID>(output_index / 2);
    int field_within_frame = static_cast<int>(output_index % 2);

    auto descriptor = repr->get_frame_descriptor(frame_id);
    if (!descriptor) {
      return result;
    }

    size_t f1_lines = (descriptor->system == orc::VideoSystem::PAL)
                          ? static_cast<size_t>(orc::kPalField1Lines)
                          : static_cast<size_t>(orc::kNtscField1Lines);
    size_t field_height =
        (field_within_frame == 0) ? f1_lines : (descriptor->height - f1_lines);
    size_t field_line_offset = (field_within_frame == 0) ? 0 : f1_lines;

    if (line_number < 0 || static_cast<size_t>(line_number) >= field_height) {
      return result;
    }

    size_t frame_line = field_line_offset + static_cast<size_t>(line_number);
    size_t width = descriptor->samples_per_line_nominal;

    result.has_separate_channels = repr->has_separate_channels();

    if (result.has_separate_channels) {
      const auto* y_data = repr->get_line_luma(frame_id, frame_line);
      if (y_data) {
        result.y_samples.assign(y_data, y_data + width);
      }
      const auto* c_data = repr->get_line_chroma(frame_id, frame_line);
      if (c_data) {
        result.c_samples.assign(c_data, c_data + width);
      }
      result.composite_samples = result.y_samples;
    } else {
      const auto* line_data = repr->get_line(frame_id, frame_line);
      if (!line_data) {
        return result;
      }
      result.composite_samples.assign(line_data, line_data + width);
    }

    return result;

  } catch (const std::exception&) {
    return result;
  }
}

RenderPresenter::LineSampleData RenderPresenter::getFieldSamplesForTiming(
    NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index) {
  LineSampleData result;
  result.has_separate_channels = false;
  result.first_field_height = 0;
  result.second_field_height = 0;

  if (!impl_->preview_renderer_) {
    return result;
  }

  try {
    auto repr = impl_->preview_renderer_->get_representation_at_node(node_id);
    if (!repr) {
      return result;
    }

    result.has_separate_channels = repr->has_separate_channels();

    auto collect_lines =
        [&](orc::FrameID frame_id, size_t line_offset, size_t line_count,
            size_t spl, std::vector<int16_t>& composite,
            std::vector<int16_t>& y_out, std::vector<int16_t>& c_out) {
          for (size_t l = 0; l < line_count; ++l) {
            size_t fl = line_offset + l;
            if (result.has_separate_channels) {
              const auto* y = repr->get_line_luma(frame_id, fl);
              const auto* c = repr->get_line_chroma(frame_id, fl);
              if (y) y_out.insert(y_out.end(), y, y + spl);
              if (c) c_out.insert(c_out.end(), c, c + spl);
            } else {
              const auto* d = repr->get_line(frame_id, fl);
              if (d) composite.insert(composite.end(), d, d + spl);
            }
          }
        };

    const bool is_single_field =
        (output_type == orc::PreviewOutputType::Frame_Field1 ||
         output_type == orc::PreviewOutputType::Frame_Field2 ||
         output_type == orc::PreviewOutputType::Luma);
    const bool is_frame_type =
        (output_type == orc::PreviewOutputType::Frame_Field1_First ||
         output_type == orc::PreviewOutputType::Frame_Reversed ||
         output_type == orc::PreviewOutputType::Split);

    if (is_single_field) {
      orc::FrameID frame_id = static_cast<orc::FrameID>(output_index / 2);
      int field_within_frame = static_cast<int>(output_index % 2);

      auto desc = repr->get_frame_descriptor(frame_id);
      if (!desc) {
        return result;
      }

      size_t f1_lines = (desc->system == orc::VideoSystem::PAL)
                            ? static_cast<size_t>(orc::kPalField1Lines)
                            : static_cast<size_t>(orc::kNtscField1Lines);
      size_t field_height =
          (field_within_frame == 0) ? f1_lines : (desc->height - f1_lines);
      size_t field_offset = (field_within_frame == 0) ? 0 : f1_lines;
      result.first_field_height = static_cast<int>(field_height);

      collect_lines(frame_id, field_offset, field_height,
                    desc->samples_per_line_nominal, result.composite_samples,
                    result.y_samples, result.c_samples);

    } else if (is_frame_type) {
      orc::FrameID frame_id = static_cast<orc::FrameID>(output_index);

      auto desc = repr->get_frame_descriptor(frame_id);
      if (!desc) {
        return result;
      }

      size_t f1_lines = (desc->system == orc::VideoSystem::PAL)
                            ? static_cast<size_t>(orc::kPalField1Lines)
                            : static_cast<size_t>(orc::kNtscField1Lines);
      size_t f2_lines = desc->height - f1_lines;
      size_t spl = desc->samples_per_line_nominal;

      size_t field1_offset = 0;
      size_t field1_height = f1_lines;
      size_t field2_offset = f1_lines;
      size_t field2_height = f2_lines;

      if (output_type == orc::PreviewOutputType::Frame_Reversed) {
        std::swap(field1_offset, field2_offset);
        std::swap(field1_height, field2_height);
      }

      result.first_field_height = static_cast<int>(field1_height);
      result.second_field_height = static_cast<int>(field2_height);

      collect_lines(frame_id, field1_offset, field1_height, spl,
                    result.composite_samples, result.y_samples,
                    result.c_samples);
      std::vector<int16_t> comp2, y2, c2;
      collect_lines(frame_id, field2_offset, field2_height, spl, comp2, y2, c2);

      result.composite_samples.insert(result.composite_samples.end(),
                                      comp2.begin(), comp2.end());
      result.y_samples.insert(result.y_samples.end(), y2.begin(), y2.end());
      result.c_samples.insert(result.c_samples.end(), c2.begin(), c2.end());
    }

    // No composite copy of the luma here: LineSampleData::composite()
    // substitutes y_samples for a Y/C source, which saves duplicating a whole
    // frame of samples on every extraction.
    return result;

  } catch (const std::exception&) {
    return result;
  }
}

std::optional<orc::SourceParameters> RenderPresenter::getVideoParameters(
    NodeID node_id) {
  if (!impl_->preview_renderer_) {
    return std::nullopt;
  }

  try {
    auto repr = impl_->preview_renderer_->get_representation_at_node(node_id);
    if (!repr) {
      return std::nullopt;
    }

    return repr->get_video_parameters();

  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::vector<std::string> RenderPresenter::getAudioChannelPairNames(
    NodeID node_id) {
  std::vector<std::string> names;
  if (!impl_->preview_renderer_) {
    return names;
  }

  try {
    auto repr = impl_->preview_renderer_->get_representation_at_node(node_id);
    if (!repr) {
      return names;
    }
    const size_t count = repr->audio_channel_pair_count();
    names.reserve(count);
    for (size_t p = 0; p < count; ++p) {
      auto desc = repr->get_audio_channel_pair_descriptor(p);
      names.push_back(desc ? desc->name : std::string());
    }
  } catch (const std::exception&) {
    names.clear();
  }
  return names;
}

// Both audio accessors resolve the node's representation exactly as
// getAudioChannelPairNames() does, so they inherit the upstream BFS fallback in
// get_representation_at_node(): the pair list and the samples always come from
// the same resolved representation, even when that is an ancestor node's.
std::vector<orc::AudioPairView> RenderPresenter::getAudioChannelPairs(
    NodeID node_id) {
  if (!impl_->preview_renderer_) {
    return {};
  }

  try {
    auto repr = impl_->preview_renderer_->get_representation_at_node(node_id);
    if (!repr) {
      return {};
    }
    return enumerate_audio_channel_pairs(*repr);
  } catch (const std::exception&) {
    return {};
  }
}

std::shared_ptr<IAudioStreamReader> RenderPresenter::createAudioStreamReader(
    NodeID node_id, size_t pair) {
  if (!impl_->preview_renderer_) {
    return nullptr;
  }

  try {
    auto repr = impl_->preview_renderer_->get_representation_at_node(node_id);
    return make_audio_stream_reader(std::move(repr), pair);
  } catch (const std::exception&) {
    return nullptr;
  }
}

// === Analysis Data Access ===

namespace {

// Locate a DAG node by id, returning nullptr if absent.
const orc::DAGNode* find_node(const orc::DAG& dag, NodeID node_id) {
  for (const auto& node : dag.nodes()) {
    if (node.node_id == node_id) {
      return &node;
    }
  }
  return nullptr;
}

}  // namespace

std::optional<DropoutDisplaySeries> RenderPresenter::getDropoutAnalysisData(
    NodeID node_id) {
  auto dag = impl_->getConcreteDAG();
  if (!dag) {
    return std::nullopt;
  }

  const orc::DAGNode* target_node = find_node(*dag, node_id);
  if (!target_node) {
    return std::nullopt;
  }

  auto* sink =
      dynamic_cast<orc::IDropoutAnalysisResults*>(target_node->stage.get());
  if (!sink || !sink->has_results()) {
    return std::nullopt;
  }

  // The sink exposes the full-resolution per-frame series; decimate it to the
  // display point budget before handing it to the graph as a typed view series.
  DropoutDisplaySeries series;
  series.points =
      decimate_dropout_series(sink->frame_stats(), Impl::kDisplayPoints);
  series.total_frames = sink->total_frames();
  series.decimated = series_is_decimated(series.points);
  return series;
}

std::optional<SNRDisplaySeries> RenderPresenter::getSNRAnalysisData(
    NodeID node_id) {
  auto dag = impl_->getConcreteDAG();
  if (!dag) {
    return std::nullopt;
  }

  const orc::DAGNode* target_node = find_node(*dag, node_id);
  if (!target_node) {
    return std::nullopt;
  }

  auto* sink =
      dynamic_cast<orc::ISNRAnalysisResults*>(target_node->stage.get());
  if (!sink || !sink->has_results()) {
    return std::nullopt;
  }

  SNRDisplaySeries series;
  series.points =
      decimate_snr_series(sink->frame_stats(), Impl::kDisplayPoints);
  series.total_frames = sink->total_frames();
  series.decimated = series_is_decimated(series.points);
  return series;
}

std::optional<BurstLevelDisplaySeries>
RenderPresenter::getBurstLevelAnalysisData(NodeID node_id) {
  auto dag = impl_->getConcreteDAG();
  if (!dag) {
    return std::nullopt;
  }

  const orc::DAGNode* target_node = find_node(*dag, node_id);
  if (!target_node) {
    return std::nullopt;
  }

  auto* sink =
      dynamic_cast<orc::IBurstLevelAnalysisResults*>(target_node->stage.get());
  if (!sink || !sink->has_results()) {
    return std::nullopt;
  }

  BurstLevelDisplaySeries series;
  series.points =
      decimate_burst_series(sink->frame_stats(), Impl::kDisplayPoints);
  series.total_frames = sink->total_frames();
  series.decimated = series_is_decimated(series.points);
  return series;
}

std::optional<orc::CatalogueDataset> RenderPresenter::getCatalogueData(
    NodeID node_id, const std::string& view_option,
    const std::optional<std::vector<std::string>>& active_toggles) {
  auto dag = impl_->getConcreteDAG();
  if (!dag) {
    return std::nullopt;
  }

  const orc::DAGNode* target_node = find_node(*dag, node_id);
  if (!target_node) {
    return std::nullopt;
  }

  auto* browser =
      dynamic_cast<orc::ICatalogueResults*>(target_node->stage.get());
  if (!browser || !browser->has_results()) {
    return std::nullopt;
  }

  // Handed over whole: the stage has already bounded the catalogue by its own
  // cap, and building the payloads is the stage's work rather than something
  // the host repeats per item. The view option and the toggles go to the stage
  // untouched — what they mean is the stage's business, and asking for them is
  // a rebuild of the payloads rather than anything the DAG runs again.
  //
  // A reader who has not been asked yet gets the stage's own defaults rather
  // than every toggle off: the schema that comes back says which ones those
  // are, and the viewer shows them switched on accordingly. Asking with an
  // empty list instead would mean the stage could never offer a toggle that
  // starts on.
  if (!active_toggles) {
    return browser->catalogue(view_option);
  }
  return browser->catalogue(view_option, *active_toggles);
}

std::shared_ptr<const void> RenderPresenter::executeToNode(NodeID node_id) {
  if (!impl_->getConcreteDAG()) {
    return nullptr;
  }

  try {
    orc::DAGExecutor executor;
    auto node_outputs =
        executor.execute_to_node(*impl_->getConcreteDAG(), node_id);

    auto it = node_outputs.find(node_id);
    if (it != node_outputs.end() && !it->second.empty()) {
      // Return the first output (typically VideoFrameRepresentation)
      return std::static_pointer_cast<const void>(it->second[0]);
    }
  } catch (const std::exception&) {
    return nullptr;
  }

  return nullptr;
}

const void* RenderPresenter::getObservationContext(NodeID node_id,
                                                   FieldID field_id) {
  if (!impl_->field_renderer_) {
    return nullptr;
  }

  orc::FrameID frame_id = static_cast<orc::FrameID>(field_id.value() / 2);
  impl_->field_renderer_->render_frame_at_node(node_id, frame_id);

  return &impl_->field_renderer_->get_observation_context();
}

}  // namespace orc::presenters
