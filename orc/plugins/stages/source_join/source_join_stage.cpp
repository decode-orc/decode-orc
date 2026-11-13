/*
 * File:        source_join_stage.cpp
 * Module:      orc-stage-plugin-source-join
 * Purpose:     Join several sources end to end into a single output sequence
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "source_join_stage.h"

#include <orc/stage/error_types.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <set>
#include <sstream>
#include <variant>

namespace orc {

// ============================================================================
// JoinedVideoFrameRepresentation
// ============================================================================

JoinedVideoFrameRepresentation::JoinedVideoFrameRepresentation(
    std::vector<std::shared_ptr<const VideoFrameRepresentation>> sources,
    std::vector<Entry> mapping, const std::string& tag)
    : VideoFrameRepresentationWrapper(sources.empty() ? nullptr : sources[0]),
      Artifact(ArtifactID("source_join_" + tag), Provenance{}),
      sources_(std::move(sources)),
      mapping_(std::move(mapping)) {}

const JoinedVideoFrameRepresentation::Entry*
JoinedVideoFrameRepresentation::resolve(FrameID id) const {
  if (id >= static_cast<FrameID>(mapping_.size())) return nullptr;
  return &mapping_[static_cast<size_t>(id)];
}

const VideoFrameRepresentation* JoinedVideoFrameRepresentation::source_for(
    FrameID id, FrameID& source_frame) const {
  const Entry* entry = resolve(id);
  if (!entry) return nullptr;
  if (entry->source_index >= sources_.size()) return nullptr;
  const auto& source = sources_[entry->source_index];
  if (!source) return nullptr;
  source_frame = entry->source_frame;
  return source.get();
}

FrameIDRange JoinedVideoFrameRepresentation::frame_range() const {
  if (mapping_.empty()) return FrameIDRange{FrameID{0}, FrameID{0}};
  // FrameIDRange.last is inclusive; the last valid index is size() - 1.
  return FrameIDRange{FrameID{0}, FrameID{mapping_.size() - 1}};
}

bool JoinedVideoFrameRepresentation::has_frame(FrameID id) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  return source != nullptr && source->has_frame(source_frame);
}

std::optional<FrameDescriptor>
JoinedVideoFrameRepresentation::get_frame_descriptor(FrameID id) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  if (!source) return std::nullopt;
  auto desc = source->get_frame_descriptor(source_frame);
  // The descriptor describes this representation's frame, so it carries this
  // representation's numbering.
  if (desc) desc->frame_id = id;
  return desc;
}

const VideoFrameRepresentation::sample_type*
JoinedVideoFrameRepresentation::get_frame(FrameID id) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  return source ? source->get_frame(source_frame) : nullptr;
}

const VideoFrameRepresentation::sample_type*
JoinedVideoFrameRepresentation::get_line(FrameID id, size_t line) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  return source ? source->get_line(source_frame, line) : nullptr;
}

std::vector<VideoFrameRepresentation::sample_type>
JoinedVideoFrameRepresentation::get_frame_copy(FrameID id) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  return source ? source->get_frame_copy(source_frame)
                : std::vector<sample_type>{};
}

std::vector<VideoFrameRepresentation::sample_type>
JoinedVideoFrameRepresentation::get_line_samples(FrameID id,
                                                 size_t line) const {
  // Forwarded rather than derived so a source that can seek and read a single
  // line from disk keeps that fast path through the join — analysis sinks scan
  // whole recordings but touch only a handful of lines per frame.
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  return source ? source->get_line_samples(source_frame, line)
                : std::vector<sample_type>{};
}

const VideoFrameRepresentation::sample_type*
JoinedVideoFrameRepresentation::get_frame_luma(FrameID id) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  return source ? source->get_frame_luma(source_frame) : nullptr;
}

const VideoFrameRepresentation::sample_type*
JoinedVideoFrameRepresentation::get_frame_chroma(FrameID id) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  return source ? source->get_frame_chroma(source_frame) : nullptr;
}

const VideoFrameRepresentation::sample_type*
JoinedVideoFrameRepresentation::get_line_luma(FrameID id, size_t line) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  return source ? source->get_line_luma(source_frame, line) : nullptr;
}

const VideoFrameRepresentation::sample_type*
JoinedVideoFrameRepresentation::get_line_chroma(FrameID id, size_t line) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  return source ? source->get_line_chroma(source_frame, line) : nullptr;
}

std::vector<DropoutRun> JoinedVideoFrameRepresentation::get_dropout_hints(
    FrameID id) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  if (!source) return {};
  // Rewrite frame IDs so the runs describe this representation's frame, not
  // the input frame they came from: consumers such as dropout_map key on the
  // frame_id field.
  auto runs = source->get_dropout_hints(source_frame);
  for (auto& run : runs) {
    run.frame_id = id;
  }
  return runs;
}

std::shared_ptr<const VideoFrameRepresentation>
JoinedVideoFrameRepresentation::video_passthrough_source(FrameID id) const {
  const Entry* entry = resolve(id);
  if (!entry || entry->source_index >= sources_.size()) return nullptr;
  // The hook is answered per frame ID, so it holds only where the join left
  // the frame's ID alone — the leading input of a join that starts at frame 0.
  if (entry->source_frame != id) return nullptr;
  return sources_[entry->source_index];
}

std::vector<int32_t> JoinedVideoFrameRepresentation::get_audio_samples(
    size_t pair, FrameID id) const {
  if (pair >= audio_channel_pair_count()) return {};

  const Entry* entry = resolve(id);
  if (!entry || entry->source_index >= sources_.size()) return {};
  const auto& source = sources_[entry->source_index];
  if (!source) return {};

  const auto params = get_video_parameters();
  const VideoSystem system = params ? params->system : VideoSystem::Unknown;
  // Every output frame must serve exactly audio_pairs_in_frame(id) stereo
  // pairs regardless of the joined frame's native count.
  const size_t out_pairs = audio_pairs_in_frame(id, system);

  if (pair >= source->audio_channel_pair_count()) {
    // The joined output carries the leading input's channel-pair layout; an
    // input with fewer pairs contributes silence for the ones it lacks rather
    // than a short (or absent) block that would desynchronise the pair.
    return std::vector<int32_t>(out_pairs * 2, 0);
  }

  auto samples = source->get_audio_samples(pair, entry->source_frame);
  if (samples.empty() || out_pairs == 0) return samples;
  // SMPTE 272M-1994 §14.3: NTSC/PAL-M frames carry 1602 or 1601 stereo pairs
  // by position in the five-frame audio sequence. A join that lands a source
  // frame at an output index of a different sequence phase changes the
  // required count by one pair — truncate one trailing pair or append one
  // trailing silence pair. PAL is constant-cadence, so nothing changes there.
  samples.resize(out_pairs * 2, 0);
  return samples;
}

void JoinedVideoFrameRepresentation::prime_audio_decode(
    const AudioDecodeProgressFn& progress) const {
  for (const auto& source : sources_) {
    if (source) source->prime_audio_decode(progress);
  }
}

uint32_t JoinedVideoFrameRepresentation::get_efm_sample_count(
    FrameID id) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  return source ? source->get_efm_sample_count(source_frame) : 0;
}

std::vector<uint8_t> JoinedVideoFrameRepresentation::get_efm_samples(
    FrameID id) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  return source ? source->get_efm_samples(source_frame)
                : std::vector<uint8_t>{};
}

uint32_t JoinedVideoFrameRepresentation::get_ac3_symbol_count(
    FrameID id) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  return source ? source->get_ac3_symbol_count(source_frame) : 0;
}

std::vector<uint8_t> JoinedVideoFrameRepresentation::get_ac3_symbols(
    FrameID id) const {
  FrameID source_frame{0};
  const VideoFrameRepresentation* source = source_for(id, source_frame);
  return source ? source->get_ac3_symbols(source_frame)
                : std::vector<uint8_t>{};
}

// ============================================================================
// SourceJoinStage
// ============================================================================

SourceJoinStage::SourceJoinStage() {
  set_configuration_status(orc::ConfigurationStatus::Yellow);
}

std::optional<std::vector<NodeID::value_type>>
SourceJoinStage::parse_node_id_list(const std::string& spec) {
  std::vector<NodeID::value_type> ids;
  std::istringstream iss(spec);
  std::string token;
  while (std::getline(iss, token, ',')) {
    const auto first = token.find_first_not_of(" \t");
    if (first == std::string::npos) continue;  // Empty token: ignore
    const auto last = token.find_last_not_of(" \t");
    token = token.substr(first, last - first + 1);

    NodeID::value_type id = 0;
    try {
      size_t consumed = 0;
      const int64_t parsed = std::stoll(token, &consumed);
      if (consumed != token.size()) return std::nullopt;
      if (parsed < 0 || parsed > INT32_MAX) return std::nullopt;
      id = static_cast<NodeID::value_type>(parsed);
    } catch (...) {
      return std::nullopt;
    }

    // Each input appears in the output once: a node has a single output to
    // connect, so a repeated ID is a typo rather than a request.
    if (std::find(ids.begin(), ids.end(), id) != ids.end()) {
      return std::nullopt;
    }
    ids.push_back(id);
  }
  return ids;
}

std::vector<size_t> SourceJoinStage::resolve_input_positions(
    const std::vector<NodeID::value_type>& order,
    const std::vector<NodeID::value_type>& connected,
    std::vector<NodeID::value_type>& unresolved_out,
    std::vector<NodeID::value_type>& skipped_out) {
  unresolved_out.clear();
  skipped_out.clear();

  std::vector<size_t> positions;
  if (order.empty()) {
    // No order configured — join in connection order.
    positions.resize(connected.size());
    for (size_t i = 0; i < connected.size(); ++i) positions[i] = i;
    return positions;
  }

  for (const auto id : order) {
    const auto it = std::find(connected.begin(), connected.end(), id);
    if (it == connected.end()) {
      unresolved_out.push_back(id);
      continue;
    }
    positions.push_back(
        static_cast<size_t>(std::distance(connected.begin(), it)));
  }

  for (size_t i = 0; i < connected.size(); ++i) {
    if (std::find(positions.begin(), positions.end(), i) == positions.end()) {
      skipped_out.push_back(connected[i]);
    }
  }
  return positions;
}

bool SourceJoinStage::order_matches_inputs(
    const std::vector<NodeID::value_type>& order,
    const std::vector<NodeID::value_type>& connected) {
  if (order.size() != connected.size()) return false;
  const std::set<NodeID::value_type> ordered(order.begin(), order.end());
  const std::set<NodeID::value_type> available(connected.begin(),
                                               connected.end());
  return ordered == available;
}

void SourceJoinStage::refresh_configuration_status() {
  if (input_order_.empty()) {
    // Runs, but in whatever order the connections happen to have been made.
    set_configuration_status(orc::ConfigurationStatus::Yellow);
    return;
  }
  if (!input_identity_known_) {
    // A host that does not supply input identity leaves the order
    // uncheckable, so take it at face value.
    set_configuration_status(orc::ConfigurationStatus::Green);
    return;
  }
  // The configuration is a list of node IDs, so rewiring the node can leave it
  // describing a graph that no longer exists. Say so rather than letting it
  // look configured.
  set_configuration_status(
      order_matches_inputs(parsed_order_, parsed_connected_)
          ? orc::ConfigurationStatus::Green
          : orc::ConfigurationStatus::Yellow);
}

std::optional<SourceJoinStage::RunConfig> SourceJoinStage::config_for(
    const std::map<std::string, ParameterValue>& parameters) const {
  RunConfig config;
  config.order = parsed_order_;
  config.connected = parsed_connected_;

  const auto order_it = parameters.find("input_order");
  if (order_it != parameters.end()) {
    if (const auto* v = std::get_if<std::string>(&order_it->second)) {
      auto parsed = parse_node_id_list(*v);
      if (!parsed) {
        ORC_LOG_ERROR("SourceJoinStage: invalid input order '{}'", *v);
        return std::nullopt;
      }
      config.order = std::move(*parsed);
    }
  }

  const auto ids_it = parameters.find(kInputNodeIdsParameter);
  if (ids_it != parameters.end()) {
    if (const auto* v = std::get_if<std::string>(&ids_it->second)) {
      auto parsed = parse_node_id_list(*v);
      if (!parsed) {
        ORC_LOG_ERROR(
            "SourceJoinStage: host supplied a malformed {} value '{}'",
            kInputNodeIdsParameter, *v);
        return std::nullopt;
      }
      config.connected = std::move(*parsed);
    }
  }
  return config;
}

void SourceJoinStage::require_uniform_geometry(
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
        sources) {
  if (sources.empty()) return;
  const auto reference = sources[0]->get_video_parameters();
  const bool reference_yc = sources[0]->has_separate_channels();
  if (!reference) return;

  for (size_t i = 1; i < sources.size(); ++i) {
    const auto params = sources[i]->get_video_parameters();
    if (!params) continue;
    if (params->system != reference->system ||
        params->frame_width_nominal != reference->frame_width_nominal ||
        params->frame_height != reference->frame_height) {
      throw DAGExecutionError(
          "SourceJoinStage: joined inputs must share one signal geometry; "
          "input " +
          std::to_string(i + 1) + " differs from the first input");
    }
    if (sources[i]->has_separate_channels() != reference_yc) {
      throw DAGExecutionError(
          "SourceJoinStage: joined inputs must all be composite or all be "
          "Y/C; input " +
          std::to_string(i + 1) + " differs from the first input");
    }
  }
}

std::vector<ArtifactPtr> SourceJoinStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  if (inputs.empty()) {
    throw DAGExecutionError("SourceJoinStage requires at least one input");
  }

  std::vector<std::shared_ptr<const VideoFrameRepresentation>> connected;
  connected.reserve(inputs.size());
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto source =
        std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[i]);
    if (!source) {
      throw DAGExecutionError("SourceJoinStage: input " +
                              std::to_string(i + 1) +
                              " must be a VideoFrameRepresentation");
    }
    connected.push_back(std::move(source));
  }

  // Resolve the run's parameters onto the stack. The stage's own members are
  // left alone: execute() is not the configuration path, and a DAG's stage
  // instances are shared between worker threads.
  const auto config_opt = config_for(parameters);
  if (!config_opt) {
    throw DAGExecutionError("SourceJoinStage: invalid input order");
  }
  const RunConfig& config = *config_opt;

  std::vector<size_t> positions;
  if (config.connected.size() != connected.size()) {
    // No usable input identity (an unpatched host, or a stale value): the only
    // order left is the one the artifacts arrived in.
    if (!config.order.empty()) {
      ORC_LOG_WARN(
          "SourceJoinStage: the host reported {} input node id(s) for {} "
          "connected input(s); joining in connection order instead",
          config.connected.size(), connected.size());
    }
    positions.resize(connected.size());
    for (size_t i = 0; i < connected.size(); ++i) positions[i] = i;
  } else {
    std::vector<NodeID::value_type> unresolved;
    std::vector<NodeID::value_type> skipped;
    positions = resolve_input_positions(config.order, config.connected,
                                        unresolved, skipped);
    for (const auto id : unresolved) {
      ORC_LOG_WARN(
          "SourceJoinStage: input order names node {}, which is not connected "
          "to this stage; ignoring it",
          id);
    }
    for (const auto id : skipped) {
      ORC_LOG_WARN(
          "SourceJoinStage: node {} is connected but is not named in the input "
          "order; its frames are not in the output",
          id);
    }
    if (!skipped.empty()) {
      observation_context.set(FieldID(0), "source_join", "inputs_skipped",
                              static_cast<int64_t>(skipped.size()));
    }
    if (positions.empty()) {
      throw DAGExecutionError(
          "SourceJoinStage: the input order names no connected node; update it "
          "to match the sources feeding this stage");
    }
  }

  std::vector<std::shared_ptr<const VideoFrameRepresentation>> ordered;
  ordered.reserve(positions.size());
  for (const auto position : positions) {
    ordered.push_back(connected[position]);
  }

  // One input joins to itself; hand the artifact straight back so the frame
  // IDs, and any pass-through analysis keyed on them, are left alone.
  if (ordered.size() == 1) {
    cached_output_ = ordered[0];
    observation_context.set(FieldID(0), "source_join", "inputs_joined",
                            static_cast<int64_t>(1));
    observation_context.set(FieldID(0), "source_join", "output_frames",
                            static_cast<int64_t>(ordered[0]->frame_count()));
    return {inputs[positions[0]]};
  }

  require_uniform_geometry(ordered);

  std::vector<JoinedVideoFrameRepresentation::Entry> mapping;
  {
    size_t total = 0;
    for (const auto& source : ordered) total += source->frame_count();
    mapping.reserve(total);
  }
  std::ostringstream tag;
  for (size_t index = 0; index < ordered.size(); ++index) {
    const auto& source = ordered[index];
    const FrameIDRange range = source->frame_range();
    size_t contributed = 0;
    for (FrameID fid = range.first; fid <= range.last; ++fid) {
      if (!source->has_frame(fid)) continue;
      mapping.push_back(JoinedVideoFrameRepresentation::Entry{
          static_cast<uint32_t>(index), fid});
      ++contributed;
    }
    ORC_LOG_DEBUG("SourceJoinStage: input {} contributes {} frame(s)",
                  index + 1, contributed);
    if (index > 0) tag << "_";
    tag << contributed;
  }

  if (mapping.empty()) {
    throw DAGExecutionError(
        "SourceJoinStage: the joined inputs contain no frames");
  }

  observation_context.set(FieldID(0), "source_join", "inputs_joined",
                          static_cast<int64_t>(ordered.size()));
  observation_context.set(FieldID(0), "source_join", "output_frames",
                          static_cast<int64_t>(mapping.size()));

  auto result = std::make_shared<JoinedVideoFrameRepresentation>(
      std::move(ordered), std::move(mapping), tag.str());

  cached_output_ = result;
  return {result};
}

std::vector<ParameterDescriptor> SourceJoinStage::get_parameter_descriptors(
    VideoSystem /*project_format*/, SourceType /*source_type*/) const {
  return {
      ParameterDescriptor{
          "input_order", "Input Order (node IDs)",
          "The order the connected sources are joined in, given as a "
          "comma-separated list of the node IDs of the stages connected to "
          "this stage's input — e.g. '16,2,4'. A node's ID is the number "
          "shown in the corner of the node in the graph editor. Empty = join "
          "in the order the connections were made.",
          ParameterType::STRING,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue{std::string("")},
                               {},
                               false,
                               std::nullopt}},
      // Host-owned: filled in by the DAG builder from this node's incoming
      // connections and hidden from the GUI and CLI parameter surfaces.
      ParameterDescriptor{
          kInputNodeIdsParameter, "Connected Input Nodes",
          "Host-supplied identity of the connected inputs; not user-editable.",
          ParameterType::STRING,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue{std::string("")},
                               {},
                               false,
                               std::nullopt}},
  };
}

