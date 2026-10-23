/*
 * File:        frame_provenance.h
 * Module:      orc-core
 * Purpose:     Static, content-addressed provenance fingerprints for DAG nodes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// Host-internal provenance machinery. Only orc-core and orc-presenters may
// include this header; GUI/CLI code must go through presenters.
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/frame_provenance.h. Use a presenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/frame_provenance.h. Use a presenter instead."
#endif

#include <orc/stage/node_id.h>
#include <orc/stage/params/parameter_types.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace orc {

class DAG;  // Defined in dag_executor.h

/**
 * @brief Identity of a source file, folded into SOURCE-node fingerprints.
 *
 * Deliberately coarse (size + modification time): it changes whenever the file
 * content plausibly changes without reading the file. A missing/unreadable file
 * yields `exists == false`, which maps to a distinct, stable "unknown" identity
 * rather than throwing.
 */
struct FileIdentity {
  bool exists = false;     ///< False when the path could not be stat-ed.
  std::uint64_t size = 0;  ///< File size in bytes (0 when unknown).
  std::int64_t mtime = 0;  ///< Last-write time as an implementation-defined,
                           ///< platform-stable tick count (0 when unknown).

  bool operator==(const FileIdentity& other) const {
    return exists == other.exists && size == other.size && mtime == other.mtime;
  }
  bool operator!=(const FileIdentity& other) const { return !(*this == other); }
};

/**
 * @brief Supplies file identity for SOURCE-node file parameters.
 *
 * Injected so unit tests can provide deterministic identities without touching
 * the filesystem (see TESTING.md §4.2). The default filesystem implementation
 * is `FilesystemFileIdentityProvider`.
 *
 * Thread safety: implementations must be safe to call concurrently on distinct
 * or identical paths (the filesystem default is).
 */
class IFileIdentityProvider {
 public:
  virtual ~IFileIdentityProvider() = default;

  /// Return the identity of `path`. Must never throw; report an unreadable or
  /// missing file via `FileIdentity{.exists = false}`.
  virtual FileIdentity identify(const std::string& path) const = 0;
};

/**
 * @brief Default `IFileIdentityProvider` that stats the real filesystem.
 *
 * Not usable in unit tests (filesystem access). Thread-safe.
 */
class FilesystemFileIdentityProvider final : public IFileIdentityProvider {
 public:
  FileIdentity identify(const std::string& path) const override;
};

/**
 * @brief A stable per-node provenance hash.
 *
 * Two nodes share a fingerprint iff everything determining their output content
 * (stage identity + version + ordered parameters + input fingerprints + source
 * file identity) is identical. The value is a lowercase hex SHA-256 digest.
 */
struct NodeFingerprint {
  std::string value;  ///< 64-char lowercase hex digest.

  bool operator==(const NodeFingerprint& other) const {
    return value == other.value;
  }
  bool operator!=(const NodeFingerprint& other) const {
    return value != other.value;
  }
  bool operator<(const NodeFingerprint& other) const {
    return value < other.value;
  }
};

/// Map from node identity to its static provenance fingerprint.
using NodeFingerprintMap = std::unordered_map<NodeID, NodeFingerprint>;

/**
 * @brief Serialise a node's provenance into the canonical, deterministic string
 *        that both the fingerprint machinery and the DAG executor hash/key on.
 *
 * This is the single definition of provenance composition, mirroring
 * `DAGExecutor::compute_expected_artifact_id()`. The format is:
 *
 *     stage_name ":" stage_version
 *         { ":" input_token }
 *         { ":" key "=" typed_value }
 *         [ ":source_identity=" source_identity ]
 *
 * Parameters are emitted in `std::map` key order (deterministic). Each value is
 * type-tagged and floating-point values use a shortest round-trip
 * representation, so distinct values never collide.
 *
 * @param stage_name       Stage type identifier.
 * @param stage_version    Stage version string.
 * @param input_tokens     Provenance tokens of the resolved inputs, in order.
 * @param parameters       Node parameters (ordered by key via std::map).
 * @param source_identity  Optional folded source-file identity; appended only
 *                         when non-empty.
 */
std::string serialize_node_provenance(
    const std::string& stage_name, const std::string& stage_version,
    const std::vector<std::string>& input_tokens,
    const std::map<std::string, ParameterValue>& parameters,
    const std::string& source_identity = "");

/**
 * @brief Compute the provenance fingerprint of every node in `dag`.
 *
 * Fingerprints compose recursively over inputs, so a parameter or source change
 * on one node alters that node's fingerprint and every transitive descendant's,
 * while unrelated branches stay byte-identical. Runs in one pass in dependency
 * order; O(V + E) plus one file-identity lookup per SOURCE file parameter.
 *
 * File identities are captured at call time via `file_identity_provider`.
 *
 * @param dag                    The DAG to fingerprint (need not be executed).
 * @param file_identity_provider Supplies identity for SOURCE file parameters.
 * @return Map of every node's ID to its fingerprint.
 */
NodeFingerprintMap compute_node_fingerprints(
    const DAG& dag, const IFileIdentityProvider& file_identity_provider);

/**
 * @brief The key that names a DAG's per-source observation sidecar.
 *
 * The concatenated provenance (stage, version, parameters — which include the
 * source file path) of every SOURCE node, in DAG order. File identity is left
 * out so the key survives a touch of the file; the records inside are keyed by
 * full fingerprints, which do carry it.
 *
 * Empty when the DAG has no source, and when any source has not been given its
 * input file yet (it declares FILE_PATH inputs and every one is unset). Such a
 * key would change the moment the file is chosen, so a sidecar opened under it
 * would collect the whole session's observations under a name the next session
 * shares with every other source that was added before its file was picked.
 */
std::string sidecar_source_key(const DAG& dag);

}  // namespace orc

// Hash specialisation so fingerprints can key unordered containers (retention
// sets, etc.).
namespace std {
template <>
struct hash<orc::NodeFingerprint> {
  size_t operator()(const orc::NodeFingerprint& fp) const noexcept {
    return std::hash<std::string>{}(fp.value);
  }
};
}  // namespace std
