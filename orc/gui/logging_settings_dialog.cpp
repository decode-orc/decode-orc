/*
 * File:        logging_settings_dialog.cpp
 * Module:      orc-gui
 * Purpose:     Tools > Logging dialogue for capturing a diagnostic log
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "logging_settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

namespace orc {

namespace {

// Characters per line of explanatory text. Measured against the widget's own
// font rather than fixed in pixels, so the dialogue keeps the same proportions
// under a larger interface font or a scaled display.
constexpr int kExplanationCharacters = 62;

// Lines reserved for the summary. The sentence changes length as the controls
// change, and reserving the tallest case it normally reaches stops the
// dialogue resizing under the pointer as the user works.
constexpr int kSummaryLines = 3;

QLabel* makeWrappedLabel(const QString& text, const QWidget& reference) {
  auto* label = new QLabel(text);
  label->setWordWrap(true);
  label->setMinimumWidth(reference.fontMetrics().averageCharWidth() *
                         kExplanationCharacters);

  // A word-wrapped label is only as tall as the width it is finally given.
  // Without height-for-width the layout budgets a single line's worth of
  // height for it and the wrapped remainder is clipped.
  QSizePolicy policy(QSizePolicy::MinimumExpanding, QSizePolicy::Minimum,
                     QSizePolicy::Label);
  policy.setHeightForWidth(true);
  label->setSizePolicy(policy);

  return label;
}

}  // namespace

LoggingSettingsDialog::LoggingSettingsDialog(const LoggingSettings& initial,
                                             const QString& default_log_file,
                                             QWidget* parent)
    : QDialog(parent), default_log_file_(default_log_file) {
  setWindowTitle(QStringLiteral("Logging"));
  setObjectName(QStringLiteral("LoggingSettingsDialog"));

  auto* layout = new QVBoxLayout(this);
  // The wrapped explanations decide how tall the dialogue has to be, and the
  // summary line changes length while it is open. Constraining the dialogue to
  // its layout's minimum keeps every sentence visible and stops the window
  // being resized smaller than its contents.
  layout->setSizeConstraint(QLayout::SetMinimumSize);

  layout->addWidget(makeWrappedLabel(
      QStringLiteral(
          "Record what the application is doing to a file. Attach the file to "
          "a bug report to show what happened; turn logging off again "
          "afterwards, as a detailed log grows quickly."),
      *this));

  auto* form = new QFormLayout();
  form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

  file_logging_check_ =
      new QCheckBox(QStringLiteral("Write log messages to a file"));
  file_logging_check_->setObjectName(QStringLiteral("fileLoggingCheck"));
  form->addRow(file_logging_check_);

  level_combo_ = new QComboBox();
  level_combo_->setObjectName(QStringLiteral("levelCombo"));
  for (const QString& level : LoggingSettingsModel::levelNames()) {
    level_combo_->addItem(level);
  }
  form->addRow(QStringLiteral("Detail:"), level_combo_);

  // Spans both form columns: in the narrow field column the description wraps
  // into a tall, ragged block beside a one-line combo box.
  level_description_ = makeWrappedLabel(QString(), *this);
  level_description_->setObjectName(QStringLiteral("levelDescription"));
  form->addRow(level_description_);

  file_path_edit_ = new QLineEdit();
  file_path_edit_->setObjectName(QStringLiteral("filePathEdit"));
  file_path_edit_->setPlaceholderText(default_log_file_);
  file_path_edit_->setClearButtonEnabled(true);

  browse_button_ = new QPushButton(QStringLiteral("Browse..."));
  browse_button_->setObjectName(QStringLiteral("browseButton"));
  browse_button_->setAutoDefault(false);

  auto* path_row = new QHBoxLayout();
  path_row->setContentsMargins(0, 0, 0, 0);
  path_row->addWidget(file_path_edit_, 1);
  path_row->addWidget(browse_button_);
  form->addRow(QStringLiteral("Log file:"), path_row);

  frame_timing_check_ =
      new QCheckBox(QStringLiteral("Record per-frame preview timings"));
  frame_timing_check_->setObjectName(QStringLiteral("frameTimingCheck"));
  frame_timing_check_->setToolTip(QStringLiteral(
      "Writes one record per displayed frame breaking down where the preview "
      "and observer path spends its time, plus a summary every second. Use it "
      "to find what is slowing playback; leave it off otherwise."));
  form->addRow(frame_timing_check_);

  gpu_render_check_ =
      new QCheckBox(QStringLiteral("Draw the preview and scopes on the GPU"));
  gpu_render_check_->setObjectName(QStringLiteral("gpuRenderCheck"));
  gpu_render_check_->setToolTip(QStringLiteral(
      "Uses the graphics hardware for the preview surface and the scope "
      "canvases. Turn it off if drawing is wrong or unstable on this machine; "
      "the CPU path produces the same picture. Takes effect on windows opened "
      "afterwards."));
  form->addRow(gpu_render_check_);

  layout->addLayout(form);

  summary_label_ = makeWrappedLabel(QString(), *this);
  summary_label_->setObjectName(QStringLiteral("summaryLabel"));
  summary_label_->setMinimumHeight(summary_label_->fontMetrics().lineSpacing() *
                                   kSummaryLines);
  summary_label_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  layout->addWidget(summary_label_);

  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  open_folder_button_ = buttons->addButton(QStringLiteral("Open Log Folder"),
                                           QDialogButtonBox::ActionRole);
  open_folder_button_->setObjectName(QStringLiteral("openFolderButton"));
  open_folder_button_->setAutoDefault(false);
  layout->addWidget(buttons);

  connect(buttons, &QDialogButtonBox::accepted, this,
          &LoggingSettingsDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this,
          &LoggingSettingsDialog::reject);
  connect(open_folder_button_, &QPushButton::clicked, this,
          &LoggingSettingsDialog::openLogFolder);
  connect(browse_button_, &QPushButton::clicked, this,
          &LoggingSettingsDialog::browseForLogFile);
  connect(file_logging_check_, &QCheckBox::toggled, this,
          &LoggingSettingsDialog::refreshState);
  connect(frame_timing_check_, &QCheckBox::toggled, this,
          &LoggingSettingsDialog::refreshState);
  connect(level_combo_, &QComboBox::currentTextChanged, this,
          &LoggingSettingsDialog::refreshState);
  connect(file_path_edit_, &QLineEdit::textChanged, this,
          &LoggingSettingsDialog::refreshState);

  setSettings(initial);
}

LoggingSettings LoggingSettingsDialog::settings() const {
  LoggingSettings settings;
  settings.file_logging_enabled = file_logging_check_->isChecked();
  settings.level =
      LoggingSettingsModel::normaliseLevel(level_combo_->currentText());
  settings.file_path = file_path_edit_->text().trimmed();
  settings.frame_timing_enabled = frame_timing_check_->isChecked();
  return settings;
}

void LoggingSettingsDialog::setSettings(const LoggingSettings& settings) {
  const QSignalBlocker block_check(file_logging_check_);
  const QSignalBlocker block_level(level_combo_);
  const QSignalBlocker block_path(file_path_edit_);
  const QSignalBlocker block_frame_timing(frame_timing_check_);

  file_logging_check_->setChecked(settings.file_logging_enabled);
  frame_timing_check_->setChecked(settings.frame_timing_enabled);
  level_combo_->setCurrentText(
      LoggingSettingsModel::normaliseLevel(settings.level));
  file_path_edit_->setText(settings.file_path);

  refreshState();
}

void LoggingSettingsDialog::refreshState() {
  const bool enabled = file_logging_check_->isChecked();
  file_path_edit_->setEnabled(enabled);
  browse_button_->setEnabled(enabled);

  const LoggingSettings current = settings();
  level_description_->setText(
      LoggingSettingsModel::levelDescription(current.level));
  summary_label_->setText(
      LoggingSettingsModel::summaryText(current, default_log_file_));
}

void LoggingSettingsDialog::browseForLogFile() {
  const QString start =
      LoggingSettingsModel::resolveLogFile(settings(), default_log_file_);
  const QString chosen = QFileDialog::getSaveFileName(
      this, QStringLiteral("Select Log File"), start,
      QStringLiteral("Log files (*.log *.txt);;All files (*)"));
  if (!chosen.isEmpty()) {
    file_path_edit_->setText(chosen);
  }
}

void LoggingSettingsDialog::openLogFolder() {
  const LoggingSettings current = settings();
  QString path = current.file_path.trimmed();
  if (path.isEmpty()) {
    path = default_log_file_;
  }
  const QString folder = QFileInfo(path).absolutePath();
  if (!folder.isEmpty()) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
  }
}

bool LoggingSettingsDialog::gpuRenderEnabled() const {
  return gpu_render_check_->isChecked();
}

void LoggingSettingsDialog::setGpuRenderEnabled(bool enabled) {
  const QSignalBlocker block(gpu_render_check_);
  gpu_render_check_->setChecked(enabled);
}

void LoggingSettingsDialog::setGpuRenderAvailable(bool available,
                                                  const QString& reason) {
  gpu_render_check_->setEnabled(available);
  if (!available) {
    gpu_render_check_->setToolTip(reason);
  }
}

}  // namespace orc
