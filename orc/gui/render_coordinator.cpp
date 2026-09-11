/*
 * File:        render_coordinator.cpp
 * Module:      orc-gui
 * Purpose:     Thread-safe coordinator for rendering operations using
 * presenters
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "render_coordinator.h"

#include <orc/stage/common_types.h>  // For analysis result types

#include <algorithm>

#include "closed_caption_observation_presenter.h"
#include "frame_profiler.h"
#include "logging.h"
#include "ntsc_observation_presenter.h"
#include "render_presenter.h"
#include "vbi_presenter.h"
#include "video_parameter_observation_presenter.h"

namespace {

class RenderPresenterAdapter final : public orc::presenters::IRenderPresenter {
 public:
  explicit RenderPresenterAdapter(void* project_handle)
      : presenter_(project_handle) {}

  void setDAG(std::shared_ptr<void> dag_handle) override {
    presenter_.setDAG(std::move(dag_handle));
  }
  bool getShowDropouts() const override { return presenter_.getShowDropouts(); }
  void setShowDropouts(bool show) override { presenter_.setShowDropouts(show); }
  void setBackgroundObservationEnabled(bool enabled) override {
    presenter_.setBackgroundObservationEnabled(enabled);
  }
  void setExecutionProgressCallback(
      orc::presenters::DagExecutionProgressCallback callback) override {
    presenter_.setExecutionProgressCallback(std::move(callback));
  }

  uint64_t subscribeInvalidation(
      orc::presenters::ObservationInvalidationCallback callback) override {
    return presenter_.subscribeInvalidation(std::move(callback));
  }
  void unsubscribeInvalidation(uint64_t subscription_id) override {
    presenter_.unsubscribeInvalidation(subscription_id);
  }

  uint64_t requestObservations(
      orc::NodeID node_id, orc::FieldID field_id,
      orc::presenters::ObservationDataReadyCallback callback) override {
    return presenter_.requestObservations(node_id, field_id,
                                          std::move(callback));
  }
  uint64_t subscribeObservationProgress(
      orc::presenters::ObservationProgressCallback callback) override {
    return presenter_.subscribeObservationProgress(std::move(callback));
  }
  void unsubscribeObservationProgress(uint64_t subscription_id) override {
    presenter_.unsubscribeObservationProgress(subscription_id);
  }

  orc::PreviewRenderResult renderPreview(
      orc::NodeID node_id, orc::PreviewOutputType output_type,
      uint64_t output_index, const std::string& option_id,
      orc::PreviewNavigationHint hint) override {
    return presenter_.renderPreview(node_id, output_type, output_index,
                                    option_id, hint);
  }

  std::optional<orc::presenters::DropoutDisplaySeries> getDropoutAnalysisData(
      orc::NodeID node_id) override {
    return presenter_.getDropoutAnalysisData(node_id);
  }

  std::optional<orc::presenters::SNRDisplaySeries> getSNRAnalysisData(
      orc::NodeID node_id) override {
    return presenter_.getSNRAnalysisData(node_id);
  }

  std::optional<orc::presenters::BurstLevelDisplaySeries>
  getBurstLevelAnalysisData(orc::NodeID node_id) override {
    return presenter_.getBurstLevelAnalysisData(node_id);
  }

  std::optional<orc::CatalogueDataset> getCatalogueData(
      orc::NodeID node_id, const std::string& view_option,
      const std::optional<std::vector<std::string>>& active_toggles) override {
    return presenter_.getCatalogueData(node_id, view_option, active_toggles);
  }

  std::vector<orc::PreviewOutputInfo> getAvailableOutputs(
      orc::NodeID node_id) override {
    return presenter_.getAvailableOutputs(node_id);
  }

  std::vector<orc::AudioPairView> getAudioChannelPairs(
      orc::NodeID node_id) override {
    return presenter_.getAudioChannelPairs(node_id);
  }

  std::shared_ptr<orc::presenters::IAudioStreamReader> createAudioStreamReader(
      orc::NodeID node_id, size_t pair) override {
    return presenter_.createAudioStreamReader(node_id, pair);
  }

  LineSampleData getLineSamplesWithYC(orc::NodeID node_id,
                                      orc::PreviewOutputType output_type,
                                      uint64_t output_index, int line_number,
                                      int sample_x,
                                      int preview_width) override {
    auto source =
        presenter_.getLineSamplesWithYC(node_id, output_type, output_index,
                                        line_number, sample_x, preview_width);
    return LineSampleData{
        std::move(source.composite_samples), std::move(source.y_samples),
        std::move(source.c_samples),         source.has_separate_channels,
        source.first_field_height,           source.second_field_height,
    };
  }

  std::optional<orc::SourceParameters> getVideoParameters(
      orc::NodeID node_id) override {
    return presenter_.getVideoParameters(node_id);
  }

  LineSampleData getFieldSamplesForTiming(orc::NodeID node_id,
                                          orc::PreviewOutputType output_type,
                                          uint64_t output_index) override {
    auto source =
        presenter_.getFieldSamplesForTiming(node_id, output_type, output_index);
    return LineSampleData{
        std::move(source.composite_samples), std::move(source.y_samples),
        std::move(source.c_samples),         source.has_separate_channels,
        source.first_field_height,           source.second_field_height,
    };
  }

  orc::FrameLineNavigationResult navigateFrameLine(
      orc::NodeID node_id, orc::PreviewOutputType output_type,
      uint64_t current_field, int current_line, int direction,
      int field_height) override {
    auto result =
        presenter_.navigateFrameLine(node_id, output_type, current_field,
                                     current_line, direction, field_height);
    return orc::FrameLineNavigationResult{
        result.is_valid, result.new_field_index, result.new_line_number};
  }

  uint64_t triggerStage(orc::NodeID node_id,
                        TriggerProgressCallback callback) override {
    return presenter_.triggerStage(node_id, std::move(callback));
  }

  uint64_t triggerStage(
      orc::NodeID node_id, TriggerProgressCallback callback,
      std::map<std::string, orc::ParameterValue> parameter_overrides) override {
    return presenter_.triggerStage(node_id, std::move(callback),
                                   std::move(parameter_overrides));
  }

  void cancelTrigger() override { presenter_.cancelTrigger(); }

  bool savePNG(orc::NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, const std::string& filename,
               const std::string& option_id,
               double aspect_correction) override {
    return presenter_.savePNG(node_id, output_type, output_index, filename,
                              option_id, aspect_correction);
  }

  orc::ImageToFieldMappingResult mapImageToField(
      orc::NodeID node_id, orc::PreviewOutputType output_type,
      uint64_t output_index, int image_y, int image_height,
      const std::string& option_id) override {
    auto result = presenter_.mapImageToField(node_id, output_type, output_index,
                                             image_y, image_height, option_id);
    return orc::ImageToFieldMappingResult{result.is_valid, result.field_index,
                                          result.field_line};
  }

  orc::FieldToImageMappingResult mapFieldToImage(
      orc::NodeID node_id, orc::PreviewOutputType output_type,
      uint64_t output_index, uint64_t field_index, int field_line,
      int image_height, const std::string& option_id) override {
    auto result = presenter_.mapFieldToImage(node_id, output_type, output_index,
                                             field_index, field_line,
                                             image_height, option_id);
    return orc::FieldToImageMappingResult{result.is_valid, result.image_y};
  }

  orc::FrameFieldsResult getFrameFields(orc::NodeID node_id,
                                        uint64_t frame_index) override {
    auto result = presenter_.getFrameFields(node_id, frame_index);
    return orc::FrameFieldsResult{result.is_valid, result.first_field,
                                  result.second_field};
  }

  std::vector<orc::PreviewViewDescriptor> getAvailablePreviewViews(
      orc::NodeID node_id, orc::VideoDataType data_type) override {
    return presenter_.getAvailablePreviewViews(node_id, data_type);
  }

  std::vector<orc::VideoDataType> getStageDataTypes(
      orc::NodeID node_id) override {
    return presenter_.getStageDataTypes(node_id);
  }

  orc::PreviewViewDataResult requestPreviewViewData(
      orc::NodeID node_id, const std::string& view_id,
      orc::VideoDataType data_type,
      const orc::PreviewCoordinate& coordinate) override {
    return presenter_.requestPreviewViewData(node_id, view_id, data_type,
                                             coordinate);
  }

  orc::PreviewScopePayloads getPreviewScopes(
      orc::NodeID node_id, const orc::PreviewScopeRequest& request) override {
    return presenter_.getPreviewScopes(node_id, request);
  }

 private:
  orc::presenters::RenderPresenter presenter_;
};

}  // namespace

std::shared_ptr<orc::presenters::IRenderPresenter> makeRenderPresenterAdapter(
    void* project_handle) {
  return std::make_shared<RenderPresenterAdapter>(project_handle);
}

// Forward declarations for core types used via opaque pointers
namespace orc {
class DAG;
class Project;
}  // namespace orc

// Phase 2.4: Analysis sink stage headers removed - now using RenderPresenter
// abstraction Removed: #include "dropout_analysis_sink_stage.h" Removed:
// #include "snr_analysis_sink_stage.h" Removed: #include
// "burst_level_analysis_sink_stage.h"

// Phase 2.7: Trigger operations migrated to RenderPresenter
// Removed: #include "tbc_sink_stage.h"

namespace {

// Wrap a progress-emitting function so it only fires when the whole-percent
// value or the message changes (final updates always pass). Stage triggers
// report progress per processed frame; forwarding every call as a queued Qt
// signal floods the GUI event queue (tens of thousands of events for a long
// source) and starves painting — the cause of a blank, beach-balling progress
// dialog. The gate runs on the worker thread, before anything is queued.
template <typename EmitFn>
auto makePercentGatedProgress(EmitFn emit_fn) {
  return [emit_fn = std::move(emit_fn), last_pct = -1,
          last_message = std::string()](int current, int total,
                                        const std::string& message) mutable {
    const int pct =
        total > 0
            ? static_cast<int>(static_cast<int64_t>(current) * 100 / total)
            : 0;
    const bool final_update = total > 0 && current >= total;
    if (pct == last_pct && message == last_message && !final_update) {
      return;
    }
    last_pct = pct;
    last_message = message;
    emit_fn(current, total, message);
  };
}

}  // namespace

RenderCoordinator::RenderCoordinator(QObject* parent)
    : QObject(parent), presenter_factory_([](void* project_handle) {
        return std::make_shared<RenderPresenterAdapter>(project_handle);
      }) {}

RenderCoordinator::RenderCoordinator(RenderPresenterFactory presenter_factory,
                                     QObject* parent)
    : QObject(parent), presenter_factory_(std::move(presenter_factory)) {}

RenderCoordinator::~RenderCoordinator() { stop(); }

void RenderCoordinator::start() {
  if (worker_thread_.joinable()) {
    ORC_LOG_WARN("RenderCoordinator: Worker thread already running");
    return;
  }

  shutdown_requested_ = false;
  worker_thread_ = std::thread(&RenderCoordinator::workerLoop, this);

  ORC_LOG_DEBUG("RenderCoordinator: Worker thread started");
}

void RenderCoordinator::stop() {
  if (!worker_thread_.joinable()) {
    return;
  }

  ORC_LOG_DEBUG("RenderCoordinator: Requesting shutdown...");

  // Send shutdown request
  shutdown_requested_ = true;

  // Wake up worker if waiting
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_cv_.notify_one();
  }

  // Wait for worker to finish
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }

  ORC_LOG_DEBUG("RenderCoordinator: Worker thread stopped");
}

uint64_t RenderCoordinator::nextRequestId() {
  return next_request_id_.fetch_add(1);
}

void RenderCoordinator::enqueueRequest(std::unique_ptr<RenderRequest> request) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    request_queue_.push_back(std::move(request));
  }
  queue_cv_.notify_one();
}

size_t RenderCoordinator::discardQueuedPreviewsLocked() {
  const size_t before = request_queue_.size();
  auto end = std::remove_if(
      request_queue_.begin(), request_queue_.end(),
      [](const std::unique_ptr<RenderRequest>& queued) {
        return queued && queued->type == RenderRequestType::RenderPreview;
      });
  request_queue_.erase(end, request_queue_.end());
  return before - request_queue_.size();
}

void RenderCoordinator::updateDAG(std::shared_ptr<const void> dag) {
  auto req =
      std::make_unique<UpdateDAGRequest>(nextRequestId(), std::move(dag));
  enqueueRequest(std::move(req));
}

void RenderCoordinator::setProject(void* project) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  worker_project_ = project;
}

uint64_t RenderCoordinator::requestPreview(
    const orc::NodeID& node_id, orc::PreviewOutputType output_type,
    uint64_t output_index, const std::string& option_id,
    orc::PreviewNavigationHint hint, const orc::PreviewScopeRequest& scopes) {
  uint64_t id = nextRequestId();
  latest_preview_request_id_.store(id);
  auto req = std::make_unique<RenderPreviewRequest>(
      id, node_id, output_type, output_index, option_id, hint, scopes);

  size_t discarded = 0;
  size_t queue_depth = 0;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    // Only the newest preview is ever displayed, so anything still queued is
    // already dead work. Removing it here (rather than rendering it and
    // dropping the response) is what lets a burst of navigations reach the
    // latest frame in one render instead of N.
    discarded = discardQueuedPreviewsLocked();
    request_queue_.push_back(std::move(req));
    queue_depth = request_queue_.size();
  }
  queue_cv_.notify_one();

  // Depth is only knowable under the queue lock, and this is the one enqueue
  // site the GUI thread drives per displayed frame, so the profiler is told
  // here rather than from the caller.
  orc::gui::FrameProfiler::instance().markPreviewRequested(
      static_cast<int>(queue_depth));

  if (discarded > 0) {
    ORC_LOG_DEBUG(
        "RenderCoordinator: Discarded {} superseded queued preview request(s) "
        "for request {}",
        discarded, id);
  }
  return id;
}

uint64_t RenderCoordinator::requestVBIData(const orc::NodeID& node_id,
                                           orc::FieldID field_id) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetVBIDataRequest>(id, node_id, field_id);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestClosedCaptionData(const orc::NodeID& node_id,
                                                     orc::FieldID field_id) {
  uint64_t id = nextRequestId();
  auto req =
      std::make_unique<GetClosedCaptionDataRequest>(id, node_id, field_id);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestObservations(const orc::NodeID& node_id,
                                                orc::FieldID field_id) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetObservationsRequest>(id, node_id, field_id);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestDropoutData(const orc::NodeID& node_id,
                                               orc::DropoutAnalysisMode mode) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetDropoutDataRequest>(id, node_id, mode);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestSNRData(const orc::NodeID& node_id,
                                           orc::SNRAnalysisMode mode) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetSNRDataRequest>(id, node_id, mode);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestBurstLevelData(const orc::NodeID& node_id) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetBurstLevelDataRequest>(id, node_id);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestCatalogueData(
    const orc::NodeID& node_id, const std::string& view_option,
    const std::optional<std::vector<std::string>>& active_toggles) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetCatalogueDataRequest>(id, node_id, view_option,
                                                       active_toggles);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestAvailableOutputs(
    const orc::NodeID& node_id) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetAvailableOutputsRequest>(id, node_id);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestAudioChannelPairs(
    const orc::NodeID& node_id) {
  uint64_t id = nextRequestId();
  latest_audio_pairs_request_id_.store(id);
  auto req = std::make_unique<GetAudioChannelPairsRequest>(id, node_id);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestAudioStreamReader(const orc::NodeID& node_id,
                                                     size_t pair) {
  uint64_t id = nextRequestId();
  latest_audio_reader_request_id_.store(id);
  auto req =
      std::make_unique<CreateAudioStreamReaderRequest>(id, node_id, pair);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestLineSamples(
    const orc::NodeID& node_id, orc::PreviewOutputType output_type,
    uint64_t output_index, int line_number, int sample_x,
    int preview_image_width) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetLineSamplesRequest>(
      id, node_id, output_type, output_index, line_number, sample_x,
      preview_image_width);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestFrameSamples(
    const orc::NodeID& node_id, orc::PreviewOutputType output_type,
    uint64_t output_index, bool for_frame_timing, bool for_waveform_monitor) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetFrameSamplesRequest>(
      id, node_id, output_type, output_index, for_frame_timing,
      for_waveform_monitor);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestSavePNG(const orc::NodeID& node_id,
                                           orc::PreviewOutputType output_type,
                                           uint64_t output_index,
                                           const std::string& filename,
                                           const std::string& option_id,
                                           double aspect_correction) {
  uint64_t id = nextRequestId();
  auto req =
      std::make_unique<SavePNGRequest>(id, node_id, output_type, output_index,
                                       filename, option_id, aspect_correction);
  enqueueRequest(std::move(req));
  return id;
}

orc::ImageToFieldMappingResult RenderCoordinator::mapImageToField(
    const orc::NodeID& node_id, orc::PreviewOutputType output_type,
    uint64_t output_index, int image_y, int image_height,
    const std::string& option_id) {
  // This is a synchronous call - safe to call render presenter directly
  // since it's just a calculation with no state changes
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!worker_render_presenter_) {
    return orc::ImageToFieldMappingResult{false, 0, 0};
  }
  return worker_render_presenter_->mapImageToField(
      node_id, output_type, output_index, image_y, image_height, option_id);
}

orc::FieldToImageMappingResult RenderCoordinator::mapFieldToImage(
    const orc::NodeID& node_id, orc::PreviewOutputType output_type,
    uint64_t output_index, uint64_t field_index, int field_line,
    int image_height, const std::string& option_id) {
  // This is a synchronous call - safe to call render presenter directly
  // since it's just a calculation with no state changes
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!worker_render_presenter_) {
    return orc::FieldToImageMappingResult{false, 0};
  }
  return worker_render_presenter_->mapFieldToImage(
      node_id, output_type, output_index, field_index, field_line, image_height,
      option_id);
}

orc::FrameFieldsResult RenderCoordinator::getFrameFields(
    const orc::NodeID& node_id, uint64_t frame_index) {
  // This is a synchronous call - safe to call render presenter directly
  // since it's just a calculation with no state changes
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!worker_render_presenter_) {
    return orc::FrameFieldsResult{false, 0, 0};
  }
  return worker_render_presenter_->getFrameFields(node_id, frame_index);
}

std::vector<orc::PreviewViewDescriptor>
RenderCoordinator::getAvailablePreviewViews(const orc::NodeID& node_id,
                                            orc::VideoDataType data_type) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!worker_render_presenter_) {
    return {};
  }
  return worker_render_presenter_->getAvailablePreviewViews(node_id, data_type);
}

std::vector<orc::VideoDataType> RenderCoordinator::getStageDataTypes(
    const orc::NodeID& node_id) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!worker_render_presenter_) {
    return {};
  }
  return worker_render_presenter_->getStageDataTypes(node_id);
}

orc::PreviewViewDataResult RenderCoordinator::requestPreviewViewData(
    const orc::NodeID& node_id, const std::string& view_id,
    orc::VideoDataType data_type, const orc::PreviewCoordinate& coordinate) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!worker_render_presenter_) {
    return {false, "Render presenter not initialized",
            orc::PreviewViewPayloadKind::None, std::nullopt, std::nullopt};
  }
  return worker_render_presenter_->requestPreviewViewData(
      node_id, view_id, data_type, coordinate);
}

uint64_t RenderCoordinator::requestTrigger(const orc::NodeID& node_id) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<TriggerStageRequest>(id, node_id);
  enqueueRequest(std::move(req));
  return id;
}

void RenderCoordinator::cancelTrigger() {
  // Call cancelTrigger on the presenter (thread-safe)
  // The presenter's implementation sets a flag that the trigger operation will
  // check
  if (worker_render_presenter_) {
    worker_render_presenter_->cancelTrigger();
  }
  ORC_LOG_DEBUG("RenderCoordinator: Trigger cancellation requested");
}

// ============================================================================
// Worker Thread Implementation
// ============================================================================

void RenderCoordinator::workerLoop() {
  ORC_LOG_DEBUG("RenderCoordinator: Worker thread loop started");

  while (!shutdown_requested_) {
    std::unique_ptr<RenderRequest> request;

    // Wait for a request
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] {
        return !request_queue_.empty() || shutdown_requested_;
      });

      if (shutdown_requested_) {
        break;
      }

      if (!request_queue_.empty()) {
        request = std::move(request_queue_.front());
        request_queue_.pop_front();
      }
    }

    // Process the request
    if (request) {
      try {
        processRequest(std::move(request));
      } catch (const std::exception& e) {
        ORC_LOG_ERROR("RenderCoordinator: Exception processing request: {}",
                      e.what());
        emit error(request->request_id, QString::fromStdString(e.what()));
      } catch (...) {
        ORC_LOG_ERROR(
            "RenderCoordinator: Unknown exception processing request");
        emit error(request->request_id, "Unknown error");
      }
    }
  }

  // Tear the presenter down here, on the worker thread, so its background
  // scheduler is stopped (and any awaited observation callbacks fire, emitting
  // their final signals) while this coordinator QObject is still fully alive.
  if (worker_render_presenter_ && worker_invalidation_subscription_ != 0) {
    worker_render_presenter_->unsubscribeInvalidation(
        worker_invalidation_subscription_);
    worker_invalidation_subscription_ = 0;
  }
  if (worker_render_presenter_ && worker_progress_subscription_ != 0) {
    worker_render_presenter_->unsubscribeObservationProgress(
        worker_progress_subscription_);
    worker_progress_subscription_ = 0;
  }
  worker_render_presenter_.reset();

  ORC_LOG_DEBUG("RenderCoordinator: Worker thread loop exiting");
}

void RenderCoordinator::processRequest(std::unique_ptr<RenderRequest> request) {
  switch (request->type) {
    case RenderRequestType::UpdateDAG:
      handleUpdateDAG(*static_cast<UpdateDAGRequest*>(request.get()));
      break;

    case RenderRequestType::RenderPreview:
      handleRenderPreview(*static_cast<RenderPreviewRequest*>(request.get()));
      break;

    case RenderRequestType::GetObservations:
      handleGetObservations(
          *static_cast<GetObservationsRequest*>(request.get()));
      break;

    case RenderRequestType::GetVBIData:
      handleGetVBIData(*static_cast<GetVBIDataRequest*>(request.get()));
      break;

    case RenderRequestType::GetClosedCaptionData:
      handleGetClosedCaptionData(
          *static_cast<GetClosedCaptionDataRequest*>(request.get()));
      break;

    case RenderRequestType::GetDropoutData:
      handleGetDropoutData(*static_cast<GetDropoutDataRequest*>(request.get()));
      break;

    case RenderRequestType::GetSNRData:
      handleGetSNRData(*static_cast<GetSNRDataRequest*>(request.get()));
      break;

    case RenderRequestType::GetBurstLevelData:
      handleGetBurstLevelData(
          *static_cast<GetBurstLevelDataRequest*>(request.get()));
      break;

    case RenderRequestType::GetCatalogueData:
      handleGetCatalogueData(
          *static_cast<GetCatalogueDataRequest*>(request.get()));
      break;

    case RenderRequestType::GetAvailableOutputs:
      handleGetAvailableOutputs(
          *static_cast<GetAvailableOutputsRequest*>(request.get()));
      break;

    case RenderRequestType::GetAudioChannelPairs:
      handleGetAudioChannelPairs(
          *static_cast<GetAudioChannelPairsRequest*>(request.get()));
      break;

    case RenderRequestType::CreateAudioStreamReader:
      handleCreateAudioStreamReader(
          *static_cast<CreateAudioStreamReaderRequest*>(request.get()));
      break;

    case RenderRequestType::GetLineSamples:
      handleGetLineSamples(*static_cast<GetLineSamplesRequest*>(request.get()));
      break;

    case RenderRequestType::GetFrameSamples:
      handleGetFrameSamples(
          *static_cast<GetFrameSamplesRequest*>(request.get()));
      break;

    case RenderRequestType::SavePNG:
      handleSavePNG(*static_cast<SavePNGRequest*>(request.get()));
      break;

    case RenderRequestType::NavigateFrameLine:
      handleNavigateFrameLine(
          *static_cast<NavigateFrameLineRequest*>(request.get()));
      break;

    case RenderRequestType::TriggerStage:
      handleTriggerStage(*static_cast<TriggerStageRequest*>(request.get()));
      break;

    case RenderRequestType::Shutdown:
      shutdown_requested_ = true;
      break;

    default:
      ORC_LOG_WARN("RenderCoordinator: Unknown request type: {}",
                   static_cast<int>(request->type));
      break;
  }
}

void RenderCoordinator::handleUpdateDAG(const UpdateDAGRequest& req) {
  ORC_LOG_DEBUG("RenderCoordinator: Updating DAG (request {})", req.request_id);

  if (!req.dag) {
    // Null DAG is valid - happens with empty projects or projects with no
    // stages
    ORC_LOG_WARN(
        "RenderCoordinator: Received null DAG (empty project with no stages)");

    // Clear all worker state
    worker_dag_.reset();
    if (worker_render_presenter_ && worker_invalidation_subscription_ != 0) {
      worker_render_presenter_->unsubscribeInvalidation(
          worker_invalidation_subscription_);
      worker_invalidation_subscription_ = 0;
    }
    if (worker_render_presenter_ && worker_progress_subscription_ != 0) {
      worker_render_presenter_->unsubscribeObservationProgress(
          worker_progress_subscription_);
      worker_progress_subscription_ = 0;
    }
    worker_render_presenter_.reset();

    ORC_LOG_DEBUG(
        "RenderCoordinator: Cleared all rendering state for empty project");
    return;
  }

  // Save current show_dropouts state before recreating presenter
  bool show_dropouts = false;
  if (worker_render_presenter_) {
    show_dropouts = worker_render_presenter_->getShowDropouts();
    ORC_LOG_DEBUG("RenderCoordinator: Preserving show_dropouts={}",
                  show_dropouts);
  }

  // Update DAG
  worker_dag_ = req.dag;

  // Create or update render presenter
  try {
    if (!worker_project_) {
      ORC_LOG_ERROR("RenderCoordinator: No project set for presenter");
      return;
    }

    if (!worker_render_presenter_) {
      worker_render_presenter_ = presenter_factory_(worker_project_);

      // Subscribe to invalidation notifications and re-emit them on the GUI
      // thread. The callback fires synchronously on the worker thread inside
      // setDAG(); the queued signal marshals to the GUI thread.
      worker_invalidation_subscription_ =
          worker_render_presenter_->subscribeInvalidation(
              [this](
                  const orc::presenters::ObservationInvalidationEvent& event) {
                QVector<int> ids;
                ids.reserve(static_cast<int>(event.changed_nodes.size()));
                for (const auto& node : event.changed_nodes) {
                  ids.push_back(node.value());
                }
                emit observationsInvalidated(ids);
              });

      // Subscribe to background-workload snapshots and re-emit them on the GUI
      // thread (Task 5.4). The callback fires on the scheduler's worker thread;
      // the queued signal marshals to the GUI thread.
      worker_progress_subscription_ =
          worker_render_presenter_->subscribeObservationProgress(
              [this](const orc::presenters::ObservationProgressEvent& event) {
                emit observationProgress(
                    event.active, event.percent_complete, event.computing,
                    static_cast<qulonglong>(event.outstanding_nodes));
              });

      // Report on-demand execution so the view can show what the worker is
      // doing while a large source is opened. The callback fires on this
      // worker thread inside a request; the queued signal marshals to the GUI
      // thread. Node-granular and low-rate (one event per node), so unlike the
      // per-frame trigger progress it needs no rate gate.
      worker_render_presenter_->setExecutionProgressCallback(
          [this](const orc::presenters::DagExecutionProgressEvent& event) {
            emit executionProgress(event.node_id,
                                   static_cast<qulonglong>(event.current),
                                   static_cast<qulonglong>(event.total));
          });
    }

    // Set the new DAG (cast away const since setDAG signature uses non-const
    // void*)
    worker_render_presenter_->setDAG(
        std::const_pointer_cast<void>(worker_dag_));

    // Restore show_dropouts state
    worker_render_presenter_->setShowDropouts(show_dropouts);
    ORC_LOG_DEBUG("RenderCoordinator: Restored show_dropouts={}",
                  show_dropouts);

    ORC_LOG_DEBUG("RenderCoordinator: DAG updated successfully");
  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: Failed to create presenter: {}",
                  e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleRenderPreview(const RenderPreviewRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Rendering preview for node '{}', type {}, index {} "
      "(request {})",
      req.node_id.to_string(), static_cast<int>(req.output_type),
      req.output_index, req.request_id);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    auto result = worker_render_presenter_->renderPreview(
        req.node_id, req.output_type, req.output_index, req.option_id,
        req.hint);

    // Drop stale preview responses when a newer preview request exists.
    if (req.request_id != latest_preview_request_id_.load()) {
      ORC_LOG_DEBUG(
          "RenderCoordinator: Dropping stale preview response {} (latest {})",
          req.request_id, latest_preview_request_id_.load());
      return;
    }

    if (!result.success && !result.error_message.empty()) {
      ORC_LOG_ERROR("RenderCoordinator: Preview render failed: {}",
                    result.error_message);
    }
    ORC_LOG_DEBUG("RenderCoordinator: Preview render complete, success={}",
                  result.success);

    auto delivery = std::make_shared<PreviewRenderDelivery>();
    delivery->result = std::move(result);

    // Scope payloads come from the carrier this render already decoded, on
    // this worker thread. Extracting them here is what keeps the chroma
    // decode off the GUI thread: asking for them afterwards decoded the same
    // frame again, once per open scope, while the GUI thread waited.
    if (!req.scopes.wantsNothing()) {
      delivery->scopes =
          worker_render_presenter_->getPreviewScopes(req.node_id, req.scopes);
    }

    // Emit result on GUI thread
    emit previewReady(req.request_id, std::move(delivery));

  } catch (const std::exception& e) {
    if (req.request_id != latest_preview_request_id_.load()) {
      ORC_LOG_DEBUG(
          "RenderCoordinator: Suppressing stale preview error {} (latest {})",
          req.request_id, latest_preview_request_id_.load());
      return;
    }
    ORC_LOG_ERROR("RenderCoordinator: Preview render failed: {}", e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetObservations(
    const GetObservationsRequest& req) {
  if (!worker_render_presenter_) {
    emit observationDataReady(req.request_id, false,
                              static_cast<qulonglong>(req.field_id.value()),
                              orc::presenters::VideoParameterObservationView{},
                              orc::presenters::NtscFieldObservationsView{});
    return;
  }

  // Capture the signal parameters the observation view models need up front, on
  // this worker thread. The delivery callback may run on the scheduler's worker
  // thread and must not touch the single-threaded presenter render state; it
  // only extracts value-type view models from the delivered (pure) context.
  // Held in a shared_ptr so the callback closure copies cheaply and without
  // throwing (a bare std::optional<SourceParameters> copy can allocate, which
  // would make the noexcept delivery lambda ill-formed).
  auto video_params = std::make_shared<std::optional<orc::SourceParameters>>(
      worker_render_presenter_->getVideoParameters(req.node_id));
  const uint64_t request_id = req.request_id;
  const orc::FieldID field_id = req.field_id;

  worker_render_presenter_->requestObservations(
      req.node_id, field_id,
      [this, request_id, field_id, video_params](
          uint64_t /*presenter_request_id*/, bool available,
          const void* obs_context) noexcept {
        // This callback may run on the scheduler's worker thread inside a
        // completion callback that must never throw; contain everything.
        try {
          orc::presenters::VideoParameterObservationView video_view;
          orc::presenters::NtscFieldObservationsView ntsc_view;
          if (available && obs_context != nullptr) {
            video_view = orc::presenters::VideoParameterObservationPresenter::
                extractObservations(field_id, obs_context, *video_params);
            ntsc_view = orc::presenters::NtscObservationPresenter::
                extractFieldObservations(field_id, obs_context);
          }
          emit observationDataReady(
              request_id, available, static_cast<qulonglong>(field_id.value()),
              std::move(video_view), std::move(ntsc_view));
        } catch (const std::exception& e) {
          ORC_LOG_ERROR("RenderCoordinator: observation delivery failed: {}",
                        e.what());
        } catch (...) {
          ORC_LOG_ERROR("RenderCoordinator: observation delivery failed");
        }
      });
}

