/*
 * File:        plugin_view_host.h
 * Module:      orc-gui
 * Purpose:     Opens plugins' view windows and carries their tool sessions:
 *              requests, jobs, parameter write-back and the previewer link
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/common_types.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "presenters/include/stage_tool_session.h"

class QThread;
class QWidget;
class RenderCoordinator;
class IToolFrameRenderer;

/**
 * @brief The GUI end of plugin views (plugin-architecture.md, "View Plugins")
 *
 * Opens the window a plugin's view binary registered for a tool and connects
 * it to the node's stage through a StageToolSession:
 *
 * - requests run on the render coordinator, beside previews and triggers;
 * - jobs each run on a job worker thread of their own;
 * - parameter writes, preview moves and the choice of what to render go to
 *   the main window through @ref Actions;
 * - frames are rendered on one render thread the host owns, by a renderer
 *   made for the current graph.
 *
 * A window holds stage results, so like the other result viewers it is
 * closed whenever the graph is rebuilt, unless its view asked to stay open
 * across rebuilds; every window closes when the project closes.
 *
 * Thread safety: GUI thread only. Destruction cancels running jobs and waits
 * for their threads.
 */
class PluginViewHost : public QObject {
  Q_OBJECT

 public:
  /// A prepared job: runs on a job worker, reporting progress and polling
  /// for cancel, and returns the stage's reply.
  using JobRunner = std::function<orc::view::ToolReply(
      const std::function<void(orc::view::ToolJobProgress)>& on_progress,
      const std::function<bool()>& is_cancelled)>;
  using AnalysisRunner = std::function<orc::view::ToolAnalysisReport(
      const std::function<void(orc::view::ToolJobProgress)>& on_progress,
      const std::function<bool()>& is_cancelled)>;

  /// What the host needs from the main window. Each is called on the GUI
  /// thread; any may be left empty, and the matching session call then
  /// fails (or does nothing).
  struct Actions {
    /// Write the values to the node and rebuild the graph. Return ok, or
    /// the reason nothing was written.
    std::function<orc::view::ToolReply(const orc::NodeID& node,
                                       const orc::view::ToolParameterMap&,
                                       const std::string& label)>
        apply_parameters;
    /// Move the main preview to a frame.
    std::function<void(const orc::NodeID& node, uint64_t frame_index)>
        show_frame;
    /// Prepare a job from the project as it is now.
    std::function<JobRunner(
        const orc::NodeID& node, const std::string& contract_id,
        const std::string& verb, std::vector<uint8_t> payload)>
        prepare_job;
    /// The node a render asked of @p node draws: the node itself or its
    /// first input. Empty when there is none.
    std::function<std::optional<orc::NodeID>(const orc::NodeID& node,
                                             bool from_input)>
        render_target;
    /// Make a frame renderer for the graph as it is now.
    std::function<std::shared_ptr<IToolFrameRenderer>()> make_frame_renderer;
    /// The project's amplitude unit, given to each view as it opens.
    std::function<orc::AmplitudeDisplayUnit()> amplitude_unit;
    /// The node's stored parameters as text, without input_node_ids.
    std::function<orc::view::ToolParameterMap(const orc::NodeID& node)>
        parameters;
    /// The folder of the project file; empty when it has not been saved.
    std::function<std::string()> project_directory;
    /// Prepare a run of the node's analysis tool with @p contract_id, on
    /// the GUI thread (the runner is called on a worker); null with the
    /// reason in @p why when it cannot run.
    std::function<AnalysisRunner(
        const orc::NodeID& node, const std::string& contract_id,
        const orc::view::ToolParameterMap& parameters, std::string* why)>
        prepare_analysis;
    std::function<orc::view::ToolAnalysisInfo(const orc::NodeID& node,
                                              const std::string& contract_id)>
        analysis_info;
    std::function<orc::view::ToolRefreshState(const orc::NodeID& node,
                                              const std::string& contract_id)>
        refresh_state;
    std::function<orc::view::ToolReply(
        const orc::NodeID& node, const std::string& contract_id,
        bool use_this_tool, const orc::view::ToolParameterMap& parameters)>
        set_refresh;
  };

  PluginViewHost(RenderCoordinator* coordinator, QWidget* window_parent,
                 Actions actions = {});
  ~PluginViewHost() override;

  /**
   * @brief Open (or raise) the view for a PluginView tool on a node
   *
   * A window already open for the same node and tool is raised and told its
   * results changed, since a trigger is what reopens it.
   *
   * @param analysis The tool is an analysis tool: with no loaded view for
   *        its contract, the host's AnalysisToolWindow opens instead
   * @return false with @p error set when no loaded view serves the contract
   *         or the view refused to open
   */
  bool open(const orc::NodeID& node, const std::string& contract_id,
            const QString& title, QString* error, bool analysis = false);

  /// The graph was rebuilt: close the views that did not ask to stay open,
  /// tell the others, and drop the frame renderer (it renders the old
  /// graph).
  void onGraphRebuilt();

  /// The main preview moved to @p frame_index (0-based): tell every view
  /// following it. Repeats of the same frame are not passed on. A view
  /// opened later is told the last frame once it is open.
  void notifyPreviewFrame(uint64_t frame_index);

  /// The project's amplitude unit changed: tell every view.
  void notifyAmplitudeUnit(orc::AmplitudeDisplayUnit unit);

  /// Close every open view and its session.
  void closeAll();

  /// Number of jobs whose threads are still running (tests).
  size_t runningJobCount() const;

  /// The transport every session of this host uses (tests drive it
  /// directly, without a view binary).
  std::shared_ptr<orc::presenters::IStageToolTransport> transport() const;

 private:
  class Transport;

  struct OpenView {
    orc::NodeID node;
    std::string contract_id;
    std::shared_ptr<orc::presenters::StageToolSession> session;
    QPointer<QWidget> widget;
  };

  std::function<orc::AmplitudeDisplayUnit()> amplitude_unit_;
  std::shared_ptr<Transport> transport_;
  QWidget* window_parent_;
  std::vector<OpenView> open_;
  std::optional<uint64_t> last_preview_frame_;
};
