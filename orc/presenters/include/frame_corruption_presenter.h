/*
 * File:        frame_corruption_presenter.h
 * Module:      orc-presenters
 * Purpose:     Presenter for Frame Corruption Generator analysis tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_PRESENTERS_FRAME_CORRUPTION_PRESENTER_H
#define ORC_PRESENTERS_FRAME_CORRUPTION_PRESENTER_H

#include <functional>

#include "analysis_tool_presenter.h"

namespace orc::presenters {

/**
 * @brief Presenter for Frame Corruption Generator analysis tool
 *
 * Handles:
 * - Preparing DAG context for the tool
 * - Executing input node to get frame count
 * - Formatting results for display
 * - Applying generated pattern to frame_map stage
 *
 * This presenter encapsulates the business logic for running frame corruption
 * analysis, keeping the core tool focused on the algorithm and the GUI focused
 * on UI concerns.
 */
class FrameCorruptionPresenter : public AnalysisToolPresenter {
 public:
  /**
   * @brief Construct a Frame Corruption presenter
   * @param project_handle Opaque handle to project
   */
  explicit FrameCorruptionPresenter(void* project_handle);

  /**
   * @brief Run frame corruption analysis
   *
   * This method:
   * 1. Validates the node is a frame_map stage
   * 2. Builds the DAG from the project
   * 3. Executes the input node to get frame count
   * 4. Calls the core frame corruption tool with prepared context
   * 5. Formats results for display
   *
   * @param node_id The frame_map node to analyze
   * @param parameters User-selected parameters (pattern type, regenerate_seed,
   * etc.)
   * @param progress_callback Optional progress updates (percentage, status
   * message)
   * @param cancel_check Optional poll; return true to cancel the analysis
   * @return Analysis result with generated pattern or error information
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
   * @brief Get frame count from the input node
   * @param node_id The frame_map node
   * @return Frame count, or 0 if unable to determine
   */
  uint64_t getInputFrameCount(NodeID node_id);

  /**
   * @brief Extract existing seed from node parameters
   * @param node_id The frame_map node
   * @return Existing seed value, or 0 if not set
   */
  int32_t getExistingSeed(NodeID node_id);
};

}  // namespace orc::presenters

#endif  // ORC_PRESENTERS_FRAME_CORRUPTION_PRESENTER_H
