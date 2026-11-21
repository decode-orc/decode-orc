/*
 * File:        analysis_presenter.h
 * Module:      orc-presenters
 * Purpose:     The tools a stage offers, and their parameters
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/params/parameter_types.h>
#include <orc_analysis.h>  // Public API analysis types

#include <memory>
#include <string>
#include <vector>

// Forward declare core Project type
namespace orc {
class Project;
}  // namespace orc

namespace orc::presenters {

/**
 * @brief The tools each stage offers (StageToolProvider), as orc-cli and
 * orc-gui list them
 */
class AnalysisPresenter {
 public:
  /**
   * @brief Construct presenter for a project
   * @param project_handle Opaque handle to project
   */
  explicit AnalysisPresenter(void* project_handle);

  ~AnalysisPresenter();

  // Disable copy, enable move
  AnalysisPresenter(const AnalysisPresenter&) = delete;
  AnalysisPresenter& operator=(const AnalysisPresenter&) = delete;
  AnalysisPresenter(AnalysisPresenter&&) noexcept;
  AnalysisPresenter& operator=(AnalysisPresenter&&) noexcept;

  /**
   * @brief The tools a stage type offers
   * @param stage_name Name of the stage type (e.g. "field_map")
   * @return Its tools, sorted by name; empty when the stage is
   *         not registered or offers none
   */
  std::vector<orc::AnalysisToolInfo> getToolsForStage(
      const std::string& stage_name) const;

  /**
   * @brief The parameters of an analysis tool a stage offers
   * @param tool_id The tool's id
   * @param stage_name The stage offering it
   * @return Its parameter descriptors; empty for a tool that is not an
   *         analysis tool, or that the stage does not offer
   */
  std::vector<orc::ParameterDescriptor> getToolParameters(
      const std::string& tool_id, const std::string& stage_name) const;

  /// The parameters of @p tool, from getToolsForStage().
  std::vector<orc::ParameterDescriptor> getToolParameters(
      const orc::AnalysisToolInfo& tool) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orc::presenters
