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

#include <vector>

namespace orc {

namespace {

std::shared_ptr<spdlog::logger> g_logger;

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

  // Console sink with color. stderr, not stdout: the CLI writes a sink's own
  // binary output to real stdout via the "-" convention (orc/support/pipe_io.h)
  // or a network stream URL, and log text interleaved into that stream would
  // corrupt it — the same reason any well-behaved CLI tool keeps diagnostics
  // off its stdout. See orc/common/logging.cpp's own "app" logger, which
  // already made this choice; this is the "core" logger's counterpart.
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

}  // namespace

void init_logging(const std::string& level, const std::string& pattern,
                  const std::string& log_file, LogDestination destination) {
  // Startup always replaces the log file, so a log collected for a bug report
  // only ever holds the run it was collected from.
  g_dist_sink = std::make_shared<spdlog::sinks::dist_sink_mt>(make_sinks(
      pattern, log_file, destination, /*truncate_log_file=*/true, nullptr));

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
  g_logger = std::make_shared<spdlog::logger>("core", g_dist_sink);
  g_logger->set_pattern(pattern);

  // Flush on every log message to ensure immediate file writing
  g_logger->flush_on(spdlog::level::trace);

  // Register with spdlog
  spdlog::register_logger(g_logger);

  // Set log level
  set_log_level(level);
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
  if (!g_logger) {
    // Reuse the shared "core" logger if another module in the process already
    // created it (spdlog's registry is process-global even though g_logger is
    // per-module). This keeps host, plugins, and test binaries logging through
    // one logger instead of racing to register a duplicate "core" name.
    g_logger = spdlog::get("core");
    if (!g_logger) {
      init_logging();
    }
  }
  return g_logger;
}

void set_log_level(const std::string& level) {
  auto logger = get_logger();

  if (level == "trace") {
    logger->set_level(spdlog::level::trace);
  } else if (level == "debug") {
    logger->set_level(spdlog::level::debug);
  } else if (level == "info") {
    logger->set_level(spdlog::level::info);
  } else if (level == "warn" || level == "warning") {
    logger->set_level(spdlog::level::warn);
  } else if (level == "error") {
    logger->set_level(spdlog::level::err);
  } else if (level == "critical") {
    logger->set_level(spdlog::level::critical);
  } else if (level == "off") {
    logger->set_level(spdlog::level::off);
  } else {
    logger->warn("Unknown log level '{}', using 'info'", level.c_str());
    logger->set_level(spdlog::level::info);
  }
}

}  // namespace orc
