/*
 * File:        plugin_view_host.h
 * Module:      orc-gui
 * Purpose:     Opens plugins' view windows and keeps their tool sessions
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <memory>
#include <string>
#include <vector>

#include "presenters/include/stage_tool_session.h"

class QWidget;
class RenderCoordinator;

/**
 * @brief The GUI end of plugin views (plugin-architecture.md, "View Plugins")
 *
 * Opens the window a plugin's view binary registered for a tool, connects it
 * to the node's stage through a StageToolSession whose requests run on the
 * render coordinator, and closes the session when the window goes. A window
 * holds stage results, so like the other result viewers it is closed whenever
 * the DAG is rebuilt or the project closes.
 */
class PluginViewHost : public QObject {
  Q_OBJECT

 public:
  PluginViewHost(RenderCoordinator* coordinator, QWidget* window_parent);
  ~PluginViewHost() override;

  /**
   * @brief Open (or raise) the view for a PluginView tool on a node
   *
   * A window already open for the same node and tool is raised and told its
   * results changed, since a trigger is what reopens it.
   *
   * @return false with @p error set when no loaded view serves the contract
   *         or the view refused to open
   */
  bool open(const orc::NodeID& node, const std::string& contract_id,
            const QString& title, QString* error);

  /// Close every open view and its session.
  void closeAll();

  /// Number of views open (tests).
  size_t openCount() const;

 private:
  class Transport;

  struct OpenView {
    orc::NodeID node;
    std::string contract_id;
    std::shared_ptr<orc::presenters::StageToolSession> session;
    QPointer<QWidget> widget;
  };

  std::shared_ptr<Transport> transport_;
  QWidget* window_parent_;
  std::vector<OpenView> open_;
};
