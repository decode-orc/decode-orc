/*
 * File:        project_presenter_types.h
 * Module:      orc-presenters
 * Purpose:     Shared project presenter view types
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc/stage/node_type.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace orc::presenters {

/**
 * @brief Video format enumeration for GUI use
 */
enum class VideoFormat { NTSC, PAL, PAL_M, Unknown };

/**
 * @brief Source type enumeration for GUI use
 */
enum class SourceType { Composite, YC, Unknown };

enum class PluginDiagnosticSeverity { Info, Warning, Error };

struct PluginDiagnosticInfo {
  PluginDiagnosticSeverity severity = PluginDiagnosticSeverity::Info;
  std::string path;
  std::string message;
};

struct LoadedPluginInfo {
  std::string path;
  std::string plugin_id;
  std::string plugin_version;
  std::string license_spdx;
  bool is_core_plugin = false;
  std::vector<std::string> registered_stage_names;
};

/// Values the registry records in @ref PluginRegistryEntryInfo::trust_state.
/// Front ends set these when they build an entry; they never compare against
/// them to work out whether a plugin loads — that is what
/// @ref PluginLoadState is for.
inline constexpr const char* kPluginTrustStateTrusted = "trusted";
inline constexpr const char* kPluginTrustStateUntrusted = "untrusted";

/// Whether a registry entry will load at the next launch and, when it will
/// not, the single reason why. Computed once by the presenter so the GUI and
/// the CLI can never disagree about an entry's state.
enum class PluginLoadState {
  WillLoad,     ///< Enabled, trusted, and the artifact is usable.
  Disabled,     ///< Enabled flag cleared by the user.
  NotTrusted,   ///< Trust has not been granted for this binary yet.
  AbiMismatch,  ///< Built against a different Orc ABI than this host.
  FileMissing,  ///< A locally registered binary is not on disk.
  Core,         ///< Bundled with the application; always loads.
};

struct PluginRegistryEntryInfo {
  /// Canonical handle for this entry: the string every command that takes a
  /// plugin accepts, and the only identifier a front end should print where a
  /// selector belongs. Never empty and never a placeholder.
  std::string selector;
  std::string plugin_id;
  std::string plugin_version;
  std::string path;
  std::string source_repo_url;
  std::string artifact_source = "local_path";
  std::string release_asset_url;
  std::string release_tag;
  std::string release_asset_name;
  std::string target_platform;
  std::string local_dev_path;
  bool enabled = true;
  std::string trust_state = "untrusted";
  std::string license_spdx;
  bool is_core_plugin = false;
  uint32_t required_host_abi = 0;
  uint32_t host_abi_version = 0;  ///< The running host's ABI version.
  bool abi_compatible = true;     ///< false when required_host_abi is set and
                                  ///< does not match host_abi_version.
  std::string sha256;
  /// The plugin's view binary (the windows its tools open), when it has one.
  std::string view_path;
  std::string view_release_asset_url;
  std::string view_release_asset_name;
  std::string view_sha256;
  bool has_view = false;
  /// orc-gui loaded the view this run. Always false in orc-cli, which never
  /// loads views.
  bool view_loaded = false;
  /// This process loads views (orc-gui), so a view that is not loaded was
  /// refused rather than never tried.
  bool views_attempted = false;
  bool is_loaded = false;
  bool path_exists = false;
  /// Where the entry came from, worded once for both front ends: the release
  /// asset URL, else the source repository, else the local path, and "Core"
  /// for a bundled plugin.
  std::string source_label;
  PluginLoadState load_state = PluginLoadState::WillLoad;
  /// Lower-case clause explaining @ref load_state for this entry (carries the
  /// ABI numbers or the missing path); empty for states that need no detail.
  std::string load_state_detail;
};

struct PluginRegistryInfo {
  std::string registry_path;
  std::vector<PluginRegistryEntryInfo> entries;
};

/// Outcome of turning a user-supplied plugin selector into a registry entry.
enum class PluginSelectorStatus {
  Resolved,   ///< Exactly one entry matched.
  NotFound,   ///< No entry matched.
  Ambiguous,  ///< More than one entry matched; the caller must disambiguate.
};

struct PluginSelectorResolution {
  PluginSelectorStatus status = PluginSelectorStatus::NotFound;
  PluginRegistryEntryInfo entry;  ///< Valid only when Resolved.
  /// Position of @ref entry in the list that was searched. Valid only when
  /// Resolved; lets callers map the match back onto their own storage.
  size_t entry_index = 0;
  /// Unambiguous selectors to choose from; populated only when Ambiguous.
  std::vector<std::string> candidates;
  /// The selector was a loaded plugin's runtime id resolved onto an id-less
  /// registry row by path; mutation paths backfill the id when this is set.
  bool resolved_via_runtime_id = false;
};

