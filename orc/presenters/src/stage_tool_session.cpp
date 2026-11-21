/*
 * File:        stage_tool_session.cpp
 * Module:      orc-presenters
 * Purpose:     The host's IStageToolSession
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../include/stage_tool_session.h"

#include <utility>

namespace orc::presenters {

namespace {

// Puts one kind of reply back in the order its calls were made. Each call
// takes a sequence number; a reply that arrives early is held until every
// earlier one has been delivered.
template <typename Reply>
class OrderedReplies {
 public:
  using Handler = std::function<void(Reply)>;

  uint64_t add(Handler handler) {
    const uint64_t sequence = next_sequence_++;
    handlers_[sequence] = std::move(handler);
    return sequence;
  }

  void clear() {
    handlers_.clear();
    held_.clear();
  }

  // Store @p reply, then deliver every reply now next in line while
  // @p still_open() holds: a handler may close the session.
  template <typename StillOpen>
  void arrive(uint64_t sequence, Reply reply, const StillOpen& still_open) {
    held_[sequence] = std::move(reply);
    while (still_open()) {
      auto ready = held_.find(next_delivery_);
      if (ready == held_.end()) {
        break;
      }
      Reply next = std::move(ready->second);
      held_.erase(ready);
      Handler handler;
      auto handler_it = handlers_.find(next_delivery_);
      if (handler_it != handlers_.end()) {
        handler = std::move(handler_it->second);
        handlers_.erase(handler_it);
      }
      ++next_delivery_;
      if (handler) {
        handler(std::move(next));
      }
    }
  }

 private:
  uint64_t next_sequence_ = 0;
  uint64_t next_delivery_ = 0;
  std::map<uint64_t, Handler> handlers_;
  std::map<uint64_t, Reply> held_;
};

}  // namespace

// Shared with the transport's completion callbacks and the job handles
// through weak_ptrs, so a reply that arrives after the session is gone finds
// nothing to call.
struct StageToolSession::State {
  std::shared_ptr<IStageToolTransport> transport;
  bool open = true;
  OrderedReplies<orc::view::ToolReply> requests;
  OrderedReplies<orc::view::ToolFrameImage> renders;
  std::function<void()> results_changed;
  std::function<void(uint64_t)> preview_frame;
  std::function<void()> graph_rebuilt;
  orc::view::AmplitudeUnit amplitude_unit = orc::view::AmplitudeUnit::IRE;
  std::function<void(orc::view::AmplitudeUnit)> amplitude_unit_changed;

  // The running job, if any.
  struct Job {
    uint64_t token = 0;
    uint64_t transport_id = 0;
    bool started = false;
    orc::view::ToolProgressHandler on_progress;
    // One of the two, as the job was started by run_job() or run_analysis().
    orc::view::ToolReplyHandler on_reply;
    orc::view::ToolAnalysisHandler on_report;
  };
  bool job_running = false;
  Job job;
  uint64_t next_job_token = 1;
  // Applies in flight, by sequence number. Each is independent: an apply is
  // answered when its own write and rebuild are done.
  uint64_t next_apply = 0;
  std::map<uint64_t, orc::view::ToolReplyHandler> applies;

  bool still_open() const { return open; }
};

// The handle run_job() returns.
class StageToolSession::Job final : public orc::view::IToolJob {
 public:
  Job(std::weak_ptr<State> state, uint64_t token)
      : state_(std::move(state)), token_(token) {}

  void cancel() override {
    auto state = state_.lock();
    if (state && state->open && state->job_running &&
        state->job.token == token_ && state->job.started && state->transport) {
      state->transport->cancel_job(state->job.transport_id);
    }
  }

  bool is_running() const override {
    auto state = state_.lock();
    return state && state->open && state->job_running &&
           state->job.token == token_;
  }

 private:
  std::weak_ptr<State> state_;
  uint64_t token_;
};

StageToolSession::StageToolSession(
    std::shared_ptr<IStageToolTransport> transport, NodeID node,
    std::string contract_id, std::string title,
    orc::view::AmplitudeUnit amplitude_unit)
    : node_(node),
      contract_id_(std::move(contract_id)),
      title_(std::move(title)),
      state_(std::make_shared<State>()) {
  state_->transport = std::move(transport);
  state_->amplitude_unit = amplitude_unit;
}

StageToolSession::~StageToolSession() { close(); }

bool StageToolSession::is_open() const { return state_->open; }

void StageToolSession::close() {
  if (state_->open && state_->job_running && state_->job.started &&
      state_->transport) {
    state_->transport->cancel_job(state_->job.transport_id);
  }
  state_->open = false;
  state_->requests.clear();
  state_->renders.clear();
  state_->applies.clear();
  state_->job_running = false;
  state_->job = State::Job{};
  state_->results_changed = nullptr;
  state_->preview_frame = nullptr;
  state_->graph_rebuilt = nullptr;
  state_->amplitude_unit_changed = nullptr;
}

void StageToolSession::set_results_changed_handler(
    std::function<void()> handler) {
  if (state_->open) {
    state_->results_changed = std::move(handler);
  }
}

void StageToolSession::notifyResultsChanged() {
  if (!state_->open || !state_->results_changed) {
    return;
  }
  // Copied first: the handler may replace itself or close the session.
  auto handler = state_->results_changed;
  handler();
}

void StageToolSession::set_preview_frame_handler(
    std::function<void(uint64_t)> handler) {
  if (state_->open) {
    state_->preview_frame = std::move(handler);
  }
}

void StageToolSession::notifyPreviewFrame(uint64_t frame_index) {
  if (!state_->open || !state_->preview_frame) {
    return;
  }
  auto handler = state_->preview_frame;
  handler(frame_index);
}

void StageToolSession::set_graph_rebuilt_handler(
    std::function<void()> handler) {
  if (state_->open) {
    state_->graph_rebuilt = std::move(handler);
  }
}

bool StageToolSession::keepsOpenAcrossRebuild() const {
  return state_->open && static_cast<bool>(state_->graph_rebuilt);
}

void StageToolSession::notifyGraphRebuilt() {
  if (!state_->open || !state_->graph_rebuilt) {
    return;
  }
  auto handler = state_->graph_rebuilt;
  handler();
}

orc::view::AmplitudeUnit StageToolSession::amplitude_unit() const {
  return state_->amplitude_unit;
}

void StageToolSession::set_amplitude_unit_handler(
    std::function<void(orc::view::AmplitudeUnit)> handler) {
  if (state_->open) {
    state_->amplitude_unit_changed = std::move(handler);
  }
}

void StageToolSession::notifyAmplitudeUnit(orc::view::AmplitudeUnit unit) {
  if (!state_->open || unit == state_->amplitude_unit) {
    return;
  }
  state_->amplitude_unit = unit;
  if (state_->amplitude_unit_changed) {
    auto handler = state_->amplitude_unit_changed;
    handler(unit);
  }
}

orc::view::ToolParameterMap StageToolSession::parameters() const {
  if (!state_->open || !state_->transport) {
    return {};
  }
  return state_->transport->parameters(node_);
}

std::string StageToolSession::project_directory() const {
  if (!state_->open || !state_->transport) {
    return {};
  }
  return state_->transport->project_directory();
}

void StageToolSession::request(const std::string& verb,
                               std::vector<uint8_t> payload,
                               orc::view::ToolReplyHandler on_reply) {
  if (!state_->open || !state_->transport) {
    return;
  }
  const uint64_t sequence = state_->requests.add(std::move(on_reply));
  std::weak_ptr<State> weak = state_;
  state_->transport->send(node_, contract_id_, verb, std::move(payload),
                          [weak, sequence](orc::view::ToolReply reply) {
                            auto state = weak.lock();
                            if (!state || !state->open) {
                              return;
                            }
                            state->requests.arrive(
                                sequence, std::move(reply),
                                [&state] { return state->still_open(); });
                          });
}

void StageToolSession::render_frame(
    uint64_t frame_index, const orc::view::ToolFrameRenderOptions& options,
    orc::view::ToolFrameHandler on_image) {
  if (!state_->open || !state_->transport) {
    return;
  }
  const uint64_t sequence = state_->renders.add(std::move(on_image));
  std::weak_ptr<State> weak = state_;
  state_->transport->render_frame(
      node_, frame_index, options,
      [weak, sequence](orc::view::ToolFrameImage image) {
        auto state = weak.lock();
        if (!state || !state->open) {
          return;
        }
        state->renders.arrive(sequence, std::move(image),
                              [&state] { return state->still_open(); });
      });
}

void StageToolSession::apply_parameters(orc::view::ToolParameterMap values,
                                        const std::string& label,
                                        orc::view::ToolReplyHandler on_done) {
  if (!state_->open || !state_->transport) {
    return;
  }
  const uint64_t sequence = state_->next_apply++;
  state_->applies[sequence] = std::move(on_done);
  std::weak_ptr<State> weak = state_;
  state_->transport->apply_parameters(
      node_, std::move(values), label,
      [weak, sequence](orc::view::ToolReply reply) {
        auto state = weak.lock();
        if (!state || !state->open) {
          return;
        }
        auto it = state->applies.find(sequence);
        if (it == state->applies.end()) {
          return;
        }
        auto handler = std::move(it->second);
        state->applies.erase(it);
        if (handler) {
          handler(std::move(reply));
        }
      });
}

void StageToolSession::show_frame(uint64_t frame_index) {
  if (state_->open && state_->transport) {
    state_->transport->show_frame(node_, frame_index);
  }
}

std::shared_ptr<orc::view::IToolJob> StageToolSession::run_job(
    const std::string& verb, std::vector<uint8_t> payload,
    orc::view::ToolProgressHandler on_progress,
    orc::view::ToolReplyHandler on_reply) {
  return startJob(std::move(on_progress), std::move(on_reply), nullptr,
                  [&](auto progress, auto reply, auto /*report*/) {
                    return state_->transport->start_job(
                        node_, contract_id_, verb, std::move(payload),
                        std::move(progress), std::move(reply));
                  });
}

