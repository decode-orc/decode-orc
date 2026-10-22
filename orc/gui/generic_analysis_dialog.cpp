/*
 * File:        generic_analysis_dialog.cpp
 * Module:      gui
 * Purpose:     Generic Qt dialog for running and monitoring analysis tools
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "generic_analysis_dialog.h"

#include "presenters/include/analysis_presenter.h"
#include "presenters/include/analysis_tool_presenter_factory.h"
#include "presenters/include/project_presenter.h"

// Forward declaration for core type used via opaque pointer
namespace orc {
class Project;
}

#include <QCloseEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <map>

namespace orc {
namespace gui {

/**
 * @brief Worker thread for running analysis asynchronously
 */
class GenericAnalysisDialog::AnalysisWorker : public QThread {
  Q_OBJECT

 public:
  AnalysisWorker(orc::presenters::AnalysisToolPresenter* tool_presenter,
                 orc::NodeID node_id,
                 std::map<std::string, orc::ParameterValue> parameters)
      : tool_presenter_(tool_presenter),
        node_id_(node_id),
        parameters_(std::move(parameters)) {}

  orc::AnalysisResult getResult() const { return result_; }

 signals:
  void progressUpdate(int percentage, QString status);

 protected:
  void run() override {
    // Progress callback to update UI (must use queued signal for thread safety)
    auto progress_callback = [this](int percentage, const std::string& status) {
      emit progressUpdate(percentage, QString::fromStdString(status));
    };

    // Cancel button requests QThread interruption; poll it so the core
    // tool can stop cooperatively instead of relying on terminate().
    auto cancel_check = [this] { return isInterruptionRequested(); };

    result_ = tool_presenter_->runAnalysis(node_id_, parameters_,
                                           progress_callback, cancel_check);
  }

 private:
  orc::presenters::AnalysisToolPresenter* tool_presenter_;  // Not owned
  orc::NodeID node_id_;
  std::map<std::string, orc::ParameterValue> parameters_;
  orc::AnalysisResult result_;
};

GenericAnalysisDialog::GenericAnalysisDialog(
    const std::string& tool_id, const orc::AnalysisToolInfo& tool_info,
    orc::presenters::AnalysisPresenter* presenter, const orc::NodeID& node_id,
    void* project, QWidget* parent)
    : QDialog(parent),
      tool_id_(tool_id),
      tool_info_(tool_info),
      presenter_(presenter),
      tool_presenter_(
          orc::presenters::makeAnalysisToolPresenter(tool_id, project)),
      project_(project),
      node_id_(node_id) {
  setupUI();
  populateParameters();
  setWindowTitle(QString::fromStdString(tool_info_.name));
  resize(900, 700);
}

GenericAnalysisDialog::~GenericAnalysisDialog() {
  // Clean up worker thread
  if (worker_) {
    worker_->wait();
    delete worker_;
  }

  delete presenter_;
}

void GenericAnalysisDialog::setupUI() {
  auto* layout = new QVBoxLayout(this);

  // Description
  descriptionLabel_ =
      new QLabel(QString::fromStdString(tool_info_.description));
  descriptionLabel_->setWordWrap(true);
  layout->addWidget(descriptionLabel_);

  // Parameters group
  auto* paramsGroup = new QGroupBox("Parameters");
  parametersLayout_ = new QFormLayout();
  paramsGroup->setLayout(parametersLayout_);
  layout->addWidget(paramsGroup);

  // Progress group
  auto* progressGroup = new QGroupBox("Progress");
  auto* progLayout = new QVBoxLayout();
  statusLabel_ = new QLabel("Ready");
  progressBar_ = new QProgressBar();
  progressBar_->setMaximum(100);
  progressBar_->setValue(0);
  progLayout->addWidget(statusLabel_);
  progLayout->addWidget(progressBar_);
  progressGroup->setLayout(progLayout);
  layout->addWidget(progressGroup);

  // Results/report text area
  auto* reportGroup = new QGroupBox("Report");
  auto* reportLayout = new QVBoxLayout();
  reportText_ = new QTextEdit();
  reportText_->setReadOnly(true);
  reportText_->setMinimumHeight(300);
  reportText_->setLineWrapMode(QTextEdit::WidgetWidth);
  reportLayout->addWidget(reportText_);
  reportGroup->setLayout(reportLayout);
  layout->addWidget(reportGroup);

  // Buttons
  auto* buttonLayout = new QHBoxLayout();
  runButton_ = new QPushButton("Run Analysis");
  cancelButton_ = new QPushButton("Cancel");
  applyButton_ = new QPushButton("Apply to Stage");
  closeButton_ = new QPushButton("OK");

  cancelButton_->setEnabled(false);
  applyButton_->setEnabled(false);

  buttonLayout->addWidget(runButton_);
  buttonLayout->addWidget(cancelButton_);
  buttonLayout->addWidget(applyButton_);
  buttonLayout->addStretch();
  buttonLayout->addWidget(closeButton_);

  layout->addLayout(buttonLayout);

  // Connections
  connect(runButton_, &QPushButton::clicked, this,
          &GenericAnalysisDialog::runAnalysis);
  connect(cancelButton_, &QPushButton::clicked, this,
          &GenericAnalysisDialog::cancelAnalysis);
  connect(applyButton_, &QPushButton::clicked, this,
          &GenericAnalysisDialog::applyResults);
  connect(closeButton_, &QPushButton::clicked, this, &QDialog::accept);
}

