/*
 * File:        logging_settings.cpp
 * Module:      orc-gui
 * Purpose:     User-controlled diagnostic logging configuration
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "logging_settings.h"

#include <spdlog/common.h>

namespace orc {

QStringList LoggingSettingsModel::levelNames() {
  return QStringList{
      QStringLiteral("trace"), QStringLiteral("debug"),
      QStringLiteral("info"),  QStringLiteral("warn"),
      QStringLiteral("error"), QStringLiteral("critical"),
      QStringLiteral("off"),
  };
}

QString LoggingSettingsModel::levelDescription(const QString& level) {
  const QString name = normaliseLevel(level);
  if (name == QLatin1String("trace")) {
    return QStringLiteral(
        "Everything, including per-frame detail. Very large.");
  }
  if (name == QLatin1String("debug")) {
    return QStringLiteral(
        "Diagnostic detail useful in a bug report. Recommended.");
  }
  if (name == QLatin1String("info")) {
    return QStringLiteral("Normal progress messages. The default.");
  }
  if (name == QLatin1String("warn")) {
    return QStringLiteral("Warnings and worse only.");
  }
  if (name == QLatin1String("error")) {
    return QStringLiteral("Errors and worse only.");
  }
  if (name == QLatin1String("critical")) {
    return QStringLiteral("Only failures that stop work.");
  }
  return QStringLiteral("Nothing is recorded.");
}

QString LoggingSettingsModel::normaliseLevel(const QString& level) {
  const QString lowered = level.trimmed().toLower();
  // The logger treats "warning" as a spelling of "warn"; every other
  // unrecognised name falls back to info, so the dialogue must agree.
  if (lowered == QLatin1String("warning")) {
    return QStringLiteral("warn");
  }
  if (levelNames().contains(lowered)) {
    return lowered;
  }
  return QStringLiteral("info");
}

bool LoggingSettingsModel::isValidLevel(const QString& level) {
  const QString lowered = level.trimmed().toLower();
  return lowered == QLatin1String("warning") || levelNames().contains(lowered);
}

LogDestination LoggingSettingsModel::destinationFor(
    const LoggingSettings& settings) {
  return settings.file_logging_enabled ? LogDestination::kBoth
                                       : LogDestination::kConsole;
}

QString LoggingSettingsModel::resolveLogFile(const LoggingSettings& settings,
                                             const QString& default_path) {
  if (!settings.file_logging_enabled) {
    return QString{};
  }
  const QString configured = settings.file_path.trimmed();
  return configured.isEmpty() ? default_path : configured;
}

bool LoggingSettingsModel::isLevelCompiledIn(const QString& level) {
  const QString name = normaliseLevel(level);
  // Call sites go through SPDLOG_LOGGER_*, which compiles a record out
  // entirely when its level is below SPDLOG_ACTIVE_LEVEL. Release builds set
  // that to debug, so trace records do not exist in the binary and no runtime
  // setting can bring them back.
  if (name == QLatin1String("trace")) {
    return SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_TRACE;
  }
  if (name == QLatin1String("debug")) {
    return SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG;
  }
  return true;
}

QString LoggingSettingsModel::summaryText(const LoggingSettings& settings,
                                          const QString& default_path) {
  const QString frame_timing =
      settings.frame_timing_enabled
          ? QStringLiteral(
                " Per-frame preview timings are recorded, one line per "
                "displayed frame.")
          : QString{};

  if (!settings.file_logging_enabled) {
    return QStringLiteral(
               "Logging to a file is off. Messages still go to the console "
               "when the application is started from a terminal.") +
           frame_timing;
  }

  const QString path = resolveLogFile(settings, default_path);
  const QString level = normaliseLevel(settings.level);

  QString text =
      QStringLiteral("Recording %1 and above to %2.")
          .arg(level, path.isEmpty() ? QStringLiteral("the log file") : path);
  if (!isLevelCompiledIn(level)) {
    text += QStringLiteral(
                " This build was compiled without %1 records, so nothing "
                "below debug will appear; choose debug instead.")
                .arg(level);
  }
  return text + frame_timing;
}

}  // namespace orc
