/*
 * File:        view_plugins.h
 * Module:      orc-presenters
 * Purpose:     orc-gui's access to plugins' view binaries: load them once at
 *              startup, then look up the widget factory for a tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/view/orc_view_plugin_abi.h>

#include <cstdint>
#include <string>

namespace orc::presenters {

/**
 * @brief Load the view binaries of the installed plugins
 *
 * orc-gui calls this once on the GUI thread, after the stage plugins have
 * loaded, with the version of the Qt it is running. orc-cli never calls it, so
 * it never loads Qt through a plugin. Views come from trusted, enabled
 * registry entries and from view binaries (files named orc-view_*) in the
 * plugin search folders. What was loaded or refused is reported with the
 * other plugin diagnostics.
 */
void loadViewPlugins(uint32_t qt_major, uint32_t qt_minor);

/// The factory a loaded view registered for @p contract_id, or nullptr.
orc::view::OrcViewFactoryFn findViewFactory(const std::string& contract_id);

}  // namespace orc::presenters
