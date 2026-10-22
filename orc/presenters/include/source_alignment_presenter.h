/*
 * File:        source_alignment_presenter.h
 * Module:      orc-presenters
 * Purpose:     Presenter for Source Alignment analysis tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_PRESENTERS_SOURCE_ALIGNMENT_PRESENTER_H
#define ORC_PRESENTERS_SOURCE_ALIGNMENT_PRESENTER_H

#include <functional>

#include "analysis_tool_presenter.h"

namespace orc::presenters {

/**
 * @brief Presenter for Source Alignment analysis tool
 *
 * Handles:
 * - Preparing DAG context for the tool
 * - Executing input nodes to get all source artifacts
 * - Passing DAG and project context to the core tool
 * - Formatting results for display
 * - Applying generated alignment map to source_align stage
 *
 * The Source Alignment tool analyzes multiple input sources (typically
 * different captures of the same disc) and determines the optimal alignment
 * by finding common VBI frame numbers or CLV timecodes across all sources.
 *
 * **Requirements:**
 * - Node must be a source_align stage
 * - Must have at least one input (typically multiple)
 * - Input nodes must produce VideoFrameRepresentation artifacts
 * - Sources should have VBI data (CAV picture numbers or CLV timecodes)
 */
class SourceAlignmentPresenter : public AnalysisToolPresenter {
 public:
  /**
   * @brief Construct a Source Alignment presenter
   * @param project_handle Opaque handle to project
   */
  explicit SourceAlignmentPresenter(void* project_handle);

  /**
   * @brief Run source alignment analysis
   *
   * This method:
   * 1. Validates the node is a source_align stage
   * 2. Builds the DAG from the project
   * 3. Executes all input nodes to get source artifacts
   * 4. Calls the core source alignment tool with DAG/project context
   * 5. Formats results for display
   *
   * The tool will analyze VBI data across all sources to find common
   * frame numbers and generate an alignment map.
   *
   * @param node_id The source_align node to analyze
   * @param parameters User-selected parameters (currently no parameters
   * defined)
   * @param progress_callback Optional progress updates (percentage, status
   * message)
   * @param cancel_check Optional poll; return true to cancel the analysis
   * @return Analysis result with alignment map or error information
   */
  orc::AnalysisResult runAnalysis(
      NodeID node_id,
      const std::map<std::string, orc::ParameterValue>& parameters,
      std::function<void(int, const std::string&)> progress_callback,
      std::function<bool()> cancel_check) override;

 protected:
  std::string toolId() const override;
  std::string toolName() const override;

 private:
  /**
   * @brief Validate that the node is a source_align stage with inputs
   * @param node_id The node to validate
   * @param error_message Set to error description if validation fails
   * @return true if valid, false otherwise
   */
  bool validateNode(NodeID node_id, std::string& error_message);
};

}  // namespace orc::presenters

#endif  // ORC_PRESENTERS_SOURCE_ALIGNMENT_PRESENTER_H