void RenderCoordinator::handleGetClosedCaptionData(
    const GetClosedCaptionDataRequest& req) {
  // One presenter request covers both fields of the field's parent frame
  // (requestObservations() maps field -> frame), so extract both here and let
  // the closed caption dialog fill its trailing-frame-window cache at half the
  // request rate. Temporal order is ascending derived field id.
  const uint64_t frame_index = req.field_id.value() / 2;
  const orc::FieldID field1_id(frame_index * 2);
  const orc::FieldID field2_id(frame_index * 2 + 1);

  if (!worker_render_presenter_) {
    emit closedCaptionDataReady(req.request_id, false,
                                static_cast<qulonglong>(field1_id.value()),
                                orc::presenters::ClosedCaptionFieldDataView{},
                                static_cast<qulonglong>(field2_id.value()),
                                orc::presenters::ClosedCaptionFieldDataView{});
    return;
  }

  const uint64_t request_id = req.request_id;
  worker_render_presenter_->requestObservations(
      req.node_id, req.field_id,
      [this, request_id, field1_id, field2_id](
          uint64_t /*presenter_request_id*/, bool available,
          const void* obs_context) noexcept {
        // This callback may run on the scheduler's worker thread inside a
        // completion callback that must never throw; contain everything.
        try {
          orc::presenters::ClosedCaptionFieldDataView field1_view;
          orc::presenters::ClosedCaptionFieldDataView field2_view;
          if (available && obs_context != nullptr) {
            field1_view = orc::presenters::ClosedCaptionObservationPresenter::
                extractFieldObservations(field1_id, obs_context);
            field2_view = orc::presenters::ClosedCaptionObservationPresenter::
                extractFieldObservations(field2_id, obs_context);
          }
          emit closedCaptionDataReady(
              request_id, available, static_cast<qulonglong>(field1_id.value()),
              field1_view, static_cast<qulonglong>(field2_id.value()),
              field2_view);
        } catch (const std::exception& e) {
          ORC_LOG_ERROR("RenderCoordinator: closed caption delivery failed: {}",
                        e.what());
        } catch (...) {
          ORC_LOG_ERROR("RenderCoordinator: closed caption delivery failed");
        }
      });
}

