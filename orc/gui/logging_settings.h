/*
 * File:        logging_settings.h
 * Module:      orc-gui
 * Purpose:     User-controlled diagnostic logging configuration
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_LOGGING_SETTINGS_H
#define ORC_GUI_LOGGING_SETTINGS_H

#include <orc/support/log_destination.h>

#include <QString>
#include <QStringList>

namespace orc {

/**
 * @brief Diagnostic logging configuration as edited in Tools > Logging.
 *
 * These are the three things a user needs in order to capture a log for a bug
 * report: whether logging to a file is on at all, how much detail it records,
 * and where the file goes. Console output is unaffected; it stays as the
 * process was started.
 */
struct LoggingSettings {
  /// True when log records are also written to `file_path`.
  bool file_logging_enabled = false;
  /// Severity threshold: trace, debug, info, warn, error, critical or off.
  QString level = QStringLiteral("info");
  /// Absolute path of the log file. Empty means "use the default location".
  QString file_path;
  /// True when the preview path records a per-frame timing breakdown. Off by
  /// default: it writes a line per displayed frame, which is what makes it
  /// useful for a performance report and unwanted the rest of the time.
  bool frame_timing_enabled = false;

  bool operator==(const LoggingSettings& other) const {
    return file_logging_enabled == other.file_logging_enabled &&
           level == other.level && file_path == other.file_path &&
           frame_timing_enabled == other.frame_timing_enabled;
  }
  bool operator!=(const LoggingSettings& other) const {
    return !(*this == other);
  }
};

/**
 * @brief Pure helpers backing the logging dialogue.
 *
 * Everything here is a value-in/value-out transformation with no filesystem,
 * settings-store or logger access, so the dialogue's rules can be tested
 * directly. LoggingController owns the impure side.
 */
class LoggingSettingsModel {
 public:
  /// Level names in increasing severity, as offered by the dialogue and
  /// accepted by `--log-level`.
  static QStringList levelNames();

  /// Human-readable one-line description of what a level records.
  static QString levelDescription(const QString& level);

  /// Canonical spelling of a level name. Unknown names, and the "warning"
  /// spelling the logger also accepts, resolve the way the logger resolves
  /// them: to "warn" and "info" respectively.
  static QString normaliseLevel(const QString& level);

  /// True when `level` is one of the accepted names (in any accepted spelling).
  static bool isValidLevel(const QString& level);

  /// Sink selection implied by the settings: console plus the file when file
  /// logging is on, console alone otherwise. The console is never dropped, so
  /// a terminal-launched session keeps its output either way.
  static LogDestination destinationFor(const LoggingSettings& settings);

  /// Log file to hand the logger: nothing while file logging is off, the
  /// configured path when set, and `default_path` when the user has not chosen
  /// one.
  static QString resolveLogFile(const LoggingSettings& settings,
                                const QString& default_path);

  /// True when the build compiles in records at `level`. Trace records are
  /// stripped from release builds at compile time (SPDLOG_ACTIVE_LEVEL), so
  /// selecting trace there cannot produce them.
  static bool isLevelCompiledIn(const QString& level);

  /// Sentence describing what the settings will capture, shown beneath the
  /// dialogue's controls.
  static QString summaryText(const LoggingSettings& settings,
                             const QString& default_path);
};

}  // namespace orc

#endif  // ORC_GUI_LOGGING_SETTINGS_H
