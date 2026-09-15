/*
 * File:        stage_registry.cpp
 * Module:      orc-core
 * Purpose:     Stage type registration
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "stage_registry.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <set>

#include "include/plugin_safe_call.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace orc {

namespace {

std::vector<std::string> split_plugin_search_paths(
    const std::string& raw_paths) {
  std::vector<std::string> paths;

#if defined(_WIN32)
  constexpr char kPathSeparator = ';';
#else
  constexpr char kPathSeparator = ':';
#endif

  std::string current;
  for (char ch : raw_paths) {
    if (ch == kPathSeparator) {
      if (!current.empty()) {
        paths.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }

  if (!current.empty()) {
    paths.push_back(current);
  }

  return paths;
}

bool is_shared_library_path(const std::filesystem::path& path) {
#if defined(_WIN32)
  return path.extension() == ".dll";
#elif defined(__APPLE__)
  return path.extension() == ".dylib";
#else
  return path.extension() == ".so";
#endif
}

std::vector<std::string> discover_plugin_candidates(
    const std::vector<std::string>& search_paths,
    std::vector<StagePluginDiagnostic>& diagnostics) {
  std::vector<std::string> candidates;
  std::set<std::string> unique_candidates;

  for (const auto& raw_path : search_paths) {
    std::filesystem::path path(raw_path);
    std::error_code error_code;
    const auto status = std::filesystem::status(path, error_code);

    if (error_code) {
      diagnostics.push_back(
          {StagePluginDiagnosticSeverity::Warning, raw_path,
           "Unable to inspect plugin search path: " + error_code.message()});
      continue;
    }

    if (!std::filesystem::exists(status)) {
      diagnostics.push_back({StagePluginDiagnosticSeverity::Warning, raw_path,
                             "Configured plugin search path does not exist"});
      continue;
    }

    if (std::filesystem::is_regular_file(status)) {
      if (is_shared_library_path(path)) {
        std::error_code ec2;
        const auto canonical = std::filesystem::canonical(path, ec2);
        if (!ec2) {
          unique_candidates.insert(canonical.string());
        } else {
          unique_candidates.insert(std::filesystem::absolute(path).string());
        }
      } else {
        diagnostics.push_back(
            {StagePluginDiagnosticSeverity::Warning, raw_path,
             "Configured plugin file does not look like a shared library"});
      }
      continue;
    }

    if (!std::filesystem::is_directory(status)) {
      diagnostics.push_back(
          {StagePluginDiagnosticSeverity::Warning, raw_path,
           "Configured plugin search path is neither a file nor a directory"});
      continue;
    }

    for (const auto& entry :
         std::filesystem::directory_iterator(path, error_code)) {
      if (error_code) {
        diagnostics.push_back({StagePluginDiagnosticSeverity::Warning, raw_path,
                               "Failed while enumerating plugin directory: " +
                                   error_code.message()});
        break;
      }

      if (!entry.is_regular_file()) {
        continue;
      }

      const auto& entry_path = entry.path();
      if (!is_shared_library_path(entry_path)) {
        continue;
      }

      std::error_code ec2;
      const auto canonical = std::filesystem::canonical(entry_path, ec2);
      if (!ec2) {
        unique_candidates.insert(canonical.string());
      } else {
        unique_candidates.insert(entry_path.string());
      }
    }
  }

  candidates.assign(unique_candidates.begin(), unique_candidates.end());
  std::sort(candidates.begin(), candidates.end());
  return candidates;
}

std::vector<std::string> read_configured_plugin_search_paths() {
  const char* raw_paths = std::getenv("ORC_STAGE_PLUGIN_PATHS");
  if (!raw_paths || std::string(raw_paths).empty()) {
    return {};
  }

  return split_plugin_search_paths(raw_paths);
}

std::vector<std::string> collect_default_plugin_search_paths() {
  std::vector<std::string> preferred_paths;
  bool found_executable_relative_path = false;

  std::filesystem::path executable_path;

#if defined(_WIN32)
  std::vector<char> buffer(MAX_PATH, '\0');
  DWORD size = GetModuleFileNameA(nullptr, buffer.data(),
                                  static_cast<DWORD>(buffer.size()));
  if (size > 0 && size < buffer.size()) {
    executable_path = std::filesystem::path(std::string(buffer.data(), size));
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  if (size > 0) {
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
      executable_path = std::filesystem::path(buffer.data());
    }
  }
#else
  char buffer[PATH_MAX] = {};
  const ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (size > 0) {
    buffer[size] = '\0';
    executable_path = std::filesystem::path(buffer);
  }
#endif

  const auto plugin_dir = plugin_dir_for_executable(executable_path);
  if (!plugin_dir.empty()) {
    std::error_code error_code;
    if (std::filesystem::exists(plugin_dir, error_code) && !error_code) {
      preferred_paths.push_back(plugin_dir.string());
      found_executable_relative_path = true;
    }
  }

#if defined(ORC_STAGE_PLUGIN_BUILD_DIR)
  if (!found_executable_relative_path) {
    preferred_paths.push_back(ORC_STAGE_PLUGIN_BUILD_DIR);
  }
#endif

  std::vector<std::string> paths;
  std::set<std::string> unique_paths;
  for (const auto& path : preferred_paths) {
    std::error_code error_code;
    if (std::filesystem::exists(std::filesystem::path(path), error_code) &&
        !error_code && unique_paths.insert(path).second) {
      paths.push_back(path);
    }
  }

  return paths;
}

}  // namespace

std::filesystem::path plugin_dir_for_executable(
    const std::filesystem::path& executable_path) {
  if (executable_path.empty()) {
    return {};
  }

  // Resolve symlinks before deriving the layout below: the search is relative
  // to where the executable really lives, not to the path it was invoked
  // through. See the header for why only Linux arrives here already resolved.
  std::error_code resolve_error;
  const auto resolved_path =
      std::filesystem::canonical(executable_path, resolve_error);
  const auto executable_dir = resolve_error ? executable_path.parent_path()
                                            : resolved_path.parent_path();

#if defined(_WIN32)
  return (executable_dir / "orc-stage-plugins").lexically_normal();
#elif defined(__APPLE__)
  return (executable_dir / ".." / "PlugIns" / "orc-stage-plugins")
      .lexically_normal();
#else
  return (executable_dir / ".." / "lib" / "orc-stage-plugins")
      .lexically_normal();
#endif
}

std::vector<std::string> collect_trusted_registry_plugin_paths(
    const std::vector<StagePluginRegistryEntry>& entries,
    std::vector<StagePluginDiagnostic>& diagnostics) {
  std::vector<std::string> paths;

  for (const auto& entry : entries) {
    if (!entry.enabled) {
      continue;
    }

    if (!StagePluginRegistry::is_entry_trusted(entry)) {
      const std::string label =
          entry.plugin_id.empty() ? entry.path : entry.plugin_id;
      diagnostics.push_back(
          {StagePluginDiagnosticSeverity::Warning, entry.path,
           "Plugin registry entry '" + label +
               "' is untrusted and was not loaded; mark it trusted to enable "
               "it (e.g. 'orc-cli plugins trust " +
               label + "')"});
      continue;
    }

    if (entry.path.empty()) {
      diagnostics.push_back({StagePluginDiagnosticSeverity::Warning, "",
                             "Plugin registry entry has empty path"});
      continue;
    }

    paths.push_back(entry.path);
  }

  return paths;
}

StageRegistry& StageRegistry::instance() {
  static StageRegistry registry;
  registry.initialize();
  return registry;
}

void StageRegistry::initialize() {
  if (initialized_) {
    return;
  }

  initialize_runtime_plugins();
  initialized_ = true;
}

void StageRegistry::register_stage(const std::string& stage_name,
                                   StageFactory factory) {
  if (factories_.find(stage_name) != factories_.end()) {
    throw StageRegistryError("Stage already registered: " + stage_name);
  }
  factories_[stage_name] = factory;
}

DAGStagePtr StageRegistry::create_stage(const std::string& stage_name) const {
  auto it = factories_.find(stage_name);
  if (it == factories_.end()) {
    throw StageRegistryError("Unknown stage: " + stage_name);
  }
  return it->second();
}

bool StageRegistry::has_stage(const std::string& stage_name) const {
  return factories_.find(stage_name) != factories_.end();
}

std::vector<std::string> StageRegistry::get_registered_stages() const {
  std::vector<std::string> names;
  names.reserve(factories_.size());
  for (const auto& pair : factories_) {
    names.push_back(pair.first);
  }
  std::sort(names.begin(), names.end());
  return names;
}

const std::vector<LoadedStagePlugin>& StageRegistry::get_loaded_plugins()
    const {
  return plugin_loader_.loaded_plugins();
}

const std::vector<StagePluginDiagnostic>&
StageRegistry::get_plugin_diagnostics() const {
  return plugin_diagnostics_;
}

const std::vector<std::string>& StageRegistry::get_plugin_search_paths() const {
  return plugin_search_paths_;
}

const std::vector<StagePluginRegistryEntry>&
StageRegistry::get_plugin_registry_entries() const {
  return plugin_registry_entries_;
}

void StageRegistry::clear() {
  plugin_loader_.unload_all();
  factories_.clear();
  plugin_diagnostics_.clear();
  plugin_search_paths_.clear();
  plugin_registry_path_.clear();
  plugin_registry_entries_.clear();
  initialized_ = false;
}

void StageRegistry::initialize_runtime_plugins() {
  plugin_diagnostics_.clear();
  plugin_search_paths_.clear();
  plugin_registry_entries_.clear();

  auto registry_result = StagePluginRegistry::load_default();
  plugin_registry_path_ = registry_result.registry_path;
  plugin_registry_entries_ = std::move(registry_result.entries);

  for (const auto& warning : registry_result.warnings) {
    add_plugin_diagnostic(StagePluginDiagnosticSeverity::Warning,
                          plugin_registry_path_, warning);
  }

  if (!registry_result.loaded_from_disk) {
    add_plugin_diagnostic(StagePluginDiagnosticSeverity::Info,
                          plugin_registry_path_,
                          "Plugin registry file not loaded from disk; using "
                          "in-memory empty registry");
  }

  std::set<std::string> unique_search_paths;
  const auto default_paths = collect_default_plugin_search_paths();
  unique_search_paths.insert(default_paths.begin(), default_paths.end());

  if (!default_paths.empty()) {
    add_plugin_diagnostic(
        StagePluginDiagnosticSeverity::Info, "",
        "Using default runtime plugin search paths from build/install layout");
  }

  const auto registry_paths = collect_trusted_registry_plugin_paths(
      plugin_registry_entries_, plugin_diagnostics_);
  unique_search_paths.insert(registry_paths.begin(), registry_paths.end());

  const auto env_paths = read_configured_plugin_search_paths();
  if (!env_paths.empty()) {
    add_plugin_diagnostic(
        StagePluginDiagnosticSeverity::Info, "",
        "Using runtime plugin paths from ORC_STAGE_PLUGIN_PATHS in addition to "
        "YAML registry entries");
    unique_search_paths.insert(env_paths.begin(), env_paths.end());
  }

  plugin_search_paths_.assign(unique_search_paths.begin(),
                              unique_search_paths.end());
  std::sort(plugin_search_paths_.begin(), plugin_search_paths_.end());

  if (plugin_search_paths_.empty()) {
    add_plugin_diagnostic(
        StagePluginDiagnosticSeverity::Info, plugin_registry_path_,
        "No runtime stage plugins configured (registry has no enabled entries "
        "and ORC_STAGE_PLUGIN_PATHS is empty)");
    return;
  }

  auto plugin_candidates =
      discover_plugin_candidates(plugin_search_paths_, plugin_diagnostics_);
  if (plugin_candidates.empty()) {
    add_plugin_diagnostic(StagePluginDiagnosticSeverity::Info, "",
                          "No runtime stage plugin binaries were discovered in "
                          "configured search paths");
    return;
  }

  for (const auto& plugin_path : plugin_candidates) {
    std::map<std::string, StageFactory> pending_factories;

    auto load_result = plugin_loader_.load_plugin(
        plugin_path, [this, &pending_factories, &plugin_path](
                         const std::string& stage_name,
                         StagePluginLoader::RegisterStageFactory factory) {
          if (factories_.find(stage_name) != factories_.end() ||
              pending_factories.find(stage_name) != pending_factories.end()) {
            return false;
          }

          DAGStagePtr stage;
          std::optional<NodeTypeInfo> node_type_info;
          std::string fault_error;
          // Fault-guard note (see plugin_safe_call.h): this guarded region
          // does NOT meet the "raw C function pointers only" constraint — the
          // factory constructs a C++ stage object and get_node_type_info()
          // builds strings. If the plugin faults here, siglongjmp abandons
          // those objects in flight (leaking memory). Accepted residual risk:
          // this runs once per stage at startup, and surviving a faulty
          // plugin with a diagnostic beats crashing the host.
          bool metadata_ok = core_internal::plugin_safe_call(
              [&] {
                try {
                  stage = factory();
                } catch (const std::exception& ex) {
                  add_plugin_diagnostic(
                      StagePluginDiagnosticSeverity::Error, plugin_path,
                      "Plugin stage '" + stage_name +
                          "' factory threw while validating metadata: " +
                          ex.what());
                  stage = nullptr;
                  return;
                } catch (...) {
                  add_plugin_diagnostic(
                      StagePluginDiagnosticSeverity::Error, plugin_path,
                      "Plugin stage '" + stage_name +
                          "' factory threw while validating metadata");
                  stage = nullptr;
                  return;
                }
                if (stage) {
                  node_type_info = stage->get_node_type_info();
                }
              },
              fault_error);

          if (!metadata_ok) {
            add_plugin_diagnostic(
                StagePluginDiagnosticSeverity::Error, plugin_path,
                "Plugin stage '" + stage_name +
                    "' caused a fatal fault during metadata validation: " +
                    fault_error);
            return false;
          }

          if (!stage) {
            // Error already recorded inside the safe-call lambda above
            // (factory threw an exception); if stage is still null without
            // an explicit diagnostic, add a generic one.
            bool already_diagnosed = [&] {
              for (const auto& d : plugin_diagnostics_) {
                if (d.message.find(stage_name) != std::string::npos) {
                  return true;
                }
              }
              return false;
            }();
            if (!already_diagnosed) {
              add_plugin_diagnostic(StagePluginDiagnosticSeverity::Error,
                                    plugin_path,
                                    "Plugin stage '" + stage_name +
                                        "' returned null from factory during "
                                        "metadata validation");
            }
            return false;
          }
          if (node_type_info->display_name.empty()) {
            add_plugin_diagnostic(
                StagePluginDiagnosticSeverity::Error, plugin_path,
                "Plugin stage '" + stage_name +
                    "' is missing required display_name metadata");
            return false;
          }

          pending_factories[stage_name] = std::move(factory);
          return true;
        });

    if (!load_result.success) {
      add_plugin_diagnostic(StagePluginDiagnosticSeverity::Error, plugin_path,
                            load_result.error_message);
      ORC_LOG_WARN("{}", load_result.error_message);
      continue;
    }

    if (!load_result.plugin.has_value()) continue;
    const auto& plugin = *load_result.plugin;
    const std::string plugin_version = plugin.plugin_version.empty()
                                           ? std::string("unknown")
                                           : plugin.plugin_version;
    factories_.insert(pending_factories.begin(), pending_factories.end());
    add_plugin_diagnostic(
        StagePluginDiagnosticSeverity::Info, plugin_path,
        "Loaded plugin '" + plugin.plugin_id + "' v" + plugin_version +
            " registering " +
            std::to_string(plugin.registered_stage_names.size()) + " stage(s)");
    ORC_LOG_DEBUG(
        "Loaded stage plugin '{}' v{} from '{}' registering {} stage(s)",
        plugin.plugin_id, plugin_version, plugin_path,
        plugin.registered_stage_names.size());
  }
}

void StageRegistry::add_plugin_diagnostic(
    StagePluginDiagnosticSeverity severity, const std::string& path,
    const std::string& message) {
  plugin_diagnostics_.push_back({severity, path, message});
}

}  // namespace orc