void RenderCoordinator::handleGetVBIData(const GetVBIDataRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting VBI data for node '{}', field {} (request "
      "{})",
      req.node_id.to_string(), req.field_id.value(), req.request_id);

  if (!worker_render_presenter_) {
    emit vbiDataReady(req.request_id, orc::presenters::VBIFieldInfoView{});
    return;
  }

  const uint64_t request_id = req.request_id;
  const orc::FieldID field_id = req.field_id;

  worker_render_presenter_->requestObservations(
      req.node_id, field_id,
      [this, request_id, field_id](uint64_t /*presenter_request_id*/,
                                   bool available,
                                   const void* obs_context) noexcept {
        // This callback may run on the scheduler's worker thread inside a
        // completion callback that must never throw; contain everything.
        try {
          orc::presenters::VBIFieldInfoView vbi_view;
          if (available && obs_context != nullptr) {
            auto decoded =
                orc::presenters::VbiPresenter::decodeVbiFromObservation(
                    obs_context, field_id);
            if (decoded.has_value()) {
              vbi_view = std::move(*decoded);
            }
          }
          emit vbiDataReady(request_id, std::move(vbi_view));
        } catch (const std::exception& e) {
          ORC_LOG_ERROR("RenderCoordinator: VBI delivery failed: {}", e.what());
        } catch (...) {
          ORC_LOG_ERROR("RenderCoordinator: VBI delivery failed");
        }
      });
}

