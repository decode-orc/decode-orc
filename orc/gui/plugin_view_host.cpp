/*
 * File:        plugin_view_host.cpp
 * Module:      orc-gui
 * Purpose:     Opens plugins' view windows and keeps their tool sessions
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "plugin_view_host.h"

#include <QWidget>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <utility>

#include "logging.h"
#include "presenters/include/view_plugins.h"
#include "render_coordinator.h"

// Carries session requests through the render coordinator and hands each
// reply to the callback that asked for it.
class PluginViewHost::Transport final
    : public orc::presenters::IStageToolTransport {
 public:
  explicit Transport(RenderCoordinator* coordinator)
      : coordinator_(coordinator) {}

  void send(orc::NodeID node, const std::string& contract_id,
            const std::string& verb, std::vector<uint8_t> payload,
            std::function<void(orc::view::ToolReply)> on_done) override {
    if (!coordinator_) {
      orc::view::ToolReply reply;
      reply.error = "Decode-Orc is shutting down";
      on_done(std::move(reply));
      return;
    }
    const uint64_t id = coordinator_->requestStageTool(node, contract_id, verb,
                                                       std::move(payload));
    pending_[id] = std::move(on_done);
  }

  void deliver(uint64_t id, orc::view::ToolReply reply) {
    auto it = pending_.find(id);
    if (it == pending_.end()) {
      return;
    }
    auto on_done = std::move(it->second);
    pending_.erase(it);
    on_done(std::move(reply));
  }

  void detach() {
    coordinator_ = nullptr;
    pending_.clear();
  }

 private:
  RenderCoordinator* coordinator_;
  std::unordered_map<uint64_t, std::function<void(orc::view::ToolReply)>>
      pending_;
};

PluginViewHost::PluginViewHost(RenderCoordinator* coordinator,
                               QWidget* window_parent)
    : QObject(window_parent),
      transport_(std::make_shared<Transport>(coordinator)),
      window_parent_(window_parent) {
  if (coordinator) {
    connect(
        coordinator, &RenderCoordinator::stageToolReplyReady, this,
        [this](uint64_t id, orc::view::ToolReply reply) {
          transport_->deliver(id, std::move(reply));
        },
        Qt::QueuedConnection);
  }
}

PluginViewHost::~PluginViewHost() {
  closeAll();
  transport_->detach();
}

size_t PluginViewHost::openCount() const { return open_.size(); }

bool PluginViewHost::open(const orc::NodeID& node,
                          const std::string& contract_id, const QString& title,
                          QString* error) {
  for (auto& view : open_) {
    if (view.node == node && view.contract_id == contract_id && view.widget) {
      view.widget->show();
      view.widget->raise();
      view.widget->activateWindow();
      view.session->notifyResultsChanged();
      return true;
    }
  }

  const orc::view::OrcViewFactoryFn factory =
      orc::presenters::findViewFactory(contract_id);
  if (!factory) {
    if (error) {
      *error = QString(
                   "The plugin's view for this tool is not loaded. Check that "
                   "the plugin is installed with its view, and see "
                   "Diagnostics in Tools > Plugin Manager for why it did not "
                   "load.\n\nTool contract: %1")
                   .arg(QString::fromStdString(contract_id));
    }
    return false;
  }

  auto session = std::make_shared<orc::presenters::StageToolSession>(
      transport_, node, contract_id, title.toStdString());
  orc::view::ViewOpenRequest request;
  request.session = session;
  request.parent = window_parent_;

  QWidget* widget = nullptr;
  try {
    widget = factory(request);
  } catch (const std::exception& e) {
    ORC_LOG_ERROR("Plugin view for '{}' threw while opening: {}", contract_id,
                  e.what());
  } catch (...) {
    ORC_LOG_ERROR("Plugin view for '{}' threw while opening", contract_id);
  }
  if (!widget) {
    session->close();
    if (error) {
      *error = "The plugin's view refused to open.";
    }
    return false;
  }

  // A view is its own window. A plain QWidget parented to the main window
  // would otherwise be drawn inside it.
  if (!widget->isWindow()) {
    widget->setWindowFlag(Qt::Window, true);
  }
  widget->setAttribute(Qt::WA_DeleteOnClose, true);
  if (widget->windowTitle().isEmpty()) {
    widget->setWindowTitle(title);
  }
  // The session ends with its window: no reply reaches a view that is gone.
  connect(widget, &QObject::destroyed, this, [this, raw = widget]() {
    auto it =
        std::find_if(open_.begin(), open_.end(), [raw](const OpenView& v) {
          return v.widget.isNull() || v.widget.data() == raw;
        });
    if (it != open_.end()) {
      it->session->close();
      open_.erase(it);
    }
  });
  open_.push_back({node, contract_id, session, widget});

  widget->show();
  widget->raise();
  widget->activateWindow();
  return true;
}

void PluginViewHost::closeAll() {
  // Sessions close first, so nothing a closing window does can still reach a
  // stage; the windows' destroyed handlers then find nothing left to erase.
  std::vector<QPointer<QWidget>> widgets;
  for (auto& view : open_) {
    view.session->close();
    widgets.push_back(view.widget);
  }
  open_.clear();
  for (auto& widget : widgets) {
    if (widget) {
      widget->close();
    }
  }
}
