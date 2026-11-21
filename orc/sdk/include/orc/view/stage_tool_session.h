/*
 * File:        stage_tool_session.h
 * Module:      decode-orc Plugin SDK (view tier)
 * Purpose:     The view's only connection to the host: requests and jobs for
 *              its stage, parameter write-back and the previewer link, all
 *              answered on the GUI thread
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * STABILITY: PUBLIC — This header is part of the stable plugin SDK.
 */

#pragma once

// SDK TIER: view — a layout change here bumps kViewPluginAbiVersion.

#include <orc/view/signal_units.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class QWidget;

namespace orc::view {

/// What the stage (or the host) answered. @ref ok is false when the stage
/// refused the request, does not answer tool requests, the node no longer
/// exists or a job was cancelled; @ref error then says which.
struct ToolReply {
  bool ok = false;
  std::vector<uint8_t> data;
  std::string error;
};

using ToolReplyHandler = std::function<void(ToolReply)>;

/// How far a job has got, as its stage reported it.
struct ToolJobProgress {
  /// 0.0 to 1.0, or negative when the stage cannot say.
  double fraction = -1.0;
  /// A status line for the user, e.g. "Mapping frame 1200 of 54000".
  std::string text;
};

using ToolProgressHandler = std::function<void(ToolJobProgress)>;

/**
 * @brief A job started with IStageToolSession::run_job()
 *
 * Call on the GUI thread. Dropping the handle does not stop the job; closing
 * the session does.
 */
class IToolJob {
 public:
  virtual ~IToolJob() = default;

  /// Ask the job to stop. The stage sees it the next time it checks; the
  /// reply still arrives, normally ok == false with error "Cancelled".
  /// Does nothing once the job has replied.
  virtual void cancel() = 0;

  /// True from run_job() until the reply has been delivered (or the session
  /// closed).
  virtual bool is_running() const = 0;
};

/// Parameter values for IStageToolSession::apply_parameters(), by parameter
/// name, written as they are on the orc-cli command line ("true", "12",
/// "0.5", "PAL"). The host converts each to the type the stage declares.
using ToolParameterMap = std::map<std::string, std::string>;

/// What IStageToolSession::render_frame() renders.
struct ToolFrameRenderOptions {
  /// Render the node's first input rather than the node itself. A tool that
  /// edits what its stage does to the picture wants the picture before it.
  bool from_input = true;
};

/// A dropout the rendered node reports on a frame, in image coordinates.
/// The image rows are the frame's lines, so @ref line is also the frame-flat
/// 0-based line.
struct ToolFrameDropout {
  uint32_t line = 0;
  uint32_t start_sample = 0;
  uint32_t end_sample = 0;
};

/// A frame rendered by the host's preview renderer for a view.
struct ToolFrameImage {
  bool ok = false;
  std::string error;
  /// The frame that was rendered (0-based).
  uint64_t frame_index = 0;
  /// Frames the rendered node has, so a view can size its navigation.
  uint64_t frame_count = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  /// 8-bit RGB, three bytes per pixel, rows top to bottom, no padding. The
  /// rows are the frame's lines in order, both fields interleaved.
  std::vector<uint8_t> rgb;
  /// Horizontal scale that shows the image at its display aspect ratio.
  double dar_aspect_correction = 1.0;
  /// The dropouts the rendered node reports on this frame. They are not drawn
  /// into @ref rgb: a view that shows them draws its own.
  std::vector<ToolFrameDropout> dropouts;
};

using ToolFrameHandler = std::function<void(ToolFrameImage)>;

/// One parameter of an analysis tool, as its window offers it. Values are
/// text, as on the orc-cli command line.
struct ToolAnalysisParameter {
  enum class Type { Bool, Integer, Real, Text };

  std::string name;
  std::string display_name;
  std::string description;
  Type type = Type::Text;
  /// Empty when the tool gives none.
  std::string default_value;
  /// Integer and Real: the range, empty when open at that end.
  std::string min_value;
  std::string max_value;
  /// Text: when not empty, the value is one of these.
  std::vector<std::string> allowed_values;
  /// When not empty, the parameter means something only while the parameter
  /// of this name has one of depends_on_values; the window greys it out
  /// otherwise.
  std::string depends_on;
  std::vector<std::string> depends_on_values;
};

/// The session's analysis tool, as its window introduces it.
struct ToolAnalysisInfo {
  /// What the tool does, for the user.
  std::string description;
  /// The parameters it takes, in the order to show them.
  std::vector<ToolAnalysisParameter> parameters;
  /// The project's video system: "PAL", "NTSC" or "PAL-M" (so a window can
  /// count a CLV time code's pictures, say); empty when unknown.
  std::string video_system;
};

/// What one run of the session's analysis tool found.
struct ToolAnalysisReport {
  enum class Status { Success, Failed, Cancelled };

