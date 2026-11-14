/*
 * File:        view_plugin_loader.h
 * Module:      orc-core
 * Purpose:     Loads plugins' view binaries (orc-gui only) and keeps the
 *              contract-id -> widget-factory table
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#if defined(ORC_GUI_BUILD) || defined(ORC_CLI_BUILD)
#error \
    "view_plugin_loader.h is a core-only header. Use orc/presenters/include/view_plugins.h."
#endif

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../sdk/include/orc/view/orc_view_plugin_abi.h"
#include "stage_plugin_registry.h"

namespace orc {

/// What the running host offers a view binary.
struct ViewHostVersion {
  uint32_t view_abi = view::kViewPluginAbiVersion;
  uint32_t qt_major = 0;  ///< Of the Qt the host is running.
  uint32_t qt_minor = 0;
  std::string toolchain_tag;
};

/// The gate (plugin-architecture.md, "View Plugins"). Returns an empty string
/// when @p descriptor can load into @p host, else why it cannot.
std::string check_view_descriptor(const view::ViewPluginDescriptor& descriptor,
                                  const ViewHostVersion& host);

struct LoadedViewPlugin {
  std::string path;
  std::string plugin_id;
  std::string plugin_version;
  std::vector<std::string> contract_ids;
};

struct ViewPluginDiagnostic {
  enum class Severity { Info, Warning, Error };
  Severity severity = Severity::Info;
  std::string path;
  std::string message;
};

/**
 * @brief The view binaries loaded into this process
 *
 * Only orc-gui fills it. Libraries are never closed: widgets a view created
 * can outlive any point at which unloading would be safe, so a loaded view
 * stays mapped until the process exits.
 *
 * Thread safety: load() and load_binary() run on the GUI thread at startup;
 * find_factory() may then be called from the GUI thread at any time.
 */
class ViewPluginRegistry {
 public:
  static ViewPluginRegistry& instance();

  /// Load the views of every trusted, enabled registry @p entries that has
  /// one (downloading a release asset when its view_path is empty), then
  /// every view binary found in @p folders. A plugin id is loaded once: the
  /// registry's copy wins over a folder's.
  void load(const ViewHostVersion& host,
            const std::vector<StagePluginRegistryEntry>& entries,
            const std::vector<std::string>& folders);

  /// Load one view binary. Returns false, with a diagnostic recorded, when it
  /// was refused.
  bool load_binary(const std::string& path, const ViewHostVersion& host);

  /// The factory registered for @p contract_id, or nullptr.
  view::OrcViewFactoryFn find_factory(const std::string& contract_id) const;

  const std::vector<LoadedViewPlugin>& loaded() const { return loaded_; }
  const std::vector<ViewPluginDiagnostic>& diagnostics() const {
    return diagnostics_;
  }

  /// load() has run in this process: it is orc-gui.
  bool attempted() const { return attempted_; }

  /// Forget every factory and diagnostic (tests). Libraries stay mapped.
  void reset();

 private:
  static bool register_trampoline(void* context, const char* contract_id,
                                  view::OrcViewFactoryFn factory);
  void diagnose(ViewPluginDiagnostic::Severity severity,
                const std::string& path, const std::string& message);
  bool is_plugin_loaded(const std::string& plugin_id) const;

  std::map<std::string, view::OrcViewFactoryFn> factories_;
  std::vector<LoadedViewPlugin> loaded_;
  std::vector<ViewPluginDiagnostic> diagnostics_;
  bool attempted_ = false;
};

}  // namespace orc
