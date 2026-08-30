/*
 * File:        orcgraphicsscene.h
 * Module:      orc-gui
 * Purpose:     Custom QtNodes scene with context menu support
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc_analysis.h>  // For AnalysisToolInfo

#include <QMenu>
#include <QtNodes/BasicGraphicsScene>

#include "orcgraphmodel.h"

namespace orc {
class AnalysisTool;
}

// Use orc::NodeID for Qt signals/slots
using orc::NodeID;

/**
 * @brief Custom QtNodes graphics scene with DAG-specific context menus
 *
 * Extends QtNodes::BasicGraphicsScene to provide:
 * - Context menus for adding new nodes
 * - Node selection signals
 * - Stage inspection, triggering, and analysis integration
 *
 * Manages the visual representation of the processing DAG and handles
 * user interactions for node manipulation.
 */
class OrcGraphicsScene : public QtNodes::BasicGraphicsScene {
  Q_OBJECT

 public:
  /**
   * @brief Construct a new graphics scene
   * @param graphModel The DAG model to visualize
   * @param parent Parent QObject
   */
  explicit OrcGraphicsScene(OrcGraphModel& graphModel,
                            QObject* parent = nullptr);
  ~OrcGraphicsScene() override;

  /**
   * @brief Programmatically select a node in the scene
   * @param nodeId QtNodes node identifier
   */
  void selectNode(QtNodes::NodeId nodeId);

  /**
   * @brief Node the scene last reported as selected
   *
   * Keyboard navigation anchors on this when the current selection holds no
   * node graphics item (for example after a connection was selected).
   *
   * @return Last selected node id, or QtNodes::InvalidNodeId if none
   */
  QtNodes::NodeId lastSelectedNodeId() const { return last_selected_node_id_; }

  /**
   * @brief Create context menu for scene background
   * @param scenePos Position where menu was requested
   * @return Context menu with node creation options
   */
  QMenu* createSceneMenu(QPointF const scenePos) override;

 signals:
  void nodeSelected(
      QtNodes::NodeId nodeId);  ///< Emitted when a node is selected
  void editParametersRequested(
      const NodeID&
          node_id);  ///< Emitted when user wants to edit node parameters
  void triggerStageRequested(
      const NodeID& node_id);  ///< Emitted when user wants to trigger a stage

  /**
   * @brief Emitted when user requests to run an analysis tool on a node
   * @param tool_info The analysis tool information
   * @param node_id Node to analyze
   * @param stage_name Stage type name
   */
  void runAnalysisRequested(const orc::AnalysisToolInfo& tool_info,
                            const NodeID& node_id,
                            const std::string& stage_name);

 public slots:
  // Overridden to reset _draftConnection before QGraphicsScene::clear() runs.
  // Without this, onModelReset() deletes the draft ConnectionGraphicsObject
  // via Qt scene ownership while _draftConnection still holds the pointer,
  // causing a use-after-free vtable crash on the next makeDraftConnection call.
  void onModelReset() override;

 private slots:
  void onSelectionChanged();
  void onNodeContextMenu(QtNodes::NodeId nodeId, QPointF const pos);

 private:
  OrcGraphModel& graph_model_;
  QtNodes::NodeId last_selected_node_id_ = QtNodes::InvalidNodeId;
  bool restoring_selection_ = false;
};
