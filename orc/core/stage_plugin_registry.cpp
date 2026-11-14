/*
 * File:        stage_plugin_registry.cpp
 * Module:      orc-core
 * Purpose:     Persistent YAML registry for stage plugins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "include/stage_plugin_registry.h"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <cctype>
#include <filesystem>
#include <fstream>

#include "include/plugin_artifact_name.h"
#include "include/plugin_remote_loader.h"
#include "orc/abi/orc_plugin_abi.h"

namespace orc {
namespace {

constexpr int kRegistrySchemaVersion = 2;

bool is_valid_sha256_hex(const std::string& value) {
  if (value.size() != 64) {
    return false;
  }
  for (char ch : value) {
    if (!std::isxdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
  }
  return true;
}

std::string env_or_empty(const char* name) {
  const char* value = std::getenv(name);
  if (!value) {
    return "";
  }
  return std::string(value);
}

std::filesystem::path resolve_default_config_dir() {
#if defined(_WIN32)
  std::string appdata = env_or_empty("APPDATA");
  if (!appdata.empty()) {
    return std::filesystem::path(appdata) / "decode-orc";
  }

  std::string userprofile = env_or_empty("USERPROFILE");
  if (!userprofile.empty()) {
    return std::filesystem::path(userprofile) / "AppData" / "Roaming" /
           "decode-orc";
  }
#else
  std::string xdg_config_home = env_or_empty("XDG_CONFIG_HOME");
  if (!xdg_config_home.empty()) {
    return std::filesystem::path(xdg_config_home) / "decode-orc";
  }

  std::string home = env_or_empty("HOME");
  if (!home.empty()) {
    return std::filesystem::path(home) / ".config" / "decode-orc";
  }
#endif

  return std::filesystem::current_path() / ".decode-orc";
}

StagePluginRegistryEntry parse_plugin_entry(
    const YAML::Node& node, std::vector<std::string>& warnings) {
  StagePluginRegistryEntry entry;

  if (!node.IsMap()) {
    warnings.push_back("Registry entry is not a map and was skipped");
    return entry;
  }

  entry.plugin_id = node["plugin_id"].as<std::string>("");
  entry.plugin_version = node["plugin_version"].as<std::string>("");
  entry.path = node["path"].as<std::string>("");
  entry.source_repo_url = node["source_repo_url"].as<std::string>("");
  entry.artifact_source = node["artifact_source"].as<std::string>("local_path");
  entry.release_asset_url = node["release_asset_url"].as<std::string>("");
  entry.release_tag = node["release_tag"].as<std::string>("");
  entry.release_asset_name = node["release_asset_name"].as<std::string>("");
  entry.target_platform = node["target_platform"].as<std::string>("");
  entry.local_dev_path = node["local_dev_path"].as<std::string>("");
  entry.enabled = node["enabled"].as<bool>(true);
  entry.trust_state = node["trust_state"].as<std::string>("untrusted");
  entry.license_spdx = node["license_spdx"].as<std::string>("");
  entry.is_core_plugin = node["is_core_plugin"].as<bool>(false);
  entry.required_host_abi = node["required_host_abi"].as<uint32_t>(0);
  entry.sha256 = node["sha256"].as<std::string>("");
  entry.view_path = node["view_path"].as<std::string>("");
  entry.view_release_asset_url =
      node["view_release_asset_url"].as<std::string>("");
  entry.view_release_asset_name =
      node["view_release_asset_name"].as<std::string>("");
  entry.view_sha256 = node["view_sha256"].as<std::string>("");

  if (!entry.view_sha256.empty() && !is_valid_sha256_hex(entry.view_sha256)) {
    warnings.push_back(
        "Registry entry has invalid view_sha256 '" + entry.view_sha256 +
        "' (expected 64 hexadecimal characters); the view binary will fail "
        "checksum verification");
  }

  if (!entry.sha256.empty() && !is_valid_sha256_hex(entry.sha256)) {
    warnings.push_back(
        "Registry entry has invalid sha256 '" + entry.sha256 +
        "' (expected 64 hexadecimal characters); the artifact will fail "
        "checksum verification");
  }

  if (entry.artifact_source != "local_path" &&
      entry.artifact_source != "github_release_asset") {
    warnings.push_back("Registry entry has unknown artifact_source '" +
                       entry.artifact_source +
                       "'; expected 'local_path' or 'github_release_asset'");
  }

  if (!entry.release_asset_name.empty() &&
      !is_valid_release_asset_name(entry.release_asset_name)) {
    warnings.push_back(
        "Registry entry has invalid release_asset_name '" +
        entry.release_asset_name +
        "' (expected "
        "orc-plugin_<stage-name>_<platform>[_abi<N>].<so|dylib|dll>)");
  }

  if (entry.path.empty() && !entry.local_dev_path.empty()) {
    entry.path = entry.local_dev_path;
  }

  if (entry.path.empty()) {
    if (entry.artifact_source == "github_release_asset" &&
        !entry.release_asset_url.empty()) {
      // Note: empty path is now acceptable for remote entries; download will be
      // attempted during registry loading in parse_yaml().
      // Only warn if asset_name is also missing, which prevents download.
      if (entry.release_asset_name.empty()) {
        warnings.push_back(
            "Registry entry references a remote release asset but has no "
            "release_asset_name "
            "(required for download)");
      }
    } else {
      warnings.push_back("Registry entry has empty path");
    }
  }

  return entry;
}

YAML::Node to_yaml_node(const StagePluginRegistryEntry& entry) {
  YAML::Node node;
  node["plugin_id"] = entry.plugin_id;
  node["plugin_version"] = entry.plugin_version;
  node["path"] = entry.path;
  node["source_repo_url"] = entry.source_repo_url;
  node["artifact_source"] = entry.artifact_source;
  node["release_asset_url"] = entry.release_asset_url;
  node["release_tag"] = entry.release_tag;
  node["release_asset_name"] = entry.release_asset_name;
  node["target_platform"] = entry.target_platform;
  node["local_dev_path"] = entry.local_dev_path;
  node["enabled"] = entry.enabled;
  node["trust_state"] = entry.trust_state;
  node["license_spdx"] = entry.license_spdx;
  node["is_core_plugin"] = entry.is_core_plugin;
  node["required_host_abi"] = entry.required_host_abi;
  node["sha256"] = entry.sha256;
  // Written only for a plugin with a view, so a registry of stage-only
  // plugins reads exactly as it did before views existed.
  if (entry.has_view()) {
    node["view_path"] = entry.view_path;
    node["view_release_asset_url"] = entry.view_release_asset_url;
    node["view_release_asset_name"] = entry.view_release_asset_name;
    node["view_sha256"] = entry.view_sha256;
  }
  return node;
}

}  // namespace

std::string StagePluginRegistry::default_registry_path() {
  return (resolve_default_config_dir() / "stage-plugins.yaml").string();
}

bool StagePluginRegistry::is_entry_trusted(
    const StagePluginRegistryEntry& entry) {
  return entry.is_core_plugin || entry.trust_state == "trusted";
}

StagePluginRegistry::LoadResult StagePluginRegistry::parse_yaml(
    const std::string& yaml_text, const std::string& registry_path) {
  LoadResult result;
  result.registry_path = registry_path;

  YAML::Node root;
  try {
    root = YAML::Load(yaml_text);
  } catch (const YAML::Exception& e) {
    result.warnings.push_back(
        std::string("Failed to parse plugin registry YAML: ") + e.what());
    return result;
  }

  const int version = root["version"].as<int>(kRegistrySchemaVersion);
  if (version > kRegistrySchemaVersion) {
    result.warnings.push_back("Unexpected plugin registry schema version " +
                              std::to_string(version) + "; expected " +
                              std::to_string(kRegistrySchemaVersion));
  }

  const YAML::Node plugins_node = root["plugins"];
  if (!plugins_node || !plugins_node.IsSequence()) {
    return result;
  }

  for (const auto& plugin_node : plugins_node) {
    auto entry = parse_plugin_entry(plugin_node, result.warnings);

    // ABI gate: an entry that declares an incompatible required_host_abi is
    // neither downloaded nor loaded — the load-time gate in the loader would
    // reject the binary anyway, so fail early with a clearer message. A zero
    // value means "unspecified" and is not gated (legacy entries). Core
    // plugins are compiled in-tree against the current ABI and are exempt
    // (mirroring is_entry_trusted). Keep the entry visible with no resolved
    // path so GUI/CLI listings can explain why, mirroring the trust gate below.
    if (!entry.is_core_plugin && entry.required_host_abi != 0 &&
        entry.required_host_abi != kStagePluginHostAbiVersion) {
      result.warnings.push_back(
          "Plugin '" + entry.plugin_id + "' targets Orc ABI " +
          std::to_string(entry.required_host_abi) + " but the host is ABI " +
          std::to_string(kStagePluginHostAbiVersion) +
          "; it needs a rebuild for ABI " +
          std::to_string(kStagePluginHostAbiVersion) +
          ". Skipping download and load.");
      entry.path.clear();
      result.entries.push_back(std::move(entry));
      continue;
    }

    // If entry has no path but references a remote release asset, attempt to
    // download it
    if (entry.path.empty() && entry.artifact_source == "github_release_asset" &&
        !entry.release_asset_url.empty() && !entry.release_asset_name.empty()) {
      if (!is_entry_trusted(entry)) {
        // Trust gate: never fetch code the loader would refuse to load.
        // Keep the entry (with its empty path) so GUI/CLI listings still
        // show it and the user can mark it trusted.
        result.warnings.push_back(
            "Plugin '" + entry.plugin_id +
            "' is untrusted; skipping artifact download and load (mark it "
            "trusted to enable it, e.g. 'orc-cli plugins trust " +
            entry.plugin_id + "')");
        result.entries.push_back(std::move(entry));
        continue;
      }

      spdlog::debug(
          "Attempting to download remote plugin '{}' from GitHub release: {}",
          entry.plugin_id, entry.release_asset_url);

      // Determine target platform for cache
      std::string target_platform = entry.target_platform;
      if (target_platform.empty()) {
        // Infer from asset name if not explicitly set
#if defined(_WIN32)
        target_platform = "windows";
#elif defined(__APPLE__)
        target_platform = "macos";
#else
        target_platform = "linux";
#endif
      }

      auto download_result = PluginRemoteLoader::download_release_asset(
          entry.release_asset_url, entry.release_asset_name, target_platform,
          &result.warnings, entry.sha256);

      if (download_result.success) {
        entry.path = download_result.downloaded_path;
        spdlog::debug("Successfully resolved remote plugin '{}' to cache: {}",
                      entry.plugin_id, entry.path);
      } else {
        result.warnings.push_back("Failed to download plugin '" +
                                  entry.plugin_id +
                                  "': " + download_result.error_message);
        continue;  // Skip this entry if download failed
      }
    }

    // Only add entries that have a valid path (either pre-existing or
    // downloaded)
    if (entry.path.empty()) {
      continue;
    }

    result.entries.push_back(std::move(entry));
  }

  return result;
}

StagePluginRegistry::LoadResult StagePluginRegistry::load_default() {
  LoadResult result;
  result.registry_path = default_registry_path();

  const std::filesystem::path path(result.registry_path);
  if (!std::filesystem::exists(path)) {
    result.warnings.push_back(
        "Plugin registry file not found; starting with empty registry");
    return result;
  }

  std::ifstream file(result.registry_path, std::ios::in);
  if (!file) {
    result.warnings.push_back(
        "Failed to open plugin registry file for reading");
    return result;
  }

  std::string yaml_text((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

  result = parse_yaml(yaml_text, result.registry_path);
  result.loaded_from_disk = true;
  return result;
}

std::string StagePluginRegistry::serialize_yaml(
    const std::vector<StagePluginRegistryEntry>& entries) {
  YAML::Node root;
  root["version"] = kRegistrySchemaVersion;
  YAML::Node plugins = YAML::Node(YAML::NodeType::Sequence);

  for (const auto& entry : entries) {
    plugins.push_back(to_yaml_node(entry));
  }

  root["plugins"] = plugins;

  YAML::Emitter out;
  out << root;
  return out.c_str();
}

bool StagePluginRegistry::save(
    const std::string& registry_path,
    const std::vector<StagePluginRegistryEntry>& entries,
    std::string* error_message) {
  try {
    const std::filesystem::path path(registry_path);
    std::filesystem::create_directories(path.parent_path());

    std::ofstream file(registry_path, std::ios::out | std::ios::trunc);
    file << serialize_yaml(entries);
    return true;
  } catch (const std::exception& e) {
    if (error_message) {
      *error_message = e.what();
    }
    return false;
  }
}

}  // namespace orc
