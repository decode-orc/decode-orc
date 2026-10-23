/*
 * File:        frame_provenance.cpp
 * Module:      orc-core
 * Purpose:     Static, content-addressed provenance fingerprints for DAG nodes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_provenance.h"

#include <fmt/format.h>
#include <orc/stage/params/stage_parameter.h>

#include <filesystem>
#include <sstream>
#include <unordered_set>
#include <variant>

#include "dag_executor.h"
#include "sha256_hash.h"

namespace orc {

namespace {

// Serialise a single parameter value with a one-character type tag so values of
// different variant alternatives (e.g. the integer 1, the boolean true, and the
// string "1") never produce the same token. Floating-point values use fmt's
// shortest round-trip representation, so two distinct doubles never collide.
std::string canonical_parameter_value(const ParameterValue& value) {
  return std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, int32_t>) {
          return fmt::format("i{}", v);
        } else if constexpr (std::is_same_v<T, uint32_t>) {
          return fmt::format("u{}", v);
        } else if constexpr (std::is_same_v<T, double>) {
          return fmt::format("d{}", v);
        } else if constexpr (std::is_same_v<T, bool>) {
          return v ? "b1" : "b0";
        } else {  // std::string
          return fmt::format("s{}", v);
        }
      },
      value);
}

// Render a FileIdentity into a stable folding token. A missing/unreadable file
// maps to a fixed "unknown" sentinel (distinct from any real size/mtime pair).
std::string canonical_file_identity(const FileIdentity& identity) {
  if (!identity.exists) {
    return "unknown";
  }
  return fmt::format("{}:{}", identity.size, identity.mtime);
}

// Fold the identity of every FILE_PATH input parameter of a SOURCE node into a
// single deterministic token. Non-source nodes and nodes without file inputs
// yield an empty string (no folding).
std::string source_identity_token(
    const DAGNode& node, const IFileIdentityProvider& file_identity_provider) {
  if (node.stage->get_node_type_info().type != NodeType::SOURCE) {
    return "";
  }

  const auto* parameterized =
      dynamic_cast<const ParameterizedStage*>(node.stage.get());
  if (parameterized == nullptr) {
    return "";
  }

  // Collect FILE_PATH input parameters, ordered by descriptor name for
  // determinism.
  std::map<std::string, std::string> file_paths;  // param name -> path value
  for (const auto& descriptor : parameterized->get_parameter_descriptors()) {
    if (descriptor.type != ParameterType::FILE_PATH || descriptor.output_path) {
      continue;
    }
    auto it = node.parameters.find(descriptor.name);
    if (it == node.parameters.end()) {
      continue;
    }
    if (const auto* path = std::get_if<std::string>(&it->second)) {
      file_paths.emplace(descriptor.name, *path);
    }
  }

  if (file_paths.empty()) {
    return "";
  }

  std::ostringstream oss;
  bool first = true;
  for (const auto& [name, path] : file_paths) {
    if (!first) {
      oss << ";";
    }
    first = false;
    const FileIdentity identity = file_identity_provider.identify(path);
    oss << name << "=" << canonical_file_identity(identity);
  }
  return oss.str();
}

// True when a SOURCE node declares FILE_PATH inputs and none of them is set:
// the node has been added but its file not yet chosen. A stage with no file
// inputs (a stream or synthetic source) never awaits one.
bool source_awaits_input_file(const DAGNode& node) {
  const auto* parameterized =
      dynamic_cast<const ParameterizedStage*>(node.stage.get());
  if (parameterized == nullptr) {
    return false;
  }

  bool declares_file_input = false;
  for (const auto& descriptor : parameterized->get_parameter_descriptors()) {
    if (descriptor.type != ParameterType::FILE_PATH || descriptor.output_path) {
      continue;
    }
    declares_file_input = true;
    auto it = node.parameters.find(descriptor.name);
    if (it == node.parameters.end()) {
      continue;
    }
    const auto* path = std::get_if<std::string>(&it->second);
    if (path != nullptr && !path->empty()) {
      return false;
    }
  }
  return declares_file_input;
}

// Recursively compute a node's fingerprint, memoising into `out`. `visiting`
// guards against cycles (a malformed DAG); a cyclic back-edge contributes an
// empty input token rather than recursing forever.
const NodeFingerprint& fingerprint_of(
    const NodeID& node_id, const std::map<NodeID, size_t>& node_index,
    const std::vector<DAGNode>& nodes,
    const IFileIdentityProvider& file_identity_provider,
    NodeFingerprintMap& out, std::unordered_set<NodeID>& visiting) {
  auto existing = out.find(node_id);
  if (existing != out.end()) {
    return existing->second;
  }

  static const NodeFingerprint kEmptyFingerprint{};
  auto index_it = node_index.find(node_id);
  if (index_it == node_index.end()) {
    // Dangling reference: no such node. Treat as empty so composition stays
    // deterministic without throwing.
    return kEmptyFingerprint;
  }

  if (!visiting.insert(node_id).second) {
    // Cycle detected; break it with an empty contribution.
    return kEmptyFingerprint;
  }

  const DAGNode& node = nodes[index_it->second];

  // Resolve input tokens: each is the input node's fingerprint plus the
  // selected output index, so distinct output selections never alias.
  std::vector<std::string> input_tokens;
  input_tokens.reserve(node.input_node_ids.size());
  for (size_t i = 0; i < node.input_node_ids.size(); ++i) {
    const NodeFingerprint& input_fp =
        fingerprint_of(node.input_node_ids[i], node_index, nodes,
                       file_identity_provider, out, visiting);
    const size_t output_index =
        i < node.input_indices.size() ? node.input_indices[i] : 0;
    input_tokens.push_back(fmt::format("{}#{}", input_fp.value, output_index));
  }

  const std::string serialized = serialize_node_provenance(
      node.stage->get_node_type_info().stage_name, node.stage->version(),
      input_tokens, node.parameters,
      source_identity_token(node, file_identity_provider));

  visiting.erase(node_id);
  auto [inserted, ok] =
      out.emplace(node_id, NodeFingerprint{sha256_hex(serialized)});
  (void)ok;
  return inserted->second;
}

}  // namespace

FileIdentity FilesystemFileIdentityProvider::identify(
    const std::string& path) const {
  FileIdentity identity;
  std::error_code ec;
  const std::filesystem::path fs_path(path);

  const auto size = std::filesystem::file_size(fs_path, ec);
  if (ec) {
    return identity;  // exists == false
  }

  const auto write_time = std::filesystem::last_write_time(fs_path, ec);
  if (ec) {
    return identity;  // exists == false
  }

  identity.exists = true;
  identity.size = static_cast<std::uint64_t>(size);
  identity.mtime =
      static_cast<std::int64_t>(write_time.time_since_epoch().count());
  return identity;
}

std::string serialize_node_provenance(
    const std::string& stage_name, const std::string& stage_version,
    const std::vector<std::string>& input_tokens,
    const std::map<std::string, ParameterValue>& parameters,
    const std::string& source_identity) {
  std::ostringstream oss;
  oss << stage_name << ":" << stage_version;

  for (const auto& token : input_tokens) {
    oss << ":" << token;
  }

  // std::map iterates in sorted key order, giving deterministic parameter
  // ordering.
  for (const auto& [key, value] : parameters) {
    oss << ":" << key << "=" << canonical_parameter_value(value);
  }

  if (!source_identity.empty()) {
    oss << ":source_identity=" << source_identity;
  }

  return oss.str();
}

NodeFingerprintMap compute_node_fingerprints(
    const DAG& dag, const IFileIdentityProvider& file_identity_provider) {
  const auto node_index = dag.build_node_index();
  const auto& nodes = dag.nodes();

  NodeFingerprintMap out;
  out.reserve(nodes.size());
  std::unordered_set<NodeID> visiting;

  for (const auto& node : nodes) {
    fingerprint_of(node.node_id, node_index, nodes, file_identity_provider, out,
                   visiting);
  }

  return out;
}

std::string sidecar_source_key(const DAG& dag) {
  std::string key;
  for (const auto& node : dag.nodes()) {
    if (!node.stage) {
      continue;
    }
    const auto info = node.stage->get_node_type_info();
    if (info.type != NodeType::SOURCE) {
      continue;
    }
    if (source_awaits_input_file(node)) {
      return {};
    }
    key += serialize_node_provenance(info.stage_name, node.stage->version(),
                                     /*input_tokens=*/{}, node.parameters);
  }
  return key;
}

}  // namespace orc
