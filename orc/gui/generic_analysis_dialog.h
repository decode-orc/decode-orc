/*
 * File:        generic_analysis_dialog.h
 * Module:      gui
 * Purpose:     Generic Qt dialog for running and monitoring analysis tools
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_GUI_GENERIC_ANALYSIS_DIALOG_H
#define ORC_GUI_GENERIC_ANALYSIS_DIALOG_H

#include <orc/stage/node_id.h>
#include <orc_analysis.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTextEdit>
#include <QThread>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace orc::presenters {
class AnalysisPresenter;
class AnalysisToolPresenter;
}  // namespace orc::presenters

namespace orc {
namespace gui {

/**
 * @brief Generic analysis dialog for tools using the public API
 *
 * This dialog:
 * - Auto-generates parameter UI from tool parameter descriptors
 * - Shows progress during analysis
 * - Displays results in the report widget
 * - Allows applying results to the graph
 */
class GenericAnalysisDialog : public QDialog {
  Q_OBJECT

 public:
  GenericAnalysisDialog(const std::string& tool_id,
                        const orc::AnalysisToolInfo& tool_info,
                        orc::presenters::AnalysisPresenter* presenter,
                        const orc::NodeID& node_id, void* project,
                        QWidget* parent = nullptr);

  ~GenericAnalysisDialog();

  /// Stores the refresh option, then closes (OK and Apply to Stage).
  void accept() override;

 signals:
  void analysisApplied();
  void applyResultsRequested(const orc::AnalysisResult& result);
  /// The stage's refresh tool was stored or cleared (the project changed).
  void refreshConfigChanged();

 private slots:
  void runAnalysis();
  void cancelAnalysis();
  void applyResults();
  void updateParameterDependencies();
  void onAnalysisComplete();
  void onAnalysisProgress(int percentage, QString status);

 private:
  void setupUI();
  void populateParameters();
  QWidget* createParameterWidget(const std::string& name,
                                 const orc::ParameterDescriptor& param);
  void collectParameters();
  void closeEvent(QCloseEvent* event) override;
  void displayResults(const orc::AnalysisResult& result);
  void setupFrameMapRangeWidgets();
  void syncTimecodeFromPicture(bool is_start);
  void syncPictureFromTimecode(bool is_start);
  int timecodeFps() const;
  /// The parameters as the widgets show them.
  std::map<std::string, orc::ParameterValue> currentParameters() const;
  /// Show stored parameter values in the widgets.
  void showParameters(const std::map<std::string, orc::ParameterValue>& values);
  /// Show the stage's refresh option as stored.
  void setupRefreshOption();
  /// Store or clear the stage's refresh tool as the option says.
  void commitRefreshOption();

  std::string tool_id_;
  orc::AnalysisToolInfo tool_info_;
  orc::presenters::AnalysisPresenter* presenter_;  // Not owned
  /// Runs and applies the tool; chosen by makeAnalysisToolPresenter()
  std::unique_ptr<orc::presenters::AnalysisToolPresenter> tool_presenter_;
  void* project_;  // Not owned (opaque handle)
  orc::NodeID node_id_;
  orc::AnalysisResult last_result_;
  std::vector<orc::ParameterDescriptor> parameter_descriptors_;
  /// The parameters of the last run, which the refresh option stores.
  std::optional<std::map<std::string, orc::ParameterValue>>
      last_run_parameters_;

  // Frame map range custom controls
  bool frame_map_range_sync_in_progress_ = false;
  int frame_map_range_fps_ = 30;
  QSpinBox* picture_start_spin_ = nullptr;
  QSpinBox* picture_end_spin_ = nullptr;
  QSpinBox* tc_start_hours_ = nullptr;
  QSpinBox* tc_start_minutes_ = nullptr;
  QSpinBox* tc_start_seconds_ = nullptr;
  QSpinBox* tc_start_pictures_ = nullptr;
  QSpinBox* tc_end_hours_ = nullptr;
  QSpinBox* tc_end_minutes_ = nullptr;
  QSpinBox* tc_end_seconds_ = nullptr;
  QSpinBox* tc_end_pictures_ = nullptr;

  // UI widgets
  QLabel* descriptionLabel_;
  QLabel* statusLabel_;
  QProgressBar* progressBar_;
  QTextEdit* reportText_;
  QPushButton* runButton_;
  QPushButton* cancelButton_;
  QPushButton* applyButton_;
  QPushButton* closeButton_;
  QFormLayout* parametersLayout_;
  /// "Use this tool when refreshing from the CLI"; null for tools that
  /// cannot refresh a stage.
  QCheckBox* refreshCheck_ = nullptr;

  // Parameter widgets
  struct ParameterWidget {
    std::string name;
    QWidget* widget;
    orc::ParameterType type;
    QLabel* label = nullptr;
  };
  std::vector<ParameterWidget> parameterWidgets_;

  // Worker thread for async analysis
  class AnalysisWorker;
  AnalysisWorker* worker_ = nullptr;
};

}  // namespace gui
}  // namespace orc

#endif  // ORC_GUI_GENERIC_ANALYSIS_DIALOG_H
