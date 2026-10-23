/*
 * File:        analysis_tool_presenter.h
 * Module:      orc-presenters
 * Purpose:     Base class for analysis tool presenters
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
#include <vector>

namespace orc {
// Forward declarations from core
class Project;
class DAG;
struct DAGNode;
class Artifact;
}  // namespace orc

namespace orc::presenters {

/**
 * @brief Base class for all analysis tool presenters
 *
 * This class provides common functionality needed by all analysis tool
 * presenters:
 * - DAG building and caching
 * - Node input/output detection
 * - DAG execution up to a specific node
 * - Progress reporting
 * - Result application to graph
 *
 * Each specialized presenter (e.g., FrameCorruptionPresenter) extends this base
 * class and implements tool-specific logic in its runAnalysis() method.
 *
 * **MVP Architecture:**
 * - GUI calls specialized presenter methods
 * - Presenter prepares context and data for core tool
 * - Core tool performs algorithm/analysis
 * - Presenter formats results and handles apply-to-graph
 *
 * **Example Usage:**
 * ```cpp
 * class FrameCorruptionPresenter : public AnalysisToolPresenter {
 * public:
 *     explicit FrameCorruptionPresenter(void* project_handle)
 *         : AnalysisToolPresenter(project_handle) {}
 *
 *     orc::AnalysisResult runAnalysis(
 *         NodeID node_id,
 *         const std::map<std::string, orc::ParameterValue>& parameters,
 *         std::function<void(int, const std::string&)> progress_callback,
 *         std::function<bool()> cancel_check) override;
 * };
 * ```
 */
class AnalysisToolPresenter {
 public:
  /// Progress callback type: (percentage, status_message)
  using ProgressCallback = std::function<void(int, const std::string&)>;

  /**
   * @brief Apply analysis result to graph
   * @param result Analysis result containing data to apply
   * @param node_id Node to apply result to
   * @return true if successfully applied, false otherwise
   *
   * This delegates to the core tool's applyToGraph() method.
   * Call this from GUI after successful analysis to update the stage
   * parameters.
   */
  bool applyResultToGraph(const orc::AnalysisResult& result,
                          orc::NodeID node_id);

  /**
   * @brief Work out what applying a result would write, without writing it
   * @param result Analysis result from runAnalysis()
   * @param node_id Node the result would be applied to
   * @param changes Receives the parameter values Apply would set (may be null)
   * @return false if the tool cannot apply to the graph or the result holds
   *         nothing to apply
   *
   * applyResultToGraph() writes exactly these values, merged into the node's
   * stored parameters, so a dry run can report them first.
   */
  bool prepareResultChanges(
      const orc::AnalysisResult& result, orc::NodeID node_id,
      std::map<std::string, orc::ParameterValue>* changes);

  /**
   * @brief Run the tool against a node
   * @param node_id Node the tool analyses
   * @param parameters Tool parameters, keyed by descriptor name
   * @param progress_callback Receives (percentage, status) updates
   * @param cancel_check Polled by the tool; returning true cancels the run
   * @return The tool's result; parameterChanges holds what Apply would write
   */
  virtual orc::AnalysisResult runAnalysis(
      NodeID node_id,
      const std::map<std::string, orc::ParameterValue>& parameters,
      std::function<void(int, const std::string&)> progress_callback,
      std::function<bool()> cancel_check) = 0;

  /**
   * @brief Virtual destructor
   *
   * Public so the presenter factory can hand out owning pointers to the base.
   */
  virtual ~AnalysisToolPresenter();

 protected:
  /**
   * @brief Construct base presenter
   * @param project_handle Opaque handle to project
   * @throws std::invalid_argument if project_handle is null
   */
  explicit AnalysisToolPresenter(void* project_handle);

  // Prevent copying
  AnalysisToolPresenter(const AnalysisToolPresenter&) = delete;
  AnalysisToolPresenter& operator=(const AnalysisToolPresenter&) = delete;

  // Allow moving
  AnalysisToolPresenter(AnalysisToolPresenter&&) noexcept;
  AnalysisToolPresenter& operator=(AnalysisToolPresenter&&) noexcept;

  // =========================================================================
  // Subclass Interface
  // =========================================================================

  /**
   * @brief Get the unique identifier for this tool
   * @return Tool ID (e.g., "frame_corruption")
   */
  virtual std::string toolId() const = 0;

  /**
   * @brief Get the human-readable name for this tool
   * @return Tool name (e.g., "Frame Corruption Generator")
   */
  virtual std::string toolName() const = 0;

  // =========================================================================
  // Common Utilities for Subclasses
  // =========================================================================

  /**
   * @brief Build or retrieve cached DAG from project
   * @return Opaque handle to DAG
   * @throws std::runtime_error if DAG cannot be built
   *
   * The DAG is cached on first call. Subsequent calls return the cached DAG.
   * Call invalidateDAG() if the project structure changes.
   */
  std::shared_ptr<void> getOrBuildDAG();

  /**
   * @brief Check if a node has at least one input connection
   * @param node_id Node to check
   * @return true if node has inputs, false otherwise
   */
  bool hasNodeInput(orc::NodeID node_id) const;

  /**
   * @brief Get the first input node ID for a given node
   * @param node_id Node to query
   * @return Input node ID if exists, invalid NodeID otherwise
   */
  orc::NodeID getFirstInputNodeId(orc::NodeID node_id) const;

  /**
   * @brief Execute DAG up to specified node and get its output artifacts
   * @param node_id Node to execute to
   * @return Vector of opaque handles to output artifacts
   * @throws std::runtime_error if execution fails
   *
   * This executes the DAG incrementally up to the specified node,
   * caching intermediate results. Subsequent calls with the same or
   * later nodes will reuse cached data.
   */
  std::vector<std::shared_ptr<void>> executeToNode(orc::NodeID node_id);

  /**
   * @brief Invalidate cached DAG
   *
   * Call this if the project structure changes (nodes added/removed)
   * to force DAG rebuild on next getOrBuildDAG() call.
   */
  void invalidateDAG();

 protected:
  /**
   * @brief Get project pointer for derived classes
   * @return Opaque handle to project (cast to Project* in derived .cpp files)
   *
   * This allows derived analysis tool presenters to access the project
   * when building analysis contexts.
   */
  void* getProjectPointer() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orc::presenters

#endif  // ORC_PRESENTERS_ANALYSIS_TOOL_PRESENTER_H
