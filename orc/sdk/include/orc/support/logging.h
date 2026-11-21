/*
 * File:        logging.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Logging system implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/abi/orc_plugin_services.h>
#include <orc/support/log_destination.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace orc {

/// Initialize the logging system
/// Should be called once at application startup
/// @param level Log level (trace, debug, info, warn, error, critical, off)
/// @param pattern Optional custom pattern (default: "[%Y-%m-%d %H:%M:%S.%e]
/// [%n] [%^%l%$] %v")
/// @param log_file Optional file path to write logs to
/// @param destination Which sinks to install (console, file, or both)
void init_logging(
    const std::string& level = "info",
    const std::string& pattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v",
    const std::string& log_file = "",
    LogDestination destination = LogDestination::kBoth);

/// Reconfigure the already-initialised logger's sinks and level at runtime.
///
/// Unlike init_logging(), the logger object itself is preserved: only the set
/// of sinks behind it is swapped. Modules that cached the shared_ptr returned
/// by get_logger() (the host, and every dlopen'd plugin, share one "core"
/// logger object) therefore follow the change without being re-initialised.
///
/// Thread-safe: sinks are swapped under the same mutex that serialises writes,
/// so records already in flight are never lost or torn.
///
/// @param level Log level (trace, debug, info, warn, error, critical, off)
/// @param pattern Log pattern applied to every installed sink
/// @param log_file File to write to; ignored when empty
/// @param destination Which sinks to install (console, file, or both)
/// @param truncate_log_file True to replace the log file's contents, false to
///        append to it. init_logging() always replaces, so each run of the
///        application starts a fresh log; pass false here to keep adding to a
///        file this run already opened
/// @param error_message Optional; set to the reason a requested file sink
///        could not be opened (the console sink is kept in that case)
/// @return True when the requested destination was installed in full
bool reconfigure_logging(const std::string& level, const std::string& pattern,
                         const std::string& log_file,
                         LogDestination destination, bool truncate_log_file,
                         std::string* error_message = nullptr);

/// Get the default logger
///
/// Resolution order, on first use:
///  1. The "core" logger in spdlog's registry. Host modules, bundled plugins
///     and test binaries share one libspdlog, so they share this logger.
///  2. In the private-logging build of this library (ORC_SDK_PRIVATE_LOGGING:
///     the one third-party plugins link, with their own header-only spdlog,
///     so they never depend on the host's spdlog version), the registry is
///     the plugin's own and is empty. Once the host has handed over its
///     services table, the logger is one that forwards every record to the
///     host's OrcPluginServices::log callback: see
///     make_host_forwarding_logger().
///  3. Otherwise, a new console logger, registered as "core".
std::shared_ptr<spdlog::logger> get_logger();

/// The host's log entry point, as OrcPluginServices::log declares it.
using HostLogFunction = void (*)(OrcPluginLogLevel level, const char* message);

/// Build a logger named "core" whose only sink hands each record's message
/// text, unformatted, to `host_log` at the record's level. The host applies its
/// own pattern, level filter and destinations. The logger itself passes every
/// level through, because a plugin cannot ask the host which levels are on.
///
/// get_logger() uses this inside a plugin with a private spdlog; it is public
/// so the forwarding can be tested without loading a plugin.
std::shared_ptr<spdlog::logger> make_host_forwarding_logger(
    HostLogFunction host_log);

/// Set log level at runtime
void set_log_level(const std::string& level);

}  // namespace orc

// Convenient logging macros
#define ORC_LOG_TRACE(...) SPDLOG_LOGGER_TRACE(orc::get_logger(), __VA_ARGS__)
#define ORC_LOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(orc::get_logger(), __VA_ARGS__)
#define ORC_LOG_INFO(...) SPDLOG_LOGGER_INFO(orc::get_logger(), __VA_ARGS__)
#define ORC_LOG_WARN(...) SPDLOG_LOGGER_WARN(orc::get_logger(), __VA_ARGS__)
#define ORC_LOG_ERROR(...) SPDLOG_LOGGER_ERROR(orc::get_logger(), __VA_ARGS__)
#define ORC_LOG_CRITICAL(...) \
  SPDLOG_LOGGER_CRITICAL(orc::get_logger(), __VA_ARGS__)
