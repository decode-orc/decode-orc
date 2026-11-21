/*
 * File:        analysis_tool_window.h
 * Module:      decode-orc Plugin SDK (view-support tier)
 * Purpose:     The window of a stage's analysis tool: its parameters, a run
 *              with progress, the report, and Apply to Stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * STABILITY: PUBLIC — This header is part of the stable plugin SDK.
 */

#pragma once

// SDK TIER: view-support — see <orc/view-support/view_support_export.h>.

#include <orc/view-support/view_support_export.h>
#include <orc/view/stage_tool_session.h>

#include <QDialog>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QCloseEvent;
class QShowEvent;
class QFormLayout;
class QLabel;
class QProgressBar;
class QPushButton;
class QTextEdit;
class QWidget;

namespace orc::view {

/**
 * @brief The window of a StageToolKind::Analysis tool
 *
 * Everything comes from the session: the tool's description and parameters
 * (IStageToolSession::analysis_info()), the run (run_analysis()), the write
 * (apply_parameters()) and the refresh option (refresh_state() and
 * set_refresh()). orc-gui opens one for any analysis tool no view binary
 * registered a window for; a view that wants other parameter controls
 * subclasses it and overrides the three parameter hooks. The controls are
 * built when the window is first shown (or first runs, applies or closes),
 * not in the constructor, so a subclass's hooks are the ones called.
 *
 * Apply to Stage writes the report's parameter changes, stores the refresh
 * option, and closes once both are done. OK stores the refresh option and
 * closes. A refused write leaves the window open and says why.
 */
class ORC_VIEW_SUPPORT_EXPORT AnalysisToolWindow : public QDialog {
  Q_OBJECT

 public:
  /// @throws std::invalid_argument if @p session is null
  explicit AnalysisToolWindow(std::shared_ptr<IStageToolSession> session,
                              QWidget* parent = nullptr);
  ~AnalysisToolWindow() override;

  AnalysisToolWindow(const AnalysisToolWindow&) = delete;
  AnalysisToolWindow& operator=(const AnalysisToolWindow&) = delete;

  /// Store the refresh option, then close.
  void accept() override;

  // ---- for tests and subclasses ----

  /// Start a run with the parameters as shown; does nothing while one runs.
  void runAnalysis();
  /// Ask the running analysis to stop.
  void cancelAnalysis();
  /// Write the last report's changes to the node, then close.
  void applyResults();

  bool isRunning() const;
  /// The last report, if a run has finished.
  const std::optional<ToolAnalysisReport>& lastReport() const {
    return last_report_;
  }
  /// The report text as shown.
  QString reportText() const;
  /// The refresh option, or null when the tool cannot refresh the node.
  QCheckBox* refreshCheck() const { return refresh_check_; }

 protected:
  /**
   * @brief Build the parameter controls into @p form
   *
   * Called once. The default makes one control per parameter of @p info: a
   * check box, a spin box, a list of the allowed values or a line edit.
   */
  virtual void buildParameters(const ToolAnalysisInfo& info, QFormLayout* form);
  /// The parameters as the controls show them, as text.
  virtual ToolParameterMap currentParameters() const;
  /// Show stored values in the controls (the refresh tool's parameters).
  virtual void showParameters(const ToolParameterMap& values);

  IStageToolSession& session() const { return *session_; }

  void closeEvent(QCloseEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private:
  struct Control;

  /// Build the parameter controls, the first time only.
  void ensureParameters();

  QWidget* makeControl(const ToolAnalysisParameter& parameter);
  void updateDependencies();
  void onProgress(const ToolJobProgress& progress);
  void onReport(ToolAnalysisReport report);
  void showReport(const ToolAnalysisReport& report);
  void setRunning(bool running);
  /// Store or clear the refresh tool as the option says, then @p done.
  void commitRefresh(std::function<void()> done);

  std::shared_ptr<IStageToolSession> session_;
  ToolAnalysisInfo info_;
  bool parameters_built_ = false;
  std::shared_ptr<IToolJob> job_;
  std::vector<Control> controls_;
  std::optional<ToolAnalysisReport> last_report_;
  std::optional<ToolParameterMap> last_run_parameters_;
  ToolRefreshState refresh_;
  bool busy_ = false;

  QLabel* description_label_ = nullptr;
  QFormLayout* parameters_layout_ = nullptr;
  QCheckBox* refresh_check_ = nullptr;
  QLabel* status_label_ = nullptr;
  QProgressBar* progress_bar_ = nullptr;
  QTextEdit* report_text_ = nullptr;
  QPushButton* run_button_ = nullptr;
  QPushButton* cancel_button_ = nullptr;
  QPushButton* apply_button_ = nullptr;
  QPushButton* ok_button_ = nullptr;
};

}  // namespace orc::view
