/*
 * File:        logging.cpp
 * Module:      orc-common
 * Purpose:     Shared logging implementation for GUI/CLI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "include/logging.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/version.h>

#include <mutex>
#include <utility>
#include <vector>

namespace orc {

// This file is compiled into orc-core only, so a process holds one copy of
// g_logger (orc/common/CMakeLists.txt). Before issue #393 it was a static
// library that Windows executables linked alongside orc-core.dll, giving two
// copies sharing spdlog's registry (in spdlog.dll); a second
// register_logger() under a name the other copy had registered threw, and an
// exception escaping a log call ends the process. Every registration below
// still expects the name may already be taken: spdlog's registry is
// process-wide, and anything else may register the same name.
static std::shared_ptr<spdlog::logger> g_logger;
static std::mutex g_logger_mutex;

static constexpr const char* kDefaultAppLoggerName = "orc-app";

// Register `logger`, replacing any logger already registered under its name.
// For init_app_logging(), whose configuration is meant to win.
static void register_replacing(const std::shared_ptr<spdlog::logger>& logger) {
#if SPDLOG_VERSION >= 11600
  spdlog::register_or_replace(logger);
#else
  // register_or_replace() arrived in spdlog 1.16 (the Flatpak builds 1.14).
  // Another module can register the name between the drop and the
  // registration; then drop theirs and try once more. If even that loses, the
  // logger still works through g_logger, it is only missing from the registry.
  for (int attempt = 0; attempt < 2; ++attempt) {
    spdlog::drop(logger->name());
    try {
      spdlog::register_logger(logger);
      return;
    } catch (const spdlog::spdlog_ex&) {  // NOLINT(bugprone-empty-catch)
    }
  }
#endif
}

std::shared_ptr<spdlog::logger> get_app_logger() {
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  if (!g_logger) {
    // Another copy of this file in the process may already have made (and
    // configured) the logger: log through that one rather than replacing it.
    g_logger = spdlog::get(kDefaultAppLoggerName);
  }
  if (!g_logger) {
    // Create a default console logger
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>(kDefaultAppLoggerName, sink);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    logger->set_level(spdlog::level::info);
    try {
      spdlog::register_logger(logger);
    } catch (const spdlog::spdlog_ex&) {
      // The other copy registered its own between the lookup and here.
      if (auto existing = spdlog::get(kDefaultAppLoggerName)) {
        logger = std::move(existing);
      }
    }
    g_logger = std::move(logger);
  }
  return g_logger;
}

void reset_logging() {
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  if (g_logger) {
    spdlog::drop(g_logger->name());
    g_logger.reset();
  }
}

static spdlog::level::level_enum parse_level(const std::string& level) {
  std::string l = level;
  for (auto& c : l) c = static_cast<char>(::tolower(c));
  if (l == "trace") return spdlog::level::trace;
  if (l == "debug") return spdlog::level::debug;
  if (l == "info") return spdlog::level::info;
  if (l == "warn" || l == "warning") return spdlog::level::warn;
  if (l == "error") return spdlog::level::err;
  if (l == "critical") return spdlog::level::critical;
  if (l == "off") return spdlog::level::off;
  return spdlog::level::info;
}

void init_app_logging(const std::string& level, const std::string& pattern,
                      const std::string& log_file,
                      const std::string& logger_name,
                      LogDestination destination) {
  std::lock_guard<std::mutex> lock(g_logger_mutex);

  // Drop existing logger if present
  if (g_logger) {
    spdlog::drop(g_logger->name());
    g_logger.reset();
  }

  const LogSinkSelection selection =
      resolve_log_sinks(destination, !log_file.empty());

  std::vector<spdlog::sink_ptr> sinks;

  if (selection.console) {
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    console_sink->set_pattern(pattern);
    sinks.push_back(console_sink);
  }

  if (selection.file) {
    try {
      auto file_sink =
          std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, true);
      file_sink->set_pattern(pattern);
      sinks.push_back(file_sink);
    } catch (...) {  // NOLINT(bugprone-empty-catch)
      // If file sink creation fails, fall through to the console fallback
    }
  }

  if (sinks.empty()) {
    // File-only logging was requested but the file sink could not be created;
    // keep the console so records are not silently discarded.
    sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    sinks.back()->set_pattern(pattern);
  }

  // Create new logger with proper name. Configured fully before it is
  // registered, so no other module can pick it up half set up.
  auto logger =
      std::make_shared<spdlog::logger>(logger_name, sinks.begin(), sinks.end());
  logger->set_pattern(pattern);
  logger->set_level(parse_level(level));
  register_replacing(logger);
  g_logger = std::move(logger);
}

}  // namespace orc