void RenderCoordinator::handleGetDropoutData(const GetDropoutDataRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting dropout analysis data for node '{}', mode {} "
      "(request {})",
      req.node_id.to_string(), static_cast<int>(req.mode), req.request_id);

  try {
    if (!worker_render_presenter_) {
      emit error(req.request_id, "Render presenter not initialized");
      return;
    }

    // Use the RenderPresenter abstraction instead of direct DAG access.
    auto series = worker_render_presenter_->getDropoutAnalysisData(req.node_id);
    if (!series) {
      // Reading results never runs the stage — see handleGetCatalogueData.
      ORC_LOG_DEBUG(
          "RenderCoordinator: Dropout stage has no results (request {})",
          req.request_id);
      emit resultsNotAvailable(req.request_id);
      return;
    }

    ORC_LOG_DEBUG(
        "RenderCoordinator: Served dropout dataset from sink ({} points, {} "
        "frames total)",
        series->points.size(), series->total_frames);
    emit dropoutDataReady(req.request_id, std::move(*series));

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: Dropout analysis failed: {}", e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetSNRData(const GetSNRDataRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting SNR analysis data for node '{}', mode {} "
      "(request {})",
      req.node_id.to_string(), static_cast<int>(req.mode), req.request_id);

  try {
    if (!worker_render_presenter_) {
      emit error(req.request_id, "Render presenter not initialized");
      return;
    }

    // Use the RenderPresenter abstraction instead of direct DAG access.
    auto series = worker_render_presenter_->getSNRAnalysisData(req.node_id);
    if (!series) {
      // Reading results never runs the stage — see handleGetCatalogueData.
      ORC_LOG_DEBUG("RenderCoordinator: SNR stage has no results (request {})",
                    req.request_id);
      emit resultsNotAvailable(req.request_id);
      return;
    }

    ORC_LOG_DEBUG("RenderCoordinator: Served SNR dataset from sink ({} points)",
                  series->points.size());
    emit snrDataReady(req.request_id, std::move(*series));

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: SNR analysis failed: {}", e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetBurstLevelData(
    const GetBurstLevelDataRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting burst level analysis data for node '{}' "
      "(request {})",
      req.node_id.to_string(), req.request_id);

  try {
    if (!worker_render_presenter_) {
      emit error(req.request_id, "Render presenter not initialized");
      return;
    }

    // Use the RenderPresenter abstraction instead of direct DAG access.
    auto series =
        worker_render_presenter_->getBurstLevelAnalysisData(req.node_id);
    if (!series) {
      // Reading results never runs the stage — see handleGetCatalogueData.
      ORC_LOG_DEBUG(
          "RenderCoordinator: Burst level stage has no results (request {})",
          req.request_id);
      emit resultsNotAvailable(req.request_id);
      return;
    }

    ORC_LOG_DEBUG(
        "RenderCoordinator: Served burst dataset from sink ({} points)",
        series->points.size());
    emit burstLevelDataReady(req.request_id, std::move(*series));

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: Burst level analysis failed: {}",
                  e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetCatalogueData(
    const GetCatalogueDataRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting catalogue for node '{}' (request {})",
      req.node_id.to_string(), req.request_id);

  try {
    if (!worker_render_presenter_) {
      emit error(req.request_id, "Render presenter not initialized");
      return;
    }

    auto data = worker_render_presenter_->getCatalogueData(
        req.node_id, req.view_option, req.active_toggles);
    if (!data) {
      // Reading results never runs the stage. A viewer that decoded on demand
      // would duplicate Trigger Stage — the same work behind a second name,
      // and a menu pick that costs minutes on a long source. So the read is a
      // read: it serves what the last trigger left behind, and says plainly
      // when there is nothing there. Rebuilding the DAG (which any parameter
      // edit does) replaces the stage objects, so results never outlive the
      // parameters that produced them.
      ORC_LOG_DEBUG("RenderCoordinator: Stage has no catalogue (request {})",
                    req.request_id);
      emit resultsNotAvailable(req.request_id);
      return;
    }

    ORC_LOG_DEBUG("RenderCoordinator: Served catalogue ({} items)",
                  data->items.size());
    emit catalogueDataReady(req.request_id, std::move(*data));

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: Catalogue read failed: {}", e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetAvailableOutputs(
    const GetAvailableOutputsRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting available outputs for node '{}' (request {})",
      req.node_id.to_string(), req.request_id);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    auto outputs = worker_render_presenter_->getAvailableOutputs(req.node_id);

    ORC_LOG_DEBUG("RenderCoordinator: Found {} available outputs",
                  outputs.size());

    // Emit result on GUI thread (using public_api types directly)
    emit availableOutputsReady(req.request_id, std::move(outputs));

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: Get available outputs failed: {}",
                  e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetAudioChannelPairs(
    const GetAudioChannelPairsRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Enumerating audio channel pairs for node '{}' "
      "(request {})",
      req.node_id.to_string(), req.request_id);

  if (!worker_render_presenter_) {
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    auto pairs = worker_render_presenter_->getAudioChannelPairs(req.node_id);

    if (req.request_id != latest_audio_pairs_request_id_.load()) {
      ORC_LOG_DEBUG(
          "RenderCoordinator: Dropping stale audio pair response {} (latest "
          "{})",
          req.request_id, latest_audio_pairs_request_id_.load());
      return;
    }

    ORC_LOG_DEBUG("RenderCoordinator: Node '{}' carries {} audio channel pairs",
                  req.node_id.to_string(), pairs.size());

    emit audioChannelPairsReady(req.request_id, std::move(pairs));

  } catch (const std::exception& e) {
    if (req.request_id != latest_audio_pairs_request_id_.load()) {
      return;
    }
    ORC_LOG_ERROR(
        "RenderCoordinator: Audio channel pair enumeration failed: {}",
        e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleCreateAudioStreamReader(
    const CreateAudioStreamReaderRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Creating audio stream reader for node '{}', pair {} "
      "(request {})",
      req.node_id.to_string(), req.pair, req.request_id);

  if (!worker_render_presenter_) {
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    // Creation only resolves the representation; the expensive whole-stream
    // decode happens later in the caller's prime() on the playback thread, so
    // this never blocks preview rendering for minutes.
    auto reader = worker_render_presenter_->createAudioStreamReader(req.node_id,
                                                                    req.pair);

    if (req.request_id != latest_audio_reader_request_id_.load()) {
      ORC_LOG_DEBUG(
          "RenderCoordinator: Dropping stale audio reader response {} (latest "
          "{})",
          req.request_id, latest_audio_reader_request_id_.load());
      return;
    }

    if (!reader) {
      ORC_LOG_DEBUG("RenderCoordinator: No usable audio pair {} at node '{}'",
                    req.pair, req.node_id.to_string());
    }

    emit audioStreamReaderReady(req.request_id, std::move(reader));

  } catch (const std::exception& e) {
    if (req.request_id != latest_audio_reader_request_id_.load()) {
      return;
    }
    ORC_LOG_ERROR("RenderCoordinator: Audio stream reader creation failed: {}",
                  e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetLineSamples(const GetLineSamplesRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting line samples for node '{}', line {} (request "
      "{})",
      req.node_id.to_string(), req.line_number, req.request_id);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    // Get line samples with Y/C separation if available
    auto sample_data = worker_render_presenter_->getLineSamplesWithYC(
        req.node_id, req.output_type, req.output_index, req.line_number,
        req.sample_x, req.preview_image_width);

    if (sample_data.composite_samples.empty()) {
      // Line data not available (expected for sink stages that don't produce
      // field representations)
      ORC_LOG_DEBUG(
          "RenderCoordinator: Line data not available for node '{}' (expected "
          "for sink stages)",
          req.node_id.to_string());
      emit error(req.request_id, "Line data not available");
      return;
    }

    auto delivery = std::make_shared<LineSamplesDelivery>();
    delivery->field_index = req.output_index;
    delivery->line_number = req.line_number;
    delivery->sample_x = req.sample_x;

    // Converted here rather than on the GUI thread, so the view model arrives
    // ready to use.
    if (const auto params =
            worker_render_presenter_->getVideoParameters(req.node_id)) {
      delivery->video_params = orc::presenters::toVideoParametersView(*params);
    }

    // Emit samples with Y/C separation when available
    if (sample_data.has_separate_channels) {
      ORC_LOG_DEBUG(
          "RenderCoordinator: Emitting line samples with Y/C separation (Y: {} "
          "samples, C: {} samples)",
          sample_data.y_samples.size(), sample_data.c_samples.size());
    }

    delivery->samples = std::move(sample_data);
    emit lineSamplesReady(req.request_id, std::move(delivery));

  } catch (const std::exception& e) {
    ORC_LOG_DEBUG(
        "RenderCoordinator: Get line samples failed: {} (expected for sink "
        "stages)",
        e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetFrameSamples(
    const GetFrameSamplesRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting frame samples for node '{}', index {} "
      "(request {}, timing={}, waveform={})",
      req.node_id.to_string(), req.output_index, req.request_id,
      req.for_frame_timing, req.for_waveform_monitor);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    // One extraction for both dialogues. Serving them separately walked the
    // same frame twice whenever both were open.
    auto sample_data = worker_render_presenter_->getFieldSamplesForTiming(
        req.node_id, req.output_type, req.output_index);

    if (sample_data.empty()) {
      ORC_LOG_DEBUG("RenderCoordinator: Field data not available for node '{}'",
                    req.node_id.to_string());
      emit error(req.request_id, "Field data not available");
      return;
    }

    auto delivery = std::make_shared<FrameSamplesDelivery>();
    delivery->for_frame_timing = req.for_frame_timing;
    delivery->for_waveform_monitor = req.for_waveform_monitor;
    delivery->field_index = req.output_index;

    if (req.output_type == orc::PreviewOutputType::Frame_Field1_First ||
        req.output_type == orc::PreviewOutputType::Frame_Reversed ||
        req.output_type == orc::PreviewOutputType::Split) {
      // For frame modes output_index counts frames, so convert to the field
      // pair it weaves: frame N is fields (N*2) and (N*2 + 1). The samples
      // themselves stay concatenated; the widget splits them by field height.
      delivery->field_index = req.output_index * 2;
      delivery->field_index_2 = delivery->field_index + 1;
    }

    // Video parameters come from the worker's own presenter. The dialogues
    // used to build a throwaway presenter and fingerprint the whole DAG on
    // the GUI thread to get these, once per dialogue per frame.
    if (const auto params =
            worker_render_presenter_->getVideoParameters(req.node_id)) {
      delivery->video_params = orc::presenters::toVideoParametersView(*params);
    }

    ORC_LOG_DEBUG(
        "RenderCoordinator: Emitting frame samples (field {}{}, {} composite, "
        "{} Y, {} C samples)",
        delivery->field_index,
        delivery->field_index_2.has_value()
            ? std::string(" + ") + std::to_string(*delivery->field_index_2)
            : "",
        sample_data.composite().size(), sample_data.y_samples.size(),
        sample_data.c_samples.size());

    delivery->samples = std::move(sample_data);
    emit frameSamplesReady(req.request_id, std::move(delivery));

  } catch (const std::exception& e) {
    ORC_LOG_DEBUG("RenderCoordinator: Get frame samples failed: {}", e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleNavigateFrameLine(
    const NavigateFrameLineRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Navigating frame line for node '{}', field {}, line "
      "{}, direction {} (request {})",
      req.node_id.to_string(), req.current_field, req.current_line,
      req.direction, req.request_id);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    // Use the render presenter's method to navigate
    auto result = worker_render_presenter_->navigateFrameLine(
        req.node_id, req.output_type, req.current_field, req.current_line,
        req.direction, req.field_height);

    // Emit result on GUI thread (using public_api types)
    emit frameLineNavigationReady(req.request_id, result);

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: Frame line navigation failed: {}",
                  e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleTriggerStage(const TriggerStageRequest& req) {
  ORC_LOG_DEBUG("RenderCoordinator: Triggering stage '{}' (request {})",
                req.node_id.to_string(), req.request_id);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    emit triggerComplete(req.request_id, false,
                         "Render presenter not initialized");
    return;
  }

  try {
    // Use RenderPresenter to handle triggering
    // The presenter abstracts all DAG access and stage interaction
    worker_render_presenter_->triggerStage(
        req.node_id,
        makePercentGatedProgress([this](int current, int total,
                                        const std::string& message) {
          // Emit progress updates (Qt will queue to GUI thread)
          emit triggerProgress(current, total, QString::fromStdString(message));
        }));

    ORC_LOG_DEBUG("RenderCoordinator: Trigger complete successfully");
    emit triggerComplete(req.request_id, true,
                         "Trigger completed successfully");

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: Trigger failed: {}", e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
    emit triggerComplete(req.request_id, false,
                         QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::setShowDropouts(bool show) {
  if (worker_render_presenter_) {
    worker_render_presenter_->setShowDropouts(show);
    ORC_LOG_DEBUG("RenderCoordinator: Show dropouts set to {}", show);
  }
}

void RenderCoordinator::handleSavePNG(const SavePNGRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Saving PNG for node '{}', type {}, index {} to '{}'",
      req.node_id.to_string(), static_cast<int>(req.output_type),
      req.output_index, req.filename);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    // Use presenter's PNG save functionality
    bool success = worker_render_presenter_->savePNG(
        req.node_id, req.output_type, req.output_index, req.filename,
        req.option_id, req.aspect_correction);

    if (success) {
      ORC_LOG_DEBUG("RenderCoordinator: PNG saved successfully to '{}'",
                    req.filename);
    } else {
      ORC_LOG_ERROR("RenderCoordinator: Failed to save PNG to '{}'",
                    req.filename);
      emit error(
          req.request_id,
          QString::fromStdString("Failed to save PNG file: " + req.filename));
    }

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: PNG export failed: {}", e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}
