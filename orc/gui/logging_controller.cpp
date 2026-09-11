/*
 * File:        logging_controller.cpp
 * Module:      orc-gui
 * Purpose:     Runtime application and persistence of diagnostic logging
 * settings
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "logging_controller.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include "crash_handler.h"
#include "frame_profiler.h"
#include "logging.h"
#include "project_presenter.h"  // For reconfigureCoreLogging

namespace orc {

namespace {

// QSettings keys. Kept under a "logging/" group so the saved configuration is
// obvious in the settings file a user may be asked to inspect.
constexpr auto kFileEnabledKey = "logging/file_enabled";
constexpr auto kLevelKey = "logging/level";
constexpr auto kFilePathKey = "logging/file_path";
constexpr auto kFrameTimingKey = "logging/frame_timing_enabled";

// Folder the default log file lives in, alongside the crash-bundle folder the
// crash handler writes to, so both are found in the same place.
constexpr auto kLogFolderName = "decode-orc-logs";
constexpr auto kLogFileName = "orc-gui.log";

// Reconfigure both of the application's loggers. The GUI and core each own one
// ("gui" and "core"); loaded stage plugins share the core logger, so they pick
// the change up too.
bool applyToLoggers(const std::string& level, const std::string& log_file,
                    LogDestination destination, bool truncate_log_file,
                    std::string* error_message) {
  const std::string pattern = LoggingController::logPattern().toStdString();

  // The GUI logger opens the file first; the core logger then attaches to the
  // same path in append mode so it adds to what the GUI has just started
  // rather than emptying the file a second time.
  std::string gui_error;
  const bool gui_ok = reconfigure_gui_logging(
      level, pattern, log_file, destination, truncate_log_file, &gui_error);

  std::string core_error;
  const bool core_ok = presenters::reconfigureCoreLogging(
      level, pattern, log_file, destination, /*truncate_log_file=*/false,
      &core_error);

  // Keep the crash bundle pointing at the log that is actually being written,
  // so a log turned on to chase a crash is collected with it.
  set_crash_log_file(log_file);

  if (!gui_ok || !core_ok) {
    if (error_message != nullptr) {
      *error_message = gui_ok ? core_error : gui_error;
    }
    return false;
  }
  return true;
}

}  // namespace

LoggingController* LoggingController::s_instance = nullptr;

LoggingController::LoggingController(const LoggingSettings& initial,
                                     QObject* parent)
    : LoggingController(initial, &applyToLoggers, true, parent) {}

LoggingController::LoggingController(const LoggingSettings& initial,
                                     LogApplyFunction apply_function,
                                     QObject* parent)
    : LoggingController(initial, std::move(apply_function), false, parent) {}

LoggingController::LoggingController(const LoggingSettings& initial,
                                     LogApplyFunction apply_function,
                                     bool manage_log_directory, QObject* parent)
    : QObject(parent),
      settings_(initial),
      apply_function_(std::move(apply_function)),
      manage_log_directory_(manage_log_directory) {
  settings_.level = LoggingSettingsModel::normaliseLevel(settings_.level);
  // Frame profiling follows the setting from startup, not only from the first
  // time the dialogue is used, so a persisted choice is live for the session's
  // first rendered frame.
  gui::FrameProfiler::instance().setEnabled(settings_.frame_timing_enabled);
  s_instance = this;
}

LoggingController::~LoggingController() {
  if (s_instance == this) {
    s_instance = nullptr;
  }
}

LoggingController* LoggingController::instance() { return s_instance; }

const LoggingSettings& LoggingController::settings() const { return settings_; }

QString LoggingController::activeLogFile() const {
  return LoggingSettingsModel::resolveLogFile(settings_, defaultLogFilePath());
}

LoggingController::ApplyResult LoggingController::apply(
    const LoggingSettings& settings) {
  LoggingSettings requested = settings;
  requested.level = LoggingSettingsModel::normaliseLevel(requested.level);
  requested.file_path = requested.file_path.trimmed();

  const QString log_file =
      LoggingSettingsModel::resolveLogFile(requested, defaultLogFilePath());

  // Opening a file that is not already being written starts a fresh capture;
  // re-applying while the same file stays open - changing only the detail
  // level, say - keeps what it has recorded so far.
  const bool truncate_log_file = log_file != activeLogFile();

  ApplyResult result;
  result.log_file = log_file;

  // A path the user typed may name a folder that does not exist yet; creating
  // it is part of honouring the request, and the sink would otherwise fail.
  if (manage_log_directory_ && !log_file.isEmpty()) {
    const QString directory = QFileInfo(log_file).absolutePath();
    if (!directory.isEmpty()) {
      QDir().mkpath(directory);
    }
  }

  std::string error;
  const bool ok =
      apply_function_
          ? apply_function_(requested.level.toStdString(),
                            log_file.toStdString(),
                            LoggingSettingsModel::destinationFor(requested),
                            truncate_log_file, &error)
          : true;

  result.ok = ok;
  if (!ok) {
    result.error = QString::fromStdString(error);
  }

  // The settings are recorded as requested even when the file could not be
  // opened, so the dialogue reopens showing the path the user needs to fix.
  settings_ = requested;
  persistSettings(settings_);
  gui::FrameProfiler::instance().setEnabled(settings_.frame_timing_enabled);
  emit settingsChanged(settings_);

  return result;
}

LoggingSettings LoggingController::loadPersistedSettings() {
  QSettings settings;
  LoggingSettings loaded;
  loaded.file_logging_enabled = settings.value(kFileEnabledKey, false).toBool();
  loaded.level = LoggingSettingsModel::normaliseLevel(
      settings.value(kLevelKey, QStringLiteral("info")).toString());
  loaded.file_path = settings.value(kFilePathKey, QString{}).toString();
  loaded.frame_timing_enabled = settings.value(kFrameTimingKey, false).toBool();
  return loaded;
}

void LoggingController::persistSettings(const LoggingSettings& settings) {
  QSettings store;
  store.setValue(kFileEnabledKey, settings.file_logging_enabled);
  store.setValue(kLevelKey, settings.level);
  store.setValue(kFilePathKey, settings.file_path);
  store.setValue(kFrameTimingKey, settings.frame_timing_enabled);
}

QString LoggingController::defaultLogFilePath() {
  QString base =
      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  if (base.isEmpty()) {
    base = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
  }
  if (base.isEmpty()) {
    base = QDir::currentPath();
  }
  return QDir(base).filePath(QStringLiteral("%1/%2").arg(
      QLatin1String(kLogFolderName), QLatin1String(kLogFileName)));
}

QString LoggingController::logPattern() {
  return QStringLiteral("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
}

}  // namespace orc