void GenericAnalysisDialog::populateParameters() {
  if (tool_id_ == "frame_map_range") {
    setupFrameMapRangeWidgets();
    return;
  }

  // Get parameters from presenter
  orc::AnalysisSourceType source_type = orc::AnalysisSourceType::LaserDisc;
  parameter_descriptors_ = presenter_->getToolParameters(tool_id_, source_type);

  for (const auto& param : parameter_descriptors_) {
    QWidget* widget = createParameterWidget(param.name, param);

    // Create label with tooltip
    auto* label = new QLabel(QString::fromStdString(param.display_name) + ":");
    label->setToolTip(QString::fromStdString(param.description));
    if (widget) {
      widget->setToolTip(QString::fromStdString(param.description));
    }

    parametersLayout_->addRow(label, widget ? widget : new QLabel("N/A"));

    if (widget) {
      ParameterWidget pw;
      pw.name = param.name;
      pw.widget = widget;
      pw.type = param.type;
      pw.label = label;
      parameterWidgets_.push_back(pw);

      // Connect change signals for dependency updates
      switch (param.type) {
        case orc::ParameterType::STRING:
          if (auto* combo = qobject_cast<QComboBox*>(widget)) {
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, &GenericAnalysisDialog::updateParameterDependencies);
          }
          break;
        case orc::ParameterType::INT32:
          connect(static_cast<QSpinBox*>(widget),
                  QOverload<int>::of(&QSpinBox::valueChanged), this,
                  &GenericAnalysisDialog::updateParameterDependencies);
          break;
        case orc::ParameterType::DOUBLE:
          connect(static_cast<QDoubleSpinBox*>(widget),
                  QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                  &GenericAnalysisDialog::updateParameterDependencies);
          break;
        case orc::ParameterType::BOOL: {
          auto* cb = static_cast<QCheckBox*>(widget);
          connect(cb, &QCheckBox::checkStateChanged, this,
                  [this](Qt::CheckState) { updateParameterDependencies(); });
        } break;
        default:
          break;
      }
    }
  }
}

QWidget* GenericAnalysisDialog::createParameterWidget(
    const std::string& name, const orc::ParameterDescriptor& param) {
  switch (param.type) {
    case orc::ParameterType::BOOL: {
      auto* cb = new QCheckBox();
      if (param.constraints.default_value &&
          std::holds_alternative<bool>(*param.constraints.default_value)) {
        cb->setChecked(std::get<bool>(*param.constraints.default_value));
      }
      return cb;
    }
    case orc::ParameterType::INT32: {
      auto* spin = new QSpinBox();
      if (param.constraints.min_value &&
          std::holds_alternative<int32_t>(*param.constraints.min_value)) {
        spin->setMinimum(std::get<int32_t>(*param.constraints.min_value));
      } else {
        spin->setMinimum(std::numeric_limits<int32_t>::min());
      }
      if (param.constraints.max_value &&
          std::holds_alternative<int32_t>(*param.constraints.max_value)) {
        spin->setMaximum(std::get<int32_t>(*param.constraints.max_value));
      } else {
        spin->setMaximum(std::numeric_limits<int32_t>::max());
      }
      if (param.constraints.default_value &&
          std::holds_alternative<int32_t>(*param.constraints.default_value)) {
        spin->setValue(std::get<int32_t>(*param.constraints.default_value));
      }
      return spin;
    }
    case orc::ParameterType::DOUBLE: {
      auto* spin = new QDoubleSpinBox();
      if (param.constraints.default_value &&
          std::holds_alternative<double>(*param.constraints.default_value)) {
        spin->setValue(std::get<double>(*param.constraints.default_value));
      }
      return spin;
    }
    case orc::ParameterType::STRING: {
      if (!param.constraints.allowed_strings.empty()) {
        auto* combo = new QComboBox();
        for (const auto& allowed : param.constraints.allowed_strings) {
          combo->addItem(QString::fromStdString(allowed));
        }
        if (param.constraints.default_value &&
            std::holds_alternative<std::string>(
                *param.constraints.default_value)) {
          combo->setCurrentText(QString::fromStdString(
              std::get<std::string>(*param.constraints.default_value)));
        }
        return combo;
      } else {
        auto* edit = new QLineEdit();
        if (param.constraints.default_value &&
            std::holds_alternative<std::string>(
                *param.constraints.default_value)) {
          edit->setText(QString::fromStdString(
              std::get<std::string>(*param.constraints.default_value)));
        }
        return edit;
      }
    }
    default:
      return nullptr;
  }
}