std::map<std::string, ParameterValue> SourceJoinStage::get_parameters() const {
  return {{"input_order", ParameterValue{input_order_}},
          {kInputNodeIdsParameter, ParameterValue{input_node_ids_}}};
}

bool SourceJoinStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key == "input_order") {
      const auto* v = std::get_if<std::string>(&value);
      if (!v) return false;
      auto parsed = parse_node_id_list(*v);
      if (!parsed) {
        ORC_LOG_ERROR("SourceJoinStage: invalid input order '{}'", *v);
        return false;
      }
      input_order_ = *v;
      parsed_order_ = std::move(*parsed);
    } else if (key == kInputNodeIdsParameter) {
      const auto* v = std::get_if<std::string>(&value);
      if (!v) return false;
      auto parsed = parse_node_id_list(*v);
      if (!parsed) {
        ORC_LOG_ERROR(
            "SourceJoinStage: host supplied a malformed {} value '{}'",
            kInputNodeIdsParameter, *v);
        return false;
      }
      input_node_ids_ = *v;
      parsed_connected_ = std::move(*parsed);
      input_identity_known_ = true;
    } else {
      ORC_LOG_WARN("SourceJoinStage: unknown parameter '{}'", key);
      return false;
    }
  }
  refresh_configuration_status();
  return true;
}

StagePreviewCapability SourceJoinStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

}  // namespace orc