  Status status = Status::Failed;
  /// What was found, or why the run failed; plain text.
  std::string summary;
  /// What applying the result writes to the node, as apply_parameters()
  /// takes it. Empty when the run found nothing to write.
  ToolParameterMap parameter_changes;
};

using ToolAnalysisHandler = std::function<void(ToolAnalysisReport)>;

/// Whether `orc-cli tools refresh` runs the session's analysis tool for the
/// node (the node's refresh tool, stored with the project).
struct ToolRefreshState {
  /// False when the tool cannot refresh the node; offer no option then.
  bool available = false;
  /// The node's refresh tool is this one, with @c parameters.
  bool this_tool = false;
  ToolParameterMap parameters;
  /// The node's refresh tool is another of its tools: its name. Empty when
  /// it is this one or there is none.
  std::string other_tool;
};

/**
 * @brief A view's conversation with the stage of the node it was opened on
 *
 * The verbs and payloads are the plugin's own: the host passes them to the
 * stage's IStageToolRequestHandler or IStageToolJobHandler untouched.
 *
 * Threading: call every member on the GUI thread. Nothing blocks: each call
 * returns at once, and every handler runs later on the GUI thread, never
 * inside the call that passed it.
 *
 * - request() is answered on the host's render worker, which also renders
 *   previews and triggers stages, so a request never runs while the stage is
 *   being triggered. Keep requests quick.
 * - run_job() is for work that takes long enough to need progress and a
 *   Cancel button. It runs on a job worker of its own, so previews and
 *   requests carry on meanwhile. One job per session at a time.
 *
 * Ordering: each request, job, apply and render gets exactly one reply.
 * Replies to request() arrive in the order the requests were made, and so do
 * replies to render_frame(). A job's progress always arrives before its
 * reply. There is no ordering between the different kinds.
 *
 * Shutdown: the session closes when the view's widget is destroyed, when the
 * node is removed, or when the project closes. Closing cancels a running
 * job. From then on is_open() is false, every call is refused, and replies
 * still in flight are dropped without their handlers being called. No
 * handler runs after the widget is gone.
 *
 * Graph rebuilds: any parameter or connection edit rebuilds the processing
 * graph, which replaces every stage object and with it the results of the
 * last trigger. A view that has not set a graph-rebuilt handler is closed by
 * the host when that happens, since what it shows came from objects that no
 * longer exist. A view that sets one stays open and is told instead.
 */
class IStageToolSession {
 public:
  virtual ~IStageToolSession() = default;

  /// The contract id of the tool this session was opened for.
  virtual std::string contract_id() const = 0;

  /// A window title naming the tool and the node, e.g. "Teletext Pages -
  /// Teletext Sink (node_4)".
  virtual std::string title() const = 0;

  virtual bool is_open() const = 0;

  /// Ask the stage something. @p on_reply runs later on the GUI thread, or
  /// never if the session closes first. On a closed session the request is
  /// dropped and @p on_reply is not called.
  virtual void request(const std::string& verb, std::vector<uint8_t> payload,
                       ToolReplyHandler on_reply) = 0;

  /// Run @p handler on the GUI thread each time the node finishes being
  /// triggered again, so the view can ask for fresh results. Replaces any
  /// earlier handler; an empty function removes it.
  virtual void set_results_changed_handler(std::function<void()> handler) = 0;

  /**
   * @brief Start a long job in the stage
   *
   * The stage's IStageToolJobHandler::run_tool_job() runs on a job worker
   * with the node's inputs. @p on_progress runs each time the stage reports
   * progress (the host may skip reports that come faster than a view can
   * show them); @p on_reply runs once at the end.
   *
   * @return The job, or nullptr (and no handler is ever called) when the
   *         session is closed or another of its jobs is still running
   */
  virtual std::shared_ptr<IToolJob> run_job(const std::string& verb,
                                            std::vector<uint8_t> payload,
                                            ToolProgressHandler on_progress,
                                            ToolReplyHandler on_reply) = 0;