void GenericAnalysisDialog::collectParameters() {
  // Note: Parameters are collected directly in runAnalysis() and passed to the
  // presenter This method is kept for potential future use
}

void GenericAnalysisDialog::runAnalysis() {
  collectParameters();

  runButton_->setEnabled(false);
  cancelButton_->setEnabled(true);
  applyButton_->setEnabled(false);
  closeButton_->setEnabled(false);
  reportText_->clear();
  statusLabel_->setText("Running analysis...");
  progressBar_->setValue(0);

  // Prepare parameters
  std::map<std::string, orc::ParameterValue> parameters;
  if (tool_id_ == "frame_map_range") {
    if (picture_start_spin_ && picture_end_spin_) {
      parameters["startAddress"] = std::to_string(picture_start_spin_->value());
      parameters["endAddress"] = std::to_string(picture_end_spin_->value());
    }
  } else {
    for (const auto& pw : parameterWidgets_) {
      switch (pw.type) {
        case orc::ParameterType::BOOL: {
          auto* cb = qobject_cast<QCheckBox*>(pw.widget);
          if (cb) parameters[pw.name] = cb->isChecked();
          break;
        }
        case orc::ParameterType::INT32: {
          auto* spin = qobject_cast<QSpinBox*>(pw.widget);
          if (spin) parameters[pw.name] = spin->value();
          break;
        }
        case orc::ParameterType::DOUBLE: {
          auto* spin = qobject_cast<QDoubleSpinBox*>(pw.widget);
          if (spin) parameters[pw.name] = spin->value();
          break;
        }
        case orc::ParameterType::STRING: {
          auto* combo = qobject_cast<QComboBox*>(pw.widget);
          if (combo) {
            parameters[pw.name] = combo->currentText().toStdString();
          } else {
            auto* edit = qobject_cast<QLineEdit*>(pw.widget);
            if (edit) parameters[pw.name] = edit->text().toStdString();
          }
          break;
        }
        default:
          break;
      }
    }
  }

  // Clean up old worker if it exists
  if (worker_) {
    worker_->wait();
    delete worker_;
    worker_ = nullptr;
  }

  // Create and start worker thread
  worker_ = new AnalysisWorker(tool_presenter_.get(), node_id_, parameters);

  // Connect signals for progress updates and completion
  connect(worker_, &AnalysisWorker::progressUpdate, this,
          &GenericAnalysisDialog::onAnalysisProgress, Qt::QueuedConnection);
  connect(worker_, &AnalysisWorker::finished, this,
          &GenericAnalysisDialog::onAnalysisComplete, Qt::QueuedConnection);

  // Start the worker thread
  worker_->start();
}

void GenericAnalysisDialog::onAnalysisProgress(int percentage, QString status) {
  statusLabel_->setText(status);
  progressBar_->setValue(percentage);
}

void GenericAnalysisDialog::onAnalysisComplete() {
  if (!worker_) {
    return;
  }

  // Get result from worker
  last_result_ = worker_->getResult();

  // Display results
  displayResults(last_result_);

  // Re-enable buttons
  runButton_->setEnabled(true);
  cancelButton_->setEnabled(false);
  closeButton_->setEnabled(true);
  if (last_result_.status == orc::AnalysisResult::Status::Success) {
    applyButton_->setEnabled(true);
    statusLabel_->setText("Analysis complete");
    progressBar_->setValue(100);
  } else {
    statusLabel_->setText("Analysis failed");
    progressBar_->setValue(0);
  }
}

