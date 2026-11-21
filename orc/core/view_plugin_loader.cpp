/*
 * File:        view_plugin_loader.cpp
 * Module:      orc-core
 * Purpose:     Loads plugins' view binaries (orc-gui only) and keeps the
 *              contract-id -> widget-factory table
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "include/view_plugin_loader.h"

#include <logging.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <system_error>

#include "include/plugin_artifact_name.h"
#include "include/plugin_remote_loader.h"
#include "include/plugin_safe_call.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace orc {
namespace {

void* open_library(const std::string& path, std::string& error) {
#if defined(_WIN32)
  HMODULE handle = LoadLibraryA(path.c_str());
  if (!handle) {
    error = "LoadLibrary failed";
  }
  return reinterpret_cast<void*>(handle);
#else
  // RTLD_LOCAL as for stage binaries: each view keeps its symbols to itself.
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* message = dlerror();
    error = message ? message : "dlopen failed";
  }
  return handle;
#endif
}

void* find_symbol(void* handle, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(
      GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
  return dlsym(handle, name);
#endif
}

bool is_library_file(const std::filesystem::path& path) {
#if defined(_WIN32)
  return path.extension() == ".dll";
#elif defined(__APPLE__)
  return path.extension() == ".dylib";
#else
  return path.extension() == ".so";
#endif
}

std::string text_or_empty(const char* value) {
  return value ? std::string(value) : std::string();
}

void host_log(view::ViewLogLevel level, const char* message) {
  if (!message) {
    return;
  }
  switch (level) {
    case view::ViewLogLevel::Trace:
      ORC_LOG_TRACE("[view] {}", message);
      break;
    case view::ViewLogLevel::Debug:
      ORC_LOG_DEBUG("[view] {}", message);
      break;
    case view::ViewLogLevel::Info:
      ORC_LOG_INFO("[view] {}", message);
      break;
    case view::ViewLogLevel::Warn:
      ORC_LOG_WARN("[view] {}", message);
      break;
    case view::ViewLogLevel::Error:
      ORC_LOG_ERROR("[view] {}", message);
      break;
  }
}

const view::ViewHostServices kHostServices{sizeof(view::ViewHostServices),
                                           &host_log};

struct RegisterContext {
  ViewPluginRegistry* registry = nullptr;
  std::map<std::string, view::OrcViewFactoryFn>* factories = nullptr;
  std::vector<std::string>* accepted = nullptr;
  std::string path;
  std::vector<std::string>* refused = nullptr;
};

std::string host_platform() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

}  // namespace

std::string check_view_descriptor(const view::ViewPluginDescriptor& descriptor,
                                  const ViewHostVersion& host) {
  if (text_or_empty(descriptor.plugin_id).empty()) {
    return "its descriptor has no plugin_id";
  }
  if (descriptor.view_abi_version != host.view_abi) {
    return "it is built for view ABI " +
           std::to_string(descriptor.view_abi_version) +
           ", but this host uses view ABI " + std::to_string(host.view_abi);
  }
  if (descriptor.qt_version_major != host.qt_major ||
      descriptor.qt_version_minor > host.qt_minor) {
    return "it is built for Qt " + std::to_string(descriptor.qt_version_major) +
           "." + std::to_string(descriptor.qt_version_minor) +
           ", but this host runs Qt " + std::to_string(host.qt_major) + "." +
           std::to_string(host.qt_minor) +
           " (the major version must match and the minor must not be newer)";
  }
  const std::string tag = text_or_empty(descriptor.toolchain_tag);
  if (tag != host.toolchain_tag) {
    return "it is built with toolchain '" + tag +
           "', but this host requires '" + host.toolchain_tag + "'";
  }
  return {};
}

ViewPluginRegistry& ViewPluginRegistry::instance() {
  static ViewPluginRegistry registry;
  return registry;
}

void ViewPluginRegistry::reset() {
  factories_.clear();
  loaded_.clear();
  diagnostics_.clear();
  attempted_ = false;
}

void ViewPluginRegistry::diagnose(ViewPluginDiagnostic::Severity severity,
                                  const std::string& path,
                                  const std::string& message) {
  diagnostics_.push_back({severity, path, message});
  switch (severity) {
    case ViewPluginDiagnostic::Severity::Info:
      ORC_LOG_DEBUG("View plugin {}: {}", path, message);
      break;
    case ViewPluginDiagnostic::Severity::Warning:
      ORC_LOG_WARN("View plugin {}: {}", path, message);
      break;
    case ViewPluginDiagnostic::Severity::Error:
      ORC_LOG_ERROR("View plugin {}: {}", path, message);
      break;
  }
}

bool ViewPluginRegistry::is_plugin_loaded(const std::string& plugin_id) const {
  return std::any_of(loaded_.begin(), loaded_.end(),
                     [&](const LoadedViewPlugin& view) {
                       return view.plugin_id == plugin_id;
                     });
}

view::OrcViewFactoryFn ViewPluginRegistry::find_factory(
    const std::string& contract_id) const {
  const auto it = factories_.find(contract_id);
  return it == factories_.end() ? nullptr : it->second;
}

bool ViewPluginRegistry::register_trampoline(void* context,
                                             const char* contract_id,
                                             view::OrcViewFactoryFn factory) {
  auto* ctx = static_cast<RegisterContext*>(context);
  const std::string id = text_or_empty(contract_id);
  if (!ctx || id.empty() || !factory) {
    return false;
  }
  if (ctx->registry->factories_.count(id) > 0 ||
      ctx->factories->count(id) > 0 ||
      std::find(ctx->accepted->begin(), ctx->accepted->end(), id) !=
          ctx->accepted->end()) {
    // The first view registered for a contract id keeps it. Refusing only
    // this one lets the binary's other views still register.
    ctx->refused->push_back(id);
    return true;
  }
  ctx->accepted->push_back(id);
  (*ctx->factories)[id] = factory;
  return true;
}

bool ViewPluginRegistry::load_binary(const std::string& path,
                                     const ViewHostVersion& host) {
  using Severity = ViewPluginDiagnostic::Severity;
  std::string error;
  void* handle = open_library(path, error);
  if (!handle) {
    diagnose(Severity::Error, path, "Failed to open view binary: " + error);
    return false;
  }

  auto get_descriptor = reinterpret_cast<view::OrcGetViewPluginDescriptorFn>(
      find_symbol(handle, view::kGetViewPluginDescriptorSymbol));
  auto register_plugin = reinterpret_cast<view::OrcRegisterViewPluginFn>(
      find_symbol(handle, view::kRegisterViewPluginSymbol));
  if (!get_descriptor || !register_plugin) {
    diagnose(Severity::Error, path,
             std::string("View binary does not export ") +
                 view::kGetViewPluginDescriptorSymbol + " and " +
                 view::kRegisterViewPluginSymbol);
    return false;
  }

  const view::ViewPluginDescriptor* descriptor = nullptr;
  std::string fault;
  if (!core_internal::plugin_safe_call([&] { descriptor = get_descriptor(); },
                                       fault) ||
      !descriptor) {
    diagnose(Severity::Error, path,
             "View binary returned no descriptor" +
                 (fault.empty() ? std::string() : ": " + fault));
    return false;
  }

  const std::string refusal = check_view_descriptor(*descriptor, host);
  if (!refusal.empty()) {
    diagnose(Severity::Warning, path, "View binary not loaded: " + refusal);
    return false;
  }

  LoadedViewPlugin plugin;
  plugin.path = path;
  plugin.plugin_id = text_or_empty(descriptor->plugin_id);
  plugin.plugin_version = text_or_empty(descriptor->plugin_version);
  if (is_plugin_loaded(plugin.plugin_id)) {
    diagnose(Severity::Warning, path,
             "View binary not loaded: the view of plugin '" + plugin.plugin_id +
                 "' is already loaded from another file");
    return false;
  }

  std::map<std::string, view::OrcViewFactoryFn> pending;
  std::vector<std::string> refused;
  RegisterContext context{this, &pending, &plugin.contract_ids, path, &refused};
  bool registered = false;
  const char* message = nullptr;
  if (!core_internal::plugin_safe_call(
          [&] {
            try {
              registered = register_plugin(&kHostServices, &context,
                                           &register_trampoline, &message);
            } catch (...) {
              registered = false;
            }
          },
          fault) ||
      !registered) {
    diagnose(Severity::Error, path,
             "View binary failed to register: " +
                 (!fault.empty() ? fault
                  : message      ? std::string(message)
                                 : std::string("no reason given")));
    return false;
  }

  for (const auto& id : refused) {
    diagnose(Severity::Warning, path,
             "View for tool contract '" + id +
                 "' was refused: another view already serves it");
  }
  for (auto& [id, factory] : pending) {
    factories_.emplace(id, factory);
  }
  diagnose(Severity::Info, path,
           "Loaded view of plugin '" + plugin.plugin_id + "' (" +
               std::to_string(plugin.contract_ids.size()) + " tool view(s))");
  loaded_.push_back(std::move(plugin));
  return true;
}

void ViewPluginRegistry::load(
    const ViewHostVersion& host,
    const std::vector<StagePluginRegistryEntry>& entries,
    const std::vector<std::string>& folders) {
  using Severity = ViewPluginDiagnostic::Severity;
  attempted_ = true;
  std::set<std::string> attempted;

  for (const auto& entry : entries) {
    if (!entry.has_view() || !entry.enabled ||
        !StagePluginRegistry::is_entry_trusted(entry)) {
      continue;
    }
    std::string path = entry.view_path;
    if (path.empty()) {
      std::vector<std::string> warnings;
      const auto download = PluginRemoteLoader::download_release_asset(
          entry.view_release_asset_url, entry.view_release_asset_name,
          entry.target_platform.empty() ? host_platform()
                                        : entry.target_platform,
          &warnings, entry.view_sha256);
      for (const auto& warning : warnings) {
        diagnose(Severity::Warning, entry.view_release_asset_url, warning);
      }
      if (!download.success) {
        diagnose(Severity::Error, entry.view_release_asset_url,
                 "Failed to download the view of plugin '" + entry.plugin_id +
                     "': " + download.error_message);
        continue;
      }
      path = download.downloaded_path;
    }
    attempted.insert(path);
    load_binary(path, host);
  }

  for (const auto& folder : folders) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) {
      continue;
    }
    std::vector<std::string> candidates;
    for (const auto& item : std::filesystem::directory_iterator(folder, ec)) {
      if (item.is_regular_file() && is_library_file(item.path()) &&
          is_view_binary_file_name(item.path().string())) {
        candidates.push_back(item.path().string());
      }
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto& candidate : candidates) {
      if (attempted.insert(candidate).second) {
        load_binary(candidate, host);
      }
    }
  }
}

}  // namespace orc
