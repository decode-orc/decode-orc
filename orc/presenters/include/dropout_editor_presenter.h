/*
 * File:        dropout_editor_presenter.h
 * Module:      orc-presenters
 * Purpose:     Presenter for Dropout Editor analysis tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_PRESENTERS_DROPOUT_EDITOR_PRESENTER_H
#define ORC_PRESENTERS_DROPOUT_EDITOR_PRESENTER_H

#include <functional>

#include "analysis_tool_presenter.h"

namespace orc::presenters {

/**
 * @brief Presenter for Dropout Editor analysis tool
 *
 * Handles:
 * - Validating the node is a dropout_map stage
 * - Executing input node to get field data
 * - Launching the dropout editor dialog with prepared data
 *
 * The dropout editor is a GUI-only tool that provides an interactive
 * interface for marking and removing dropout regions in video fields.
 * This presenter prepares the necessary context (DAG, field artifacts)
 * for the editor dialog.
 */
class DropoutEditorPresenter : public AnalysisToolPresenter {
 public:
  /**
   * @brief Construct a Dropout Editor presenter
   * @param project_handle Opaque handle to project
   */
  explicit DropoutEditorPresenter(void* project_handle);

  /**
   * @brief Run dropout editor analysis
   *
   * This method:
   * 1. Validates the node is a dropout_map stage
   * 2. Builds the DAG from the project
   * 3. Executes the input node to get field data
   * 4. Calls the core dropout editor tool with prepared context
   * 5. Returns result indicating the editor was launched
   *
   * Note: The actual editing happens in the GUI dialog. This presenter
   * just prepares the data and launches the tool.
   *
   * @param node_id The dropout_map node to analyze
   * @param parameters User-selected parameters (currently none for this tool)
   * @param progress_callback Optional progress updates (percentage, status
   * message)
   * @param cancel_check Optional poll; return true to cancel the analysis
   * @return Analysis result indicating success or failure
   */
  orc::AnalysisResult runAnalysis(
      NodeID node_id,
      const std::map<std::string, orc::ParameterValue>& parameters,
      std::function<void(int, const std::string&)> progress_callback,
      std::function<bool()> cancel_check) override;

 protected:
  std::string toolId() const override { return "dropout_editor"; }

  std::string toolName() const override { return "Edit Dropout Map"; }
};

}  // namespace orc::presenters

#endif  // ORC_PRESENTERS_DROPOUT_EDITOR_PRESENTER_H
