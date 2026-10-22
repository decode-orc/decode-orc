/*
 * File:        orcgraphicsscene.cpp
 * Module:      orc-gui
 * Purpose:     Custom QtNodes scene with context menu support
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "orcgraphicsscene.h"

#include <orc/stage/common_types.h>  // For VideoSystem, SourceType
#include <orc/stage/node_type.h>

#include "logging.h"
#include "orcnodepainter.h"
#include "presenters/include/analysis_presenter.h"
#include "presenters/include/project_presenter.h"
#include "stage_help_dialog.h"
// Removed: #include "analysis_registry.h"  // Phase 2.4: Now using
// AnalysisPresenter
#include <QGraphicsView>
#include <QInputDialog>
#include <QMessageBox>
#include <QTimer>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <vector>

using orc::is_stage_compatible_with_format;
using orc::NodeID;
using orc::NodeType;
using orc::SourceType;
using orc::VideoSystem;

OrcGraphicsScene::OrcGraphicsScene(OrcGraphModel& graphModel, QObject* parent)
    : QtNodes::BasicGraphicsScene(graphModel, parent),
      graph_model_(graphModel) {
  // Disable BSP indexing for dynamic node graph to prevent BSP tree crashes
  // See: https://doc.qt.io/qt-6/qgraphicsscene.html#ItemIndexMethod-enum
  // Node graphs have frequent add/remove/update operations which can cause
  // stale BSP tree entries and crashes during paint traversal
  setItemIndexMethod(QGraphicsScene::NoIndex);

  // Set custom node painter that distinguishes "one" vs "many" ports
  setNodePainter(std::make_unique<OrcNodePainter>());

  // Connect to scene's selection changed signal
  connect(this, &QGraphicsScene::selectionChanged, this,
          &OrcGraphicsScene::onSelectionChanged);

  // Connect to node context menu signal from BasicGraphicsScene
  connect(this, &QtNodes::BasicGraphicsScene::nodeContextMenu, this,
          &OrcGraphicsScene::onNodeContextMenu);
}

OrcGraphicsScene::~OrcGraphicsScene() {
  // Disconnect all signals to prevent callbacks during destruction
  // This prevents Qt from trying to call methods on partially-destructed
  // objects
  disconnect();
}

void OrcGraphicsScene::onModelReset() {
  resetDraftConnection();
  BasicGraphicsScene::onModelReset();
}

void OrcGraphicsScene::selectNode(QtNodes::NodeId nodeId) {
  // Clear current selection first
  clearSelection();

  // Find the node graphics item and select it
  const auto items_list = items();
  for (auto* item : items_list) {
    auto* node_graphics = dynamic_cast<QtNodes::NodeGraphicsObject*>(item);
    if (node_graphics && node_graphics->nodeId() == nodeId) {
      node_graphics->setSelected(true);
      break;
    }
  }
}

void OrcGraphicsScene::onSelectionChanged() {
  if (restoring_selection_) {
    return;
  }

  // Get selected items and emit nodeSelected for the first selected node
  auto selected = selectedItems();
  for (auto* item : selected) {
    auto* connection_graphics =
        dynamic_cast<QtNodes::ConnectionGraphicsObject*>(item);
    if (connection_graphics) {
      const auto connection_id = connection_graphics->connectionId();
      QtNodes::NodeId source_node_id = connection_id.outNodeId;
      if (source_node_id != QtNodes::InvalidNodeId) {
        last_selected_node_id_ = source_node_id;
        Q_EMIT nodeSelected(source_node_id);
        return;
      }
    }
  }
  for (auto* item : selected) {
    auto* node_graphics = dynamic_cast<QtNodes::NodeGraphicsObject*>(item);
    if (node_graphics) {
      QtNodes::NodeId nodeId = node_graphics->nodeId();
      last_selected_node_id_ = nodeId;
      Q_EMIT nodeSelected(nodeId);
      return;  // Only handle first selected node
    }
  }

  // Selection was cleared; avoid restoring selection here to prevent
  // re-entrant item selection changes during mouse event processing.
  last_selected_node_id_ = QtNodes::InvalidNodeId;
}

QMenu* OrcGraphicsScene::createSceneMenu(QPointF const scenePos) {
  // Check if project has a valid name (indicating it's been created/loaded)
  bool has_project = !graph_model_.presenter().getProjectName().empty();

  if (!has_project) {
    return nullptr;
  }

  QMenu* menu = new QMenu(views().isEmpty() ? nullptr : views().first());

  {
    auto project_format_enum = graph_model_.presenter().getVideoFormat();
    auto project_source_enum = graph_model_.presenter().getSourceType();
    auto stages = graph_model_.presenter().listAvailableStagesForFormat(
        project_format_enum);

    // Convert presenter enums to core enums
    VideoSystem project_format =
        (project_format_enum == orc::presenters::VideoFormat::NTSC)
            ? VideoSystem::NTSC
        : (project_format_enum == orc::presenters::VideoFormat::PAL)
            ? VideoSystem::PAL
        : (project_format_enum == orc::presenters::VideoFormat::PAL_M)
            ? VideoSystem::PAL_M
            : VideoSystem::Unknown;
    SourceType project_source_type =
        (project_source_enum == orc::presenters::SourceType::Composite)
            ? SourceType::Composite
        : (project_source_enum == orc::presenters::SourceType::YC)
            ? SourceType::YC
            : SourceType::Unknown;

    // Organize stages by the category derived from their node type, keeping
    // stages from bundled (core) plugins separate from external plugin stages.
    std::map<orc::StageCategory, std::vector<orc::presenters::StageInfo>>
        core_stages_by_category;
    std::map<orc::StageCategory, std::vector<orc::presenters::StageInfo>>
        plugin_stages_by_category;

    for (const auto& stage : stages) {
      if (!is_stage_compatible_with_format(stage.name, project_format)) {
        continue;
      }

      switch (stage.node_type) {
        case NodeType::SOURCE: {
          // Filter source stages by source type when the project source is
          // constrained. Dual-mode stages (tbc_source and CVBS sources) are
          // compatible with any source format and are never filtered out.
          if (project_source_type != SourceType::Unknown) {
            bool is_dual_mode = (stage.name == "tbc_source" ||
                                 stage.name == "NTSC_CVBS_Source" ||
                                 stage.name == "PAL_CVBS_Source" ||
                                 stage.name == "PAL_M_CVBS_Source");
            if (!is_dual_mode) {
              bool is_yc_stage = (stage.name.find("YC") != std::string::npos);
              SourceType stage_type =
                  is_yc_stage ? SourceType::YC : SourceType::Composite;

              if (stage_type != project_source_type) {
                continue;
              }
            }
          }
          break;
        }
        case NodeType::TRANSFORM:
        case NodeType::MERGER:
        case NodeType::COMPLEX:
        case NodeType::SINK:
        case NodeType::ANALYSIS_SINK:
          break;
      }

      auto& buckets = stage.is_core_plugin ? core_stages_by_category
                                           : plugin_stages_by_category;
      buckets[orc::stage_category_for(stage.node_type)].push_back(stage);
    }

    auto add_stages_to_menu =
        [this, scenePos](
            QMenu* parent_menu,
            const std::vector<orc::presenters::StageInfo>& category_stages) {
          std::vector<orc::presenters::StageInfo> sorted_stages =
              category_stages;
          std::sort(sorted_stages.begin(), sorted_stages.end(),
                    [](const orc::presenters::StageInfo& lhs,
                       const orc::presenters::StageInfo& rhs) {
                      return lhs.display_name < rhs.display_name;
                    });

          for (const auto& stage : sorted_stages) {
            QString display_name = QString::fromStdString(stage.display_name);
            QString tooltip = QString::fromStdString(stage.description);

            auto* action = parent_menu->addAction(
                display_name, [this, scenePos, stage_name = stage.name]() {
                  // Defer graph mutation until after popup menu processing
                  // completes to avoid re-entrant scene/model updates during
                  // mouse event handling.
                  QTimer::singleShot(0, this, [this, scenePos, stage_name]() {
                    QtNodes::NodeId nodeId = graph_model_.addNode(
                        QString::fromStdString(stage_name));
                    if (nodeId != QtNodes::InvalidNodeId) {
                      graph_model_.setNodeData(
                          nodeId, QtNodes::NodeRole::Position, scenePos);
                    }
                  });
                });
            action->setToolTip(tooltip);
          }
        };

    const std::array<orc::StageCategory, 4> kCategoryOrder = {
        orc::StageCategory::SOURCE, orc::StageCategory::TRANSFORM,
        orc::StageCategory::ANALYSIS, orc::StageCategory::SINK};

    // Core stage categories at the top level; empty categories are omitted.
    for (const auto category : kCategoryOrder) {
      auto it = core_stages_by_category.find(category);
      if (it == core_stages_by_category.end() || it->second.empty()) {
        continue;
      }
      QMenu* category_menu = menu->addMenu(orc::stage_category_label(category));
      add_stages_to_menu(category_menu, it->second);
    }

    // External plugin stages under a single "Plugins" entry, mirroring the
    // core category structure; only present when such stages exist.
    if (!plugin_stages_by_category.empty()) {
      QMenu* plugins_menu = menu->addMenu("Plugins");
      for (const auto category : kCategoryOrder) {
        auto it = plugin_stages_by_category.find(category);
        if (it == plugin_stages_by_category.end() || it->second.empty()) {
          continue;
        }
        QMenu* category_menu =
            plugins_menu->addMenu(orc::stage_category_label(category));
        add_stages_to_menu(category_menu, it->second);
      }
    }
  }

  QObject::connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
  return menu;
}

void OrcGraphicsScene::onNodeContextMenu(QtNodes::NodeId nodeId,
                                         QPointF const pos) {
  ORC_LOG_DEBUG("Node context menu requested for QtNode {}", nodeId);

  // Get ORC node ID
  NodeID orc_node_id = graph_model_.getOrcNodeId(nodeId);
  if (!orc_node_id.is_valid()) {
    ORC_LOG_WARN("Could not find ORC node ID for QtNode {}", nodeId);
    return;
  }

  ORC_LOG_DEBUG("Showing context menu for ORC node '{}'",
                orc_node_id.to_string());

  // Get node info from presenter
  try {
    auto node_info = graph_model_.presenter().getNodeInfo(orc_node_id);

    QString node_label = QString::fromStdString(
        node_info.label.empty() ? node_info.stage_name : node_info.label);

    // Debug: Log capabilities if debug logging is enabled
    qDebug() << "Node capabilities for"
             << QString::fromStdString(orc_node_id.to_string()) << "("
             << QString::fromStdString(node_info.stage_name) << "):";
    qDebug() << "  Can remove:" << node_info.can_remove
             << (node_info.can_remove
                     ? ""
                     : QString("- %1").arg(
                           QString::fromStdString(node_info.remove_reason)));
    qDebug() << "  Can trigger:" << node_info.can_trigger
             << (node_info.can_trigger
                     ? ""
                     : QString("- %1").arg(
                           QString::fromStdString(node_info.trigger_reason)));

    // Create context menu (with view as parent to ensure proper cleanup)
    QMenu* menu = new QMenu(views().isEmpty() ? nullptr : views().first());
    menu->addSection(QString("%1 (%2)")
                         .arg(node_label)
                         .arg(QString::fromStdString(orc_node_id.to_string())));

    // Rename Stage action - always available
    auto* rename_action = menu->addAction("Rename Stage...");
    connect(rename_action, &QAction::triggered, [this, nodeId, node_label]() {
      QTimer::singleShot(0, this, [this, nodeId, node_label]() {
        // Prompt for new name.  Stage names must be unique, non-empty and not
        // all digits; a refused name is explained and the prompt reopens
        // with it so the user can correct it.
        QString proposed = node_label;
        while (true) {
          bool ok;
          proposed = QInputDialog::getText(
              nullptr, "Rename Stage",
              "Enter new name for stage:", QLineEdit::Normal, proposed, &ok);
          if (!ok || proposed == node_label) {
            return;
          }
          if (graph_model_.setNodeData(nodeId, QtNodes::NodeRole::Caption,
                                       proposed)) {
            return;
          }
          QMessageBox::warning(nullptr, "Rename Stage",
                               graph_model_.lastCaptionError());
        }
      });
    });

    // Edit Parameters action - always available
    auto* edit_params_action = menu->addAction("Edit Parameters...");
    connect(edit_params_action, &QAction::triggered, [this, orc_node_id]() {
      QTimer::singleShot(0, this, [this, orc_node_id]() {
        Q_EMIT editParametersRequested(orc_node_id);
      });
    });

    menu->addSeparator();

    // Trigger Stage action
    auto* trigger_action = menu->addAction("Trigger Stage");
    trigger_action->setEnabled(node_info.can_trigger);
    if (!node_info.can_trigger && !node_info.trigger_reason.empty()) {
      trigger_action->setToolTip(
          QString::fromStdString(node_info.trigger_reason));
    }
    connect(trigger_action, &QAction::triggered, [this, orc_node_id]() {
      QTimer::singleShot(0, this, [this, orc_node_id]() {
        Q_EMIT triggerStageRequested(orc_node_id);
      });
    });

    menu->addSeparator();

    // Run Analysis submenu - populate with tools applicable to this stage
    QMenu* analysis_menu = menu->addMenu("Stage Tools");

    // Phase 2.4: Use AnalysisPresenter instead of direct registry access
    orc::presenters::AnalysisPresenter analysis_presenter(
        graph_model_.presenter().getCoreProjectHandle());
    auto tool_infos = analysis_presenter.getToolsForStage(node_info.stage_name);

    // Every tool the stage advertises is listed, result viewers included: a
    // viewer entry re-opens the last trigger's results without re-running the
    // stage, which is the one thing Trigger Stage above cannot do.
    if (tool_infos.empty()) {
      analysis_menu->setEnabled(false);
    } else {
      // Tools are already sorted by priority in getToolsForStage()
      for (const auto& tool_info : tool_infos) {
        QString tool_name = QString::fromStdString(tool_info.name);
        QString tool_desc = QString::fromStdString(tool_info.description);

        auto* tool_action = analysis_menu->addAction(tool_name);
        tool_action->setToolTip(tool_desc);

        // Pass tool_info to signal instead of raw pointer.
        // NOLINTBEGIN(bugprone-exception-escape): Qt slots — exceptions must
        // not propagate through Qt's event loop.
        connect(tool_action, &QAction::triggered,
                [this, tool_info, orc_node_id,
                 stage_name = node_info.stage_name]() {
                  // Defer dialog/tool launch until after popup menu processing
                  // completes to avoid re-entrant popup/mouse-event handling
                  // crashes in Qt.
                  QTimer::singleShot(
                      0, this, [this, tool_info, orc_node_id, stage_name]() {
                        Q_EMIT runAnalysisRequested(tool_info, orc_node_id,
                                                    stage_name);
                      });
                });
        // NOLINTEND(bugprone-exception-escape)
      }
    }

    menu->addSeparator();

    // Help action — shows stage instructions (Markdown) rendered in a dialog.
    auto* help_action = menu->addAction("Help...");
    {
      std::string instructions =
          graph_model_.presenter().getStageInstructions(node_info.stage_name);
      help_action->setEnabled(!instructions.empty());
      if (!instructions.empty()) {
        QString stage_label = QString::fromStdString(
            node_info.label.empty() ? node_info.stage_name : node_info.label);
        connect(help_action, &QAction::triggered,
                [this, stage_label, instructions]() {
                  QTimer::singleShot(
                      0, this, [this, stage_label, instructions]() {
                        auto* dlg = new StageHelpDialog(
                            stage_label, QString::fromStdString(instructions),
                            views().isEmpty() ? nullptr : views().first());
                        dlg->exec();
                      });
                });
      }
    }

    menu->addSeparator();

    // Delete Stage action
    auto* delete_action = menu->addAction("Delete Stage");
    delete_action->setEnabled(node_info.can_remove);
    if (!node_info.can_remove && !node_info.remove_reason.empty()) {
      delete_action->setToolTip(
          QString::fromStdString(node_info.remove_reason));
    }
    connect(delete_action, &QAction::triggered, [this, nodeId]() {
      QTimer::singleShot(0, this,
                         [this, nodeId]() { graph_model_.deleteNode(nodeId); });
    });

    QObject::connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);

    // Convert scene position to screen position
    if (!views().isEmpty()) {
      QPoint screen_pos =
          views().first()->mapToGlobal(views().first()->mapFromScene(pos));
      menu->popup(screen_pos);
    } else {
      menu->popup(pos.toPoint());
    }
  } catch (const std::exception& e) {
    ORC_LOG_WARN("Could not get node info for '{}': {}",
                 orc_node_id.to_string(), e.what());
  }
}
