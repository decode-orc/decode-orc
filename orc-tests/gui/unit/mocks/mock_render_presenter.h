/*
 * File:        mock_render_presenter.h
 * Module:      orc-tests/gui/unit
 * Purpose:     GMock scaffold for RenderCoordinator's IRenderPresenter seam
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <gmock/gmock.h>
#include <orc/stage/observation/observation_context.h>

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "render_coordinator.h"

namespace orc::presenters::test {

class MockRenderPresenter : public IRenderPresenter {
 public:
  MOCK_METHOD(void, setDAG, (std::shared_ptr<void> dag_handle), (override));
  MOCK_METHOD(bool, getShowDropouts, (), (const, override));
  MOCK_METHOD(void, setShowDropouts, (bool show), (override));
  MOCK_METHOD(void, setBackgroundObservationEnabled, (bool enabled),
              (override));

  // Real (non-mocked) execution-progress sink so tests can drive the
  // coordinator's worker-thread callback -> queued-signal wiring end to end.
  void setExecutionProgressCallback(
      orc::presenters::DagExecutionProgressCallback callback) override {
    std::lock_guard<std::mutex> lock(execution_progress_mutex_);
    execution_progress_ = std::move(callback);
  }

  // Test helper: simulate a node of an on-demand execution starting.
  void fireExecutionProgress(int node_id, std::uint64_t current,
                             std::uint64_t total) {
    orc::presenters::DagExecutionProgressCallback callback;
    {
      std::lock_guard<std::mutex> lock(execution_progress_mutex_);
      callback = execution_progress_;
    }
    if (!callback) {
      return;
    }
    orc::presenters::DagExecutionProgressEvent event;
    event.node_id = node_id;
    event.current = current;
    event.total = total;
    callback(event);
  }

  // Test helper: whether an execution-progress observer is installed.
  bool hasExecutionProgressCallback() {
    std::lock_guard<std::mutex> lock(execution_progress_mutex_);
    return static_cast<bool>(execution_progress_);
  }

  // Real (non-mocked) invalidation registry so tests can exercise the
  // coordinator's subscribe/fire/unsubscribe wiring end to end.
  uint64_t subscribeInvalidation(
      orc::presenters::ObservationInvalidationCallback callback) override {
    std::lock_guard<std::mutex> lock(invalidation_mutex_);
    const uint64_t id = next_invalidation_id_++;
    invalidation_subscribers_.emplace(id, std::move(callback));
    return id;
  }

  void unsubscribeInvalidation(uint64_t subscription_id) override {
    std::lock_guard<std::mutex> lock(invalidation_mutex_);
    invalidation_subscribers_.erase(subscription_id);
  }

  // Test helper: simulate a project edit invalidating the given node ids.
  void fireInvalidation(const std::vector<orc::NodeID>& changed_nodes) {
    std::vector<orc::presenters::ObservationInvalidationCallback> callbacks;
    {
      std::lock_guard<std::mutex> lock(invalidation_mutex_);
      for (const auto& [id, cb] : invalidation_subscribers_) {
        callbacks.push_back(cb);
      }
    }
    orc::presenters::ObservationInvalidationEvent event;
    event.changed_nodes = changed_nodes;
    for (const auto& cb : callbacks) {
      if (cb) {
        cb(event);
      }
    }
  }

  // Test helper: number of active invalidation subscribers.
  std::size_t invalidationSubscriberCount() {
    std::lock_guard<std::mutex> lock(invalidation_mutex_);
    return invalidation_subscribers_.size();
  }

  // --- Async observations (Phase 5, Task 5.1) ------------------------------
  // Controllable seam: when immediate-answer mode is on, the callback fires
  // synchronously (store hit); otherwise the request is queued for a test to
  // release later (store miss / deferred). Deliveries carry a real (empty)
  // ObservationContext so the coordinator's extraction path is exercised.

  uint64_t requestObservations(
      NodeID /*node_id*/, FieldID /*field_id*/,
      orc::presenters::ObservationDataReadyCallback callback) override {
    const uint64_t id = next_obs_request_id_++;
    if (immediate_answer_) {
      if (callback) {
        callback(id, true, &delivered_context_);
      }
      return id;
    }
    std::lock_guard<std::mutex> lock(obs_mutex_);
    pending_obs_.push_back({id, std::move(callback)});
    return id;
  }

  void setImmediateAnswer(bool on) { immediate_answer_ = on; }

  // Deliver the oldest deferred request. Returns false if none are pending.
  bool deliverOldestObservation(bool available) {
    PendingObs p;
    {
      std::lock_guard<std::mutex> lock(obs_mutex_);
      if (pending_obs_.empty()) {
        return false;
      }
      p = std::move(pending_obs_.front());
      pending_obs_.pop_front();
    }
    if (p.callback) {
      p.callback(p.request_id, available,
                 available ? &delivered_context_ : nullptr);
    }
    return true;
  }

  std::size_t pendingObservationCount() {
    std::lock_guard<std::mutex> lock(obs_mutex_);
    return pending_obs_.size();
  }

  // Test helper: the ObservationContext handed to delivery callbacks. Seed it
  // with observations to exercise the coordinator's extraction paths.
  orc::ObservationContext& deliveredContext() { return delivered_context_; }

  // --- Workload progress (Phase 5, Task 5.4) -------------------------------

  uint64_t subscribeObservationProgress(
      orc::presenters::ObservationProgressCallback callback) override {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    const uint64_t id = next_progress_id_++;
    progress_subscribers_.emplace(id, std::move(callback));
    return id;
  }

  void unsubscribeObservationProgress(uint64_t subscription_id) override {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_subscribers_.erase(subscription_id);
  }

  // Test helper: simulate a scheduler workload snapshot.
  void fireProgress(const orc::presenters::ObservationProgressEvent& event) {
    std::vector<orc::presenters::ObservationProgressCallback> callbacks;
    {
      std::lock_guard<std::mutex> lock(progress_mutex_);
      for (const auto& [id, cb] : progress_subscribers_) {
        callbacks.push_back(cb);
      }
    }
    for (const auto& cb : callbacks) {
      if (cb) {
        cb(event);
      }
    }
  }

  std::size_t progressSubscriberCount() {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    return progress_subscribers_.size();
  }

  MOCK_METHOD(orc::PreviewRenderResult, renderPreview,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, const std::string& option_id,
               orc::PreviewNavigationHint hint),
              (override));

  MOCK_METHOD((std::optional<orc::presenters::DropoutDisplaySeries>),
              getDropoutAnalysisData, (NodeID node_id), (override));
  MOCK_METHOD((std::optional<orc::presenters::SNRDisplaySeries>),
              getSNRAnalysisData, (NodeID node_id), (override));
  MOCK_METHOD((std::optional<orc::presenters::BurstLevelDisplaySeries>),
              getBurstLevelAnalysisData, (NodeID node_id), (override));
  MOCK_METHOD((std::optional<orc::CatalogueDataset>), getCatalogueData,
              (NodeID node_id, const std::string& view_option,
               (const std::optional<std::vector<std::string>>&)active_toggles),
              (override));
  MOCK_METHOD((std::vector<orc::PreviewOutputInfo>), getAvailableOutputs,
              (NodeID node_id), (override));

  MOCK_METHOD((std::vector<orc::AudioPairView>), getAudioChannelPairs,
              (NodeID node_id), (override));
  MOCK_METHOD((std::shared_ptr<orc::presenters::IAudioStreamReader>),
              createAudioStreamReader, (NodeID node_id, size_t pair),
              (override));

  MOCK_METHOD(LineSampleData, getLineSamplesWithYC,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, int line_number, int sample_x,
               int preview_width),
              (override));

  MOCK_METHOD((std::optional<orc::SourceParameters>), getVideoParameters,
              (NodeID node_id), (override));

  MOCK_METHOD(LineSampleData, getFieldSamplesForTiming,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index),
              (override));

  MOCK_METHOD(orc::FrameLineNavigationResult, navigateFrameLine,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t current_field, int current_line, int direction,
               int field_height),
              (override));

  MOCK_METHOD(uint64_t, triggerStage,
              (NodeID node_id, TriggerProgressCallback callback), (override));
  MOCK_METHOD(
      uint64_t, triggerStage,
      (NodeID node_id, TriggerProgressCallback callback,
       (std::map<std::string, orc::ParameterValue> parameter_overrides)),
      (override));
  MOCK_METHOD(void, cancelTrigger, (), (override));

  MOCK_METHOD(bool, savePNG,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, const std::string& filename,
               const std::string& option_id, double aspect_correction),
              (override));

  MOCK_METHOD(orc::ImageToFieldMappingResult, mapImageToField,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, int image_y, int image_height,
               const std::string& option_id),
              (override));

  MOCK_METHOD(orc::FieldToImageMappingResult, mapFieldToImage,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, uint64_t field_index, int field_line,
               int image_height, const std::string& option_id),
              (override));

  MOCK_METHOD(orc::FrameFieldsResult, getFrameFields,
              (NodeID node_id, uint64_t frame_index), (override));

  MOCK_METHOD((std::vector<orc::PreviewViewDescriptor>),
              getAvailablePreviewViews,
              (NodeID node_id, orc::VideoDataType data_type), (override));

  MOCK_METHOD((std::vector<orc::VideoDataType>), getStageDataTypes,
              (NodeID node_id), (override));

  MOCK_METHOD(orc::PreviewScopePayloads, getPreviewScopes,
              (NodeID node_id, const orc::PreviewScopeRequest& request),
              (override));

  MOCK_METHOD(orc::PreviewViewDataResult, requestPreviewViewData,
              (NodeID node_id, const std::string& view_id,
               orc::VideoDataType data_type,
               const orc::PreviewCoordinate& coordinate),
              (override));

 private:
  struct PendingObs {
    uint64_t request_id = 0;
    orc::presenters::ObservationDataReadyCallback callback;
  };

  std::mutex invalidation_mutex_;
  std::map<uint64_t, orc::presenters::ObservationInvalidationCallback>
      invalidation_subscribers_;
  uint64_t next_invalidation_id_ = 1;

  std::mutex obs_mutex_;
  std::deque<PendingObs> pending_obs_;
  uint64_t next_obs_request_id_ = 1;
  bool immediate_answer_ = false;
  orc::ObservationContext delivered_context_;

  std::mutex progress_mutex_;
  std::map<uint64_t, orc::presenters::ObservationProgressCallback>
      progress_subscribers_;
  uint64_t next_progress_id_ = 1;

  std::mutex execution_progress_mutex_;
  orc::presenters::DagExecutionProgressCallback execution_progress_;
};

}  // namespace orc::presenters::test
