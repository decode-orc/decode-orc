/*
 * File:        analysis_tool_presenter.h
 * Module:      orc-presenters
 * Purpose:     Running a stage's analysis tool and applying its result
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_PRESENTERS_ANALYSIS_TOOL_PRESENTER_H
#define ORC_PRESENTERS_ANALYSIS_TOOL_PRESENTER_H

#include <orc/stage/node_id.h>
#include <orc/stage/params/parameter_types.h>
#include <orc_analysis.h>

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace orc::presenters {

/**
 * @brief Runs one analysis tool a stage offers (StageToolKind::Analysis)
 * and applies its result
 *
 * The stage computes the result on a graph built from a copy of the
 * project (StageAnalysisRun); applying it writes the report's parameter
 * changes over the node's stored parameters. orc-cli's tools run and a
 * project refresh both use it.
 */
class AnalysisToolPresenter {
 public:
  /**
   * @param tool_id The tool's id, as the stage lists it (e.g.
   *        "field_mapping")
   * @param project_handle Opaque handle to the project the tool works on
   * @throws std::invalid_argument if project_handle is null
   */
  AnalysisToolPresenter(std::string tool_id, void* project_handle);
  ~AnalysisToolPresenter();

  AnalysisToolPresenter(const AnalysisToolPresenter&) = delete;
  AnalysisToolPresenter& operator=(const AnalysisToolPresenter&) = delete;

  /**
   * @brief Run the tool against a node
   * @param node_id Node the tool analyses
   * @param parameters Tool parameters, keyed by descriptor name, defaults
   *        filled in
   * @param progress_callback Receives (percentage, status) updates
   * @param cancel_check Polled by the tool; returning true cancels the run
   * @return The tool's result; parameterChanges holds what Apply would write
   */
  orc::AnalysisResult runAnalysis(
      NodeID node_id,
      const std::map<std::string, orc::ParameterValue>& parameters,
      std::function<void(int, const std::string&)> progress_callback,
      std::function<bool()> cancel_check);

  /**
   * @brief Work out what applying a result would write, without writing it
   * @param result Analysis result from runAnalysis()
   * @param node_id Node the result would be applied to
   * @param changes Receives the parameter values Apply would set (may be null)
   * @return false if the run did not succeed or holds nothing to apply
   *
   * applyResultToGraph() writes exactly these values, merged into the node's
   * stored parameters, so a dry run can report them first.
   */
  bool prepareResultChanges(
      const orc::AnalysisResult& result, orc::NodeID node_id,
      std::map<std::string, orc::ParameterValue>* changes) const;

  /**
   * @brief Apply a result's parameter changes to the node
   * @return false if there is nothing to apply or the node is missing
   */
  bool applyResultToGraph(const orc::AnalysisResult& result,
                          orc::NodeID node_id);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orc::presenters

#endif  // ORC_PRESENTERS_ANALYSIS_TOOL_PRESENTER_H
