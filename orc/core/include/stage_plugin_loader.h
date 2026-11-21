/*
 * File:        stage_plugin_loader.h
 * Module:      orc-core
 * Purpose:     Runtime stage plugin loading contract and loader
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/stage_plugin_loader.h. Use ProjectPresenter for plugin-aware stage access."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/stage_plugin_loader.h. Use ProjectPresenter for plugin-aware stage access."
#endif

// The public plugin SDK header is the canonical source for the ABI contract
// (StagePluginDescriptor, versioning constants, OrcStageFactoryFn, etc.).
// Internal orc-core code includes it via relative path; plugin authors use
// <orc/abi/orc_plugin_abi.h> or the umbrella <orc/abi/orc_plugin_sdk.h>.
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../sdk/include/orc/abi/orc_plugin_abi.h"
#include "dag_executor.h"

namespace orc {

struct LoadedStagePlugin {
  std::string path;
  std::string plugin_id;
  std::string plugin_version;
  std::string license_spdx;
  bool is_core_plugin = false;
  std::vector<std::string> registered_stage_names;
};

namespace core_internal {

// Wraps a raw plugin stage factory so that every stage instance created
// through it shares ownership of `library_keep_alive`. The keep-alive token
// (which closes the plugin shared library when its last reference drops) is
// released only after the stage's destructor has run, guaranteeing that a
// stage's code is never unmapped while the stage object is still alive.
//
// Thread safety: the returned factory and the stages it creates may be used
// and destroyed on any thread; the keep-alive release relies on shared_ptr's
// thread-safe reference counting.
std::function<DAGStagePtr()> make_keepalive_stage_factory(
    OrcStageFactoryFn factory, std::shared_ptr<void> library_keep_alive);

}  // namespace core_internal

// Runtime loader for stage plugin shared libraries.
//
// Lifetime guarantees: each plugin library (and its host service table) is
// reference-counted. References are held by (a) this loader's handle entry,
// (b) every registered stage factory, and (c) every live stage instance
// created by such a factory. unload_all() releases only (a); the shared
// library is actually closed when the last factory copy and stage instance
// are destroyed. Consequently stage objects remain valid — vtables included —
// even if the loader is cleared first.
//
// Thread safety: load_plugin() and unload_all() are not thread-safe and are
// expected to run on a single thread (plugin loading happens once at
// startup). Stage factories and stage instances may be used and released on
// any thread afterwards.
class StagePluginLoader {
 public:
  using RegisterStageFactory = std::function<DAGStagePtr()>;
  using RegisterStageCallback = std::function<bool(
      const std::string& stage_name, RegisterStageFactory factory)>;

  struct LoadResult {
    bool success = false;
    std::string error_message;
    std::optional<LoadedStagePlugin> plugin;
    // Set when the plugin was refused because a copy with the same plugin id
    // is already loaded: the path of that copy. error_message then says
    // which copy is used instead.
    std::string superseded_by;
  };

  StagePluginLoader() = default;
  ~StagePluginLoader();

  StagePluginLoader(const StagePluginLoader&) = delete;
  StagePluginLoader& operator=(const StagePluginLoader&) = delete;

  // Opens |path|, validates its descriptor and registers its stages through
  // |register_stage_callback|. Only one copy of a plugin id is ever loaded:
  // a plugin whose id is already loaded is refused before any registration,
  // with LoadResult::superseded_by naming the copy that stays, so a second
  // copy never collides with the first copy's stage names.
  LoadResult load_plugin(const std::string& path,
                         const RegisterStageCallback& register_stage_callback);

  // Releases the loader's references to all loaded plugin libraries and
  // clears the loaded-plugin metadata. Libraries whose factories or stage
  // instances are still referenced elsewhere stay mapped until those
  // references are released (see class comment).
  void unload_all();

  const std::vector<LoadedStagePlugin>& loaded_plugins() const {
    return loaded_plugins_;
  }

 private:
  struct PluginHandleEntry {
    // Shared ownership of the opened library and its service table; also
    // held by every factory/stage created from this plugin (type-erased —
    // the concrete type is internal to the implementation).
    std::shared_ptr<void> library;
    LoadedStagePlugin plugin;
  };

  static bool register_stage_trampoline(void* context, const char* stage_name,
                                        OrcStageFactoryFn factory);
  static std::string sanitize_c_string(const char* value);

  std::vector<PluginHandleEntry> handle_entries_;
  std::vector<LoadedStagePlugin> loaded_plugins_;
};

}  // namespace orc
