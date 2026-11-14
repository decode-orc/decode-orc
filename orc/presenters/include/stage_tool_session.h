/*
 * File:        stage_tool_session.h
 * Module:      orc-presenters
 * Purpose:     The host's IStageToolSession: a plugin view's requests to its
 *              stage, carried by a transport the GUI supplies
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
 * @brief How a session reaches the stage
 *
 * orc-gui implements it on its render coordinator, so requests run on the
 * render worker beside previews and triggers. send() is called on the owner
 * (GUI) thread; @p on_done must be called exactly once, on the owner thread.
 * Replies may come back in any order: the session puts them in order.
 */
class IStageToolTransport {
 public:
  virtual ~IStageToolTransport() = default;

  virtual void send(NodeID node, const std::string& contract_id,
                    const std::string& verb, std::vector<uint8_t> payload,
                    std::function<void(orc::view::ToolReply)> on_done) = 0;
};

/**
 * @brief The session a view is opened with
 *
 * Owned jointly by the host (to close it and to report re-triggers) and by
 * the view. Every member runs on the owner thread. See
 * orc::view::IStageToolSession for the ordering and shutdown promises; this
 * class keeps them whatever order the transport answers in: each reply is
 * held until every earlier request has been answered, and close() drops
 * everything still waiting.
 */
class StageToolSession final : public orc::view::IStageToolSession {
 public:
  StageToolSession(std::shared_ptr<IStageToolTransport> transport, NodeID node,
                   std::string contract_id, std::string title);
  ~StageToolSession() override;

  StageToolSession(const StageToolSession&) = delete;
  StageToolSession& operator=(const StageToolSession&) = delete;

  std::string contract_id() const override { return contract_id_; }
  std::string title() const override { return title_; }
  bool is_open() const override;
  void request(const std::string& verb, std::vector<uint8_t> payload,
               orc::view::ToolReplyHandler on_reply) override;
  void set_results_changed_handler(std::function<void()> handler) override;

  NodeID node() const { return node_; }

  /// End the session: no further request is sent and no pending handler
  /// runs. Idempotent.
  void close();

  /// The node finished being triggered again: run the view's handler.
  void notifyResultsChanged();

 private:
  struct State;

  std::shared_ptr<IStageToolTransport> transport_;
  NodeID node_;
  std::string contract_id_;
  std::string title_;
  std::shared_ptr<State> state_;
};

}  // namespace orc::presenters
