/*
 * File:        stage_tool_session.cpp
 * Module:      orc-presenters
 * Purpose:     The host's IStageToolSession
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../include/stage_tool_session.h"

#include <utility>

namespace orc::presenters {

// Shared with the transport's completion callbacks through a weak_ptr, so a
// reply that arrives after the session is gone finds nothing to call.
struct StageToolSession::State {
  bool open = true;
  uint64_t next_sequence = 0;
  /// The sequence number whose reply the view sees next.
  uint64_t next_delivery = 0;
  /// Handlers of requests not yet delivered, by sequence number.
  std::map<uint64_t, orc::view::ToolReplyHandler> handlers;
  /// Replies that arrived before an earlier one, by sequence number.
  std::map<uint64_t, orc::view::ToolReply> held;
  std::function<void()> results_changed;
};

StageToolSession::StageToolSession(
    std::shared_ptr<IStageToolTransport> transport, NodeID node,
    std::string contract_id, std::string title)
    : transport_(std::move(transport)),
      node_(node),
      contract_id_(std::move(contract_id)),
      title_(std::move(title)),
      state_(std::make_shared<State>()) {}

StageToolSession::~StageToolSession() { close(); }

bool StageToolSession::is_open() const { return state_->open; }

void StageToolSession::close() {
  state_->open = false;
  state_->handlers.clear();
  state_->held.clear();
  state_->results_changed = nullptr;
}

void StageToolSession::set_results_changed_handler(
    std::function<void()> handler) {
  if (state_->open) {
    state_->results_changed = std::move(handler);
  }
}

void StageToolSession::notifyResultsChanged() {
  if (!state_->open || !state_->results_changed) {
    return;
  }
  // Copied first: the handler may replace itself or close the session.
  auto handler = state_->results_changed;
  handler();
}

void StageToolSession::request(const std::string& verb,
                               std::vector<uint8_t> payload,
                               orc::view::ToolReplyHandler on_reply) {
  if (!state_->open || !transport_) {
    return;
  }
  const uint64_t sequence = state_->next_sequence++;
  state_->handlers[sequence] = std::move(on_reply);

  std::weak_ptr<State> weak = state_;
  transport_->send(node_, contract_id_, verb, std::move(payload),
                   [weak, sequence](orc::view::ToolReply reply) {
                     auto state = weak.lock();
                     if (!state || !state->open) {
                       return;
                     }
                     state->held[sequence] = std::move(reply);
                     // Deliver every reply that is now next in line. A handler
                     // may close the session, so check before each one.
                     while (state->open) {
                       auto ready = state->held.find(state->next_delivery);
                       if (ready == state->held.end()) {
                         break;
                       }
                       orc::view::ToolReply next = std::move(ready->second);
                       state->held.erase(ready);
                       auto handler_it =
                           state->handlers.find(state->next_delivery);
                       orc::view::ToolReplyHandler handler;
                       if (handler_it != state->handlers.end()) {
                         handler = std::move(handler_it->second);
                         state->handlers.erase(handler_it);
                       }
                       ++state->next_delivery;
                       if (handler) {
                         handler(std::move(next));
                       }
                     }
                   });
}

}  // namespace orc::presenters
