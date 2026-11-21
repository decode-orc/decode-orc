/*
 * File:        stage_plugin_registry.h
 * Module:      orc-core
 * Purpose:     Persistent YAML registry for stage plugins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/stage_plugin_registry.h. Use ProjectPresenter for plugin-aware stage access."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/stage_plugin_registry.h. Use ProjectPresenter for plugin-aware stage access."
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace orc {

struct StagePluginRegistryEntry {
  std::string plugin_id;
  std::string plugin_version;
  std::string path;
  std::string source_repo_url;
  // local_path: load directly from path/local_dev_path
  // github_release_asset: metadata points at a prebuilt release artifact
  std::string artifact_source = "local_path";
  std::string release_asset_url;
  std::string release_tag;
  std::string release_asset_name;
  std::string target_platform;
  // Optional path used during test/development in place of remote release URL.
  std::string local_dev_path;
  bool enabled = true;
  std::string trust_state = "untrusted";
  std::string license_spdx;
  bool is_core_plugin = false;
  uint32_t required_host_abi = 0;
  // Expected SHA-256 digest (64 hex chars) of the plugin binary for
  // github_release_asset entries; verified after download and on cache hits.
  std::string sha256;

  // The plugin's view binary (plugin-architecture.md, "View Plugins"), when it
  // has one. Read only by orc-gui; orc-cli carries the fields through
  // unchanged. view_path is a local file; the release fields let orc-gui
  // download it on first start, checked against view_sha256.
  std::string view_path;
  std::string view_release_asset_url;
  std::string view_release_asset_name;
  std::string view_sha256;

  bool has_view() const {
    return !view_path.empty() || !view_release_asset_url.empty();
  }
};

class StagePluginRegistry {
 public:
  struct LoadResult {
    std::string registry_path;
    bool loaded_from_disk = false;
    std::vector<std::string> warnings;
    std::vector<StagePluginRegistryEntry> entries;
  };

  static std::string default_registry_path();
  static LoadResult load_default();

  // Trust policy: core plugins are implicitly trusted; every other entry
  // must carry trust_state == "trusted" before its binary may be downloaded
  // or dlopen'ed.
  static bool is_entry_trusted(const StagePluginRegistryEntry& entry);

  static LoadResult parse_yaml(const std::string& yaml_text,
                               const std::string& registry_path = "<memory>");
  static std::string serialize_yaml(
      const std::vector<StagePluginRegistryEntry>& entries);
  static bool save(const std::string& registry_path,
                   const std::vector<StagePluginRegistryEntry>& entries,
                   std::string* error_message = nullptr);
};

}  // namespace orc
