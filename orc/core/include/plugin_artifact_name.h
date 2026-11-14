/*
 * File:        plugin_artifact_name.h
 * Module:      orc-core
 * Purpose:     Single source of truth for stage-plugin release-artifact naming:
 *              validation and ABI-tag parsing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace orc {

// Naming convention for a stage-plugin release artifact:
//
//   orc-plugin_<stage>_<platform>[_abi<N>].<so|dylib|dll>
//
// The optional `_abi<N>` token lets one plugin release ship builds for several
// host ABI versions side by side; legacy (untagged) names remain valid.

/// Parsed components of a release-asset filename.
struct ParsedReleaseAssetName {
  bool valid = false;    ///< true when the name matches the convention
  std::string ext;       ///< platform extension without the dot: so/dylib/dll
  bool has_abi = false;  ///< true when an `_abi<N>` token is present
  uint32_t abi = 0;      ///< the parsed ABI number when has_abi is true
};

/// Parse and validate a release-asset filename against the naming convention.
/// Accepts both the legacy `orc-plugin_<stage>_<platform>.<ext>` form and the
/// ABI-tagged `orc-plugin_<stage>_<platform>_abi<N>.<ext>` form.
ParsedReleaseAssetName parse_release_asset_name(const std::string& name);

// A plugin's view binary (plugin-architecture.md, "View Plugins") is named
//
//   [lib]orc-view_<name>....<so|dylib|dll>
//
// The prefix is how a host tells it from a stage binary without opening it:
// the stage loader skips these files, so orc-cli never loads Qt through one.
// Takes a file name or a path; only the final component is looked at.
bool is_view_binary_file_name(const std::string& name);

/// True when `name` is a view binary's release-asset filename:
/// `orc-view_<name>.<so|dylib|dll>`, one path component, no `lib` prefix.
/// Stricter than is_view_binary_file_name, which looks only at the final
/// component of a path: a release asset name becomes a cache file name, so
/// it must not carry directories.
bool is_valid_view_release_asset_name(const std::string& name);

/// Convenience wrapper: true when the name is a valid release-asset filename.
bool is_valid_release_asset_name(const std::string& name);

/// Platform-specific shared-library extension including the leading dot,
/// e.g. ".so" / ".dylib" / ".dll". Unknown platforms default to ".so".
std::string platform_artifact_extension(const std::string& target_platform);

/// Platform token embedded in artifact names, e.g. "_linux" / "_macos" /
/// "_windows". Unknown platforms default to "_linux".
std::string platform_artifact_token(const std::string& target_platform);

/// A downloadable release asset listed by a GitHub release. Selection among
/// candidates is driven by the release manifest (plugin_release_manifest.h);
/// name-based selection was removed when the manifest became mandatory.
struct ReleaseAssetCandidate {
  std::string url;
  std::string name;
};

}  // namespace orc
