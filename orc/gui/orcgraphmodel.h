/*
 * File:        orcgraphmodel.h
 * Module:      orc-gui
 * Purpose:     QtNodes AbstractGraphModel adapter for ORC projects
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc/stage/node_type.h>

#include <QJsonObject>
#include <QtNodes/AbstractGraphModel>
#include <unordered_map>

// Forward declaration
namespace orc::presenters {
class IProjectPresenter;
}

// Use orc::NodeID in this file
using orc::NodeID;

using QtNodes::ConnectionId;
using QtNodes::NodeId;
using QtNodes::NodeRole;
using QtNodes::PortIndex;
using QtNodes::PortRole;
using QtNodes::PortType;

/**
 * @brief QtNodes AbstractGraphModel adapter for ORC projects
 *
 * This adapter allows QtNodes to visualize and edit the ORC project DAG.
 * It implements the QtNodes::AbstractGraphModel interface and translates
 * between QtNodes node IDs and orc::NodeID identifiers.
 *
 * All modifications to the graph are immediately reflected in the underlying
 * orc::Project instance.
 */
class OrcGraphModel : public QtNodes::AbstractGraphModel {
  Q_OBJECT

 public:
  /**
   * @brief Construct a graph model for a project
   * @param presenter The project presenter to use
   * @param parent Parent QObject
   */
  explicit OrcGraphModel(orc::presenters::IProjectPresenter& presenter,
                         QObject* parent = nullptr);
  ~OrcGraphModel() override = default;

  /// @name QtNodes AbstractGraphModel Interface
  /// @{
  NodeId newNodeId() override;
  std::unordered_set<NodeId> allNodeIds() const override;
  std::unordered_set<ConnectionId> allConnectionIds(
      NodeId const nodeId) const override;
  std::unordered_set<ConnectionId> connections(NodeId nodeId, PortType portType,
                                               PortIndex index) const override;

  bool connectionExists(ConnectionId const connectionId) const override;
  NodeId addNode(QString const nodeType) override;
  bool connectionPossible(ConnectionId const connectionId) const override;
  void addConnection(ConnectionId const connectionId) override;
  bool nodeExists(NodeId const nodeId) const override;

  QVariant nodeData(NodeId nodeId, NodeRole role) const override;
  bool setNodeData(NodeId nodeId, NodeRole role, QVariant value) override;

  QVariant portData(NodeId nodeId, PortType portType, PortIndex portIndex,
                    PortRole role) const override;

  bool setPortData(NodeId nodeId, PortType portType, PortIndex portIndex,
                   QVariant const& value, PortRole role) override;

  bool deleteConnection(ConnectionId const connectionId) override;
  bool deleteNode(NodeId const nodeId) override;

  QJsonObject saveNode(NodeId const nodeId) const override;
  void loadNode(QJsonObject const& nodeJson) override;
  /// @}

  /**
   * @brief Refresh model from project
   *
   * Call this after external changes to the project to update the view.
   */
  void refresh();

  /**
   * @brief Convert QtNodes NodeId to ORC NodeID
   * @param qtNodeId QtNodes node identifier
   * @return Corresponding NodeID
   */
  NodeID getOrcNodeId(NodeId qtNodeId) const;

  /**
   * @brief Convert ORC NodeID to QtNodes NodeId
   * @param orc_node_id ORC node identifier
   * @return Corresponding QtNodes NodeId (or QtNodes::InvalidNodeId if not
   * found)
   */
  NodeId getQtNodeId(const NodeID& orc_node_id) const;

  /// @name Presenter Access
  /// @{
  orc::presenters::IProjectPresenter& presenter() {
    return presenter_;
  }  ///< Get presenter reference
  const orc::presenters::IProjectPresenter& presenter() const {
    return presenter_;
  }  ///< Get const presenter reference
  /// @}

  /**
   * @brief Get stage name for a node
   * @param node_id Node identifier
   * @return Stage name string
   */
  std::string getNodeStageName(const NodeID& node_id) const;

  /**
   * @brief Get the current configuration status for a node.
   *
   * Result is cached per node and invalidated whenever setNodeData() is
   * called (e.g. after the parameter dialog saves new values) or when
   * refresh() rebuilds the mapping.
   */
  orc::ConfigurationStatus getConfigurationStatus(NodeId nodeId) const;

  /**
   * @brief Why the last caption edit was refused
   *
   * setNodeData() returns false when a rename is refused (a duplicate, empty
   * or all-digit name); this holds the reason to show the user.  Empty after
   * a successful edit.
   */
  QString lastCaptionError() const { return last_caption_error_; }

 private:
  orc::presenters::IProjectPresenter& presenter_;

  // Reason the last caption edit was refused; see lastCaptionError()
  QString last_caption_error_;

  // Map between QtNodes IDs and ORC node IDs
  std::map<NodeId, NodeID> qt_to_orc_nodes_;
  std::map<NodeID, NodeId> orc_to_qt_nodes_;

  // Store all connections
  std::unordered_set<ConnectionId> connectivity_;

  // Per-node configuration status cache; populated lazily, cleared by
  // refresh() and invalidated per-node by invalidateConfigurationStatus().
  mutable std::unordered_map<NodeId, orc::ConfigurationStatus>
      config_status_cache_;

  // Helper functions
  NodeId getOrCreateQtNodeId(const NodeID& orc_node_id);

  // Drop |nodeId|'s cached status and ask the view to repaint it. A stage can
  // judge its configuration against the nodes wired to it (Source Join orders
  // its inputs by node ID), so connecting or disconnecting an edge can change
  // a status the cache would otherwise keep serving from before the change.
  void invalidateConfigurationStatus(NodeId nodeId);

  // Invalidate every node joined to |nodeId| by a connection, plus |nodeId|
  // itself.
  void invalidateConnectedConfigurationStatus(NodeId nodeId);

  void buildMappings();
};
