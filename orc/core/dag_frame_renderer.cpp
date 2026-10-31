/*
 * File:        dag_frame_renderer.cpp
 * Module:      orc-core
 * Purpose:     Frame rendering at DAG nodes using VideoFrameRepresentation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "dag_frame_renderer.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <sstream>

namespace orc {

namespace {

// Derived field ids for a frame: top field = frame*2, bottom field = frame*2+1.
// Matches Observer::process_frame() (frame_id * 2 + field_idx).
constexpr FieldID::value_type kFieldsPerFrame = 2;

// Copy every namespaced value stored for one field of a scratch context into
// the destination context.
void merge_field(const IObservationContext& from, FieldID field_id,
                 IObservationContext& into) {
  for (const auto& [ns, keys] : from.get_all_observations(field_id)) {
    for (const auto& [key, value] : keys) {
      into.set(field_id, ns, key, value);
    }
  }
}

}  // namespace

void run_frame_observer_pass(const IObservationService& service,
                             const std::vector<ObserverInfo>& observers,
                             const VideoFrameRepresentation& representation,
                             FrameID frame_id,
                             const NodeFingerprint* fingerprint,
                             ObservationStore* store,
                             IObservationContext& context) {
  std::vector<NodeFingerprint> fingerprints;
  if (fingerprint != nullptr) {
    fingerprints.push_back(*fingerprint);
  }
  run_frame_observer_pass(service, observers, representation, frame_id,
                          fingerprints, store, context);
}

void run_frame_observer_pass(const IObservationService& service,
                             const std::vector<ObserverInfo>& observers,
                             const VideoFrameRepresentation& representation,
                             FrameID frame_id,
                             const std::vector<NodeFingerprint>& fingerprints,
                             ObservationStore* store,
                             IObservationContext& context) {
  const bool caching = !fingerprints.empty() && (store != nullptr);

  const FieldID field_top(frame_id * kFieldsPerFrame);
  const FieldID field_bottom(frame_id * kFieldsPerFrame + 1);

  // Observers structurally inapplicable to this source (e.g.
  // fm_code/white_flag on PAL) are skipped outright: no run, no store
  // probes, no records. Every coverage probe (runner fast path, presenter
  // store_has_frame/sweep markers) filters by the same predicate, so the
  // absent records never read as missing coverage. Applicability fails open
  // when the representation carries no video parameters.
  const auto video_params = representation.get_video_parameters();
  const auto observer_applies = [&video_params](const ObserverInfo& observer) {
    return !video_params.has_value() ||
           standard_observer_applies(observer.id, *video_params);
  };

  // Padding frames (frame_map / source_align gap fill, or source fields the
  // TBC metadata marks as pad) carry no measurable signal, so running the
  // observers over their synthetic content would store spurious measurements
  // (issue #77). Instead each observer gets an explicit "padded, no data"
  // record: sweep-completeness probes still hit, and consumers can tell
  // padding apart from a field that was observed and yielded nothing. Any
  // stored record with different content predates the padding skip (it holds
  // measurements of the synthetic content) and is overwritten; an up-to-date
  // marker is left untouched so re-renders do not re-queue sidecar writes.
  const auto descriptor = representation.get_frame_descriptor(frame_id);
  if (descriptor && descriptor->is_padding_frame) {
    for (const FieldID field : {field_top, field_bottom}) {
      context.set(field, kPaddingObservationNamespace, kPaddingObservationKey,
                  true);
    }
    if (caching) {
      ObservationRecord pad_record;
      pad_record[kPaddingObservationNamespace][kPaddingObservationKey] = true;
      for (const auto& observer : observers) {
        if (!observer_applies(observer)) {
          continue;
        }
        for (const auto& fingerprint : fingerprints) {
          for (const FieldID field : {field_top, field_bottom}) {
            const ObservationRecordKey key{fingerprint, field, observer.id,
                                           observer.version};
            const auto existing = store->get(key);
            if (!existing || *existing != pad_record) {
              store->put(key, pad_record);
            }
          }
        }
      }
    }
    return;
  }

  for (const auto& observer : observers) {
    if (!observer_applies(observer)) {
      continue;
    }
    if (!caching) {
      // No provenance available: run directly into the target context, exactly
      // as the renderer did before the store existed.
      service.run_observer(observer.id, representation, frame_id, context);
      continue;
    }

    const auto key_at = [&](std::size_t i, FieldID field) {
      return ObservationRecordKey{fingerprints[i], field, observer.id,
                                  observer.version};
    };

    // Every listed fingerprint keys identical content (own node first, then
    // pass-through aliases): a hit under any of them satisfies the pass.
    std::size_t hit = fingerprints.size();
    for (std::size_t i = 0; i < fingerprints.size(); ++i) {
      if (store->has(key_at(i, field_top)) &&
          store->has(key_at(i, field_bottom))) {
        hit = i;
        break;
      }
    }

    if (hit < fingerprints.size()) {
      // Hit: load stored values instead of re-running the observer, then copy
      // the records to every alias still missing them so future lookups keyed
      // to any node in the pass-through chain hit directly.
      const ObservationRecordKey hit_top = key_at(hit, field_top);
      const ObservationRecordKey hit_bottom = key_at(hit, field_bottom);
      store->load_into(hit_top, context);
      store->load_into(hit_bottom, context);
      if (fingerprints.size() > 1) {
        const auto rec_top = store->get(hit_top);
        const auto rec_bottom = store->get(hit_bottom);
        for (std::size_t i = 0; i < fingerprints.size(); ++i) {
          if (i == hit) {
            continue;
          }
          if (rec_top && !store->has(key_at(i, field_top))) {
            store->put(key_at(i, field_top), *rec_top);
          }
          if (rec_bottom && !store->has(key_at(i, field_bottom))) {
            store->put(key_at(i, field_bottom), *rec_bottom);
          }
        }
      }
      continue;
    }

    // Miss everywhere: run the observer into a scratch context so we can
    // isolate exactly what this observer wrote for each field, then merge into
    // the target and persist per-field records under every listed fingerprint
    // (empty records included, so a later probe hits).
    ObservationContext scratch;
    service.run_observer(observer.id, representation, frame_id, scratch);

    merge_field(scratch, field_top, context);
    merge_field(scratch, field_bottom, context);

    const auto rec_top = scratch.get_all_observations(field_top);
    const auto rec_bottom = scratch.get_all_observations(field_bottom);
    for (std::size_t i = 0; i < fingerprints.size(); ++i) {
      store->put(key_at(i, field_top), rec_top);
      store->put(key_at(i, field_bottom), rec_bottom);
    }
  }
}

DAGFrameRenderer::DAGFrameRenderer(std::shared_ptr<const DAG> dag)
    : dag_(std::move(dag)),
      dag_version_(1),
      cache_enabled_(true),
      render_cache_(kMaxCachedRenders),
      node_index_valid_(false) {
  if (!dag_) {
    throw DAGFrameRenderError("Cannot create renderer with null DAG");
  }

  if (!dag_->validate()) {
    auto errors = dag_->get_validation_errors();
    std::ostringstream oss;
    oss << "Cannot create renderer with invalid DAG:\n";
    for (const auto& error : errors) {
      oss << "  - " << error << "\n";
    }
    throw DAGFrameRenderError(oss.str());
  }

  executor_ = std::make_unique<DAGExecutor>();
  executor_->set_cache_enabled(true);

  // Cache the observer enumeration once; the registry is fixed at build time,
  // so update_dag() need not recompute it.
  refresh_observers();
}

void DAGFrameRenderer::refresh_observers() {
  observers_ = observation_service().available_observers();
}

void DAGFrameRenderer::set_observation_store(
    std::shared_ptr<ObservationStore> store,
    std::shared_ptr<const NodeFingerprintMap> fingerprints) {
  observation_store_ = std::move(store);
  node_fingerprints_ = std::move(fingerprints);
  executor_->set_measurement_store(observation_store_, node_fingerprints_);
}

void DAGFrameRenderer::set_measurement_store(
    std::shared_ptr<ObservationStore> store,
    std::shared_ptr<const NodeFingerprintMap> fingerprints) {
  executor_->set_measurement_store(std::move(store), std::move(fingerprints));
}

void DAGFrameRenderer::set_shared_node_outputs(
    std::shared_ptr<SharedNodeOutputs> shared) {
  executor_->set_shared_node_outputs(std::move(shared));
}

void DAGFrameRenderer::set_observation_service(
    std::shared_ptr<IObservationService> service) {
  observation_service_override_ = std::move(service);
  refresh_observers();
}

void DAGFrameRenderer::ensure_node_index() const {
  if (!node_index_valid_) {
    node_index_ = dag_->build_node_index();
    node_index_valid_ = true;
  }
}

bool DAGFrameRenderer::has_node(NodeID node_id) const {
  ensure_node_index();
  return node_index_.find(node_id) != node_index_.end();
}

std::optional<SourceParameters> DAGFrameRenderer::get_video_parameters_at_node(
    NodeID node_id) {
  if (!has_node(node_id)) {
    return std::nullopt;
  }
  try {
    auto node_outputs = executor_->execute_to_node(*dag_, node_id);
    const ResolvedNodeVfr resolved =
        resolve_node_vfr(*dag_, node_outputs, node_id);
    if (resolved.status != NodeVfrResolution::kOk) {
      return std::nullopt;
    }
    return resolved.representation->get_video_parameters();
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

void DAGFrameRenderer::update_dag(std::shared_ptr<const DAG> new_dag) {
  if (!new_dag) {
    throw DAGFrameRenderError("Cannot update to null DAG");
  }

  if (!new_dag->validate()) {
    auto errors = new_dag->get_validation_errors();
    std::ostringstream oss;
    oss << "Cannot update to invalid DAG:\n";
    for (const auto& error : errors) {
      oss << "  - " << error << "\n";
    }
    throw DAGFrameRenderError(oss.str());
  }

  dag_ = std::move(new_dag);
  ++dag_version_;
  node_index_valid_ = false;
  render_cache_.clear();

  // The fingerprint map describes the previous DAG; drop it so stale
  // fingerprints can never key a lookup. The store is retained (it is content-
  // addressed and shared) and read-through re-engages once the caller supplies
  // a fresh map via set_observation_store().
  node_fingerprints_.reset();

  executor_ = std::make_unique<DAGExecutor>();
  executor_->set_cache_enabled(true);
}

void DAGFrameRenderer::clear_cache() { render_cache_.clear(); }

FrameRenderResult DAGFrameRenderer::render_frame_at_node(NodeID node_id,
                                                         FrameID frame_id) {
  ORC_LOG_TRACE("DAGFrameRenderer: render_frame_at_node node='{}' frame={}",
                node_id.to_string(), frame_id);

  if (!has_node(node_id)) {
    ORC_LOG_ERROR("DAGFrameRenderer: node '{}' does not exist",
                  node_id.to_string());
    FrameRenderResult err;
    err.is_valid = false;
    err.error_message =
        "Node '" + node_id.to_string() + "' does not exist in DAG";
    err.node_id = node_id;
    err.frame_id = frame_id;
    err.from_cache = false;
    return err;
  }

  if (cache_enabled_) {
    CacheKey key{node_id, frame_id, dag_version_};
    auto cached = render_cache_.get(key);
    if (cached.has_value()) {
      ORC_LOG_TRACE("DAGFrameRenderer: cache hit node='{}' frame={}",
                    node_id.to_string(), frame_id);
      cached->from_cache = true;
      return *cached;
    }
    ORC_LOG_DEBUG("DAGFrameRenderer: cache miss node='{}' frame={}",
                  node_id.to_string(), frame_id);
  }

  auto result = execute_to_node(node_id, frame_id);

  if (cache_enabled_ && result.is_valid) {
    CacheKey key{node_id, frame_id, dag_version_};
    render_cache_.put(key, result);
  }

  return result;
}

ResolvedNodeVfr resolve_node_vfr(
    const DAG& dag,
    const std::map<NodeID, std::vector<ArtifactPtr>>& node_outputs,
    NodeID node_id) {
  ResolvedNodeVfr resolved;
  resolved.source_node = node_id;

  auto it = node_outputs.find(node_id);
  std::size_t output_index = 0;

  if (it == node_outputs.end() || it->second.empty()) {
    const auto& dag_nodes = dag.nodes();
    auto dag_it = std::find_if(
        dag_nodes.begin(), dag_nodes.end(),
        [&node_id](const auto& n) { return n.node_id == node_id; });

    bool is_sink = false;
    if (dag_it != dag_nodes.end() && dag_it->stage) {
      const auto ntype = dag_it->stage->get_node_type_info().type;
      is_sink = (ntype == NodeType::SINK || ntype == NodeType::ANALYSIS_SINK);
    }
    if (!is_sink) {
      resolved.status = NodeVfrResolution::kNoOutput;
      return resolved;
    }

    // Sink stages produce no output by design. Fall back to the upstream node
    // its edge names so the host can show a pass-through preview (and answer
    // metadata queries) without any boilerplate in the plugin.
    if (dag_it != dag_nodes.end() && !dag_it->input_node_ids.empty()) {
      const NodeID upstream_id = dag_it->input_node_ids[0];
      // The edge's output index, not output 0: a multi-output stage's output 0
      // is not necessarily the one the sink consumes, and substituting it would
      // resolve a different branch than the sink is actually fed.
      const std::size_t upstream_output =
          dag_it->input_indices.empty() ? 0 : dag_it->input_indices[0];
      auto up_it = node_outputs.find(upstream_id);
      if (up_it != node_outputs.end() &&
          upstream_output < up_it->second.size()) {
        auto up_vfr = std::dynamic_pointer_cast<VideoFrameRepresentation>(
            up_it->second[upstream_output]);
        if (up_vfr) {
          it = up_it;
          output_index = upstream_output;
          resolved.source_node = upstream_id;
          resolved.substituted_upstream = true;
        }
      }
    }

    if (it == node_outputs.end() || it->second.size() <= output_index) {
      resolved.status = NodeVfrResolution::kSinkWithoutUpstream;
      return resolved;
    }
  }

  auto vfr = std::dynamic_pointer_cast<VideoFrameRepresentation>(
      it->second[output_index]);
  if (!vfr) {
    resolved.status = NodeVfrResolution::kNotRepresentation;
    return resolved;
  }

  resolved.status = NodeVfrResolution::kOk;
  resolved.representation = std::move(vfr);
  resolved.output_index = output_index;
  return resolved;
}

FrameRenderResult DAGFrameRenderer::execute_to_node(NodeID node_id,
                                                    FrameID frame_id) {
  FrameRenderResult result;
  result.node_id = node_id;
  result.frame_id = frame_id;
  result.from_cache = false;

  ORC_LOG_DEBUG("DAGFrameRenderer: executing DAG to node '{}' for frame {}",
                node_id.to_string(), frame_id);

  try {
    auto node_outputs = executor_->execute_to_node(*dag_, node_id);

    const ResolvedNodeVfr resolved =
        resolve_node_vfr(*dag_, node_outputs, node_id);
    switch (resolved.status) {
      case NodeVfrResolution::kNoOutput:
        ORC_LOG_ERROR("DAGFrameRenderer: node '{}' produced no output",
                      node_id.to_string());
        result.is_valid = false;
        result.error_message =
            fmt::format("Node '{}' produced no output", node_id);
        return result;
      case NodeVfrResolution::kSinkWithoutUpstream:
        ORC_LOG_DEBUG(
            "DAGFrameRenderer: sink node '{}' has no upstream VFR for preview",
            node_id.to_string());
        result.is_valid = false;
        result.error_message =
            fmt::format("Node '{}' produced no output", node_id);
        return result;
      case NodeVfrResolution::kNotRepresentation:
        ORC_LOG_ERROR(
            "DAGFrameRenderer: node '{}' output is not a "
            "VideoFrameRepresentation",
            node_id.to_string());
        result.is_valid = false;
        result.error_message = "Node '" + node_id.to_string() +
                               "' did not produce a VideoFrameRepresentation";
        return result;
      case NodeVfrResolution::kOk:
        break;
    }

    if (resolved.substituted_upstream) {
      ORC_LOG_DEBUG(
          "DAGFrameRenderer: sink node '{}' — using upstream node '{}' output "
          "{} for preview",
          node_id.to_string(), resolved.source_node.to_string(),
          resolved.output_index);
    }
    const VideoFrameRepresentationPtr& vfr = resolved.representation;

    if (!vfr->has_frame(frame_id)) {
      ORC_LOG_WARN(
          "DAGFrameRenderer: node '{}' frame {} not present in representation",
          node_id.to_string(), frame_id);
      result.is_valid = false;
      result.error_message =
          fmt::format("Frame {} not present in node '{}'", frame_id, node_id);
      return result;
    }

    result.is_valid = true;
    result.representation = vfr;

    // Populate the observation context for both fields of this frame. The
    // observer inventory comes from the shared registry
    // (CoreObservationService), so host and plugins stay in lockstep. When a
    // provenance-keyed store and a fingerprint map are attached,
    // run_frame_observer_pass reads observations through the store and only
    // runs observers on a miss.
    auto& obs_ctx = executor_->get_observation_context();
    // Provenance aliases for this frame's content: the node's own fingerprint
    // first, then — walking video_passthrough_source() up the chain — the
    // fingerprint of every upstream node whose output is byte-identical for
    // this frame (e.g. dropout correction on a frame with no dropouts). Each
    // hop is verified against the executed outputs (the declared source must
    // be exactly the input node's artifact) so a stage that wraps something
    // other than its DAG input can never alias the wrong provenance. The
    // aliased observer pass then reuses stored records across the whole chain
    // and back-fills the missing keys.
    std::vector<NodeFingerprint> fingerprints;
    if (observation_store_ && node_fingerprints_) {
      auto fp_it = node_fingerprints_->find(node_id);
      if (fp_it != node_fingerprints_->end()) {
        fingerprints.push_back(fp_it->second);

        ensure_node_index();
        const VideoFrameRepresentation* rep = vfr.get();
        NodeID cur = node_id;
        while (true) {
          const auto src = rep->video_passthrough_source(frame_id);
          if (!src) {
            break;
          }
          const auto idx_it = node_index_.find(cur);
          if (idx_it == node_index_.end()) {
            break;
          }
          const auto& cur_node = dag_->nodes()[idx_it->second];
          if (cur_node.input_node_ids.empty()) {
            break;
          }
          const NodeID parent = cur_node.input_node_ids[0];
          // The edge's output index, not output 0: a multi-output parent feeds
          // this node from one specific output, and comparing against the
          // wrong one would just lose the alias.
          const size_t parent_output =
              cur_node.input_indices.empty() ? 0 : cur_node.input_indices[0];
          const auto out_it = node_outputs.find(parent);
          if (out_it == node_outputs.end() ||
              out_it->second.size() <= parent_output) {
            break;
          }
          const auto parent_vfr =
              std::dynamic_pointer_cast<const VideoFrameRepresentation>(
                  out_it->second[parent_output]);
          if (!parent_vfr || parent_vfr.get() != src.get()) {
            break;  // declared source is not the DAG input's artifact
          }
          const auto parent_fp = node_fingerprints_->find(parent);
          if (parent_fp == node_fingerprints_->end()) {
            break;
          }
          fingerprints.push_back(parent_fp->second);
          rep = parent_vfr.get();
          cur = parent;
        }
      }
    }
    run_frame_observer_pass(observation_service(), observers_, *vfr, frame_id,
                            fingerprints, observation_store_.get(), obs_ctx);

    ORC_LOG_DEBUG("DAGFrameRenderer: node '{}' frame {} rendered successfully",
                  node_id.to_string(), frame_id);
    return result;

  } catch (const std::exception& e) {
    result.is_valid = false;
    result.error_message = std::string("Error rendering frame: ") + e.what();
    ORC_LOG_ERROR(
        "DAGFrameRenderer: exception rendering frame {} at node '{}': {}",
        frame_id, node_id.to_string(), e.what());
    return result;
  }
}

const ObservationContext& DAGFrameRenderer::get_observation_context() const {
  if (!executor_) {
    static ObservationContext empty;
    return empty;
  }
  return executor_->get_observation_context();
}

}  // namespace orc
