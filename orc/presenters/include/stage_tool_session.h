/*
 * File:        stage_tool_session.h
 * Module:      orc-presenters
 * Purpose:     The host's IStageToolSession: a plugin view's requests, jobs,
 *              parameter write-back and previewer link, carried by a
 *              transport the GUI supplies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc/view/stage_tool_session.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace orc::presenters {

/**
 * @brief How a session reaches the stage and the rest of the host
 *
 * orc-gui implements it: requests on its render coordinator (beside previews
 * and triggers), jobs on job workers of their own, parameter writes through
 * the project presenter, frames through a renderer of its own. Every member
 * is called on the owner (GUI) thread, and every completion callback must be
 * called on the owner thread, later (never inside the call that passed it).
 * Replies may come back in any order: the session puts them in order.
 */
class IStageToolTransport {
 public:
  virtual ~IStageToolTransport() = default;

  /// Ask the stage; @p on_done exactly once.
  virtual void send(NodeID node, const std::string& contract_id,
                    const std::string& verb, std::vector<uint8_t> payload,
                    std::function<void(orc::view::ToolReply)> on_done) = 0;

  /// Start a job; @p on_progress any number of times, then @p on_done
  /// exactly once. @return An id for cancel_job().
  virtual uint64_t start_job(
      NodeID node, const std::string& contract_id, const std::string& verb,
      std::vector<uint8_t> payload,
      std::function<void(orc::view::ToolJobProgress)> on_progress,
      std::function<void(orc::view::ToolReply)> on_done) = 0;

  /// Ask a job to stop. Its on_done is still called. Unknown ids are ignored.
  virtual void cancel_job(uint64_t job_id) = 0;

  /// Write parameter values to the node; @p on_done exactly once, unless the
  /// rebuild that follows closes the session (it is then dropped).
  virtual void apply_parameters(
      NodeID node, orc::view::ToolParameterMap values, const std::string& label,
      std::function<void(orc::view::ToolReply)> on_done) = 0;

  /// Move the main preview to a frame of the node.
  virtual void show_frame(NodeID node, uint64_t frame_index) = 0;

  /// Render a frame; @p on_done exactly once.
  virtual void render_frame(
      NodeID node, uint64_t frame_index,
      const orc::view::ToolFrameRenderOptions& options,
      std::function<void(orc::view::ToolFrameImage)> on_done) = 0;
  /// The node's stored parameters as text, without input_node_ids. Answered
  /// at once.
  virtual orc::view::ToolParameterMap parameters(NodeID node) = 0;
  /// The folder of the project file; empty when it has not been saved.
  virtual std::string project_directory() = 0;

  /// Run the analysis tool with @p contract_id on @p node, as start_job()
  /// runs a job; cancel_job() cancels it. @p on_done is called once.
  virtual uint64_t start_analysis(
      NodeID node, const std::string& contract_id,
      orc::view::ToolParameterMap parameters,
      std::function<void(orc::view::ToolJobProgress)> on_progress,
      std::function<void(orc::view::ToolAnalysisReport)> on_done) = 0;

  /// What that analysis tool does and takes (answered at once).
  virtual orc::view::ToolAnalysisInfo analysis_info(
      NodeID node, const std::string& contract_id) = 0;

  /// Whether that tool is the node's refresh tool (answered at once).
  virtual orc::view::ToolRefreshState refresh_state(
      NodeID node, const std::string& contract_id) = 0;

  /// Store or clear the node's refresh tool; @p on_done is called later,
  /// never inside this call.
  virtual void set_refresh(
      NodeID node, const std::string& contract_id, bool use_this_tool,
      orc::view::ToolParameterMap parameters,
      std::function<void(orc::view::ToolReply)> on_done) = 0;
};

/**
 * @brief The session a view is opened with
 *
 * Owned jointly by the host (to close it and to pass on host events) and by
 * the view. Every member runs on the owner thread. See
 * orc::view::IStageToolSession for the ordering and shutdown promises; this
 * class keeps them whatever order the transport answers in: each reply is
 * held until every earlier one of its kind has been answered, and close()
 * cancels the running job and drops everything still waiting.
 */
