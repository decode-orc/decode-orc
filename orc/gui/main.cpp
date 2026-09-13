/*
 * File:        main.cpp
 * Module:      orc-gui
 * Purpose:     Application entry point
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <orc/stage/error_types.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSettings>
#include <QSplashScreen>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <vector>

#include "crash_handler.h"
#include "gpu/gpu_surface_policy.h"
#include "logging.h"
#include "logging_controller.h"
#include "logging_settings.h"
#include "mainwindow.h"
#include "project_presenter.h"  // For initCoreLogging
#include "theme_controller.h"
#include "version.h"

namespace fs = std::filesystem;

namespace orc {

namespace {

std::shared_ptr<spdlog::logger> g_gui_logger;

// The GUI logger's only sink: a mux whose children are swapped under its own
// lock. Keeping one stable sink object in the logger is what lets
// reconfigure_gui_logging() change destinations at runtime without replacing
// the logger every ORC_LOG_* call site resolves through.
std::shared_ptr<spdlog::sinks::dist_sink_mt> g_gui_dist_sink;

// Build the console/file sinks a destination asks for. A file sink that cannot
// be opened is reported through `error_message` and omitted; the console is
// kept so records are never silently discarded.
std::vector<spdlog::sink_ptr> make_gui_sinks(const std::string& pattern,
                                             const std::string& log_file,
                                             LogDestination destination,
                                             bool truncate_log_file,
                                             std::string* error_message) {
  const LogSinkSelection selection =
      resolve_log_sinks(destination, !log_file.empty());

  std::vector<spdlog::sink_ptr> sinks;

  // Console sink (with colors)
  if (selection.console) {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
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
      // If file logging fails, just continue with console only
      if (error_message != nullptr) {
        *error_message = ex.what();
      }
      std::cerr << "Failed to create log file: " << ex.what() << std::endl;
    }
  }

  if (sinks.empty()) {
    // File-only logging was requested but the file sink could not be created;
    // keep the console so records are not silently discarded.
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern(pattern);
    sinks.push_back(console_sink);
  }

  return sinks;
}

// Translate a level name onto the spdlog enum. Anything unrecognised (and
// "info" itself) resolves to info, matching the command line's documented
// default.
spdlog::level::level_enum parse_gui_level(const std::string& level) {
  if (level == "trace") {
    return spdlog::level::trace;
  }
  if (level == "debug") {
    return spdlog::level::debug;
  }
  if (level == "warn" || level == "warning") {
    return spdlog::level::warn;
  }
  if (level == "error") {
    return spdlog::level::err;
  }
  if (level == "critical") {
    return spdlog::level::critical;
  }
  if (level == "off") {
    return spdlog::level::off;
  }
  return spdlog::level::info;
}

// While a log file is open every record is flushed as it is written. The GUI
// and core loggers hold separate handles on that one file, so a buffered
// logger's records would otherwise reach the file long after the other's and
// leave the log out of chronological order - exactly what makes it unreadable
// in a bug report. Console-only logging keeps the cheaper warn threshold.
spdlog::level::level_enum flush_level_for(LogDestination destination,
                                          const std::string& log_file) {
  const LogSinkSelection selection =
      resolve_log_sinks(destination, !log_file.empty());
  return selection.file ? spdlog::level::trace : spdlog::level::warn;
}

}  // namespace

/// Initialize GUI logging system
/// @param level Log level string
/// @param pattern Log pattern
/// @param log_file Optional log file path
/// @param destination Which sinks to install (console, file, or both)
void init_gui_logging(const std::string& level, const std::string& pattern,
                      const std::string& log_file, LogDestination destination) {
  // Reset existing logger
  if (g_gui_logger) {
    spdlog::drop(g_gui_logger->name());
  }
  g_gui_logger.reset();

  // Startup always replaces the log file, so a log collected for a bug report
  // only ever holds the run it was collected from.
  g_gui_dist_sink =
      std::make_shared<spdlog::sinks::dist_sink_mt>(make_gui_sinks(
          pattern, log_file, destination, /*truncate_log_file=*/true, nullptr));

  // Create logger
  g_gui_logger = std::make_shared<spdlog::logger>("gui", g_gui_dist_sink);
  g_gui_logger->set_pattern(pattern);
  g_gui_logger->set_level(parse_gui_level(level));
  g_gui_logger->flush_on(flush_level_for(destination, log_file));

  // Register with spdlog
  spdlog::register_logger(g_gui_logger);
}