/// Why a registry mutation failed, so a front end can map the failure onto its
/// own contract (the CLI's exit codes) without parsing the message text.
enum class PluginMutationFailure {
  Other,             ///< Bad arguments, write failures, unusable releases, ...
  NotFound,          ///< The selector or id matched nothing, or was ambiguous.
  IndexUnavailable,  ///< The curated index or a release could not be reached.
};

struct PluginRegistryMutationResult {
  bool success = false;
  /// Meaningful only when @ref success is false.
  PluginMutationFailure failure = PluginMutationFailure::Other;
  std::string error_message;
};

/// Outcome of checking one installed plugin against its latest upstream
/// release.
enum class PluginUpdateStatus {
  UpToDate,         ///< Installed version >= latest published release.
  UpdateAvailable,  ///< A newer release is published upstream.
  Unreachable,      ///< The release information could not be fetched.
  Unknown,          ///< Latest release known, but no installed version to
                    ///< compare against.
  NotApplicable,    ///< No supported source repository URL (e.g. local
                    ///< plugins).
};

/// Per-plugin result of a latest-release update check.
struct PluginUpdateStatusInfo {
  std::string plugin_id;
  std::string installed_version;
  std::string latest_version;  ///< Normalised for display; empty when unknown.
  std::string latest_tag;      ///< Raw upstream release tag (e.g. "v1.0.6").
  PluginUpdateStatus status = PluginUpdateStatus::NotApplicable;
  std::string message;  ///< Failure detail when Unreachable.
};

/// One plugin advertised by the curated index. The index curates which
/// plugins are offered; the entry's version and compatibility are resolved
/// at runtime from the latest release published by `source_repo_url`.
struct PluginIndexEntryInfo {
  std::string id;
  std::string display_name;
  std::string description;
  std::string version;     ///< Latest published release, normalised for
                           ///< display (empty when unreachable).
  std::string latest_tag;  ///< Raw release tag of the latest release.
  std::string maintainer;
  std::string license_spdx;
  std::string source_repo_url;
  std::vector<std::string> tags;
  bool has_compatible_build = false;  ///< The latest release's manifest
                                      ///< declares an artifact matching this
                                      ///< host's platform, ABI and toolchain.
  bool release_unreachable = false;   ///< Release info could not be fetched.
  bool already_installed = false;     ///< Present in the local registry.
  std::string compatibility_message;  ///< Set when not installable (no
                                      ///< matching asset, unreachable, ...).
};

/// Result of fetching the curated plugin index, including offline/cache state.
struct PluginIndexInfo {
  int schema_version = 0;
  bool available = false;   ///< An index (fresh or cached) was loaded.
  bool from_cache = false;  ///< The list came from the last-good cache.
  bool offline = false;     ///< The network fetch failed.
  uint32_t host_abi_version = 0;
  std::string source_url;
  std::string error_message;
  std::vector<PluginIndexEntryInfo> entries;
};

/**
 * @brief Information about a stage available in the registry
 */
struct StageInfo {
  std::string name;          ///< Internal stage name
  std::string display_name;  ///< User-friendly display name
  std::string description;   ///< Stage description
  NodeType node_type;        ///< Type of node (menu category derives from it)
  bool is_source;            ///< True if this is a source stage
  bool is_sink;              ///< True if this is a sink stage
  bool is_core_plugin =
      false;  ///< True if the owning plugin is bundled with Decode-Orc
  std::string owning_plugin_id;  ///< Plugin id when known
};

/**
 * @brief Information about a node in the project
 */
struct NodeInfo {
  NodeID node_id;              ///< Node identifier
  std::string stage_name;      ///< Stage type name
  std::string label;           ///< User-assigned label
  double x_position;           ///< X position in graph
  double y_position;           ///< Y position in graph
  bool can_remove;             ///< Whether node can be removed
  bool can_trigger;            ///< Whether node can be triggered
  std::string remove_reason;   ///< Reason if cannot remove
  std::string trigger_reason;  ///< Reason if cannot trigger
};

/**
 * @brief Edge between two nodes
 */
struct EdgeInfo {
  NodeID source_node;  ///< Source node ID
  NodeID target_node;  ///< Target node ID
};

/**
 * @brief Progress callback for batch operations
 */
using ProgressCallback = std::function<void(size_t current, size_t total,
                                            const std::string& message)>;

}  // namespace orc::presenters