class StageToolSession final : public orc::view::IStageToolSession {
 public:
  /// @p amplitude_unit is the project's unit when the session opens; the
  /// host passes on later changes with notifyAmplitudeUnit().
  StageToolSession(
      std::shared_ptr<IStageToolTransport> transport, NodeID node,
      std::string contract_id, std::string title,
      orc::view::AmplitudeUnit amplitude_unit = orc::view::AmplitudeUnit::IRE);
  ~StageToolSession() override;

  StageToolSession(const StageToolSession&) = delete;
  StageToolSession& operator=(const StageToolSession&) = delete;

  std::string contract_id() const override { return contract_id_; }
  std::string title() const override { return title_; }
  bool is_open() const override;
  void request(const std::string& verb, std::vector<uint8_t> payload,
               orc::view::ToolReplyHandler on_reply) override;
  void set_results_changed_handler(std::function<void()> handler) override;
  std::shared_ptr<orc::view::IToolJob> run_job(
      const std::string& verb, std::vector<uint8_t> payload,
      orc::view::ToolProgressHandler on_progress,
      orc::view::ToolReplyHandler on_reply) override;
  void apply_parameters(orc::view::ToolParameterMap values,
                        const std::string& label,
                        orc::view::ToolReplyHandler on_done) override;
  void show_frame(uint64_t frame_index) override;
  void set_preview_frame_handler(
      std::function<void(uint64_t frame_index)> handler) override;
  void render_frame(uint64_t frame_index,
                    const orc::view::ToolFrameRenderOptions& options,
                    orc::view::ToolFrameHandler on_image) override;
  void set_graph_rebuilt_handler(std::function<void()> handler) override;
  orc::view::AmplitudeUnit amplitude_unit() const override;
  void set_amplitude_unit_handler(
      std::function<void(orc::view::AmplitudeUnit)> handler) override;
  orc::view::ToolParameterMap parameters() const override;
  std::string project_directory() const override;
  orc::view::ToolAnalysisInfo analysis_info() const override;
  std::shared_ptr<orc::view::IToolJob> run_analysis(
      orc::view::ToolParameterMap parameters,
      orc::view::ToolProgressHandler on_progress,
      orc::view::ToolAnalysisHandler on_report) override;
  orc::view::ToolRefreshState refresh_state() const override;
  void set_refresh(bool use_this_tool, orc::view::ToolParameterMap parameters,
                   orc::view::ToolReplyHandler on_done) override;

  NodeID node() const { return node_; }

  /// End the session: the running job is cancelled, no further call reaches
  /// the transport and no pending handler runs. Idempotent.
  void close();

  /// The node finished being triggered again: run the view's handler.
  void notifyResultsChanged();

  /// The main preview moved to @p frame_index: run the view's handler.
  void notifyPreviewFrame(uint64_t frame_index);

  /// True when the view asked to stay open across graph rebuilds.
  bool keepsOpenAcrossRebuild() const;

  /// The graph was rebuilt and the view stays open: run its handler.
  void notifyGraphRebuilt();

  /// The project's amplitude unit changed: remember it and, when it is a
  /// change, run the view's handler.
  void notifyAmplitudeUnit(orc::view::AmplitudeUnit unit);

 private:
  struct State;
  class Job;

  /// Take the session's one job slot; null when it cannot.
  std::shared_ptr<orc::view::IToolJob> startJob(
      orc::view::ToolProgressHandler on_progress,
      orc::view::ToolReplyHandler on_reply,
      orc::view::ToolAnalysisHandler on_report,
      const std::function<
          uint64_t(std::function<void(orc::view::ToolJobProgress)>,
                   std::function<void(orc::view::ToolReply)>,
                   std::function<void(orc::view::ToolAnalysisReport)>)>& start);

  NodeID node_;
  std::string contract_id_;
  std::string title_;
  std::shared_ptr<State> state_;
};

}  // namespace orc::presenters
