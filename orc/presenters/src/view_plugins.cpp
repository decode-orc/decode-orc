/*
 * File:        view_plugins.cpp
 * Module:      orc-presenters
 * Purpose:     orc-gui's access to plugins' view binaries
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../include/view_plugins.h"

#include <orc/abi/orc_plugin_abi.h>

#include "../core/include/stage_registry.h"
#include "../core/include/view_plugin_loader.h"

namespace orc::presenters {

void loadViewPlugins(uint32_t qt_major, uint32_t qt_minor) {
  orc::ViewHostVersion host;
  host.qt_major = qt_major;
  host.qt_minor = qt_minor;
  host.toolchain_tag = ORC_SDK_TOOLCHAIN_TAG;

  const auto& stages = orc::StageRegistry::instance();
  orc::ViewPluginRegistry::instance().load(host,
                                           stages.get_plugin_registry_entries(),
                                           stages.get_plugin_search_paths());
}

orc::view::OrcViewFactoryFn findViewFactory(const std::string& contract_id) {
  return orc::ViewPluginRegistry::instance().find_factory(contract_id);
}

}  // namespace orc::presenters
