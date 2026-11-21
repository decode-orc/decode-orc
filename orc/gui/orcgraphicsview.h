/*
 * File:        orcgraphicsview.h
 * Module:      orc-gui
 * Purpose:     Custom QtNodes view with validated deletion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <QString>
#include <QtNodes/GraphicsView>
#include <optional>
#include <vector>

#include "node_navigation_mapper.h"

class OrcGraphModel;
class OrcGraphicsScene;

/**
 * Custom graphics view that validates node deletion before allowing it
 */
class OrcGraphicsView : public QtNodes::GraphicsView {
  Q_OBJECT

 public:
  explicit OrcGraphicsView(QWidget* parent = nullptr);
  ~OrcGraphicsView() override = default;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void contextMenuEvent(QContextMenuEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

  // Tab and Shift+Tab are consumed by QWidget::event() for focus traversal
  // before keyPressEvent() ever runs, so node cycling has to hook in here.
  bool focusNextPrevChild(bool next) override;

 public:
  void setShowWelcomeMessage(bool show);

  // Current canvas scale (1.0 = stages at natural size).
  double zoomScale() const;

 public slots:
  // Step the canvas zoom one notch in or out, clamped to the range in
  // dag_zoom.h. Shared by the mouse wheel, the toolbar and the View menu.
  void zoomIn();
  void zoomOut();

  // Return the canvas to its natural size.
  void resetZoom();

 signals:
  // Emitted whenever the canvas scale changes, with the new scale, so the
  // zoom actions can grey out at the ends of the range.
  void zoomScaleChanged(double scale);

 private slots:
  void onDeleteSelectedObjects() override;

 private:
  // Persist any node whose on-screen position no longer matches the position
  // stored in the model. QtNodes moves nodes visually during a drag but never
  // writes the new coordinates back to the graph model, so without this the
  // project keeps stale positions and every node snaps back to its old spot
  // the next time the scene is rebuilt (e.g. after editing parameters).
  void commitDraggedNodePositions();

  // On-screen extent of every node, in scene coordinates. Taken from the
  // graphics items rather than the model so navigation follows what the user
  // can actually see, including nodes moved but not yet committed.
  std::vector<orc::gui::NodeBounds> collectNodeBounds() const;

  // Node keyboard navigation moves from: the selected node, else the last
  // selected one if it still exists, else nothing.
  std::optional<QtNodes::NodeId> anchorNodeId(
      const std::vector<orc::gui::NodeBounds>& bounds) const;

  // Select |node_id| and scroll it into view.
  void selectAndReveal(OrcGraphicsScene* orc_scene, QtNodes::NodeId node_id);

  // Move the selection one node in |direction|; returns false when there is
  // no node to move to.
  bool navigateSelection(orc::gui::NavigationDirection direction);

  // Step the selection through all nodes in creation order.
  bool cycleSelection(bool forward);

  // Keyboard canvas panning, kept on Ctrl+arrow now that the unmodified
  // cursor keys move the selection instead.
  void scrollCanvas(int key);

  // Apply |scale| (already clamped) when it differs from the current one.
  void applyZoomScale(double scale);

  bool show_welcome_message_{true};
  QString welcome_message_;
};