void GenericAnalysisDialog::cancelAnalysis() {
  if (worker_ && worker_->isRunning()) {
    statusLabel_->setText("Cancelling analysis...");

    // Disconnect the finished signal to prevent onAnalysisComplete from firing
    disconnect(worker_, &AnalysisWorker::finished, this,
               &GenericAnalysisDialog::onAnalysisComplete);

    // Request the worker to terminate
    worker_->requestInterruption();
    // Wait for the thread to finish (with timeout)
    if (!worker_->wait(5000)) {
      // If it doesn't finish in 5 seconds, force terminate
      worker_->terminate();
      worker_->wait();
    }

    // Create a cancelled result
    last_result_.status = orc::AnalysisResult::Status::Cancelled;
    last_result_.summary = "Analysis was cancelled by user.";

    // Display the cancelled result
    displayResults(last_result_);

    statusLabel_->setText("Analysis cancelled");
    runButton_->setEnabled(true);
    cancelButton_->setEnabled(false);
    closeButton_->setEnabled(true);
    progressBar_->setValue(0);
  }
}

void GenericAnalysisDialog::closeEvent(QCloseEvent* event) {
  // If analysis is running, ask user to confirm or cancel it
  if (worker_ && worker_->isRunning()) {
    auto reply = QMessageBox::question(
        this, "Analysis Running",
        "Analysis is still running. Do you want to cancel it and close?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply == QMessageBox::Yes) {
      // Cancel the analysis
      worker_->requestInterruption();
      if (!worker_->wait(5000)) {
        worker_->terminate();
        worker_->wait();
      }
      event->accept();
    } else {
      event->ignore();
    }
  } else {
    event->accept();
  }
}

void GenericAnalysisDialog::displayResults(const orc::AnalysisResult& result) {
  QString text;

  if (result.status == orc::AnalysisResult::Status::Success) {
    text = "Analysis completed successfully.\n\n";
  } else if (result.status == orc::AnalysisResult::Status::Failed) {
    text = "Analysis failed.\n\n";
  } else {
    text = "Analysis cancelled.\n\n";
  }

  text += QString::fromStdString(result.summary);

  reportText_->setPlainText(text);
}

void GenericAnalysisDialog::applyResults() {
  if (last_result_.status != orc::AnalysisResult::Status::Success) {
    QMessageBox::warning(
        this, "Cannot Apply",
        "Analysis results are not valid. Please run the analysis again.");
    return;
  }

  // Apply through the tool's presenter, so the core tool's applyToGraph logic
  // is executed. A tool that cannot apply to the graph reports false.
  const bool applied =
      tool_presenter_->applyResultToGraph(last_result_, node_id_);

  if (!applied) {
    QMessageBox::warning(this, "Apply Failed",
                         "Failed to apply analysis results to the stage. Check "
                         "the log for details.");
    return;
  }

  // Emit signal so mainwindow can update UI (rebuild DAG, refresh preview,
  // etc.)
  emit applyResultsRequested(last_result_);

  // Close dialog
  accept();
}

void GenericAnalysisDialog::updateParameterDependencies() {
  if (tool_id_ == "frame_map_range") {
    return;
  }
  // Get current values of all parameters
  std::map<std::string, orc::ParameterValue> current_values;
  for (const auto& pw : parameterWidgets_) {
    const std::string& name = pw.name;

    switch (pw.type) {
      case orc::ParameterType::BOOL: {
        auto* cb = qobject_cast<QCheckBox*>(pw.widget);
        if (cb) current_values[name] = cb->isChecked();
        break;
      }
      case orc::ParameterType::INT32: {
        auto* spin = qobject_cast<QSpinBox*>(pw.widget);
        if (spin) current_values[name] = spin->value();
        break;
      }
      case orc::ParameterType::DOUBLE: {
        auto* spin = qobject_cast<QDoubleSpinBox*>(pw.widget);
        if (spin) current_values[name] = spin->value();
        break;
      }
      case orc::ParameterType::STRING: {
        auto* combo = qobject_cast<QComboBox*>(pw.widget);
        if (combo) {
          current_values[name] = combo->currentText().toStdString();
        } else {
          auto* edit = qobject_cast<QLineEdit*>(pw.widget);
          if (edit) current_values[name] = edit->text().toStdString();
        }
        break;
      }
      default:
        break;
    }
  }

  // Check each parameter's dependencies
  for (size_t i = 0; i < parameter_descriptors_.size(); ++i) {
    const auto& desc = parameter_descriptors_[i];

    if (!desc.constraints.depends_on.has_value()) {
      continue;  // No dependency, always enabled
    }

    const auto& dep = *desc.constraints.depends_on;
    bool should_enable = false;

    // Find the value of the parameter we depend on
    auto it = current_values.find(dep.parameter_name);
    if (it != current_values.end()) {
      // Convert current value to string for comparison
      std::string current_val =
          orc::parameter_util::value_to_string(it->second);

      // Check if current value is in the list of required values
      should_enable =
          std::find(dep.required_values.begin(), dep.required_values.end(),
                    current_val) != dep.required_values.end();
    }

    // Enable or disable the widget and label
    if (i < parameterWidgets_.size()) {
      parameterWidgets_[i].widget->setEnabled(should_enable);
      if (parameterWidgets_[i].label) {
        parameterWidgets_[i].label->setEnabled(should_enable);
      }
    }
  }
}

