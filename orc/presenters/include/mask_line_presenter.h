/*
 * File:        mask_line_presenter.h
 * Module:      orc-presenters
 * Purpose:     Presenter for Mask Line analysis tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_PRESENTERS_MASK_LINE_PRESENTER_H
#define ORC_PRESENTERS_MASK_LINE_PRESENTER_H

#include <functional>

#include "analysis_tool_presenter.h"

namespace orc::presenters {

/**
 * @brief Presenter for Mask Line configuration tool
 *
 * Handles:
 * - Preparing configuration parameters for the tool
 * - Generating line masking specifications based on presets
 * - Formatting results for display
 * - Applying generated configuration to mask_line stage
 *
 * The Mask Line tool provides convenient presets for common line masking
 * scenarios, particularly for hiding visible VBI data like:
 * - NTSC Closed Captions (field line 20, first field only)
 * - PAL Teletext/WSS (field lines 6-22, both fields)
 * - Custom line specifications
 *
 * **Requirements:**
 * - Node must be a mask_line stage
 * - Tool operates as instant configuration (no DAG execution needed)
 * - User selects presets or provides custom line specification
 */
class MaskLinePresenter : public AnalysisToolPresenter {
 public:
  /**
   * @brief Construct a Mask Line presenter
   * @param project_handle Opaque handle to project
   */
  explicit MaskLinePresenter(void* project_handle);

  /**
   * @brief Run mask line configuration
   *
   * This method:
   * 1. Validates the node is a mask_line stage
   * 2. Calls the core mask line tool with user parameters
   * 3. Generates line specification string based on presets
   * 4. Formats configuration for display
   *
   * The tool generates configuration immediately without DAG execution.
   *
   * @param node_id The mask_line node to configure
   * @param parameters User-selected parameters (maskNTSC_CC, maskPAL_TT,
   * customLines, maskSampleLevel)
   * @param progress_callback Optional progress updates (not used for instant
   * tool)
   * @param cancel_check Optional poll; return true to cancel the analysis
   * @return Analysis result with line specification configuration
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
   * @brief Validate that the node is a mask_line stage
   * @param node_id The node to validate
   * @param error_message Set to error description if validation fails
   * @return true if valid, false otherwise
   */
  bool validateNode(NodeID node_id, std::string& error_message);
};

}  // namespace orc::presenters

#endif  // ORC_PRESENTERS_MASK_LINE_PRESENTER_H