  /**
   * @brief Write parameter values to the node
   *
   * The values are merged into the node's stored parameters: those not named
   * keep their values. The host checks each name and value against the
   * parameters the stage declares and applies all of them or none. This is
   * the same edit the user makes in the node's parameter editor: the project
   * is marked modified and the graph is rebuilt, so the stage's results are
   * gone and a view without a graph-rebuilt handler closes.
   *
   * @param label Describes the edit to the user, e.g. "Apply disc map".
   * @param on_done ok == false with the reason when nothing was written. Not
   *        called if the rebuild closes the view.
   */
  virtual void apply_parameters(ToolParameterMap values,
                                const std::string& label,
                                ToolReplyHandler on_done) = 0;

  /// Move the main preview to @p frame_index (0-based), showing the preview
  /// if it is hidden. The preview stays on the node it shows; when it shows
  /// none, it opens on the session's node. Frames past the end go to the
  /// last frame.
  virtual void show_frame(uint64_t frame_index) = 0;

  /// Run @p handler with the 0-based frame the main preview shows, each time
  /// it moves to another frame, so a view can follow it. Replaces any
  /// earlier handler; an empty function removes it.
  virtual void set_preview_frame_handler(
      std::function<void(uint64_t frame_index)> handler) = 0;

  /// Render one frame (0-based) for the view to draw. @p on_image runs later
  /// on the GUI thread; check ToolFrameImage::ok.
  virtual void render_frame(uint64_t frame_index,
                            const ToolFrameRenderOptions& options,
                            ToolFrameHandler on_image) = 0;

  /// Keep the view open across graph rebuilds and run @p handler after each
  /// one. The stage's results are gone by then: ask again after the next
  /// trigger (set_results_changed_handler). An empty function removes it,
  /// and the next rebuild closes the view.
  virtual void set_graph_rebuilt_handler(std::function<void()> handler) = 0;

  /// The unit the project shows signal amplitudes in (Project Properties):
  /// IRE, millivolts or 10-bit samples. A view showing levels converts its
  /// stage's 10-bit values to this unit.
  virtual AmplitudeUnit amplitude_unit() const = 0;

  /// Run @p handler with the new unit each time the project's amplitude
  /// unit changes. Replaces any earlier handler; an empty function removes
  /// it.
  virtual void set_amplitude_unit_handler(
      std::function<void(AmplitudeUnit unit)> handler) = 0;

  /// The node's stored parameters, by name, as text in the form
  /// apply_parameters() takes ("true", "12", "0.5", "PAL"). A view that edits
  /// some of them starts from these. File paths are as the project stores
  /// them: a relative one is relative to project_directory(). Parameters the
  /// host sets itself (input_node_ids) are left out. Empty on a closed
  /// session.
  virtual ToolParameterMap parameters() const = 0;

  /// The absolute folder of the project file, which relative file paths in
  /// the node's parameters are resolved against, so a view that lets the
  /// user pick a file can store it relative to the project. Empty when the
  /// project has not been saved yet.
  virtual std::string project_directory() const = 0;

  // ---- the session's analysis tool (StageToolKind::Analysis) ----
  //
  // A session opened for any other kind of tool has no analysis: its info
  // is empty, run_analysis() reports Failed and the refresh option is
  // unavailable.

  /// What the tool does and the parameters it takes.
  virtual ToolAnalysisInfo analysis_info() const = 0;

  /**
   * @brief Run the tool on the node's inputs, on a job worker
   *
   * @param parameters Values by name, as text; those left out take their
   *        defaults. A value the tool's declaration refuses fails the run.
   * @param on_progress As for run_job()
   * @param on_report Called once, on the GUI thread and never inside this
   *        call: the report (Cancelled after cancel()). Not called once the
   *        session is closed.
   * @return The running analysis, which counts as the session's one job;
   *         null when the session is closed or a job is running
   */
  virtual std::shared_ptr<IToolJob> run_analysis(
      ToolParameterMap parameters, ToolProgressHandler on_progress,
      ToolAnalysisHandler on_report) = 0;

  /// Whether the tool is the node's refresh tool, as stored now.
  virtual ToolRefreshState refresh_state() const = 0;

  /**
   * @brief Make the tool the node's refresh tool, or stop it being one
   *
   * @param use_this_tool True stores this tool with @p parameters (in
   *        place of any other); false clears it if it is this tool's, and
   *        leaves another tool's alone
   * @param on_done ok == false with the reason when it was not stored;
   *        called on the GUI thread, never inside this call
   */
  virtual void set_refresh(bool use_this_tool, ToolParameterMap parameters,
                           ToolReplyHandler on_done) = 0;
};

/// What a view factory is given when its tool is opened.
struct ViewOpenRequest {
  std::shared_ptr<IStageToolSession> session;
  /// Parent for the returned widget (the main window).
  QWidget* parent = nullptr;
};

}  // namespace orc::view
