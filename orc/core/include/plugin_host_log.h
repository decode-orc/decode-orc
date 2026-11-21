/*
 * File:        plugin_host_log.h
 * Module:      orc-core
 * Purpose:     The host side of OrcPluginServices::log
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/abi/orc_plugin_services.h>

namespace orc {

/// Write a plugin's log record to the host's "core" logger: the logger the
/// host's own stage code and bundled plugins write to, which the GUI and CLI
/// configure with the user's level and log file. A plugin built with a private
/// spdlog reaches the host's log only through this callback.
///
/// @param level   Severity of the record.
/// @param message Null-terminated UTF-8 text; ignored when null.
void write_plugin_log_record(OrcPluginLogLevel level, const char* message);

}  // namespace orc
