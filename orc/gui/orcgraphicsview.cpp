/*
 * File:        orcgraphicsview.cpp
 * Module:      orc-gui
 * Purpose:     Custom QtNodes view with validated deletion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "orcgraphicsview.h"

#include <orc/stage/node_id.h>

#include <QAction>
#include <QContextMenuEvent>
#include <QFont>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollBar>
#include <QShowEvent>
#include <QWheelEvent>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <cmath>

#include "logging.h"
#include "orcgraphicsscene.h"
#include "orcgraphmodel.h"
#include "presenters/include/project_presenter.h"

using orc::NodeID;
#include <QAction>
#include <QKeySequence>
#include <QMessageBox>
#include <QShowEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {
// Gap left between a keyboard-selected node and the viewport edge when the
// view scrolls to reveal it, in device pixels.
constexpr int kRevealMargin = 60;
}  // namespace

OrcGraphicsView::OrcGraphicsView(QWidget* parent)
    : QtNodes::GraphicsView(parent),
      welcome_message_(QStringLiteral(
          "Welcome to decode-orc. To get started use the 'Quick Project...' "
          "option in the File menu to select a source TBC file for "
          "processing.\n\n"
          "TBC files ending in .tbc will be treated as composite video and TBC "
          "file pairs ending in .tbcy and .tbcc will be treated as Y/C "
          "sources. NTSC, NTSC-J, PAL and PAL-M are currently supported for "
          "both LaserDisc, tape and other capture sources.\n\n"
          "For a full user guide open Help > User Guide from the menu bar. "
          "Every stage node also has built-in help: right-click any node and "
          "choose Help....")) {
  // Find and disconnect the default delete action
  for (QAction* action : actions()) {
    if (action->shortcut() == QKeySequence::Delete) {
      action->setShortcuts(
          {QKeySequence::Delete, QKeySequence(Qt::Key_Backspace)});
      // Disconnect all connections from this action
      disconnect(action, nullptr, nullptr, nullptr);
      // Connect to our custom handler
      connect(action, &QAction::triggered, this,
              &OrcGraphicsView::onDeleteSelectedObjects);
      break;
    }
  }
}

void OrcGraphicsView::setShowWelcomeMessage(bool show) {
  if (show_welcome_message_ == show) {
    return;
  }

  show_welcome_message_ = show;
  viewport()->update();
}

void OrcGraphicsView::paintEvent(QPaintEvent* event) {
  QtNodes::GraphicsView::paintEvent(event);

  if (!show_welcome_message_) {
    return;
  }

  QPainter painter(viewport());
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);

  const QRect view_rect = viewport()->rect();
  const int max_text_width =
      std::min(560, std::max(320, view_rect.width() - 120));
  const int horizontal_padding = 24;
  const int vertical_padding = 18;

  QFont text_font = painter.font();
  text_font.setPointSize(text_font.pointSize() + 1);
  painter.setFont(text_font);

  const QRect text_rect = painter.boundingRect(
      QRect(0, 0, max_text_width, 1000),
      Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, welcome_message_);

  const QSize box_size(text_rect.width() + horizontal_padding * 2,
                       text_rect.height() + vertical_padding * 2);
  const int box_margin = 24;
  const QRect box_rect(box_margin, box_margin, box_size.width(),
                       box_size.height());

  painter.setPen(QPen(QColor(145, 150, 156, 210), 1));
  painter.setBrush(QColor(248, 250, 252, 240));
  painter.drawRoundedRect(box_rect, 10, 10);

  painter.setPen(QColor(40, 44, 52));
  painter.drawText(box_rect.adjusted(horizontal_padding, vertical_padding,
                                     -horizontal_padding, -vertical_padding),
                   Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                   welcome_message_);
}

void OrcGraphicsView::showEvent(QShowEvent* event) {
  // Set scale limits: 70% to 100%
  setScaleRange(0.7, 1.0);
  QtNodes::GraphicsView::showEvent(event);
}

void OrcGraphicsView::wheelEvent(QWheelEvent* event) {
  QPoint delta = event->angleDelta();

  if (delta.y() == 0) {
    event->ignore();
    return;
  }

  // Reduced sensitivity: use 1.1 (10% per scroll) instead of default 1.2 (20%)
  double const step = 1.1;
  double const d =
      delta.y() / std::abs(delta.y());  // NOLINT(bugprone-integer-division)
  double const factor = std::pow(step, d);

  // Get current scale and apply limits
  double currentScale = transform().m11();
  double newScale = currentScale * factor;

  // Clamp to 70%-100% range
  newScale = std::max(0.7, std::min(1.0, newScale));

  // Only apply if there's a meaningful change
  if (std::abs(newScale - currentScale) > 0.001) {
    setupScale(newScale);
  }

  event->accept();
}

void OrcGraphicsView::mouseReleaseEvent(QMouseEvent* event) {
  QtNodes::GraphicsView::mouseReleaseEvent(event);

  // A left-button release is the end of a potential node drag. QtNodes updates
  // the on-screen graphics item during the drag but never commits the new
  // position to the graph model, so persist it here.
  if (event->button() == Qt::LeftButton) {
    commitDraggedNodePositions();
  }
}

void OrcGraphicsView::commitDraggedNodePositions() {
  auto* orc_scene = dynamic_cast<OrcGraphicsScene*>(scene());
  if (!orc_scene) {
    return;
  }

  auto& graph_model = orc_scene->graphModel();

  // Positions are doubles; only commit when the item has actually moved to
  // avoid flagging the project modified on a plain click.
  constexpr double kEpsilon = 0.01;

  for (QGraphicsItem* item : scene()->items()) {
    auto* node_graphics = dynamic_cast<QtNodes::NodeGraphicsObject*>(item);
    if (!node_graphics) {
      continue;
    }

    const QtNodes::NodeId qt_node_id = node_graphics->nodeId();
    const QPointF stored =
        graph_model.nodeData(qt_node_id, QtNodes::NodeRole::Position)
            .toPointF();
    const QPointF current = node_graphics->pos();

    if (std::abs(current.x() - stored.x()) > kEpsilon ||
        std::abs(current.y() - stored.y()) > kEpsilon) {
      graph_model.setNodeData(qt_node_id, QtNodes::NodeRole::Position, current);
    }
  }
}

void OrcGraphicsView::keyPressEvent(QKeyEvent* event) {
  // Ctrl+arrow keeps the canvas panning that the unmodified cursor keys used
  // to provide before they were given over to node navigation.
  if (event->modifiers().testFlag(Qt::ControlModifier)) {
    switch (event->key()) {
      case Qt::Key_Left:
      case Qt::Key_Right:
      case Qt::Key_Up:
      case Qt::Key_Down:
        scrollCanvas(event->key());
        event->accept();
        return;
      default:
        break;
    }
  }

  // Unmodified cursor keys move the selection to the neighbouring node. The
  // key is accepted either way: falling through on a failed move would scroll
  // the canvas instead, which reads as the selection having jumped away.
  if (event->modifiers() == Qt::NoModifier ||
      event->modifiers() == Qt::KeypadModifier) {
    using orc::gui::NavigationDirection;
    switch (event->key()) {
      case Qt::Key_Left:
        navigateSelection(NavigationDirection::Left);
        event->accept();
        return;
      case Qt::Key_Right:
        navigateSelection(NavigationDirection::Right);
        event->accept();
        return;
      case Qt::Key_Up:
        navigateSelection(NavigationDirection::Up);
        event->accept();
        return;
      case Qt::Key_Down:
        navigateSelection(NavigationDirection::Down);
        event->accept();
        return;
      default:
        break;
    }
  }

  QtNodes::GraphicsView::keyPressEvent(event);
}

bool OrcGraphicsView::focusNextPrevChild(bool next) {
  if (cycleSelection(next)) {
    return true;
  }

  return QtNodes::GraphicsView::focusNextPrevChild(next);
}

std::vector<orc::gui::NodeBounds> OrcGraphicsView::collectNodeBounds() const {
  std::vector<orc::gui::NodeBounds> bounds;

  if (!scene()) {
    return bounds;
  }

  for (QGraphicsItem* item : scene()->items()) {
    auto* node_graphics = dynamic_cast<QtNodes::NodeGraphicsObject*>(item);
    if (!node_graphics) {
      continue;
    }

    const QRectF rect = node_graphics->sceneBoundingRect();
    bounds.push_back(orc::gui::NodeBounds{
        static_cast<std::uint64_t>(node_graphics->nodeId()), rect.x(), rect.y(),
        rect.width(), rect.height()});
  }

  return bounds;
}

std::optional<QtNodes::NodeId> OrcGraphicsView::anchorNodeId(
    const std::vector<orc::gui::NodeBounds>& bounds) const {
  if (!scene()) {
    return std::nullopt;
  }

  for (QGraphicsItem* item : scene()->selectedItems()) {
    auto* node_graphics = dynamic_cast<QtNodes::NodeGraphicsObject*>(item);
    if (node_graphics) {
      return node_graphics->nodeId();
    }
  }

  // Nothing selected right now: fall back to the node the scene reported last,
  // which is also what a selected connection resolves to.
  auto* orc_scene = dynamic_cast<OrcGraphicsScene*>(scene());
  if (!orc_scene) {
    return std::nullopt;
  }

  const QtNodes::NodeId last = orc_scene->lastSelectedNodeId();
  if (last == QtNodes::InvalidNodeId) {
    return std::nullopt;
  }

  const auto id = static_cast<std::uint64_t>(last);
  const bool still_present =
      std::any_of(bounds.begin(), bounds.end(),
                  [id](const orc::gui::NodeBounds& b) { return b.id == id; });

  return still_present ? std::optional<QtNodes::NodeId>(last) : std::nullopt;
}

void OrcGraphicsView::selectAndReveal(OrcGraphicsScene* orc_scene,
                                      QtNodes::NodeId node_id) {
  orc_scene->selectNode(node_id);

  for (QGraphicsItem* item : orc_scene->items()) {
    auto* node_graphics = dynamic_cast<QtNodes::NodeGraphicsObject*>(item);
    if (!node_graphics || node_graphics->nodeId() != node_id) {
      continue;
    }

    // Scroll only when the node is not already fully on screen. Handing the
    // margin straight to ensureVisible() would scroll a node that is merely
    // close to a viewport edge, which reads as the whole canvas jumping under
    // a selection that never left the screen.
    const QRect node_rect =
        mapFromScene(node_graphics->sceneBoundingRect()).boundingRect();
    if (!viewport()->rect().contains(node_rect)) {
      // The node was off screen, so keep it clear of the viewport edge to
      // leave its ports and the node it was reached from in sight.
      ensureVisible(node_graphics, kRevealMargin, kRevealMargin);
    }
    break;
  }
}

bool OrcGraphicsView::navigateSelection(
    orc::gui::NavigationDirection direction) {
  auto* orc_scene = dynamic_cast<OrcGraphicsScene*>(scene());
  if (!orc_scene) {
    return false;
  }

  const std::vector<orc::gui::NodeBounds> bounds = collectNodeBounds();
  if (bounds.empty()) {
    return false;
  }

  const std::optional<QtNodes::NodeId> anchor = anchorNodeId(bounds);

  // With nothing selected the first cursor key selects the most visible node
  // rather than moving from an arbitrary one.
  if (!anchor) {
    const QPointF centre = mapToScene(viewport()->rect().center());
    const auto nearest =
        orc::gui::findNodeNearestPoint(bounds, centre.x(), centre.y());
    if (!nearest) {
      return false;
    }
    selectAndReveal(orc_scene, static_cast<QtNodes::NodeId>(*nearest));
    return true;
  }

  const auto target = orc::gui::findAdjacentNode(
      bounds, static_cast<std::uint64_t>(*anchor), direction);
  if (!target) {
    return false;
  }

  selectAndReveal(orc_scene, static_cast<QtNodes::NodeId>(*target));
  return true;
}

bool OrcGraphicsView::cycleSelection(bool forward) {
  auto* orc_scene = dynamic_cast<OrcGraphicsScene*>(scene());
  if (!orc_scene) {
    return false;
  }

  const std::vector<orc::gui::NodeBounds> bounds = collectNodeBounds();
  if (bounds.empty()) {
    return false;
  }

  std::optional<std::uint64_t> current;
  if (const std::optional<QtNodes::NodeId> anchor = anchorNodeId(bounds)) {
    current = static_cast<std::uint64_t>(*anchor);
  }

  const auto target = orc::gui::findCycledNode(bounds, current, forward);
  if (!target) {
    return false;
  }

  selectAndReveal(orc_scene, static_cast<QtNodes::NodeId>(*target));
  return true;
}

void OrcGraphicsView::scrollCanvas(int key) {
  QScrollBar* bar = (key == Qt::Key_Left || key == Qt::Key_Right)
                        ? horizontalScrollBar()
                        : verticalScrollBar();
  if (!bar) {
    return;
  }

  const bool decrease = (key == Qt::Key_Left || key == Qt::Key_Up);
  bar->triggerAction(decrease ? QAbstractSlider::SliderSingleStepSub
                              : QAbstractSlider::SliderSingleStepAdd);
}

void OrcGraphicsView::contextMenuEvent(QContextMenuEvent* event) {
  if (itemAt(event->pos())) {
    // Call QGraphicsView directly, bypassing
    // QtNodes::GraphicsView::contextMenuEvent(). Newer versions of the QtNodes
    // library (used on Windows) call createStdMenu() inside their
    // contextMenuEvent(), which adds a second menu with grouping actions ("Add
    // to group", "Create group from selection") on top of ORC's own node
    // context menu. QGraphicsView::contextMenuEvent() propagates the event to
    // the item under the cursor, which causes
    // NodeGraphicsObject::contextMenuEvent() to emit nodeContextMenu(), handled
    // by OrcGraphicsScene::onNodeContextMenu() -- showing only the single ORC
    // context menu.
    QGraphicsView::contextMenuEvent(  // NOLINT(bugprone-parent-virtual-call)
        event);
    return;
  }

  auto* orc_scene = dynamic_cast<OrcGraphicsScene*>(scene());
  if (!orc_scene) {
    return;
  }

  const auto scene_pos = mapToScene(event->pos());
  QMenu* menu = orc_scene->createSceneMenu(scene_pos);
  if (menu) {
    menu->popup(event->globalPos());
  }
}

void OrcGraphicsView::onDeleteSelectedObjects() {
  auto* orc_scene = dynamic_cast<OrcGraphicsScene*>(scene());
  if (!orc_scene) {
    return;
  }

  // Check if anything is selected at all
  auto selected_items = scene()->selectedItems();
  if (selected_items.isEmpty()) {
    ORC_LOG_DEBUG("Nothing selected, ignoring delete request");
    return;
  }

  auto& graph_model = dynamic_cast<OrcGraphModel&>(orc_scene->graphModel());

  // Check if any selected nodes have connections
  std::vector<NodeID> cannot_delete;
  bool has_selected_nodes = false;

  for (QGraphicsItem* item : selected_items) {
    auto* node_graphics = dynamic_cast<QtNodes::NodeGraphicsObject*>(item);
    if (node_graphics) {
      has_selected_nodes = true;
      QtNodes::NodeId qt_node_id = node_graphics->nodeId();
      NodeID orc_node_id = graph_model.getOrcNodeId(qt_node_id);

      ORC_LOG_DEBUG("Delete check: QtNode {} -> ORC node '{}'", qt_node_id,
                    orc_node_id.to_string());

      if (orc_node_id.is_valid()) {
        std::string reason;
        if (!graph_model.presenter().canRemoveNode(orc_node_id, &reason)) {
          ORC_LOG_DEBUG("Cannot delete '{}': {}", orc_node_id.to_string(),
                        reason);
          cannot_delete.push_back(orc_node_id);
        }
      }
    }
  }

  if (!cannot_delete.empty()) {
    // Prevent deletion - show message with stage IDs
    QString msg = "Cannot delete stage";
    if (cannot_delete.size() > 1) {
      msg += "s";
    }
    msg += " with connections (";
    for (size_t i = 0; i < cannot_delete.size(); ++i) {
      if (i > 0) msg += ", ";
      msg += QString::fromStdString(cannot_delete[i].to_string());
    }
    msg += "). Disconnect all edges first.";

    QMessageBox::warning(this, "Cannot Delete Stage", msg);
    return;  // Don't proceed with deletion
  }

  // All checks passed - call parent implementation
  ORC_LOG_DEBUG(
      "All validation passed, calling parent onDeleteSelectedObjects");
  QtNodes::GraphicsView::onDeleteSelectedObjects();
}