std::shared_ptr<orc::view::IToolJob> StageToolSession::run_analysis(
    orc::view::ToolParameterMap parameters,
    orc::view::ToolProgressHandler on_progress,
    orc::view::ToolAnalysisHandler on_report) {
  return startJob(std::move(on_progress), nullptr, std::move(on_report),
                  [&](auto progress, auto /*reply*/, auto report) {
                    return state_->transport->start_analysis(
                        node_, contract_id_, std::move(parameters),
                        std::move(progress), std::move(report));
                  });
}

std::shared_ptr<orc::view::IToolJob> StageToolSession::startJob(
    orc::view::ToolProgressHandler on_progress,
    orc::view::ToolReplyHandler on_reply,
    orc::view::ToolAnalysisHandler on_report,
    const std::function<
        uint64_t(std::function<void(orc::view::ToolJobProgress)>,
                 std::function<void(orc::view::ToolReply)>,
                 std::function<void(orc::view::ToolAnalysisReport)>)>& start) {
  if (!state_->open || !state_->transport || state_->job_running) {
    return nullptr;
  }
  const uint64_t token = state_->next_job_token++;
  state_->job_running = true;
  state_->job = State::Job{};
  state_->job.token = token;
  state_->job.on_progress = std::move(on_progress);
  state_->job.on_reply = std::move(on_reply);
  state_->job.on_report = std::move(on_report);

  std::weak_ptr<State> weak = state_;
  const auto current = [weak, token]() -> std::shared_ptr<State> {
    auto state = weak.lock();
    if (!state || !state->open || !state->job_running ||
        state->job.token != token) {
      return nullptr;
    }
    return state;
  };
  // Ends the job, then hands @p answer to @p which of its handlers.
  const auto finish = [current](auto member, auto answer) {
    auto state = current();
    if (!state) {
      return;
    }
    auto handler = std::move(state->job.*member);
    state->job_running = false;
    state->job = State::Job{};
    if (handler) {
      handler(std::move(answer));
    }
  };
  const uint64_t transport_id = start(
      [current](orc::view::ToolJobProgress progress) {
        auto state = current();
        if (!state || !state->job.on_progress) {
          return;
        }
        // Copied: the handler may cancel the job or close the session.
        auto handler = state->job.on_progress;
        handler(std::move(progress));
      },
      [finish](orc::view::ToolReply reply) {
        finish(&State::Job::on_reply, std::move(reply));
      },
      [finish](orc::view::ToolAnalysisReport report) {
        finish(&State::Job::on_report, std::move(report));
      });
  if (state_->job_running && state_->job.token == token) {
    state_->job.transport_id = transport_id;
    state_->job.started = true;
  }
  return std::make_shared<Job>(state_, token);
}

orc::view::ToolAnalysisInfo StageToolSession::analysis_info() const {
  if (!state_->open || !state_->transport) {
    return {};
  }
  return state_->transport->analysis_info(node_, contract_id_);
}

orc::view::ToolRefreshState StageToolSession::refresh_state() const {
  if (!state_->open || !state_->transport) {
    return {};
  }
  return state_->transport->refresh_state(node_, contract_id_);
}

void StageToolSession::set_refresh(bool use_this_tool,
                                   orc::view::ToolParameterMap parameters,
                                   orc::view::ToolReplyHandler on_done) {
  if (!state_->open || !state_->transport) {
    return;
  }
  std::weak_ptr<State> weak = state_;
  state_->transport->set_refresh(
      node_, contract_id_, use_this_tool, std::move(parameters),
      [weak, on_done = std::move(on_done)](orc::view::ToolReply reply) {
        auto state = weak.lock();
        if (state && state->open && on_done) {
          on_done(std::move(reply));
        }
      });
}

}  // namespace orc::presenters
