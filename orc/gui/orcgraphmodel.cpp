/*
 * File:        orcgraphmodel.cpp
 * Module:      orc-gui
 * Purpose:     QtNodes AbstractGraphModel adapter for ORC projects
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "orcgraphmodel.h"

#include <QtNodes/ConnectionIdUtils>
#include <QtNodes/StyleCollection>

#include "logging.h"
#include "node_type_helper.h"
#include "presenters/include/i_project_presenter.h"

using orc::get_node_type_info;
using orc::NodeID;
using orc::NodeTypeInfo;
using QtNodes::getNodeId;
using QtNodes::getPortIndex;

OrcGraphModel::OrcGraphModel(orc::presenters::IProjectPresenter& presenter,
                             QObject* parent)
    : QtNodes::AbstractGraphModel(), presenter_(presenter) {
  setParent(parent);
  buildMappings();
}

void OrcGraphModel::buildMappings() {
  qt_to_orc_nodes_.clear();
  orc_to_qt_nodes_.clear();
  connectivity_.clear();

  // Build node mappings
  NodeId qt_id = 0;
  const auto nodes = presenter_.getNodes();

  for (const auto& node : nodes) {
    qt_to_orc_nodes_[qt_id] = node.node_id;
    orc_to_qt_nodes_[node.node_id] = qt_id;
    qt_id++;
  }

  // Build connection mappings
  const auto edges = presenter_.getEdges();

  for (const auto& edge : edges) {
    auto it_out = orc_to_qt_nodes_.find(edge.source_node);
    auto it_in = orc_to_qt_nodes_.find(edge.target_node);

    if (it_out != orc_to_qt_nodes_.end() && it_in != orc_to_qt_nodes_.end()) {
      // All nodes have single ports (index 0)
      ConnectionId conn_id{it_out->second, 0, it_in->second, 0};
      connectivity_.insert(conn_id);
      ORC_LOG_DEBUG("  Mapped connection: {} -> {}",
                    edge.source_node.to_string(), edge.target_node.to_string());
    }
  }
}

NodeId OrcGraphModel::newNodeId() {
  // Find the maximum QtNodes NodeId currently in use
  NodeId max_id = 0;
  for (const auto& pair : qt_to_orc_nodes_) {
    if (pair.first >= max_id) {
      max_id = pair.first + 1;
    }
  }
  return max_id;
}

NodeId OrcGraphModel::getOrCreateQtNodeId(const NodeID& orc_node_id) {
  auto it = orc_to_qt_nodes_.find(orc_node_id);
  if (it != orc_to_qt_nodes_.end()) {
    return it->second;
  }

  NodeId qt_id = newNodeId();
  qt_to_orc_nodes_[qt_id] = orc_node_id;
  orc_to_qt_nodes_[orc_node_id] = qt_id;
  return qt_id;
}

std::string OrcGraphModel::getNodeStageName(const NodeID& node_id) const {
  try {
    auto node_info = presenter_.getNodeInfo(node_id);
    return node_info.stage_name;
  } catch (...) {
    return "";
  }
}

void OrcGraphModel::refresh() {
  ORC_LOG_DEBUG("OrcGraphModel::refresh - Rebuilding node mappings");
  buildMappings();
  config_status_cache_.clear();
  ORC_LOG_DEBUG("OrcGraphModel::refresh - Emitting modelReset signal");
  Q_EMIT modelReset();
}

orc::ConfigurationStatus OrcGraphModel::getConfigurationStatus(
    NodeId nodeId) const {
  auto cache_it = config_status_cache_.find(nodeId);
  if (cache_it != config_status_cache_.end()) {
    return cache_it->second;
  }

  auto it = qt_to_orc_nodes_.find(nodeId);
  if (it == qt_to_orc_nodes_.end()) {
    return orc::ConfigurationStatus::Green;
  }

  orc::ConfigurationStatus status =
      presenter_.getNodeConfigurationStatus(it->second);
  config_status_cache_[nodeId] = status;
  return status;
}

void OrcGraphModel::invalidateConfigurationStatus(NodeId nodeId) {
  config_status_cache_.erase(nodeId);
  Q_EMIT nodeUpdated(nodeId);
}

void OrcGraphModel::invalidateConnectedConfigurationStatus(NodeId nodeId) {
  invalidateConfigurationStatus(nodeId);
  for (const auto& conn : connectivity_) {
    if (conn.outNodeId == nodeId) {
      invalidateConfigurationStatus(conn.inNodeId);
    } else if (conn.inNodeId == nodeId) {
      invalidateConfigurationStatus(conn.outNodeId);
    }
  }
}

std::unordered_set<NodeId> OrcGraphModel::allNodeIds() const {
  std::unordered_set<NodeId> ids;
  for (const auto& [qt_id, orc_id] : qt_to_orc_nodes_) {
    ids.insert(qt_id);
  }
  return ids;
}

std::unordered_set<ConnectionId> OrcGraphModel::allConnectionIds(
    NodeId const nodeId) const {
  std::unordered_set<ConnectionId> result;

  std::copy_if(connectivity_.begin(), connectivity_.end(),
               std::inserter(result, std::end(result)),
               [&nodeId](ConnectionId const& cid) {
                 return cid.inNodeId == nodeId || cid.outNodeId == nodeId;
               });

  return result;
}

std::unordered_set<ConnectionId> OrcGraphModel::connections(
    NodeId nodeId, PortType portType, PortIndex index) const {
  std::unordered_set<ConnectionId> result;

  std::copy_if(connectivity_.begin(), connectivity_.end(),
               std::inserter(result, std::end(result)),
               [&portType, &index, &nodeId](ConnectionId const& cid) {
                 return (getNodeId(portType, cid) == nodeId &&
                         getPortIndex(portType, cid) == index);
               });

  return result;
}

bool OrcGraphModel::connectionExists(ConnectionId const connectionId) const {
  return connectivity_.find(connectionId) != connectivity_.end();
}

NodeId OrcGraphModel::addNode(QString const nodeType) {
  // Use presenter's add_node function which generates unique IDs properly
  std::string stage_name =
      nodeType.isEmpty() ? "tbc_source" : nodeType.toStdString();

  try {
    NodeID node_id = presenter_.addNode(stage_name, 0.0, 0.0);

    NodeId qt_id = getOrCreateQtNodeId(node_id);

    Q_EMIT nodeCreated(qt_id);

    return qt_id;
  } catch (const std::exception& e) {
    // Invalid stage name or other error
    return QtNodes::InvalidNodeId;
  }
}

bool OrcGraphModel::connectionPossible(ConnectionId const connectionId) const {
  // Check if connection already exists
  if (connectionExists(connectionId)) {
    return false;
  }

  // Check if nodes exist
  if (!nodeExists(connectionId.outNodeId) ||
      !nodeExists(connectionId.inNodeId)) {
    return false;
  }

  // Don't allow self-connections
  if (connectionId.outNodeId == connectionId.inNodeId) {
    return false;
  }

  return true;
}

void OrcGraphModel::addConnection(ConnectionId const connectionId) {
  if (!connectionPossible(connectionId)) {
    return;
  }

  // Get ORC node IDs
  auto it_out = qt_to_orc_nodes_.find(connectionId.outNodeId);
  auto it_in = qt_to_orc_nodes_.find(connectionId.inNodeId);

  if (it_out == qt_to_orc_nodes_.end() || it_in == qt_to_orc_nodes_.end()) {
    return;
  }

  const NodeID& source_id = it_out->second;
  const NodeID& target_id = it_in->second;

  // Use presenter's add_edge which handles validation and modification tracking
  try {
    presenter_.addEdge(source_id, target_id);

    // Add to local connectivity
    connectivity_.insert(connectionId);

    // Both ends have a different set of neighbours now, and a stage can judge
    // its configuration by them.
    invalidateConfigurationStatus(connectionId.inNodeId);
    invalidateConfigurationStatus(connectionId.outNodeId);

    Q_EMIT connectionCreated(connectionId);
  } catch (const std::exception& e) {  // NOLINT(bugprone-empty-catch)
    // Connection validation failed (e.g., invalid connection type, exceeded
    // limits) Silently ignore - QtNodes will show the connection isn't possible
  }
}

bool OrcGraphModel::nodeExists(NodeId const nodeId) const {
  return qt_to_orc_nodes_.find(nodeId) != qt_to_orc_nodes_.end();
}

QVariant OrcGraphModel::nodeData(NodeId nodeId, NodeRole role) const {
  auto it = qt_to_orc_nodes_.find(nodeId);
  if (it == qt_to_orc_nodes_.end()) {
    return QVariant();
  }

  try {
    auto node_info = presenter_.getNodeInfo(it->second);

    switch (role) {
      case NodeRole::Type:
        return QString::fromStdString(node_info.stage_name);

      case NodeRole::Caption:
        return QString::fromStdString(node_info.label);

      case NodeRole::Position:
        return QPointF(node_info.x_position, node_info.y_position);

      case NodeRole::Size:
        return QSize(140, 80);  // Default node size (+20px total)

      case NodeRole::CaptionVisible:
        return true;

      case NodeRole::InPortCount: {
        // Return 1 port if node has inputs, 0 if it's a source node
        // The ConnectionPolicy determines if it accepts multiple connections
        NodeTypeHelper::NodeVisualInfo info =
            NodeTypeHelper::getVisualInfo(node_info.stage_name);
        return info.has_input ? 1u : 0u;
      }

      case NodeRole::OutPortCount: {
        // Return 1 port if node has outputs, 0 if it's a sink node
        // The ConnectionPolicy determines if it allows multiple connections
        NodeTypeHelper::NodeVisualInfo info =
            NodeTypeHelper::getVisualInfo(node_info.stage_name);
        return info.has_output ? 1u : 0u;
      }

      case NodeRole::Widget:
        return QVariant();

      case NodeRole::Style: {
        auto style = QtNodes::StyleCollection::nodeStyle();
        return style.toJson().toVariantMap();
      }

      default:
        return QVariant();
    }
  } catch (...) {
    return QVariant();
  }
}

bool OrcGraphModel::setNodeData(NodeId nodeId, NodeRole role, QVariant value) {
  auto it = qt_to_orc_nodes_.find(nodeId);
  if (it == qt_to_orc_nodes_.end()) {
    return false;
  }

  switch (role) {
    case NodeRole::Caption:
      try {
        presenter_.setNodeLabel(it->second, value.toString().toStdString());
        last_caption_error_.clear();
        Q_EMIT nodeUpdated(nodeId);
        return true;
      } catch (const std::exception& e) {
        last_caption_error_ = QString::fromStdString(e.what());
        return false;
      }

    case NodeRole::Position: {
      QPointF pos = value.toPointF();
      try {
        presenter_.setNodePosition(it->second, pos.x(), pos.y());
        Q_EMIT nodePositionUpdated(nodeId);
        return true;
      } catch (const std::exception&) {
        return false;
      }
    }

    default:
      return false;
  }
}

QVariant OrcGraphModel::portData(NodeId nodeId, PortType portType,
                                 PortIndex portIndex, PortRole role) const {
  auto it = qt_to_orc_nodes_.find(nodeId);
  if (it == qt_to_orc_nodes_.end()) {
    return QVariant();
  }

  try {
    auto node_info = presenter_.getNodeInfo(it->second);

    // Get node type info for port capabilities
    const NodeTypeInfo* info = get_node_type_info(node_info.stage_name);

    switch (role) {
      case PortRole::Data:
        return QVariant();

      case PortRole::DataType:
        return QString("VideoFrame");

      case PortRole::ConnectionPolicyRole: {
        // Return Many if the port can handle multiple connections
        if (!info) {
          return QVariant::fromValue(QtNodes::ConnectionPolicy::One);
        }

        if (portType == PortType::In) {
          // Input port: Many if max_inputs > 1
          return (info->max_inputs > 1)
                     ? QVariant::fromValue(QtNodes::ConnectionPolicy::Many)
                     : QVariant::fromValue(QtNodes::ConnectionPolicy::One);
        } else {
          // Output port: Many if max_outputs > 1
          return (info->max_outputs > 1)
                     ? QVariant::fromValue(QtNodes::ConnectionPolicy::Many)
                     : QVariant::fromValue(QtNodes::ConnectionPolicy::One);
        }
      }

      case PortRole::CaptionVisible:
        return false;

      case PortRole::Caption:
        return QString();

      default:
        return QVariant();
    }
  } catch (...) {
    return QVariant();
  }
}

bool OrcGraphModel::setPortData(NodeId nodeId, PortType portType,
                                PortIndex portIndex, QVariant const& value,
                                PortRole role) {
  // Ports are not directly editable in our model
  return false;
}

bool OrcGraphModel::deleteConnection(ConnectionId const connectionId) {
  if (!connectionExists(connectionId)) {
    return false;
  }

  // Get ORC node IDs
  auto it_out = qt_to_orc_nodes_.find(connectionId.outNodeId);
  auto it_in = qt_to_orc_nodes_.find(connectionId.inNodeId);

  if (it_out == qt_to_orc_nodes_.end() || it_in == qt_to_orc_nodes_.end()) {
    return false;
  }

  const NodeID& source_id = it_out->second;
  const NodeID& target_id = it_in->second;

  // Use presenter's remove_edge which handles modification tracking
  try {
    presenter_.removeEdge(source_id, target_id);

    // Remove from local connectivity
    connectivity_.erase(connectionId);

    invalidateConfigurationStatus(connectionId.inNodeId);
    invalidateConfigurationStatus(connectionId.outNodeId);

    Q_EMIT connectionDeleted(connectionId);

    return true;
  } catch (const std::exception& e) {
    ORC_LOG_WARN("Failed to delete connection: {}", e.what());
    return false;
  }
}

bool OrcGraphModel::deleteNode(NodeId const nodeId) {
  auto it = qt_to_orc_nodes_.find(nodeId);
  if (it == qt_to_orc_nodes_.end()) {
    return false;
  }

  const NodeID& orc_node_id = it->second;

  // Everything this node was wired to loses an input or an output, so their
  // cached status has to go before the connections do.
  invalidateConnectedConfigurationStatus(nodeId);

  // Remove all connections involving this node from GUI state
  std::vector<ConnectionId> to_remove;
  for (const auto& conn : connectivity_) {
    if (conn.outNodeId == nodeId || conn.inNodeId == nodeId) {
      to_remove.push_back(conn);
    }
  }
  for (const auto& conn : to_remove) {
    connectivity_.erase(conn);
    Q_EMIT connectionDeleted(conn);
  }

  // Use presenter's remove_node which handles removing edges and modification
  // tracking
  try {
    if (!presenter_.removeNode(orc_node_id)) {
      ORC_LOG_WARN("Failed to delete node '{}'", orc_node_id.to_string());
      return false;
    }

    // Update mappings
    qt_to_orc_nodes_.erase(nodeId);
    orc_to_qt_nodes_.erase(orc_node_id);

    Q_EMIT nodeDeleted(nodeId);

    return true;
  } catch (const std::exception& e) {
    // Log error - validation failed (likely has connections)
    ORC_LOG_WARN("Failed to delete node '{}': {}", orc_node_id.to_string(),
                 e.what());
    return false;
  }
}

QJsonObject OrcGraphModel::saveNode(NodeId const nodeId) const {
  QJsonObject json;

  // QtNodes expects the integer NodeId in the "id" field for undo/redo
  json["id"] = static_cast<int>(nodeId);

  // Also save ORC-specific data for completeness
  auto it = qt_to_orc_nodes_.find(nodeId);
  if (it != qt_to_orc_nodes_.end()) {
    try {
      auto node_info = presenter_.getNodeInfo(it->second);
      json["orc_node_id"] =
          QString::fromStdString(node_info.node_id.to_string());
      json["stage_name"] = QString::fromStdString(node_info.stage_name);
      json["user_label"] = QString::fromStdString(node_info.label);
      json["x"] = node_info.x_position;
      json["y"] = node_info.y_position;
    } catch (...) {  // NOLINT(bugprone-empty-catch)
      // Node not found, return basic info only
    }
  }

  return json;
}

void OrcGraphModel::loadNode(QJsonObject const& nodeJson) {
  // Project loading is handled by presenter, not by QtNodes
  ORC_LOG_WARN(
      "OrcGraphModel::loadNode not implemented - use presenter instead");
}

NodeID OrcGraphModel::getOrcNodeId(NodeId qtNodeId) const {
  auto it = qt_to_orc_nodes_.find(qtNodeId);
  if (it != qt_to_orc_nodes_.end()) {
    return it->second;
  }
  return NodeID();
}

NodeId OrcGraphModel::getQtNodeId(const NodeID& orc_node_id) const {
  auto it = orc_to_qt_nodes_.find(orc_node_id);
  if (it == orc_to_qt_nodes_.end()) {
    return QtNodes::InvalidNodeId;
  }
  return it->second;
}