bool reconfigure_gui_logging(const std::string& level,
                             const std::string& pattern,
                             const std::string& log_file,
                             LogDestination destination, bool truncate_log_file,
                             std::string* error_message) {
  if (!g_gui_logger || !g_gui_dist_sink) {
    init_gui_logging(level, pattern, log_file, destination);
    return true;
  }

  std::string sink_error;
  g_gui_dist_sink->set_sinks(make_gui_sinks(pattern, log_file, destination,
                                            truncate_log_file, &sink_error));
  g_gui_logger->set_pattern(pattern);
  g_gui_logger->set_level(parse_gui_level(level));
  g_gui_logger->flush_on(flush_level_for(destination, log_file));

  if (!sink_error.empty()) {
    if (error_message != nullptr) {
      *error_message = sink_error;
    }
    return false;
  }
  return true;
}

std::shared_ptr<spdlog::logger> get_gui_logger() {
  if (!g_gui_logger) {
    // Initialize with defaults if not already done
    init_gui_logging();
  }
  return g_gui_logger;
}

void reset_gui_logger() { g_gui_logger.reset(); }

}  // namespace orc

// Qt message handler that bridges to spdlog
void qtMessageHandler(QtMsgType type, const QMessageLogContext& /*context*/,
                      const QString& msg) {
  switch (type) {
    case QtDebugMsg:
      ORC_LOG_DEBUG("[Qt] {}", msg.toStdString());
      break;
    case QtInfoMsg:
      ORC_LOG_INFO("[Qt] {}", msg.toStdString());
      break;
    case QtWarningMsg:
      ORC_LOG_WARN("[Qt] {}", msg.toStdString());
      break;
    case QtCriticalMsg:
      ORC_LOG_ERROR("[Qt] {}", msg.toStdString());
      break;
    case QtFatalMsg:
      ORC_LOG_CRITICAL("[Qt] {}", msg.toStdString());
      break;
  }
}

