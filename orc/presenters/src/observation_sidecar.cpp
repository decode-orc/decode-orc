/*
 * File:        observation_sidecar.cpp
 * Module:      orc-presenters
 * Purpose:     The per-source observation sidecar: where it lives, and an
 *              observation store persisted to it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "observation_sidecar.h"

#include <orc/stage/node_type.h>
#include <orc/support/logging.h>
#include <orc/support/pipe_io.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <variant>

#include "../core/include/dag_executor.h"
#include "../core/include/frame_provenance.h"
#include "../core/include/observation_cache_retention.h"
#include "../core/include/observation_store.h"
#include "../core/include/sqlite_observation_persistence.h"

namespace orc::presenters {

namespace {

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

}  // namespace

std::string perSourceSidecarPath(const orc::DAG& dag) {
  const std::string serial = orc::sidecar_source_key(dag);
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
        "Observation cache: cannot create the cache directory ({}); "
        "continuing in-memory only",
        e.what());
    return {};
  }
}

std::shared_ptr<orc::ObservationStore> openSidecarStore(const orc::DAG& dag) {
  // A source read from a pipe or a network stream has no file identity, so
  // its content's fingerprint would not change with what is read: nothing
  // measured on it can be kept.
  for (const auto& node : dag.nodes()) {
    if (!node.stage ||
        node.stage->get_node_type_info().type != orc::NodeType::SOURCE) {
      continue;
    }
    for (const auto& [name, value] : node.parameters) {
      const auto* path = std::get_if<std::string>(&value);
      if (path && (orc::pipe_io::is_pipe_path(*path) ||
                   orc::pipe_io::is_network_stream_url(*path))) {
        return nullptr;
      }
    }
  }

  const std::string db_path = perSourceSidecarPath(dag);
  if (db_path.empty()) {
    return nullptr;
  }
  // A sidecar problem must never fail the run: it then keeps nothing.
  try {
    orc::enforceSidecarRetention(
        std::filesystem::path(db_path).parent_path().string(),
        orc::SidecarRetentionPolicy{}, db_path);
    orc::touchSidecar(db_path);
    auto store = std::make_shared<orc::ObservationStore>();
    store->set_persistence(
        std::make_shared<orc::SqliteObservationPersistence>(db_path));
    ORC_LOG_DEBUG("Observation cache: sidecar at {}", db_path);
    return store;
  } catch (const std::exception& e) {
    ORC_LOG_WARN(
        "Observation cache: sidecar unavailable ({}); stage measurements are "
        "not kept",
        e.what());
    return nullptr;
  }
}

}  // namespace orc::presenters
