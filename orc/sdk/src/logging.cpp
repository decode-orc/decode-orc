/*
 * File:        logging.cpp
 * Module:      orc-sdk-support
 * Purpose:     Logging system implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <orc/support/logging.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace orc {

namespace {

std::shared_ptr<spdlog::logger> g_logger;

// Set once g_logger holds the logger. The first records can come from several
// threads at once (a tool's workers, say), so the logger is made under
// g_logger_mutex and read without it only once this is set.
std::atomic<bool> g_logger_ready{false};
std::mutex g_logger_mutex;

// The logger's only sink: a mux whose children can be swapped at runtime under
// the mux's own lock. Keeping one stable sink object in the logger is what lets
// reconfigure_logging() change destinations without replacing the logger, so
// every module that cached get_logger() follows the change.
std::shared_ptr<spdlog::sinks::dist_sink_mt> g_dist_sink;

// Build the console/file sinks a destination asks for. A file sink that cannot
// be opened is reported through `error_message` and simply omitted; the caller
// keeps the console so records are not silently discarded.
std::vector<spdlog::sink_ptr> make_sinks(const std::string& pattern,
                                         const std::string& log_file,
                                         LogDestination destination,
                                         bool truncate_log_file,
                                         std::string* error_message) {
  std::vector<spdlog::sink_ptr> sinks;

  const LogSinkSelection selection =
      resolve_log_sinks(destination, !log_file.empty());

  // Console sink with color. stderr, not stdout: the CLI's stdout carries
  // command output (tables, --json documents) that scripts parse, and log
  // text interleaved into it would corrupt them. See orc/common/logging.cpp's
  // own "app" logger, which already made this choice; this is the "core"
  // logger's counterpart.
  if (selection.console) {
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    console_sink->set_pattern(pattern);
    sinks.push_back(console_sink);
  }

  // File sink
  if (selection.file) {
    try {
      auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
          log_file, truncate_log_file);
      file_sink->set_pattern(pattern);
      sinks.push_back(file_sink);
    } catch (const spdlog::spdlog_ex& ex) {
      if (error_message != nullptr) {
        *error_message = ex.what();
      }
    }
  }

  if (sinks.empty()) {
    // File-only logging was requested but the file sink could not be created;
    // keep the console (stderr — see the comment above) so records are not
    // silently discarded.
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    console_sink->set_pattern(pattern);
    sinks.push_back(console_sink);
  }

  return sinks;
}

// Recover the mux from the live logger when this module's own g_dist_sink is
// unset. The static above is per-module (this translation unit is compiled into
// the orc-sdk-support static library, so the host and each plugin hold their
// own copy) while the logger itself is shared through spdlog's process-global
// registry, so a plugin can meet a mux it did not create.
std::shared_ptr<spdlog::sinks::dist_sink_mt> resolve_dist_sink(
    const std::shared_ptr<spdlog::logger>& logger) {
  if (g_dist_sink) {
    return g_dist_sink;
  }
  if (!logger || logger->sinks().size() != 1) {
    return nullptr;
  }
  return std::dynamic_pointer_cast<spdlog::sinks::dist_sink_mt>(
      logger->sinks().front());
}

void apply_log_level(spdlog::logger& logger, const std::string& level) {
  if (level == "trace") {
    logger.set_level(spdlog::level::trace);
  } else if (level == "debug") {
    logger.set_level(spdlog::level::debug);
  } else if (level == "info") {
    logger.set_level(spdlog::level::info);
  } else if (level == "warn" || level == "warning") {
    logger.set_level(spdlog::level::warn);
  } else if (level == "error") {
    logger.set_level(spdlog::level::err);
  } else if (level == "critical") {
    logger.set_level(spdlog::level::critical);
  } else if (level == "off") {
    logger.set_level(spdlog::level::off);
  } else {
    logger.warn("Unknown log level '{}', using 'info'", level.c_str());
    logger.set_level(spdlog::level::info);
  }
}

std::shared_ptr<spdlog::logger> make_core_logger(const std::string& level,
                                                 const std::string& pattern,
                                                 const std::string& log_file,
                                                 LogDestination destination) {
  // Startup always replaces the log file, so a log collected for a bug report
  // only ever holds the run it was collected from.
  g_dist_sink = std::make_shared<spdlog::sinks::dist_sink_mt>(make_sinks(
      pattern, log_file, destination, /*truncate_log_file=*/true, nullptr));
  auto logger = std::make_shared<spdlog::logger>("core", g_dist_sink);
  logger->set_pattern(pattern);
  // Flush on every log message to ensure immediate file writing
  logger->flush_on(spdlog::level::trace);
  apply_log_level(*logger, level);
  return logger;
}

}  // namespace

void init_logging(const std::string& level, const std::string& pattern,
                  const std::string& log_file, LogDestination destination) {
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  auto logger = make_core_logger(level, pattern, log_file, destination);

  // Drop any previously registered "core" logger before (re)creating it. The
  // name may already be registered either by an earlier init_logging() call or
  // by another module in the process: this translation unit is compiled into
  // the orc-sdk-support static library, so the host, each dlopen'd plugin (via
  // its RTLD_LOCAL copy), and standalone test binaries each hold their own
  // g_logger, but they all share spdlog's single process-global registry.
  // Dropping first keeps register_logger() from throwing on a duplicate name.
  if (spdlog::get("core")) {
    spdlog::drop("core");
  }
  spdlog::register_logger(logger);
  g_logger = std::move(logger);
  g_logger_ready.store(true, std::memory_order_release);
}

bool reconfigure_logging(const std::string& level, const std::string& pattern,
                         const std::string& log_file,
                         LogDestination destination, bool truncate_log_file,
                         std::string* error_message) {
  auto logger = get_logger();
  auto dist_sink = resolve_dist_sink(logger);
  if (!dist_sink) {
    // Nothing to swap into (an older logger built before the mux existed).
    // A full re-initialisation is the correct fallback.
    init_logging(level, pattern, log_file, destination);
    return true;
  }

  std::string sink_error;
  dist_sink->set_sinks(make_sinks(pattern, log_file, destination,
                                  truncate_log_file, &sink_error));
  logger->set_pattern(pattern);
  set_log_level(level);

  if (!sink_error.empty()) {
    if (error_message != nullptr) {
      *error_message = sink_error;
    }
    return false;
  }
  return true;
}

std::shared_ptr<spdlog::logger> get_logger() {
  if (g_logger_ready.load(std::memory_order_acquire)) {
    return g_logger;
  }
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  if (!g_logger) {
    // Reuse the shared "core" logger if another module in the process already
    // created it (spdlog's registry is process-global even though g_logger is
    // per-module), so host, plugins and test binaries log through one logger.
    auto logger = spdlog::get("core");
    if (!logger) {
      logger =
          make_core_logger("info", "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v",
                           "", LogDestination::kBoth);
      try {
        spdlog::register_logger(logger);
      } catch (const spdlog::spdlog_ex&) {
        // Another module registered its own between the lookup and here:
        // log through that one, whose mux resolve_dist_sink() finds.
        g_dist_sink.reset();
        logger = spdlog::get("core");
      }
    }
    g_logger = std::move(logger);
    g_logger_ready.store(true, std::memory_order_release);
  }
  return g_logger;
}

void set_log_level(const std::string& level) {
  apply_log_level(*get_logger(), level);
}

}  // namespace orc
