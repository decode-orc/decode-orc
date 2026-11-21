/*
 * File:        plugin_view_host.cpp
 * Module:      orc-gui
 * Purpose:     Opens plugins' view windows and carries their tool sessions
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "plugin_view_host.h"

#include <amplitude_conversion.h>
#include <orc/view-support/analysis_tool_window.h>

#include <QMetaObject>
#include <QThread>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <utility>

#include "logging.h"
#include "presenters/include/view_plugins.h"
#include "render_coordinator.h"
#include "tool_frame_renderer.h"

namespace {

// A job's progress reaches its view only when it has moved by this much (or
// its text changed), so a stage reporting every frame does not flood the GUI
// thread with events it cannot paint.
constexpr double kProgressStep = 0.002;

orc::view::ToolReply refusal(std::string error) {
  orc::view::ToolReply reply;
  reply.error = std::move(error);
  return reply;
}

}  // namespace

// Carries every session call to where it runs and hands each reply back, on
// the GUI thread and never inside the call that asked for it.
class PluginViewHost::Transport final
    : public orc::presenters::IStageToolTransport {
 public:
  Transport(RenderCoordinator* coordinator, QObject* context, Actions actions)
      : coordinator_(coordinator),
        context_(context),
        actions_(std::move(actions)) {}

  ~Transport() override { shutdown(); }

  // ---- requests (render coordinator) ----

  void send(orc::NodeID node, const std::string& contract_id,
            const std::string& verb, std::vector<uint8_t> payload,
            std::function<void(orc::view::ToolReply)> on_done) override {
    if (!coordinator_) {
      later([on_done = std::move(on_done)]() mutable {
        on_done(refusal("Decode-Orc is shutting down"));
      });
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

  // ---- jobs and analyses (one worker thread each) ----
  uint64_t start_job(
      orc::NodeID node, const std::string& contract_id, const std::string& verb,
      std::vector<uint8_t> payload,
      std::function<void(orc::view::ToolJobProgress)> on_progress,
      std::function<void(orc::view::ToolReply)> on_done) override {
    JobRunner runner;
    if (!shut_down_ && context_ && actions_.prepare_job) {
      try {
        runner =
            actions_.prepare_job(node, contract_id, verb, std::move(payload));
      } catch (const std::exception& e) {
        ORC_LOG_WARN("Could not prepare tool job '{}': {}", verb, e.what());
      }
    }
    return startWorker<orc::view::ToolReply>(
        std::move(runner), std::move(on_progress), std::move(on_done),
        [](std::string error) { return refusal(std::move(error)); },
        "The job could not be prepared");
  }

  uint64_t start_analysis(
      orc::NodeID node, const std::string& contract_id,
      orc::view::ToolParameterMap parameters,
      std::function<void(orc::view::ToolJobProgress)> on_progress,
      std::function<void(orc::view::ToolAnalysisReport)> on_done) override {
    AnalysisRunner runner;
    std::string why = "The analysis could not be prepared";
    if (!shut_down_ && context_ && actions_.prepare_analysis) {
      try {
        runner = actions_.prepare_analysis(node, contract_id, parameters, &why);
      } catch (const std::exception& e) {
        why = e.what();
      }
    }
    return startWorker<orc::view::ToolAnalysisReport>(
        std::move(runner), std::move(on_progress), std::move(on_done),
        [](std::string error) {
          orc::view::ToolAnalysisReport report;
          report.summary = std::move(error);
          return report;
        },
        why);
  }

  void cancel_job(uint64_t job_id) override {
    auto it = jobs_.find(job_id);
    if (it != jobs_.end()) {
      it->second.cancelled->store(true);
    }
  }

  size_t runningJobCount() const { return jobs_.size(); }

  // ---- parameter write-back and the previewer (main window) ----

  void apply_parameters(
      orc::NodeID node, orc::view::ToolParameterMap values,
      const std::string& label,
      std::function<void(orc::view::ToolReply)> on_done) override {
    // Queued: the write rebuilds the graph, which can close the very view
    // that asked, so it must not happen inside the view's own call.
    later([this, node, values = std::move(values), label,
           on_done = std::move(on_done)]() mutable {
      if (shut_down_ || !actions_.apply_parameters) {
        on_done(refusal("Parameters cannot be written now"));
        return;
      }
      orc::view::ToolReply reply;
      try {
        reply = actions_.apply_parameters(node, values, label);
      } catch (const std::exception& e) {
        reply = refusal(e.what());
      }
      on_done(std::move(reply));
    });
  }

  void show_frame(orc::NodeID node, uint64_t frame_index) override {
    later([this, node, frame_index]() {
      if (!shut_down_ && actions_.show_frame) {
        actions_.show_frame(node, frame_index);
      }
    });
  }

  // ---- the node's parameters and the project folder (answered at once) ----

  orc::view::ToolParameterMap parameters(orc::NodeID node) override {
    if (shut_down_ || !actions_.parameters) {
      return {};
    }
    return actions_.parameters(node);
  }

  std::string project_directory() override {
    if (shut_down_ || !actions_.project_directory) {
      return {};
    }
    return actions_.project_directory();
  }

  // ---- an analysis tool's description and refresh option ----
  orc::view::ToolAnalysisInfo analysis_info(
      orc::NodeID node, const std::string& contract_id) override {
    if (shut_down_ || !actions_.analysis_info) {
      return {};
    }
    return actions_.analysis_info(node, contract_id);
  }

  orc::view::ToolRefreshState refresh_state(
      orc::NodeID node, const std::string& contract_id) override {
    if (shut_down_ || !actions_.refresh_state) {
      return {};
    }
    return actions_.refresh_state(node, contract_id);
  }

  void set_refresh(orc::NodeID node, const std::string& contract_id,
                   bool use_this_tool, orc::view::ToolParameterMap parameters,
                   std::function<void(orc::view::ToolReply)> on_done) override {
    later([this, node, contract_id, use_this_tool,
           parameters = std::move(parameters),
           on_done = std::move(on_done)]() mutable {
      if (shut_down_ || !actions_.set_refresh) {
        on_done(refusal("The refresh option cannot be stored now"));
        return;
      }
      orc::view::ToolReply reply;
      try {
        reply =
            actions_.set_refresh(node, contract_id, use_this_tool, parameters);
      } catch (const std::exception& e) {
        reply = refusal(e.what());
      }
      on_done(std::move(reply));
    });
  }

  // ---- frames (the host's render thread) ----

  void render_frame(
      orc::NodeID node, uint64_t frame_index,
      const orc::view::ToolFrameRenderOptions& options,
      std::function<void(orc::view::ToolFrameImage)> on_done) override {
    const auto fail = [this, frame_index, &on_done](std::string error) {
      later([frame_index, error = std::move(error),
             on_done = std::move(on_done)]() mutable {
        orc::view::ToolFrameImage image;
        image.frame_index = frame_index;
        image.error = std::move(error);
        on_done(std::move(image));
      });
    };
    if (shut_down_ || !actions_.render_target ||
        !actions_.make_frame_renderer) {
      fail("Frames cannot be rendered now");
      return;
    }
    const auto target = actions_.render_target(node, options.from_input);
    if (!target) {
      fail(options.from_input ? "The node has no input to render"
                              : "The node cannot be rendered");
      return;
    }
    if (!renderer_) {
      renderer_ = actions_.make_frame_renderer();
      if (!renderer_) {
        fail("Frames cannot be rendered now");
        return;
      }
    }
    if (!render_thread_) {
      render_thread_ = new QThread();
      render_thread_->setObjectName(QStringLiteral("orc-tool-render"));
      render_worker_ = new QObject();
      render_worker_->moveToThread(render_thread_);
      QObject::connect(render_thread_, &QThread::finished, render_worker_,
                       &QObject::deleteLater);
      render_thread_->start();
    }
    QPointer<QObject> context = context_;
    QMetaObject::invokeMethod(
        render_worker_,
        [renderer = renderer_, target = *target, frame_index, context,
         on_done = std::move(on_done)]() mutable {
          auto image = renderer->render(target, frame_index);
          if (context) {
            QMetaObject::invokeMethod(
                context.data(),
                [on_done = std::move(on_done),
                 image = std::move(image)]() mutable {
                  on_done(std::move(image));
                },
                Qt::QueuedConnection);
          }
        },
        Qt::QueuedConnection);
  }

  // The graph changed: the renderer draws the old one. It is released on
  // the render thread, which is the only thread that uses it.
  void dropRenderer() {
    if (!renderer_) {
      return;
    }
    if (render_worker_) {
      QMetaObject::invokeMethod(
          render_worker_,
          [renderer = std::move(renderer_)]() mutable { renderer.reset(); },
          Qt::QueuedConnection);
    }
    renderer_.reset();
  }

  // Stop everything: no reply is delivered after this, jobs are cancelled
  // and their threads, and the render thread, have finished.
  void shutdown() {
    if (shut_down_) {
      return;
    }
    shut_down_ = true;
    coordinator_ = nullptr;
    pending_.clear();
    for (auto& [id, job] : jobs_) {
      job.cancelled->store(true);
    }
    for (auto& [id, job] : jobs_) {
      job.thread->wait();
      delete job.thread;
    }
    jobs_.clear();
    renderer_.reset();
    if (render_thread_) {
      render_thread_->quit();
      render_thread_->wait();
      delete render_thread_;
      render_thread_ = nullptr;
      render_worker_ = nullptr;
    }
  }

 private:
  // Run @p runner on a thread of its own, posting its progress and result
  // back to the GUI thread (dropped if the host is gone by then). A null
  // runner, or a host that cannot run jobs now, fails with @p why.
  template <typename Result, typename Runner, typename Fail>
  uint64_t startWorker(
      Runner runner,
      std::function<void(orc::view::ToolJobProgress)> on_progress,
      std::function<void(Result)> on_done, Fail fail, std::string why) {
    const uint64_t id = next_job_id_++;
    if (shut_down_ || !context_) {
      why = "Jobs cannot be run now";
      runner = nullptr;
    }
    if (!runner) {
      later([on_done = std::move(on_done), result = fail(why)]() mutable {
        on_done(std::move(result));
      });
      return id;
    }
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    QPointer<QObject> context = context_;
    auto body = [runner = std::move(runner), cancelled, context,
                 on_progress = std::move(on_progress),
                 on_done = std::move(on_done), fail]() mutable {
      double last_fraction = -2.0;
      std::string last_text;
      const auto progress = [&](orc::view::ToolJobProgress report) {
        if (report.text == last_text &&
            std::abs(report.fraction - last_fraction) < kProgressStep) {
          return;
        }
        last_fraction = report.fraction;
        last_text = report.text;
        if (context) {
          QMetaObject::invokeMethod(
              context.data(),
              [on_progress, report = std::move(report)]() mutable {
                if (on_progress) {
                  on_progress(std::move(report));
                }
              },
              Qt::QueuedConnection);
        }
      };
      const auto is_cancelled = [&cancelled] { return cancelled->load(); };
      Result result;
      try {
        result = runner(progress, is_cancelled);
      } catch (const std::exception& e) {
        result = fail(std::string("The job failed: ") + e.what());
      } catch (...) {
        result = fail("The job failed");
      }
      if (context) {
        QMetaObject::invokeMethod(
            context.data(),
            [on_done = std::move(on_done),
             result = std::move(result)]() mutable {
              on_done(std::move(result));
            },
            Qt::QueuedConnection);
      }
    };
    QThread* thread = QThread::create(std::move(body));
    thread->setObjectName(QStringLiteral("orc-tool-job-%1").arg(id));
    jobs_[id] = {thread, cancelled};
    QObject::connect(thread, &QThread::finished, context_.data(), [this, id]() {
      auto it = jobs_.find(id);
      if (it != jobs_.end()) {
        it->second.thread->deleteLater();
        jobs_.erase(it);
      }
    });
    thread->start();
    return id;
  }

  template <typename Fn>
  void later(Fn&& fn) {
    if (context_) {
      QMetaObject::invokeMethod(context_.data(), std::forward<Fn>(fn),
                                Qt::QueuedConnection);
    }
  }

  struct RunningJob {
    QThread* thread = nullptr;
    std::shared_ptr<std::atomic<bool>> cancelled;
  };

  RenderCoordinator* coordinator_;
  // The host. Sessions held by closing windows can keep the transport alive
  // after the host is gone, so this is checked before every use.
  QPointer<QObject> context_;
  Actions actions_;
  bool shut_down_ = false;
  std::unordered_map<uint64_t, std::function<void(orc::view::ToolReply)>>
      pending_;
  uint64_t next_job_id_ = 1;
  std::unordered_map<uint64_t, RunningJob> jobs_;
  std::shared_ptr<IToolFrameRenderer> renderer_;
  QThread* render_thread_ = nullptr;
  QObject* render_worker_ = nullptr;
};

PluginViewHost::PluginViewHost(RenderCoordinator* coordinator,
                               QWidget* window_parent, Actions actions)
    : QObject(window_parent),
      amplitude_unit_(actions.amplitude_unit),
      transport_(
          std::make_shared<Transport>(coordinator, this, std::move(actions))),
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
  transport_->shutdown();
}

size_t PluginViewHost::runningJobCount() const {
  return transport_->runningJobCount();
}

std::shared_ptr<orc::presenters::IStageToolTransport>
PluginViewHost::transport() const {
  return transport_;
}

bool PluginViewHost::open(const orc::NodeID& node,
                          const std::string& contract_id, const QString& title,
                          QString* error, bool analysis) {
  for (auto& view : open_) {
    if (view.node == node && view.contract_id == contract_id && view.widget) {
      view.widget->show();
      view.widget->raise();
      view.widget->activateWindow();
      view.session->notifyResultsChanged();
      return true;
    }
  }

  orc::view::OrcViewFactoryFn factory =
      orc::presenters::findViewFactory(contract_id);
  if (!factory && analysis) {
    factory = [](const orc::view::ViewOpenRequest& request) -> QWidget* {
      return new orc::view::AnalysisToolWindow(request.session, request.parent);
    };
  }
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

  const orc::view::AmplitudeUnit unit = orc::to_view_amplitude_unit(
      amplitude_unit_ ? amplitude_unit_() : orc::AmplitudeDisplayUnit::IRE);
  auto session = std::make_shared<orc::presenters::StageToolSession>(
      transport_, node, contract_id, title.toStdString(), unit);
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
  // A view following the preview starts on the frame it shows.
  if (last_preview_frame_) {
    session->notifyPreviewFrame(*last_preview_frame_);
  }
  return true;
}

void PluginViewHost::onGraphRebuilt() {
  transport_->dropRenderer();

  // Split first: closing a window runs its destroyed handler, which edits
  // open_, and a rebuilt handler may open or close views itself.
  std::vector<OpenView> keep;
  std::vector<OpenView> close;
  for (auto& view : open_) {
    if (view.widget && view.session->keepsOpenAcrossRebuild()) {
      keep.push_back(view);
    } else {
      close.push_back(view);
    }
  }
  open_ = keep;
  for (auto& view : close) {
    view.session->close();
  }
  for (auto& view : close) {
    if (view.widget) {
      view.widget->close();
    }
  }
  for (auto& view : keep) {
    view.session->notifyGraphRebuilt();
  }
}

void PluginViewHost::notifyPreviewFrame(uint64_t frame_index) {
  if (last_preview_frame_ && *last_preview_frame_ == frame_index) {
    return;
  }
  last_preview_frame_ = frame_index;
  // Copied: a handler may close its view, which edits open_.
  const auto views = open_;
  for (const auto& view : views) {
    view.session->notifyPreviewFrame(frame_index);
  }
}

void PluginViewHost::notifyAmplitudeUnit(orc::AmplitudeDisplayUnit unit) {
  // Copied: a handler may close its view, which edits open_.
  const auto views = open_;
  for (const auto& view : views) {
    view.session->notifyAmplitudeUnit(orc::to_view_amplitude_unit(unit));
  }
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
  last_preview_frame_.reset();
  for (auto& widget : widgets) {
    if (widget) {
      widget->close();
    }
  }
}
