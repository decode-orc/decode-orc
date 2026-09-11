/*
 * File:        logging_settings_dialog.h
 * Module:      orc-gui
 * Purpose:     Tools > Logging dialogue for capturing a diagnostic log
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_LOGGING_SETTINGS_DIALOG_H
#define ORC_GUI_LOGGING_SETTINGS_DIALOG_H

#include <QDialog>
#include <QString>

#include "logging_settings.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace orc {

/**
 * @brief Turns diagnostic logging on and off without using the command line.
 *
 * The dialogue edits a LoggingSettings value and nothing else: it neither
 * reconfigures the loggers nor writes to the settings store. The caller reads
 * settings() after an accepted dialogue and hands the value to
 * LoggingController::apply(), which keeps the dialogue testable in isolation.
 */
class LoggingSettingsDialog : public QDialog {
  Q_OBJECT

 public:
  /// @param initial Configuration to show
  /// @param default_log_file Path used when no log file has been chosen; shown
  ///        as the field's placeholder and used in the summary line
  explicit LoggingSettingsDialog(const LoggingSettings& initial,
                                 const QString& default_log_file,
                                 QWidget* parent = nullptr);

  /// The configuration currently shown by the controls.
  LoggingSettings settings() const;

  /// Replaces the controls' contents.
  void setSettings(const LoggingSettings& settings);

  /// @name GPU rendering preference
  ///
  /// Kept out of LoggingSettings, which is about diagnostic logging and
  /// nothing else. The caller reads this after an accepted dialogue and hands
  /// it to GpuSurfacePolicy, which owns the persisted value.
  /// @{
  bool gpuRenderEnabled() const;
  void setGpuRenderEnabled(bool enabled);
  /// Grey the control out and say why, when the build or the environment has
  /// already decided.
  void setGpuRenderAvailable(bool available, const QString& reason);
  /// @}

 private:
  /// Enables the path controls only while file logging is on, and refreshes
  /// the level description and summary line.
  void refreshState();
  void browseForLogFile();
  void openLogFolder();

  QString default_log_file_;

  QCheckBox* file_logging_check_ = nullptr;
  QCheckBox* frame_timing_check_ = nullptr;
  QCheckBox* gpu_render_check_ = nullptr;
  QComboBox* level_combo_ = nullptr;
  QLabel* level_description_ = nullptr;
  QLineEdit* file_path_edit_ = nullptr;
  QPushButton* browse_button_ = nullptr;
  QPushButton* open_folder_button_ = nullptr;
  QLabel* summary_label_ = nullptr;
};

}  // namespace orc

#endif  // ORC_GUI_LOGGING_SETTINGS_DIALOG_H
