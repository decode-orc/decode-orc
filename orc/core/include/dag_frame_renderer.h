/*
 * File:        dag_frame_renderer.h
 * Module:      orc-core
 * Purpose:     Frame rendering at DAG nodes using VideoFrameRepresentation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/frame_id.h>
#include <orc/stage/node_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/observation/observation_service_interface.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/lru_cache.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core_observation_service.h"
#include "dag_executor.h"
#include "observation_store.h"

namespace orc {

// Observation namespace/key marking a padding frame's fields (issue #77).
// Padding frames carry no measurable signal, so the observer pass does not run
// observers over them; instead every observer's record for both fields is an
// explicit "padded, no data" marker holding just this key. Consumers use it to
// distinguish padding from a field that was observed and yielded nothing.
inline constexpr char kPaddingObservationNamespace[] = "padding";
inline constexpr char kPaddingObservationKey[] = "is_pad";

// Run the standard observer pass for one frame, with optional read-through
// caching against an ObservationStore keyed by the node's provenance.
//
// For each observer the two derived field records (frame_id*2 and frame_id*2+1)
// are considered:
//   - When both @p fingerprint and @p store are non-null and both records are
//     present, the stored values are loaded into @p context and the observer is
//     not run.
//   - Otherwise the observer runs; when caching is enabled its output for both
//     fields is written back to the store (an empty record is stored too, so a
//     later probe still hits).
//
// Padding frames (FrameDescriptor::is_padding_frame) skip the observers
// entirely: the context and, when caching is enabled, the store receive
// explicit "padded, no data" records (kPaddingObservationNamespace /
// kPaddingObservationKey = true) for every observer, replacing any stale
// record that measured the synthetic padding content.
//
// Observers structurally inapplicable to the source's video system (per
// standard_observer_applies(), e.g. fm_code outside NTSC) are skipped
// outright — no run, no store probes and no records, padding markers
// included. Coverage probes must filter by the same predicate; see
// filter_applicable_observers().
//
// When either @p fingerprint or @p store is null the observers always run and
// nothing is stored — behaviour identical to the pre-store renderer.
//
// Thread safety: reentrant with respect to distinct @p context objects; @p
// store is the sole shared state and is internally synchronised.
void run_frame_observer_pass(const IObservationService& service,
                             const std::vector<ObserverInfo>& observers,
                             const VideoFrameRepresentation& representation,
                             FrameID frame_id,
                             const NodeFingerprint* fingerprint,
                             ObservationStore* store,
                             IObservationContext& context);

// Alias-aware variant: @p fingerprints lists every provenance the frame's
// content is known under — the observed node's own fingerprint first, followed
// by the fingerprints of upstream nodes the frame passes through byte-identical
// (per VideoFrameRepresentation::video_passthrough_source()). All entries key
// the same content, so:
//   - A store hit under ANY listed fingerprint satisfies the pass without
//     running the observer, and the records are copied to every other listed
//     fingerprint still missing them (so future lookups keyed to any node in
//     the pass-through chain hit directly — including across sessions via the
//     persistence sidecar).
//   - On a miss everywhere the observer runs once and its records are stored
//     under every listed fingerprint.
// An empty @p fingerprints (or null @p store) disables caching entirely.
void run_frame_observer_pass(const IObservationService& service,
                             const std::vector<ObserverInfo>& observers,
                             const VideoFrameRepresentation& representation,
                             FrameID frame_id,
                             const std::vector<NodeFingerprint>& fingerprints,
                             ObservationStore* store,
                             IObservationContext& context);

// ---------------------------------------------------------------------------
// Node output resolution
// ---------------------------------------------------------------------------

// Why resolve_node_vfr() did or did not find a representation.
enum class NodeVfrResolution {
  kOk,                   // representation is set
  kNoOutput,             // a non-sink node produced no output at all
  kSinkWithoutUpstream,  // a sink whose edge names no usable upstream VFR
  kNotRepresentation,    // the located artifact is not a VFR
};

struct ResolvedNodeVfr {
  NodeVfrResolution status = NodeVfrResolution::kNoOutput;
  VideoFrameRepresentationPtr representation;
  // The node the artifact actually came from — the queried node, unless a sink
  // substitution took the upstream one.
  NodeID source_node;
  std::size_t output_index = 0;
  bool substituted_upstream = false;
};

// Locates the VideoFrameRepresentation carrying @p node_id's frames within
// @p node_outputs (as returned by DAGExecutor::execute_to_node).
//
// A sink stage produces no output by design, so this substitutes the upstream
// output the sink's edge actually names — output 0 is not necessarily the one
// the sink consumes, and substituting it would resolve a different branch than
// the sink is fed. Every caller that needs a node's representation goes through
// here, so a metadata query and a preview can never resolve different
// artifacts. Resolution is pure map lookup: it renders no frame and runs no
// observers.
ResolvedNodeVfr resolve_node_vfr(
    const DAG& dag,
    const std::map<NodeID, std::vector<ArtifactPtr>>& node_outputs,
    NodeID node_id);

// Exception thrown during DAG frame rendering.
class DAGFrameRenderError : public std::runtime_error {
 public:
  explicit DAGFrameRenderError(const std::string& msg)
      : std::runtime_error(msg) {}
};

// Result of rendering a specific frame at a DAG node.
struct FrameRenderResult {
  // The VideoFrameRepresentation produced at the specified node.
  // Contains all frames from the source; callers address individual frames by
  // FrameID through the VFR API.
  VideoFrameRepresentationPtr representation;

  // True if the result is valid and can be used.
  bool is_valid = false;

  // Error message when is_valid is false (empty when valid).
  std::string error_message;

  // The node that was rendered.
  NodeID node_id;

  // The frame that was requested.
  FrameID frame_id = 0;

  // True if this result was returned from the LRU cache.
  bool from_cache = false;
};

// DAGFrameRenderer — on-demand frame access at arbitrary DAG nodes.
//
// Executes the DAG up to a specified node and returns the
// VideoFrameRepresentation produced there, keyed by FrameID in an LRU cache.
// Consumers address individual frames through the VFR API.
//
// Replaces DAGFieldRenderer for the CVBS_U10_4FSC pipeline.
//
// Thread safety: not thread-safe. Use from a single thread only.
class DAGFrameRenderer {
 public:
  // Construct a renderer for the given DAG.  Throws DAGFrameRenderError if dag
  // is null or invalid.
  explicit DAGFrameRenderer(std::shared_ptr<const DAG> dag);

  ~DAGFrameRenderer() = default;

  // Non-copyable, non-movable (contains stateful executor and cache).
  DAGFrameRenderer(const DAGFrameRenderer&) = delete;
  DAGFrameRenderer& operator=(const DAGFrameRenderer&) = delete;
  DAGFrameRenderer(DAGFrameRenderer&&) = delete;
  DAGFrameRenderer& operator=(DAGFrameRenderer&&) = delete;

  // -------------------------------------------------------------------------
  // Frame Rendering API
  // -------------------------------------------------------------------------

  // Render the representation at node_id and return it, verifying that
  // frame_id is present.  Results are LRU-cached; calling update_dag()
  // invalidates all cached entries.
  //
  // The representation is NOT executed per-frame: the whole source runs once
  // and the VFR is cached.  Individual frames are accessed by FrameID through
  // the representation's get_frame() / get_line() API.
  FrameRenderResult render_frame_at_node(NodeID node_id, FrameID frame_id);

  // True if node_id exists in the current DAG.
  bool has_node(NodeID node_id) const;

  // Video parameters of the representation produced at @p node_id. Executes
  // the DAG structurally (artifact graph only — representations are lazy): no
  // frame samples are decoded and no observers run. Returns nullopt when the
  // node does not exist, produces no video representation, or execution
  // fails. Used to decide observer applicability before any render.
  std::optional<SourceParameters> get_video_parameters_at_node(NodeID node_id);

  // -------------------------------------------------------------------------
  // DAG Change Tracking
  // -------------------------------------------------------------------------

  // Replace the DAG and clear all cached render results.  Increments the
  // internal DAG version so stale FrameRenderResult objects are detectable.
  void update_dag(std::shared_ptr<const DAG> new_dag);

  // Returns the observation context populated during the most recent
  // render_frame_at_node() execution.
  const ObservationContext& get_observation_context() const;

  // Attach a shared, provenance-keyed ObservationStore and the fingerprint map
  // for the current DAG. When both are set, the observer pass reads
  // observations through the store (skipping observers whose records are
  // already present) and writes fresh results back. Pass nulls to disable
  // read-through caching. The store is owned externally so it can outlive
  // individual renderers and survive DAG rebuilds; the fingerprint map must
  // match the renderer's current DAG. The same store keeps the measurements
  // of stages that keep them (see DAGExecutor::set_measurement_store()).
  void set_observation_store(
      std::shared_ptr<ObservationStore> store,
      std::shared_ptr<const NodeFingerprintMap> fingerprints);

  // Take node outputs from @p shared, produced once for every private copy of
  // the same DAG (see SharedNodeOutputs). @p shared must be for the DAG this
  // renderer renders a copy of; update_dag() drops it, so set it again after
  // adopting a new DAG.
  void set_shared_node_outputs(std::shared_ptr<SharedNodeOutputs> shared);

  // Give stages that keep measurements @p store, without engaging the
  // observer pass's read-through (set_observation_store() does both).
  // @p fingerprints must match the current DAG; update_dag() drops both.
  void set_measurement_store(
      std::shared_ptr<ObservationStore> store,
      std::shared_ptr<const NodeFingerprintMap> fingerprints);

  // Override the observation service used by the observer pass (for testing
  // with a spy/mock). Passing nullptr restores the default host
  // CoreObservationService. Re-enumerates the observer inventory from the
  // active service.
  void set_observation_service(std::shared_ptr<IObservationService> service);

  // -------------------------------------------------------------------------
  // Cache Management
  // -------------------------------------------------------------------------

  // Discard all cached render results without incrementing the DAG version.
  void clear_cache();

  // Enable or disable result caching.  When disabled every call re-executes
  // the DAG.
  void set_cache_enabled(bool enabled) { cache_enabled_ = enabled; }
  bool is_cache_enabled() const { return cache_enabled_; }

 private:
  std::shared_ptr<const DAG> dag_;
  uint64_t dag_version_;
  bool cache_enabled_;

  // Cache key: (node_id, frame_id value, dag_version)
  struct CacheKey {
    NodeID node_id;
    uint64_t frame_id_value;
    uint64_t dag_version;

    bool operator==(const CacheKey& o) const noexcept {
      return dag_version == o.dag_version && node_id == o.node_id &&
             frame_id_value == o.frame_id_value;
    }
  };

  struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const noexcept {
      std::size_t h1 = std::hash<NodeID>{}(k.node_id);
      std::size_t h2 = std::hash<uint64_t>{}(k.frame_id_value);
      std::size_t h3 = std::hash<uint64_t>{}(k.dag_version);
      return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
  };

  static constexpr size_t kMaxCachedRenders = 500;
  LRUCache<CacheKey, FrameRenderResult, CacheKeyHash> render_cache_;

  std::unique_ptr<DAGExecutor> executor_;

  // Host-owned observation service backing the standard observer pass. The
  // registry it enumerates (see CoreObservationService) is the single source
  // of truth for observer identity, so adding an observer to the registry
  // extends the frame-render pass with no edit here. observers_ caches the
  // enumeration (id + version) so available_observers() (which constructs each
  // observer) runs once rather than per rendered frame.
  CoreObservationService default_observation_service_;
  std::shared_ptr<IObservationService> observation_service_override_;
  std::vector<ObserverInfo> observers_;

  // Optional provenance-keyed read-through cache. Both must be set for caching
  // to engage; either being null disables it (renderer behaves as before).
  std::shared_ptr<ObservationStore> observation_store_;
  std::shared_ptr<const NodeFingerprintMap> node_fingerprints_;

  // The observation service currently in effect (override if set, else
  // default).
  const IObservationService& observation_service() const {
    if (observation_service_override_) {
      return *observation_service_override_;
    }
    return default_observation_service_;
  }

  // (Re)populate observers_ from the active observation service.
  void refresh_observers();

  mutable std::map<NodeID, size_t> node_index_;
  mutable bool node_index_valid_;

  void ensure_node_index() const;

  FrameRenderResult execute_to_node(NodeID node_id, FrameID frame_id);
};

}  // namespace orc
