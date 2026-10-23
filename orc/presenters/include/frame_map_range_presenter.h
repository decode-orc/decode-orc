/*
 * File:        frame_map_range_presenter.h
 * Module:      orc-presenters
 * Purpose:     Presenter for Frame Map Range Finder analysis tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_PRESENTERS_FRAME_MAP_RANGE_PRESENTER_H
#define ORC_PRESENTERS_FRAME_MAP_RANGE_PRESENTER_H

#include <functional>

#include "analysis_tool_presenter.h"

namespace orc::presenters {

/**
 * @brief Presenter for Frame Map Range Finder analysis tool
 *
 * Prepares DAG/project context and maps progress/results for the GUI.
 */
class FrameMapRangePresenter : public AnalysisToolPresenter {
 public:
  explicit FrameMapRangePresenter(void* project_handle);

  /**
   * @brief Run the frame map range analysis
   * @param node_id Frame map node to analyse
   * @param parameters Tool parameters (startAddress/endAddress)
   * @param progress_callback Optional (percentage, status) progress sink
   * @param cancel_check Optional poll; return true to cancel the analysis
   */
  orc::AnalysisResult runAnalysis(
      NodeID node_id,
      const std::map<std::string, orc::ParameterValue>& parameters,
      std::function<void(int, const std::string&)> progress_callback,
      std::function<bool()> cancel_check) override;

 protected:
  std::string toolId() const override;
  std::string toolName() const override;
};

}  // namespace orc::presenters

#endif  // ORC_PRESENTERS_FRAME_MAP_RANGE_PRESENTER_H