int GenericAnalysisDialog::timecodeFps() const {
  return frame_map_range_fps_ > 0 ? frame_map_range_fps_ : 30;
}

void GenericAnalysisDialog::setupFrameMapRangeWidgets() {
  parametersLayout_->setContentsMargins(0, 0, 0, 0);
  parametersLayout_->setHorizontalSpacing(12);
  parametersLayout_->setVerticalSpacing(8);

  // Determine FPS from project format
  frame_map_range_fps_ = 30;
  if (project_) {
    orc::presenters::ProjectPresenter project_presenter(project_);
    auto format = project_presenter.getVideoFormat();
    if (format == orc::presenters::VideoFormat::PAL) {
      frame_map_range_fps_ = 25;
    } else if (format == orc::presenters::VideoFormat::NTSC) {
      frame_map_range_fps_ = 30;
    }
  }

  // Picture number controls
  picture_start_spin_ = new QSpinBox();
  picture_start_spin_->setMinimum(1);
  picture_start_spin_->setMaximum(std::numeric_limits<int32_t>::max());
  picture_start_spin_->setToolTip("Start picture number (1-based)");

  picture_end_spin_ = new QSpinBox();
  picture_end_spin_->setMinimum(1);
  picture_end_spin_->setMaximum(std::numeric_limits<int32_t>::max());
  picture_end_spin_->setToolTip("End picture number (1-based)");

  parametersLayout_->addRow(new QLabel("Picture Number Start:"),
                            picture_start_spin_);
  parametersLayout_->addRow(new QLabel("Picture Number End:"),
                            picture_end_spin_);

  auto make_timecode_widget = [this](QSpinBox*& hours, QSpinBox*& minutes,
                                     QSpinBox*& seconds, QSpinBox*& pictures) {
    auto* container = new QWidget();
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    hours = new QSpinBox();
    hours->setMinimum(0);
    hours->setMaximum(999);
    hours->setToolTip("Hours");

    minutes = new QSpinBox();
    minutes->setMinimum(0);
    minutes->setMaximum(59);
    minutes->setToolTip("Minutes");

    seconds = new QSpinBox();
    seconds->setMinimum(0);
    seconds->setMaximum(59);
    seconds->setToolTip("Seconds");

    pictures = new QSpinBox();
    pictures->setMinimum(0);
    pictures->setMaximum(timecodeFps() - 1);
    pictures->setToolTip("Picture (frame) within second");

    layout->addWidget(hours);
    layout->addWidget(new QLabel(":"));
    layout->addWidget(minutes);
    layout->addWidget(new QLabel(":"));
    layout->addWidget(seconds);
    layout->addWidget(new QLabel("."));
    layout->addWidget(pictures);
    layout->addStretch();

    return container;
  };

  auto* tc_start_widget =
      make_timecode_widget(tc_start_hours_, tc_start_minutes_,
                           tc_start_seconds_, tc_start_pictures_);
  auto* tc_end_widget = make_timecode_widget(tc_end_hours_, tc_end_minutes_,
                                             tc_end_seconds_, tc_end_pictures_);

  parametersLayout_->addRow(new QLabel("Timecode Start:"), tc_start_widget);
  parametersLayout_->addRow(new QLabel("Timecode End:"), tc_end_widget);

  // Initial values
  picture_start_spin_->setValue(1);
  picture_end_spin_->setValue(1);
  syncTimecodeFromPicture(true);
  syncTimecodeFromPicture(false);

  // Wiring
  connect(picture_start_spin_, QOverload<int>::of(&QSpinBox::valueChanged),
          this, [this](int) { syncTimecodeFromPicture(true); });
  connect(picture_end_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [this](int) { syncTimecodeFromPicture(false); });

  auto timecode_changed = [this](bool is_start) {
    syncPictureFromTimecode(is_start);
  };
  connect(tc_start_hours_, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [timecode_changed](int) { timecode_changed(true); });
  connect(tc_start_minutes_, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [timecode_changed](int) { timecode_changed(true); });
  connect(tc_start_seconds_, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [timecode_changed](int) { timecode_changed(true); });
  connect(tc_start_pictures_, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [timecode_changed](int) { timecode_changed(true); });

  connect(tc_end_hours_, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [timecode_changed](int) { timecode_changed(false); });
  connect(tc_end_minutes_, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [timecode_changed](int) { timecode_changed(false); });
  connect(tc_end_seconds_, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [timecode_changed](int) { timecode_changed(false); });
  connect(tc_end_pictures_, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [timecode_changed](int) { timecode_changed(false); });
}

