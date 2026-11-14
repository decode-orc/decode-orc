/*
 * File:        stage_tool_session.h
 * Module:      decode-orc Plugin SDK (view tier)
 * Purpose:     The view's only connection to the host: requests to its stage,
 *              answered on the GUI thread
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * STABILITY: PUBLIC — This header is part of the stable plugin SDK.
 */

#pragma once

// SDK TIER: view — a layout change here bumps kViewPluginAbiVersion.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QWidget;

namespace orc::view {

/// What the stage answered. @ref ok is false when the stage refused the
/// request, does not answer tool requests, or the node no longer exists;
/// @ref error then says which.
struct ToolReply {
  bool ok = false;
  std::vector<uint8_t> data;
  std::string error;
};

using ToolReplyHandler = std::function<void(ToolReply)>;

/**
 * @brief A view's conversation with the stage of the node it was opened on
 *
 * The verbs and payloads are the plugin's own: the host passes them to the
 * stage's IStageToolRequestHandler untouched.
 *
 * Threading: call every member on the GUI thread. request() returns at once;
 * the stage handles the request on the host's render worker, which also
 * renders previews and triggers stages, so a request never runs while the
 * stage is being triggered.
 *
 * Ordering: each request gets exactly one reply, and replies arrive on the
 * GUI thread in the order the requests were made.
 *
 * Shutdown: the session closes when the view's widget is destroyed, when the
 * node is removed, or when the project closes. From then on is_open() is
 * false, request() refuses, and replies still in flight are dropped without
 * their handlers being called. No handler runs after the widget is gone.
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
};

/// What a view factory is given when its tool is opened.
struct ViewOpenRequest {
  std::shared_ptr<IStageToolSession> session;
  /// Parent for the returned widget (the main window).
  QWidget* parent = nullptr;
};

}  // namespace orc::view