int main(int argc, char* argv[]) {
  try {
    // Enable high DPI scaling
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);

    // Ensure resources from orc-gui-lib are registered when linked statically.
    Q_INIT_RESOURCE(orc_gui_resources);

    app.setApplicationName("orc-gui");
    app.setApplicationVersion(ORC_VERSION);
    app.setOrganizationName("domesday86");
    app.setDesktopFileName("io.github.decode_orc.decode-orc");
    app.setWindowIcon(QIcon(":/orc-gui/icon.png"));

    // Command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription("Decode Orc GUI");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption logLevelOption(QStringList{"l", "log-level"},
                                      "Set log level (trace, debug, info, "
                                      "warn, error, critical, off) for both "
                                      "GUI and core",
                                      "level", "info");
    parser.addOption(logLevelOption);

    QCommandLineOption themeOption(
        "theme", "Set UI theme (auto, light, dark). Default: auto", "mode",
        "auto");
    parser.addOption(themeOption);

    // Single shared log file option
    QCommandLineOption sharedLogFileOption(
        QStringList{"f", "log-file"},
        "Write both GUI and core logs to the specified file", "file");
    parser.addOption(sharedLogFileOption);

    QCommandLineOption logOutOption(
        "log-out",
        "Where to send log output: console, file or both. 'file' and 'both' "
        "need --log-file. Default: both",
        "output", "both");
    parser.addOption(logOutOption);

    QCommandLineOption quickProjectOption(
        "quick", "Create a quick project from a TBC/TBCC/TBCY file",
        "filename");
    parser.addOption(quickProjectOption);

    QCommandLineOption safeCorePluginsOption(
        "safe-core-plugins",
        "Clear user plugin registry and ignore ORC_STAGE_PLUGIN_PATHS for this "
        "run (core plugins only)");
    parser.addOption(safeCorePluginsOption);

    QCommandLineOption observerThreadsOption(
        "observer-threads",
        "Number of background observation worker threads. 0 or 'auto' uses "
        "half the available CPU cores (the default).",
        "count", "auto");
    parser.addOption(observerThreadsOption);

    parser.addPositionalArgument("project", "Project file to open (optional)");
    parser.process(app);

    if (parser.isSet(safeCorePluginsOption)) {
      // Ensure no external env-configured plugin search paths are used for this
      // run.
      qputenv("ORC_STAGE_PLUGIN_PATHS", "");

      const auto reset_result =
          orc::presenters::ProjectPresenter::clearPluginRegistryForSafeMode();
      if (!reset_result.success) {
        QMessageBox::critical(
            nullptr, "Safe Startup Failed",
            QString("Failed to clear plugin registry for safe startup:\n%1")
                .arg(QString::fromStdString(reset_result.error_message)));
        return 1;
      }
    }

    // Initialize GUI and core logging
    QString logLevel = parser.value(logLevelOption);
    QString sharedLogFile = parser.value(sharedLogFileOption);

    const QString logOutValue = parser.value(logOutOption);
    const auto parsedLogOut =
        orc::parse_log_destination(logOutValue.toStdString());
    if (!parsedLogOut.has_value()) {
      std::cerr << "Error: Invalid --log-out value '"
                << logOutValue.toStdString()
                << "' (expected console, file or both)" << std::endl;
      return 1;
    }
    orc::LogDestination logDestination = *parsedLogOut;

    // The logging switches, when given, configure this run; otherwise the
    // configuration saved in Tools > Logging applies. Startup never writes the
    // saved settings back, so a one-off --log-file run leaves the user's
    // choice alone.
    const bool loggingSetFromCommandLine = parser.isSet(logLevelOption) ||
                                           parser.isSet(sharedLogFileOption) ||
                                           parser.isSet(logOutOption);
    orc::LoggingSettings loggingSettings;
    if (loggingSetFromCommandLine) {
      loggingSettings.level = logLevel;
      loggingSettings.file_path = sharedLogFile;
      loggingSettings.file_logging_enabled =
          !sharedLogFile.isEmpty() &&
          logDestination != orc::LogDestination::kConsole;
    } else {
      loggingSettings = orc::LoggingController::loadPersistedSettings();
      logLevel = loggingSettings.level;
      sharedLogFile = orc::LoggingSettingsModel::resolveLogFile(
          loggingSettings, orc::LoggingController::defaultLogFilePath());
      logDestination =
          orc::LoggingSettingsModel::destinationFor(loggingSettings);
      if (!sharedLogFile.isEmpty()) {
        // The saved path may name a folder that no longer exists.
        const QString logDirectory = QFileInfo(sharedLogFile).absolutePath();
        if (!logDirectory.isEmpty()) {
          QDir().mkpath(logDirectory);
        }
      }
    }

    const std::string logPattern =
        orc::LoggingController::logPattern().toStdString();

    // Initialize GUI logging
    orc::init_gui_logging(logLevel.toStdString(), logPattern,
                          sharedLogFile.toStdString(), logDestination);

    // Initialize core logging (same file) through presenters layer
    orc::presenters::initCoreLogging(logLevel.toStdString(), logPattern,
                                     sharedLogFile.toStdString(),
                                     logDestination);

    // Owns the live logging configuration for the run so Tools > Logging can
    // change it without a restart.
    orc::LoggingController loggingController(loggingSettings);

    // Only warn when the destination was asked for explicitly: the default
    // ("both" with no log file) is plain console logging, which is not
    // noteworthy.
    if (parser.isSet(logOutOption) &&
        logDestination != orc::LogDestination::kConsole &&
        sharedLogFile.isEmpty()) {
      ORC_LOG_WARN(
          "--log-out {} was requested without --log-file; logging to the "
          "console only",
          logOutValue.toStdString());
    }

    // Resolve the background observation worker-pool size. "auto"/0/invalid
    // means half the cores (resolved in the presenters layer); a positive
    // integer overrides. Applied before any project is opened.
    {
      const QString threadsValue =
          parser.value(observerThreadsOption).trimmed();
      unsigned observerThreads = 0;  // 0 == auto
      if (!threadsValue.isEmpty() &&
          threadsValue.compare("auto", Qt::CaseInsensitive) != 0) {
        bool ok = false;
        const uint parsed = threadsValue.toUInt(&ok);
        if (ok) {
          observerThreads = parsed;
        } else {
          ORC_LOG_WARN(
              "Ignoring invalid --observer-threads value '{}'; using auto",
              threadsValue.toStdString());
        }
      }
      orc::presenters::setBackgroundObservationWorkerCount(observerThreads);
      ORC_LOG_INFO("Background observation worker threads: {} (effective: {})",
                   observerThreads == 0 ? std::string("auto")
                                        : std::to_string(observerThreads),
                   orc::presenters::resolveBackgroundObservationWorkerCount());
    }

    // Bridge Qt messages to spdlog
    qInstallMessageHandler(qtMessageHandler);

    ORC_LOG_INFO("orc-gui {} starting", ORC_VERSION);
    if (parser.isSet(safeCorePluginsOption)) {
      ORC_LOG_WARN(
          "Safe startup mode enabled: plugin registry cleared and "
          "ORC_STAGE_PLUGIN_PATHS ignored for this run");
    }

    // Resolve the initial theme mode: an explicit --theme flag wins for this
    // run, otherwise fall back to the mode last chosen in the GUI (Tools >
    // Themes), defaulting to auto. The ThemeController applies the theme,
    // tracks OS colour-scheme changes in auto mode, and lets the GUI override
    // the mode at runtime.
    QString initialThemeMode = parser.value(themeOption);
    if (!parser.isSet(themeOption)) {
      initialThemeMode = QSettings().value("theme/mode", "auto").toString();
    }
    ThemeController themeController(app, initialThemeMode);

    // Initialize crash handler
    orc::CrashHandlerConfig crash_config;
    crash_config.application_name = "orc-gui";
    crash_config.version = ORC_VERSION;

    QString crashDir =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (crashDir.isEmpty()) {
      crashDir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    if (!crashDir.isEmpty()) {
      crashDir += "/decode-orc-crashes";
      crash_config.output_directory = crashDir.toStdString();
    } else {
      crash_config.output_directory = fs::current_path().string();
    }

    crash_config.enable_coredump = true;
    crash_config.auto_upload_info = true;
    crash_config.primary_log_file = sharedLogFile.toStdString();
    crash_config.custom_info_callback = []() -> std::string {
      std::ostringstream info;
      info << "Working directory: " << fs::current_path().string() << "\n";
      info << "Qt version: " << qVersion() << "\n";
      return info.str();
    };

    if (!orc::init_crash_handler(crash_config)) {
      ORC_LOG_WARN("Failed to initialize crash handler");
    } else {
      ORC_LOG_DEBUG("Crash handler initialized - bundles will be saved to: {}",
                    crash_config.output_directory);
    }

    // Said before the first window exists, so a log always carries the path
    // the surfaces set out on even when none of them ever gets as far as
    // naming a backend. A GPU decision is followed by the backend's own line
    // once a surface initialises; the absence of that second line is itself
    // worth seeing.
    ORC_LOG_INFO(
        "Render surfaces: {}",
        orc::gui::gpu::GpuSurfacePolicy::instance().aboutText().toStdString());

    MainWindow window;
    window.show();
    ORC_LOG_DEBUG("Main window shown");

    auto load_startup_file = [&](const QString& file_path, bool quick_mode) {
      const char* action =
          quick_mode ? "quick project creation" : "project open";

      try {
        if (quick_mode) {
          window.quickProject(file_path);
        } else {
          window.openProject(file_path);
        }
      } catch (const orc::UserDataError& e) {
        ORC_LOG_WARN("Startup {} failed for '{}': {}", action,
                     file_path.toStdString(), e.what());
        QMessageBox::warning(&window, "Startup Error",
                             QString("%1 failed for '%2':\n%3")
                                 .arg(action)
                                 .arg(file_path)
                                 .arg(e.what()));
      } catch (const std::exception& e) {
        ORC_LOG_ERROR("Unhandled startup {} exception for '{}': {}", action,
                      file_path.toStdString(), e.what());

        std::string bundle_path =
            orc::create_crash_bundle(std::string("Unhandled startup ") +
                                     action + " exception: " + e.what());

        QString message =
            QString("An unexpected error occurred while processing '%1':\n%2")
                .arg(file_path)
                .arg(e.what());
        if (!bundle_path.empty()) {
          message += QString("\n\nDiagnostic bundle created:\n%1")
                         .arg(QString::fromStdString(bundle_path));
        }

        QMessageBox::critical(&window, "Startup Error", message);
      } catch (...) {
        ORC_LOG_ERROR("Unknown startup {} exception for '{}'", action,
                      file_path.toStdString());

        std::string bundle_path = orc::create_crash_bundle(
            std::string("Unknown startup ") + action + " exception");

        QString message =
            QString("An unknown error occurred while processing '%1'.")
                .arg(file_path);
        if (!bundle_path.empty()) {
          message += QString("\n\nDiagnostic bundle created:\n%1")
                         .arg(QString::fromStdString(bundle_path));
        }

        QMessageBox::critical(&window, "Startup Error", message);
      }
    };

    if (parser.isSet(quickProjectOption)) {
      QString quickFile = parser.value(quickProjectOption);
      ORC_LOG_INFO("Loading quick project from command line: {}",
                   quickFile.toStdString());
      load_startup_file(quickFile, true);
    } else {
      const QStringList args = parser.positionalArguments();
      if (!args.isEmpty()) {
        QString filePath = args.first();
        // Auto-detect file type by extension
        if (filePath.endsWith(".tbc", Qt::CaseInsensitive) ||
            filePath.endsWith(".tbcc", Qt::CaseInsensitive) ||
            filePath.endsWith(".tbcy", Qt::CaseInsensitive)) {
          ORC_LOG_INFO("Creating quick project from TBC file: {}",
                       filePath.toStdString());
          load_startup_file(filePath, true);
        } else if (filePath.endsWith(".cvbs", Qt::CaseInsensitive) ||
                   filePath.endsWith(".cvbsy", Qt::CaseInsensitive) ||
                   filePath.endsWith(".cvbsc", Qt::CaseInsensitive)) {
          ORC_LOG_INFO("Creating quick project from CVBS file: {}",
                       filePath.toStdString());
          load_startup_file(filePath, true);
        } else if (filePath.endsWith(".orcprj", Qt::CaseInsensitive)) {
          ORC_LOG_INFO("Opening project from command line: {}",
                       filePath.toStdString());
          load_startup_file(filePath, false);
        } else {
          // Unknown file type - try opening as project (old behavior)
          ORC_LOG_WARN(
              "Unknown file extension for '{}', attempting to open as project "
              "file",
              filePath.toStdString());
          load_startup_file(filePath, false);
        }
      }
    }

    // Splash screen
    QPixmap logoPixmap(":/orc-gui/decode-orc_logotype-1024x286.png");
    QPixmap logoPixmapScaled = logoPixmap.scaled(512, 142, Qt::KeepAspectRatio,
                                                 Qt::SmoothTransformation);
    QPixmap splashPixmap(logoPixmapScaled.width(),
                         logoPixmapScaled.height() + 60);
    splashPixmap.fill(Qt::transparent);

    QPainter painter(&splashPixmap);
    painter.drawPixmap(0, 0, logoPixmapScaled);

    QFont copyrightFont = painter.font();
    copyrightFont.setPointSize(copyrightFont.pointSize());
    copyrightFont.setBold(false);
    painter.setFont(copyrightFont);
    painter.setPen(Qt::white);
    QRect copyrightRect(0, logoPixmapScaled.height() + 10, splashPixmap.width(),
                        40);
    painter.drawText(copyrightRect, Qt::AlignCenter, "(c) 2026 Simon Inns");
    painter.end();

    QSplashScreen splash(splashPixmap,
                         Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint);
    splash.show();
    app.processEvents();
    const QRect windowFrame = window.frameGeometry();
    const QPoint centeredPos =
        windowFrame.center() - QPoint(splash.width() / 2, splash.height() / 2);
    splash.move(centeredPos);
    app.processEvents();
    ORC_LOG_DEBUG("Splash screen displayed");
    QTimer::singleShot(3000, [&splash]() {
      splash.close();
      ORC_LOG_DEBUG("Splash screen closed");
    });

    int result = app.exec();
    ORC_LOG_INFO("orc-gui exiting");

    orc::cleanup_crash_handler();
    return result;
  } catch (const std::exception& e) {
    std::cerr << "\nFATAL ERROR: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "\nFATAL ERROR: Unknown exception\n";
    return 1;
  }
}