void GenericAnalysisDialog::syncTimecodeFromPicture(bool is_start) {
  if (frame_map_range_sync_in_progress_) {
    return;
  }
  frame_map_range_sync_in_progress_ = true;

  QSpinBox* pic_spin = is_start ? picture_start_spin_ : picture_end_spin_;
  QSpinBox* hours = is_start ? tc_start_hours_ : tc_end_hours_;
  QSpinBox* minutes = is_start ? tc_start_minutes_ : tc_end_minutes_;
  QSpinBox* seconds = is_start ? tc_start_seconds_ : tc_end_seconds_;
  QSpinBox* pictures = is_start ? tc_start_pictures_ : tc_end_pictures_;

  if (pic_spin && hours && minutes && seconds && pictures) {
    int64_t pn = std::max(1, pic_spin->value());
    int64_t frame_index = pn - 1;
    int fps = timecodeFps();
    int64_t frames_per_hour = static_cast<int64_t>(fps) * 3600;
    int64_t frames_per_minute = static_cast<int64_t>(fps) * 60;

    int64_t h = frame_index / frames_per_hour;
    frame_index %= frames_per_hour;
    int64_t m = frame_index / frames_per_minute;
    frame_index %= frames_per_minute;
    int64_t s = frame_index / fps;
    int64_t p = frame_index % fps;

    hours->setValue(static_cast<int>(h));
    minutes->setValue(static_cast<int>(m));
    seconds->setValue(static_cast<int>(s));
    pictures->setValue(static_cast<int>(p));
  }

  frame_map_range_sync_in_progress_ = false;
}

void GenericAnalysisDialog::syncPictureFromTimecode(bool is_start) {
  if (frame_map_range_sync_in_progress_) {
    return;
  }
  frame_map_range_sync_in_progress_ = true;

  QSpinBox* pic_spin = is_start ? picture_start_spin_ : picture_end_spin_;
  QSpinBox* hours = is_start ? tc_start_hours_ : tc_end_hours_;
  QSpinBox* minutes = is_start ? tc_start_minutes_ : tc_end_minutes_;
  QSpinBox* seconds = is_start ? tc_start_seconds_ : tc_end_seconds_;
  QSpinBox* pictures = is_start ? tc_start_pictures_ : tc_end_pictures_;

  if (pic_spin && hours && minutes && seconds && pictures) {
    int fps = timecodeFps();
    int64_t frame_index = static_cast<int64_t>(hours->value()) * 3600 * fps +
                          static_cast<int64_t>(minutes->value()) * 60 * fps +
                          static_cast<int64_t>(seconds->value()) * fps +
                          static_cast<int64_t>(pictures->value());
    int64_t pn = frame_index + 1;
    if (pn < 1) pn = 1;
    if (pn > std::numeric_limits<int32_t>::max()) {
      pn = std::numeric_limits<int32_t>::max();
    }
    pic_spin->setValue(static_cast<int>(pn));
  }

  frame_map_range_sync_in_progress_ = false;
}

}  // namespace gui
}  // namespace orc

// Include moc file for nested Q_OBJECT class
#include "generic_analysis_dialog.moc"
