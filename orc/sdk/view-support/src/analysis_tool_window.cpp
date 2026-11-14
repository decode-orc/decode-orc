/*
 * File:        analysis_tool_window.cpp
 * Module:      decode-orc Plugin SDK (view-support tier)
 * Purpose:     The window of a stage's analysis tool: its parameters, a run
 *              with progress, the report, and Apply to Stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <orc/view-support/analysis_tool_window.h>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QTextEdit>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace orc::view {

namespace {

using Type = ToolAnalysisParameter::Type;

bool readInt(const std::string& text, int* value) {
  bool ok = false;
  const int parsed = QString::fromStdString(text).toInt(&ok);
  if (ok) {
    *value = parsed;
  }
  return ok;
}

bool readReal(const std::string& text, double* value) {
  bool ok = false;
  const double parsed = QString::fromStdString(text).toDouble(&ok);
  if (ok && std::isfinite(parsed)) {
    *value = parsed;
    return true;
  }
  return false;
}

/// The fewest digits that read back as @p value ("0.1", not "0.100000").
std::string realText(double value) {
  for (int digits = 15; digits < 17; ++digits) {
    const QString text = QString::number(value, 'g', digits);
    if (text.toDouble() == value) {
      return text.toStdString();
    }
  }
  return QString::number(value, 'g', 17).toStdString();
}

}  // namespace

struct AnalysisToolWindow::Control {
  ToolAnalysisParameter parameter;
  QWidget* widget = nullptr;
  QLabel* label = nullptr;
};

AnalysisToolWindow::AnalysisToolWindow(
    std::shared_ptr<IStageToolSession> session, QWidget* parent)
    : QDialog(parent), session_(std::move(session)) {
  if (!session_) {
    throw std::invalid_argument("AnalysisToolWindow needs a session");
  }
  info_ = session_->analysis_info();
  refresh_ = session_->refresh_state();

  auto* layout = new QVBoxLayout(this);

  description_label_ = new QLabel(QString::fromStdString(info_.description));
  description_label_->setWordWrap(true);
  layout->addWidget(description_label_);

  auto* parameters_group = new QGroupBox("Parameters");
  parameters_layout_ = new QFormLayout();
  parameters_group->setLayout(parameters_layout_);
  layout->addWidget(parameters_group);

  if (refresh_.available) {
    refresh_check_ =
        new QCheckBox("Use this tool when refreshing from the CLI");
    QString tip(
        "'orc-cli tools refresh' runs this tool for this stage, with these "
        "parameters, and applies the result. Takes effect when you press "
        "Apply to Stage or OK, and is saved with the project.");
    if (!refresh_.other_tool.empty()) {
      tip += QString(
                 "\n\nThis stage is refreshed by '%1' now; ticking this "
                 "replaces it.")
                 .arg(QString::fromStdString(refresh_.other_tool));
    }
    refresh_check_->setToolTip(tip);
    refresh_check_->setChecked(refresh_.this_tool);
    layout->addWidget(refresh_check_);
  }

  auto* progress_group = new QGroupBox("Progress");
  auto* progress_layout = new QVBoxLayout();
  status_label_ = new QLabel("Ready");
  progress_bar_ = new QProgressBar();
  progress_bar_->setRange(0, 100);
  progress_bar_->setValue(0);
  progress_layout->addWidget(status_label_);
  progress_layout->addWidget(progress_bar_);
  progress_group->setLayout(progress_layout);
  layout->addWidget(progress_group);

  auto* report_group = new QGroupBox("Report");
  auto* report_layout = new QVBoxLayout();
  report_text_ = new QTextEdit();
  report_text_->setReadOnly(true);
  report_text_->setMinimumHeight(300);
  report_text_->setLineWrapMode(QTextEdit::WidgetWidth);
  report_layout->addWidget(report_text_);
  report_group->setLayout(report_layout);
  layout->addWidget(report_group);

  auto* buttons = new QHBoxLayout();
  run_button_ = new QPushButton("Run Analysis");
  cancel_button_ = new QPushButton("Cancel");
  apply_button_ = new QPushButton("Apply to Stage");
  ok_button_ = new QPushButton("OK");
  cancel_button_->setEnabled(false);
  apply_button_->setEnabled(false);
  buttons->addWidget(run_button_);
  buttons->addWidget(cancel_button_);
  buttons->addWidget(apply_button_);
  buttons->addStretch();
  buttons->addWidget(ok_button_);
  layout->addLayout(buttons);

  connect(run_button_, &QPushButton::clicked, this,
          &AnalysisToolWindow::runAnalysis);
  connect(cancel_button_, &QPushButton::clicked, this,
          &AnalysisToolWindow::cancelAnalysis);
  connect(apply_button_, &QPushButton::clicked, this,
          &AnalysisToolWindow::applyResults);
  connect(ok_button_, &QPushButton::clicked, this, &QDialog::accept);

  // An apply rebuilds the graph; the window stays open across it (and any
  // other rebuild): a run reads the project as it is when it starts.
  session_->set_graph_rebuilt_handler([] {});

  setWindowTitle(QString::fromStdString(session_->title()));
  resize(900, 700);
}

AnalysisToolWindow::~AnalysisToolWindow() {
  if (job_) {
    job_->cancel();
  }
}

void AnalysisToolWindow::ensureParameters() {
  if (parameters_built_) {
    return;
  }
  parameters_built_ = true;
  buildParameters(info_, parameters_layout_);
  if (refresh_.this_tool) {
    showParameters(refresh_.parameters);
  }
  updateDependencies();
}

void AnalysisToolWindow::showEvent(QShowEvent* event) {
  ensureParameters();
  QDialog::showEvent(event);
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

void AnalysisToolWindow::buildParameters(const ToolAnalysisInfo& info,
                                         QFormLayout* form) {
  for (const auto& parameter : info.parameters) {
    Control control;
    control.parameter = parameter;
    control.widget = makeControl(parameter);
    control.label =
        new QLabel(QString::fromStdString(parameter.display_name) + ":", this);
    const QString tip = QString::fromStdString(parameter.description);
    control.label->setToolTip(tip);
    control.widget->setToolTip(tip);
    form->addRow(control.label, control.widget);
    controls_.push_back(control);
  }
}

QWidget* AnalysisToolWindow::makeControl(
    const ToolAnalysisParameter& parameter) {
  const auto changed = [this] { updateDependencies(); };
  switch (parameter.type) {
    case Type::Bool: {
      auto* box = new QCheckBox(this);
      box->setChecked(parameter.default_value == "true");
      connect(box, &QCheckBox::toggled, this, changed);
      return box;
    }
    case Type::Integer: {
      auto* spin = new QSpinBox(this);
      int low = std::numeric_limits<int>::min();
      int high = std::numeric_limits<int>::max();
      readInt(parameter.min_value, &low);
      readInt(parameter.max_value, &high);
      spin->setRange(low, high);
      int value = std::clamp(0, low, high);
      readInt(parameter.default_value, &value);
      spin->setValue(value);
      connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, changed);
      return spin;
    }
    case Type::Real: {
      auto* spin = new QDoubleSpinBox(this);
      double low = -1.0e9;
      double high = 1.0e9;
      readReal(parameter.min_value, &low);
      readReal(parameter.max_value, &high);
      spin->setDecimals(6);
      spin->setRange(low, high);
      double value = std::clamp(0.0, low, high);
      readReal(parameter.default_value, &value);
      spin->setValue(value);
      connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
              changed);
      return spin;
    }
    case Type::Text:
      break;
  }
  if (!parameter.allowed_values.empty()) {
    auto* combo = new QComboBox(this);
    for (const auto& allowed : parameter.allowed_values) {
      combo->addItem(QString::fromStdString(allowed));
    }
    if (!parameter.default_value.empty()) {
      combo->setCurrentText(QString::fromStdString(parameter.default_value));
    }
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            changed);
    return combo;
  }
  auto* edit =
      new QLineEdit(QString::fromStdString(parameter.default_value), this);
  connect(edit, &QLineEdit::textChanged, this, changed);
  return edit;
}

ToolParameterMap AnalysisToolWindow::currentParameters() const {
  ToolParameterMap values;
  for (const auto& control : controls_) {
    const std::string& name = control.parameter.name;
    if (auto* box = qobject_cast<QCheckBox*>(control.widget)) {
      values[name] = box->isChecked() ? "true" : "false";
    } else if (auto* spin = qobject_cast<QSpinBox*>(control.widget)) {
      values[name] = std::to_string(spin->value());
    } else if (auto* real = qobject_cast<QDoubleSpinBox*>(control.widget)) {
      values[name] = realText(real->value());
    } else if (auto* combo = qobject_cast<QComboBox*>(control.widget)) {
      values[name] = combo->currentText().toStdString();
    } else if (auto* edit = qobject_cast<QLineEdit*>(control.widget)) {
      values[name] = edit->text().toStdString();
    }
  }
  return values;
}

void AnalysisToolWindow::showParameters(const ToolParameterMap& values) {
  for (const auto& control : controls_) {
    const auto it = values.find(control.parameter.name);
    if (it == values.end()) {
      continue;
    }
    const std::string& text = it->second;
    if (auto* box = qobject_cast<QCheckBox*>(control.widget)) {
      box->setChecked(text == "true");
    } else if (auto* spin = qobject_cast<QSpinBox*>(control.widget)) {
      int value = 0;
      if (readInt(text, &value)) {
        spin->setValue(value);
      }
    } else if (auto* real = qobject_cast<QDoubleSpinBox*>(control.widget)) {
      double value = 0.0;
      if (readReal(text, &value)) {
        real->setValue(value);
      }
    } else if (auto* combo = qobject_cast<QComboBox*>(control.widget)) {
      combo->setCurrentText(QString::fromStdString(text));
    } else if (auto* edit = qobject_cast<QLineEdit*>(control.widget)) {
      edit->setText(QString::fromStdString(text));
    }
  }
  updateDependencies();
}

void AnalysisToolWindow::updateDependencies() {
  const ToolParameterMap values = currentParameters();
  for (const auto& control : controls_) {
    const auto& parameter = control.parameter;
    if (parameter.depends_on.empty()) {
      continue;
    }
    const auto it = values.find(parameter.depends_on);
    const bool enabled =
        it != values.end() &&
        std::find(parameter.depends_on_values.begin(),
                  parameter.depends_on_values.end(),
                  it->second) != parameter.depends_on_values.end();
    control.widget->setEnabled(enabled);
    control.label->setEnabled(enabled);
  }
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

bool AnalysisToolWindow::isRunning() const {
  return job_ && job_->is_running();
}

void AnalysisToolWindow::setRunning(bool running) {
  run_button_->setEnabled(!running && !busy_);
  cancel_button_->setEnabled(running);
  ok_button_->setEnabled(!running && !busy_);
  apply_button_->setEnabled(!running && !busy_ && last_report_ &&
                            last_report_->status ==
                                ToolAnalysisReport::Status::Success);
}

void AnalysisToolWindow::runAnalysis() {
  ensureParameters();
  if (isRunning() || busy_) {
    return;
  }
  last_report_.reset();
  report_text_->clear();
  status_label_->setText("Running analysis...");
  progress_bar_->setValue(0);

  const ToolParameterMap parameters = currentParameters();
  last_run_parameters_ = parameters;
  QPointer<AnalysisToolWindow> self(this);
  job_ = session_->run_analysis(
      parameters,
      [self](ToolJobProgress progress) {
        if (self) {
          self->onProgress(progress);
        }
      },
      [self](ToolAnalysisReport report) {
        if (self) {
          self->onReport(std::move(report));
        }
      });
  if (!job_) {
    status_label_->setText("The analysis could not be started");
    setRunning(false);
    return;
  }
  setRunning(true);
}

void AnalysisToolWindow::cancelAnalysis() {
  if (isRunning()) {
    status_label_->setText("Cancelling analysis...");
    cancel_button_->setEnabled(false);
    job_->cancel();
  }
}

void AnalysisToolWindow::onProgress(const ToolJobProgress& progress) {
  if (progress.fraction >= 0.0) {
    progress_bar_->setValue(static_cast<int>(
        std::lround(std::clamp(progress.fraction, 0.0, 1.0) * 100.0)));
  }
  if (!progress.text.empty()) {
    status_label_->setText(QString::fromStdString(progress.text));
  }
}

void AnalysisToolWindow::onReport(ToolAnalysisReport report) {
  job_.reset();
  showReport(report);
  switch (report.status) {
    case ToolAnalysisReport::Status::Success:
      status_label_->setText("Analysis complete");
      progress_bar_->setValue(100);
      break;
    case ToolAnalysisReport::Status::Failed:
      status_label_->setText("Analysis failed");
      progress_bar_->setValue(0);
      break;
    case ToolAnalysisReport::Status::Cancelled:
      status_label_->setText("Analysis cancelled");
      progress_bar_->setValue(0);
      break;
  }
  last_report_ = std::move(report);
  setRunning(false);
}

void AnalysisToolWindow::showReport(const ToolAnalysisReport& report) {
  QString text;
  switch (report.status) {
    case ToolAnalysisReport::Status::Success:
      text = "Analysis completed successfully.\n\n";
      break;
    case ToolAnalysisReport::Status::Failed:
      text = "Analysis failed.\n\n";
      break;
    case ToolAnalysisReport::Status::Cancelled:
      text = "Analysis cancelled.\n\n";
      break;
  }
  text += QString::fromStdString(report.summary);
  report_text_->setPlainText(text);
}

QString AnalysisToolWindow::reportText() const {
  return report_text_->toPlainText();
}

// ---------------------------------------------------------------------------
// Applying and closing
// ---------------------------------------------------------------------------

void AnalysisToolWindow::applyResults() {
  ensureParameters();
  if (busy_ || isRunning()) {
    return;
  }
  if (!last_report_ ||
      last_report_->status != ToolAnalysisReport::Status::Success) {
    QMessageBox::warning(
        this, "Cannot Apply",
        "Analysis results are not valid. Please run the analysis again.");
    return;
  }
  // A successful run can still find nothing to write, such as a map of a
  // capture with no VBI; say so rather than pointing at the log.
  if (last_report_->parameter_changes.empty()) {
    QMessageBox::warning(
        this, "Nothing to Apply",
        "The analysis produced nothing to write to the stage, so the stage "
        "was left unchanged. The report says what was found.");
    return;
  }

  ToolParameterMap changes = last_report_->parameter_changes;
  busy_ = true;
  setRunning(false);
  status_label_->setText("Applying the result...");
  QPointer<AnalysisToolWindow> self(this);
  session_->apply_parameters(
      std::move(changes), windowTitle().toStdString(), [self](ToolReply reply) {
        if (!self) {
          return;
        }
        if (!reply.ok) {
          self->busy_ = false;
          self->setRunning(false);
          self->status_label_->setText("The result was not applied");
          QMessageBox::warning(
              self, "Apply Failed",
              QString("The result was not applied to the stage: %1")
                  .arg(QString::fromStdString(reply.error)));
          return;
        }
        self->status_label_->setText("Result applied to the stage");
        self->commitRefresh([self] {
          if (self) {
            self->busy_ = false;
            self->QDialog::accept();
          }
        });
      });
}

void AnalysisToolWindow::commitRefresh(std::function<void()> done) {
  ensureParameters();
  if (!refresh_check_) {
    done();
    return;
  }
  const bool wanted = refresh_check_->isChecked();
  const ToolParameterMap parameters =
      last_run_parameters_ ? *last_run_parameters_ : currentParameters();
  if (wanted == refresh_.this_tool &&
      (!wanted || parameters == refresh_.parameters)) {
    done();
    return;
  }
  if (!wanted && !refresh_.this_tool) {
    done();
    return;
  }
  QPointer<AnalysisToolWindow> self(this);
  session_->set_refresh(
      wanted, parameters,
      [self, wanted, parameters, done = std::move(done)](ToolReply reply) {
        if (!self) {
          return;
        }
        if (reply.ok) {
          self->refresh_.this_tool = wanted;
          self->refresh_.parameters = wanted ? parameters : ToolParameterMap{};
        } else {
          QMessageBox::warning(
              self, "Refresh Option Not Saved",
              QString("The refresh option could not be stored: %1")
                  .arg(QString::fromStdString(reply.error)));
        }
        done();
      });
}

void AnalysisToolWindow::accept() {
  ensureParameters();
  if (busy_ || isRunning()) {
    return;
  }
  busy_ = true;
  setRunning(false);
  QPointer<AnalysisToolWindow> self(this);
  commitRefresh([self] {
    if (self) {
      self->busy_ = false;
      self->QDialog::accept();
    }
  });
}

void AnalysisToolWindow::closeEvent(QCloseEvent* event) {
  if (!isRunning()) {
    event->accept();
    return;
  }
  const auto answer = QMessageBox::question(
      this, "Analysis Running",
      "Analysis is still running. Do you want to cancel it and close?",
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (answer == QMessageBox::Yes) {
    job_->cancel();
    event->accept();
  } else {
    event->ignore();
  }
}

}  // namespace orc::view
