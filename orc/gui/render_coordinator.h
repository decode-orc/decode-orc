/*
 * File:        render_coordinator.h
 * Module:      orc-gui
 * Purpose:     Thread-safe coordinator for rendering operations using
 * presenters
 *
 * This class implements an Actor Model pattern where rendering state
 * is owned by a single worker thread. The GUI thread sends requests via
 * a thread-safe queue and receives responses via Qt signals.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef RENDER_COORDINATOR_H
#define RENDER_COORDINATOR_H

#include <audio_stream_reader.h>  // IAudioStreamReader (preview audio playback)
#include <orc/stage/common_types.h>
#include <orc/stage/field_id.h>
#include <orc/stage/node_id.h>
#include <orc/stage/orc_source_parameters.h>   // Public API VideoParameters
#include <orc/stage/params/parameter_types.h>  // ParameterValue
#include <orc/stage/preview/orc_rendering.h>  // Public API rendering types (includes mapping result types)
#include <orc/stage/preview/preview_stage_types.h>  // PreviewNavigationHint
#include <orc/stage/tooling/catalogue_results.h>
#include <orc_analysis_series.h>  // Analysis display-series view types
#include <orc_audio_views.h>      // AudioPairView
#include <orc_closed_caption.h>   // Closed caption observation view types
#include <orc_preview_views.h>

#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "dag_execution_progress_view.h"
#include "hints_view_models.h"
#include "ntsc_observation_view_models.h"
#include "observation_invalidation_view.h"
#include "observation_progress_view.h"
#include "preview_render_cost_view.h"
#include "vbi_view_models.h"
#include "video_parameter_observation_view_models.h"

namespace orc::presenters {
class IRenderPresenter;
class RenderPresenter;
}  // namespace orc::presenters

// Forward declarations
class GUIProject;

/**
 * @brief Request types for the render coordinator
 */
enum class RenderRequestType {
  UpdateDAG,             // Update the DAG being rendered
  RenderPreview,         // Render a preview image
  GetObservations,       // Fetch a frame's observations (async, non-blocking)
  GetVBIData,            // Decode VBI data for a field
  GetClosedCaptionData,  // Fetch closed caption bytes for a frame (async)
  GetDropoutData,        // Get dropout analysis data
  GetSNRData,            // Get SNR analysis data
  GetBurstLevelData,     // Get burst level analysis data
  GetCatalogueData,  // Get the browsable catalogue of a stage that offers one
  TriggerStage,      // Trigger a stage (batch processing)
  CancelTrigger,     // Cancel ongoing trigger
  GetAvailableOutputs,      // Query available preview outputs
  GetAudioChannelPairs,     // Query the node's audio channel pairs
  CreateAudioStreamReader,  // Create a playback reader for one channel pair
  GetLineSamples,           // Get 16-bit samples for a line
  GetFrameSamples,          // Get all frame samples for timing and waveform
  SavePNG,                  // Save preview as PNG file
  NavigateFrameLine,        // Navigate to next/previous line in frame mode
  Shutdown                  // Shutdown the worker thread
};

/**
 * @brief Names queued work that a later request of the same kind replaces.
 *
 * A dialogue that re-asks on every displayed frame only ever shows the newest
 * answer, so anything of the same kind still queued for the same node is work
 * whose result would be discarded on arrival - and, worse, work the next
 * render has to wait behind on the single worker. Naming it lets the
 * coordinator drop it at enqueue instead of servicing it.
 *
 * @c slot separates the requests a consumer issues as a set for one frame. The
 * VBI and observation dialogues ask for both fields of a frame and combine the
 * two answers, so the second must not displace the first; they number them,
 * and each slot then supersedes only its own predecessor.
 */
struct RequestCoalesceKey {
  orc::NodeID node_id;
  int slot = 0;

  bool operator==(const RequestCoalesceKey& other) const {
    return node_id == other.node_id && slot == other.slot;
  }
};

/**
 * @brief Base class for all requests
 */
struct RenderRequest {
  RenderRequestType type;
  uint64_t request_id;  // Unique ID to match responses
  /// Set where a newer request of the same type and key makes this one dead
  /// work. Absent for requests that must each be serviced - a closed-caption
  /// read covers a frame no other request covers, and a trigger or a PNG save
  /// is not a question about the current frame at all.
  std::optional<RequestCoalesceKey> coalesce_key;

  virtual ~RenderRequest() = default;

 protected:
  explicit RenderRequest(RenderRequestType t, uint64_t id)
      : type(t), request_id(id) {}
};

/**
 * @brief Request to update the DAG
 */
struct UpdateDAGRequest : public RenderRequest {
  std::shared_ptr<const void> dag;

  UpdateDAGRequest(uint64_t id, std::shared_ptr<const void> d)
      : RenderRequest(RenderRequestType::UpdateDAG, id), dag(std::move(d)) {}
};

/**
 * @brief Request to render a preview
 */
struct RenderPreviewRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::PreviewOutputType output_type;
  uint64_t output_index;
  std::string option_id;
  orc::PreviewNavigationHint hint;
  /// Scopes to produce from the carrier this render decodes. Defaults to
  /// none, so a caller that has no scope dialogues open pays nothing.
  orc::PreviewScopeRequest scopes;

  RenderPreviewRequest(
      uint64_t id, orc::NodeID node, orc::PreviewOutputType type,
      uint64_t index, std::string opt_id = "",
      orc::PreviewNavigationHint nav_hint = orc::PreviewNavigationHint::Random,
      orc::PreviewScopeRequest scope_request = {})
      : RenderRequest(RenderRequestType::RenderPreview, id),
        node_id(std::move(node)),
        output_type(type),
        output_index(index),
        option_id(std::move(opt_id)),
        hint(nav_hint),
        scopes(scope_request) {}
};

/**
 * @brief Everything one completed preview render delivers to the GUI.
 *
 * The scope payloads travel with the image because they are extracted from the
 * carrier that render decoded. Delivered behind a shared pointer so the image
 * buffer crosses the thread boundary once, however many slots are connected.
 */
struct PreviewRenderDelivery {
  orc::PreviewRenderResult result;
  orc::PreviewScopePayloads scopes;
  /// What this render cost on the worker, for the frame profiler. The GUI
  /// thread measures the wait; only the worker can say what filled it.
  orc::presenters::PreviewRenderCostView cost;
};

using PreviewRenderDeliveryPtr = std::shared_ptr<const PreviewRenderDelivery>;

/**
 * @brief Request to fetch a frame's observations without blocking
 */
struct GetObservationsRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::FieldID field_id;

  GetObservationsRequest(uint64_t id, orc::NodeID node, orc::FieldID fid)
      : RenderRequest(RenderRequestType::GetObservations, id),
        node_id(std::move(node)),
        field_id(fid) {}
};

/**
 * @brief Request to get VBI data
 */
struct GetVBIDataRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::FieldID field_id;

  GetVBIDataRequest(uint64_t id, orc::NodeID node, orc::FieldID fid)
      : RenderRequest(RenderRequestType::GetVBIData, id),
        node_id(std::move(node)),
        field_id(fid) {}
};

/**
 * @brief Request to fetch closed caption bytes for the frame containing a field
 */
struct GetClosedCaptionDataRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::FieldID field_id;

  GetClosedCaptionDataRequest(uint64_t id, orc::NodeID node, orc::FieldID fid)
      : RenderRequest(RenderRequestType::GetClosedCaptionData, id),
        node_id(std::move(node)),
        field_id(fid) {}
};

/**
 * @brief Request to get dropout analysis data for all fields
 */
struct GetDropoutDataRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::DropoutAnalysisMode mode;

  GetDropoutDataRequest(uint64_t id, orc::NodeID node,
                        orc::DropoutAnalysisMode m)
      : RenderRequest(RenderRequestType::GetDropoutData, id),
        node_id(std::move(node)),
        mode(m) {}
};

/**
 * @brief Request to get SNR analysis data for all fields
 */
struct GetSNRDataRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::SNRAnalysisMode mode;

  GetSNRDataRequest(uint64_t id, orc::NodeID node, orc::SNRAnalysisMode m)
      : RenderRequest(RenderRequestType::GetSNRData, id),
        node_id(std::move(node)),
        mode(m) {}
};

/**
 * @brief Request to get burst level analysis data for all fields
 */
struct GetBurstLevelDataRequest : public RenderRequest {
  orc::NodeID node_id;

  GetBurstLevelDataRequest(uint64_t id, orc::NodeID node)
      : RenderRequest(RenderRequestType::GetBurstLevelData, id),
        node_id(std::move(node)) {}
};

/**
 * @brief Request to get the browsable catalogue of a stage that offers one
 */
struct GetCatalogueDataRequest : public RenderRequest {
  orc::NodeID node_id;
  /// Which of the schema's view options to build under; empty for the one the
  /// stage's own settings give.
  std::string view_option;
  /// Ids of the schema's toggles the reader has on; one it offers and this does
  /// not name is off. Absent where the reader has not been asked yet, which is
  /// answered with whatever the stage has its toggles at by default — an empty
  /// list would say the reader had turned them all off.
  std::optional<std::vector<std::string>> active_toggles;

  GetCatalogueDataRequest(uint64_t id, orc::NodeID node, std::string option,
                          std::optional<std::vector<std::string>> toggles)
      : RenderRequest(RenderRequestType::GetCatalogueData, id),
        node_id(std::move(node)),
        view_option(std::move(option)),
        active_toggles(std::move(toggles)) {}
};

/**
 * @brief Request to trigger a stage
 */
struct TriggerStageRequest : public RenderRequest {
  orc::NodeID node_id;

  explicit TriggerStageRequest(uint64_t id, orc::NodeID node)
      : RenderRequest(RenderRequestType::TriggerStage, id),
        node_id(std::move(node)) {}
};

/**
 * @brief Request to save preview as PNG
 */
struct SavePNGRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::PreviewOutputType output_type;
  uint64_t output_index;
  std::string filename;
  std::string option_id;
  double aspect_correction;

  SavePNGRequest(uint64_t id, orc::NodeID node, orc::PreviewOutputType type,
                 uint64_t index, std::string file, std::string opt_id = "",
                 double correction = 1.0)
      : RenderRequest(RenderRequestType::SavePNG, id),
        node_id(std::move(node)),
        output_type(type),
        output_index(index),
        filename(std::move(file)),
        option_id(std::move(opt_id)),
        aspect_correction(correction) {}
};

/**
 * @brief Request to get available outputs
 */
struct GetAvailableOutputsRequest : public RenderRequest {
  orc::NodeID node_id;

  GetAvailableOutputsRequest(uint64_t id, orc::NodeID node)
      : RenderRequest(RenderRequestType::GetAvailableOutputs, id),
        node_id(std::move(node)) {}
};

/**
 * @brief Request to enumerate a node's audio channel pairs
 */
struct GetAudioChannelPairsRequest : public RenderRequest {
  orc::NodeID node_id;

  GetAudioChannelPairsRequest(uint64_t id, orc::NodeID node)
      : RenderRequest(RenderRequestType::GetAudioChannelPairs, id),
        node_id(std::move(node)) {}
};

/**
 * @brief Request to create a playback reader for one audio channel pair
 */
struct CreateAudioStreamReaderRequest : public RenderRequest {
  orc::NodeID node_id;
  size_t pair;

  CreateAudioStreamReaderRequest(uint64_t id, orc::NodeID node, size_t p)
      : RenderRequest(RenderRequestType::CreateAudioStreamReader, id),
        node_id(std::move(node)),
        pair(p) {}
};

/**
 * @brief Request to get line samples
 */
struct GetLineSamplesRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::PreviewOutputType output_type;
  uint64_t output_index;
  int line_number;
  int sample_x;
  int preview_image_width;  // Width of the preview image for coordinate mapping

  GetLineSamplesRequest(uint64_t id, orc::NodeID node,
                        orc::PreviewOutputType type, uint64_t index, int line,
                        int x, int img_width)
      : RenderRequest(RenderRequestType::GetLineSamples, id),
        node_id(std::move(node)),
        output_type(type),
        output_index(index),
        line_number(line),
        sample_x(x),
        preview_image_width(img_width) {}
};

/**
 * @brief Request to get field timing data
 */
struct GetFrameSamplesRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::PreviewOutputType output_type;
  uint64_t output_index;
  /// Which dialogues asked. The extraction is identical for both, so one
  /// request serves whichever are open and the response says who wanted it.
  bool for_frame_timing;
  bool for_waveform_monitor;

  GetFrameSamplesRequest(uint64_t id, orc::NodeID node,
                         orc::PreviewOutputType type, uint64_t index,
                         bool timing, bool waveform)
      : RenderRequest(RenderRequestType::GetFrameSamples, id),
        node_id(std::move(node)),
        output_type(type),
        output_index(index),
        for_frame_timing(timing),
        for_waveform_monitor(waveform) {}
};

/**
 * @brief Request to navigate to next/previous line in frame mode
 */
struct NavigateFrameLineRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::PreviewOutputType output_type;
  uint64_t current_field;
  int current_line;
  int direction;     // +1 for down, -1 for up
  int field_height;  // Height of a single field in lines

  NavigateFrameLineRequest(uint64_t id, orc::NodeID node,
                           orc::PreviewOutputType type, uint64_t field,
                           int line, int dir, int height)
      : RenderRequest(RenderRequestType::NavigateFrameLine, id),
        node_id(std::move(node)),
        output_type(type),
        current_field(field),
        current_line(line),
        direction(dir),
        field_height(height) {}
};

/**
 * @brief Base class for responses
 */
struct RenderResponse {
  uint64_t request_id;
  bool success;
  std::string error_message;

  virtual ~RenderResponse() = default;

 protected:
  RenderResponse(uint64_t id, bool s, std::string err = "")
      : request_id(id), success(s), error_message(std::move(err)) {}
};

/**
 * @brief Response with preview render result
 */
struct PreviewRenderResponse : public RenderResponse {
  orc::PreviewRenderResult result;

  PreviewRenderResponse(uint64_t id, bool s, orc::PreviewRenderResult r,
                        std::string err = "")
      : RenderResponse(id, s, std::move(err)), result(std::move(r)) {}
};

/**
 * @brief Response with VBI data
 */
struct VBIDataResponse : public RenderResponse {
  orc::presenters::VBIFieldInfoView vbi_info;

  VBIDataResponse(uint64_t id, bool s, orc::presenters::VBIFieldInfoView info,
                  std::string err = "")
      : RenderResponse(id, s, std::move(err)), vbi_info(std::move(info)) {}
};

/**
 * @brief Response with dropout analysis data
 */
struct DropoutDataResponse : public RenderResponse {
  orc::presenters::DropoutDisplaySeries series;

  DropoutDataResponse(uint64_t id, bool s,
                      orc::presenters::DropoutDisplaySeries data,
                      std::string err = "")
      : RenderResponse(id, s, std::move(err)), series(std::move(data)) {}
};

/**
 * @brief Response with SNR analysis data
 */
struct SNRDataResponse : public RenderResponse {
  orc::presenters::SNRDisplaySeries series;

  SNRDataResponse(uint64_t id, bool s, orc::presenters::SNRDisplaySeries data,
                  std::string err = "")
      : RenderResponse(id, s, std::move(err)), series(std::move(data)) {}
};

/**
 * @brief Response with burst level analysis data
 */
struct BurstLevelDataResponse : public RenderResponse {
  orc::presenters::BurstLevelDisplaySeries series;

  BurstLevelDataResponse(uint64_t id, bool s,
                         orc::presenters::BurstLevelDisplaySeries data,
                         std::string err = "")
      : RenderResponse(id, s, std::move(err)), series(std::move(data)) {}
};

/**
 * @brief Response with available outputs
 */
struct AvailableOutputsResponse : public RenderResponse {
  std::vector<orc::PreviewOutputInfo> outputs;

  AvailableOutputsResponse(uint64_t id, bool s,
                           std::vector<orc::PreviewOutputInfo> out,
                           std::string err = "")
      : RenderResponse(id, s, std::move(err)), outputs(std::move(out)) {}
};

/**
 * @brief Response for trigger completion
 */
struct TriggerCompleteResponse : public RenderResponse {
  std::string status_message;

  TriggerCompleteResponse(uint64_t id, bool s, std::string status,
                          std::string err = "")
      : RenderResponse(id, s, std::move(err)),
        status_message(std::move(status)) {}
};

/**
 * @brief Response for frame line navigation
 */
struct FrameLineNavigationResponse : public RenderResponse {
  orc::FrameLineNavigationResult result;

  FrameLineNavigationResponse(uint64_t id, bool s,
                              orc::FrameLineNavigationResult nav_result,
                              std::string err = "")
      : RenderResponse(id, s, std::move(err)), result(nav_result) {}
};

namespace orc::presenters {

class IRenderPresenter {
 public:
  using TriggerProgressCallback =
      std::function<void(int, int, const std::string&)>;

  struct LineSampleData {
    std::vector<int16_t> composite_samples;
    std::vector<int16_t> y_samples;
    std::vector<int16_t> c_samples;
    bool has_separate_channels;
    int first_field_height = 0;
    int second_field_height = 0;

    // The composite trace to plot. A Y/C source has no composite signal of
    // its own; its luma is what a composite display shows. Saying so by
    // duplicating the luma buffer cost a full frame copy per extraction, so
    // the substitution is made here and composite_samples is left empty.
    const std::vector<int16_t>& composite() const {
      if (composite_samples.empty() && has_separate_channels) {
        return y_samples;
      }
      return composite_samples;
    }

    // True when neither a composite nor a luma trace was extracted.
    bool empty() const {
      return composite_samples.empty() && y_samples.empty();
    }
  };

  virtual ~IRenderPresenter() = default;

  virtual void setDAG(std::shared_ptr<void> dag_handle) = 0;
  virtual bool getShowDropouts() const = 0;
  virtual void setShowDropouts(bool show) = 0;

  // Disable the presenter's background observation pipeline (scheduler,
  // sweeps, prefetch). Call before the first setDAG() on auxiliary presenters
  // that only render frames — construction then stays cheap enough for the
  // GUI thread and no duplicate background pipeline is spawned.
  virtual void setBackgroundObservationEnabled(bool enabled) = 0;

  // Tell the background pipeline a preview is playing, so it holds back
  // whole-node sweeps until playback stops. Queued sweep work is kept;
  // interactive and prefetch observations continue. Safe from any thread.
  virtual void setPlaybackActive(bool active) = 0;

  // What the last renderPreview() cost on this thread. Collected immediately
  // after the render it describes.
  virtual orc::presenters::PreviewRenderCostView lastPreviewRenderCost()
      const = 0;

  // Observe the on-demand DAG execution that getAvailableOutputs()/
  // renderPreview() drive. Fires once per node, immediately before it runs, on
  // the calling (worker) thread; an empty callback stops delivery. Lets the
  // view report progress while a large source is being opened.
  virtual void setExecutionProgressCallback(
      orc::presenters::DagExecutionProgressCallback callback) = 0;

  // Phase 3: observation-invalidation notifications. subscribeInvalidation()
  // returns an id passed to unsubscribeInvalidation() to cancel delivery.
  virtual uint64_t subscribeInvalidation(
      orc::presenters::ObservationInvalidationCallback callback) = 0;
  virtual void unsubscribeInvalidation(uint64_t subscription_id) = 0;

  // Phase 5: async observation delivery + background-workload progress.
  virtual uint64_t requestObservations(
      NodeID node_id, FieldID field_id,
      orc::presenters::ObservationDataReadyCallback callback) = 0;
  virtual uint64_t subscribeObservationProgress(
      orc::presenters::ObservationProgressCallback callback) = 0;
  virtual void unsubscribeObservationProgress(uint64_t subscription_id) = 0;

  // @p hint tells stages whether the frame is part of a run of adjacent frames
  // (playback) or a one-off position (scrubbing, a single navigation, an
  // export). Only the playback path sends Sequential.
  virtual orc::PreviewRenderResult renderPreview(
      NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
      const std::string& option_id, orc::PreviewNavigationHint hint) = 0;

  virtual std::optional<orc::presenters::DropoutDisplaySeries>
  getDropoutAnalysisData(NodeID node_id) = 0;
  virtual std::optional<orc::presenters::SNRDisplaySeries> getSNRAnalysisData(
      NodeID node_id) = 0;
  virtual std::optional<orc::presenters::BurstLevelDisplaySeries>
  getBurstLevelAnalysisData(NodeID node_id) = 0;
  virtual std::optional<orc::CatalogueDataset> getCatalogueData(
      NodeID node_id, const std::string& view_option,
      const std::optional<std::vector<std::string>>& active_toggles) = 0;
  virtual std::vector<orc::PreviewOutputInfo> getAvailableOutputs(
      NodeID node_id) = 0;

  // Audio channel pairs available at the node, for the preview audio selector.
  // An empty list is the normal "no audio here" answer, not an error.
  virtual std::vector<orc::AudioPairView> getAudioChannelPairs(
      NodeID node_id) = 0;

  // Create a frame-addressed reader for one channel pair. Executes the DAG, so
  // it runs on the coordinator's worker thread; the reader is then primed and
  // read from the playback thread. Returns nullptr when the pair is unusable.
  virtual std::shared_ptr<orc::presenters::IAudioStreamReader>
  createAudioStreamReader(NodeID node_id, size_t pair) = 0;

  virtual LineSampleData getLineSamplesWithYC(
      NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
      int line_number, int sample_x, int preview_width) = 0;

  virtual std::optional<orc::SourceParameters> getVideoParameters(
      NodeID node_id) = 0;

  virtual LineSampleData getFieldSamplesForTiming(
      NodeID node_id, orc::PreviewOutputType output_type,
      uint64_t output_index) = 0;

  virtual orc::FrameLineNavigationResult navigateFrameLine(
      NodeID node_id, orc::PreviewOutputType output_type,
      uint64_t current_field, int current_line, int direction,
      int field_height) = 0;

  virtual uint64_t triggerStage(NodeID node_id,
                                TriggerProgressCallback callback) = 0;
  virtual uint64_t triggerStage(
      NodeID node_id, TriggerProgressCallback callback,
      std::map<std::string, orc::ParameterValue> parameter_overrides) = 0;
  virtual void cancelTrigger() = 0;

  virtual bool savePNG(NodeID node_id, orc::PreviewOutputType output_type,
                       uint64_t output_index, const std::string& filename,
                       const std::string& option_id,
                       double aspect_correction) = 0;

  virtual orc::ImageToFieldMappingResult mapImageToField(
      NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
      int image_y, int image_height, const std::string& option_id) = 0;

  virtual orc::FieldToImageMappingResult mapFieldToImage(
      NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
      uint64_t field_index, int field_line, int image_height,
      const std::string& option_id) = 0;

  virtual orc::FrameFieldsResult getFrameFields(NodeID node_id,
                                                uint64_t frame_index) = 0;

  virtual std::vector<orc::PreviewViewDescriptor> getAvailablePreviewViews(
      NodeID node_id, orc::VideoDataType data_type) = 0;

  virtual std::vector<orc::VideoDataType> getStageDataTypes(NodeID node_id) = 0;

  virtual orc::PreviewViewDataResult requestPreviewViewData(
      NodeID node_id, const std::string& view_id, orc::VideoDataType data_type,
      const orc::PreviewCoordinate& coordinate) = 0;

  /// Vectorscope and histogram payloads for one frame from a single carrier
  /// fetch. Called on the render worker beside the preview render so the
  /// scope dialogues never decode the frame a second time.
  virtual orc::PreviewScopePayloads getPreviewScopes(
      NodeID node_id, const orc::PreviewScopeRequest& request) = 0;
};

}  // namespace orc::presenters

/**
 * @brief One frame's samples, and everything the consuming dialogues need.
 *
 * The video parameters travel with the samples because the dialogues convert
 * to millivolts with them. Fetching them on the GUI thread meant building a
 * throwaway presenter and fingerprinting the whole DAG once per dialogue per
 * frame; the worker already has a presenter, so it answers here.
 */
struct FrameSamplesDelivery {
  orc::presenters::IRenderPresenter::LineSampleData samples;
  uint64_t field_index{0};
  std::optional<uint64_t> field_index_2;
  std::optional<orc::presenters::VideoParametersView> video_params;
  bool for_frame_timing{false};
  bool for_waveform_monitor{false};
};

using FrameSamplesDeliveryPtr = std::shared_ptr<const FrameSamplesDelivery>;

/**
 * @brief One line's samples for the line scope, with its video parameters.
 *
 * Shared rather than copied for the same reason as the frame samples: the
 * buffers are per-line but the signal crosses a thread boundary, and the
 * parameter conversion belongs on the worker that already holds a presenter.
 */
struct LineSamplesDelivery {
  uint64_t field_index{0};
  int line_number{0};
  int sample_x{0};
  orc::presenters::IRenderPresenter::LineSampleData samples;
  std::optional<orc::presenters::VideoParametersView> video_params;
};

using LineSamplesDeliveryPtr = std::shared_ptr<const LineSamplesDelivery>;

/**
 * @brief Coordinator that owns all core rendering state in a worker thread
 *
 * Architecture:
 * - Worker thread owns: DAG, PreviewRenderer, DAGFrameRenderer, all decoders
 * - GUI thread sends requests via thread-safe queue
 * - Worker thread processes requests serially (no races possible)
 * - Responses sent back via Qt signals (thread-safe)
 *
 * Thread Safety:
 * - ALL public methods are thread-safe (called from GUI thread)
 * - Worker thread methods are private and run on worker thread only
 * - No shared mutable state between threads
 */
// Thread-safe: all public methods are safe to call from the GUI thread;
// worker-thread state is fully private.
class RenderCoordinator : public QObject {
  Q_OBJECT

 public:
  using RenderPresenterFactory =
      std::function<std::shared_ptr<orc::presenters::IRenderPresenter>(void*)>;

  explicit RenderCoordinator(QObject* parent = nullptr);
  explicit RenderCoordinator(RenderPresenterFactory presenter_factory,
                             QObject* parent = nullptr);
  ~RenderCoordinator();

  // Prevent copying/moving
  RenderCoordinator(const RenderCoordinator&) = delete;
  RenderCoordinator& operator=(const RenderCoordinator&) = delete;

  // ========================================================================
  // Public API (thread-safe, called from GUI thread)
  // ========================================================================

  /**
   * @brief Start the worker thread
   *
   * Must be called before any other operations.
   */
  void start();

  /**
   * @brief Stop the worker thread and wait for completion
   *
   * Blocks until worker thread exits cleanly.
   */
  void stop();

  /**
   * @brief Update the DAG being rendered
   *
   * This invalidates all caches and recreates renderers.
   *
   * @param dag New DAG to use (opaque handle)
   */
  void updateDAG(std::shared_ptr<const void> dag);

  /**
   * @brief Set the project for rendering
   *
   * Must be called before updateDAG to initialize the presenter.
   *
   * @param project Project pointer (opaque handle, must outlive
   * RenderCoordinator)
   */
  void setProject(void* project);

  /**
   * @brief Request a preview render (async)
   *
   * Result will be emitted via previewReady signal.
   *
   * Queuing a preview drops any preview still waiting in the queue: the worker
   * is serial and a superseded render's response is discarded on completion
   * anyway, so rendering it only delays the one the user is waiting for.
   *
   * @param node_id Node to render from
   * @param output_type Type of output (field/frame/etc)
   * @param output_index Which field/frame to render
   * @param option_id Rendering option (preview mode plus any signal suffix)
   * @param hint Sequential while playback is running so stages can pre-fetch;
   *        Random for scrubbing and single navigations
   * @return Request ID for matching response
   */
  uint64_t requestPreview(
      const orc::NodeID& node_id, orc::PreviewOutputType output_type,
      uint64_t output_index, const std::string& option_id = "",
      orc::PreviewNavigationHint hint = orc::PreviewNavigationHint::Random,
      const orc::PreviewScopeRequest& scopes = {});

  /**
   * @brief Request VBI data for a field (async)
   *
   * Answered from the provenance-keyed store when present, otherwise computed
   * on the background scheduler (same delivery path as requestObservations()).
   * The decoded view is emitted via vbiDataReady. No DAG execution runs on the
   * GUI thread.
   *
   * @param node_id Node to decode VBI from
   * @param field_id Field to decode
   * @param frame_slot Which of the frame's fields this is (0 or 1). Requests
   *        sharing a node and slot supersede one another, so the two halves of
   *        a frame must be numbered differently or the second will discard the
   *        first and the dialogue will wait for an answer that never comes.
   * @return Request ID for matching / discarding stale responses
   */
  uint64_t requestVBIData(const orc::NodeID& node_id, orc::FieldID field_id,
                          int frame_slot);

  /**
   * @brief Request closed caption bytes for the frame containing a field
   *        (async)
   *
   * Answered from the provenance-keyed store when present, otherwise computed
   * on the background scheduler (same delivery path as requestObservations()).
   * One request covers both fields of the field's parent frame; the extracted
   * per-field byte pairs are emitted via closedCaptionDataReady.
   *
   * @param node_id  Node whose output frame is observed
   * @param field_id Any field of the frame of interest
   * @return Request ID for matching / discarding stale responses
   */
  uint64_t requestClosedCaptionData(const orc::NodeID& node_id,
                                    orc::FieldID field_id);

  /**
   * @brief Request a frame's observations without blocking the GUI (async)
   *
   * Answered from the provenance-keyed store when present, otherwise computed
   * on the background scheduler. The extracted observation view models are
   * emitted via observationDataReady. No DAG execution runs on the GUI thread.
   *
   * @param node_id  Node whose output frame is observed
   * @param field_id Field of interest (both fields of its frame are covered)
   * @param frame_slot Which of the frame's fields this is (0 or 1); see
   *        requestVBIData() for why the two halves must differ.
   * @return Request ID for matching / discarding stale responses
   */
  uint64_t requestObservations(const orc::NodeID& node_id,
                               orc::FieldID field_id, int frame_slot);

  /**
   * @brief Request dropout analysis data for all fields (async)
   *
   * Reads what the node's last trigger produced; emits resultsNotAvailable
   * when it has not been triggered. Result via dropoutDataReady.
   *
   * @param node_id Node to analyze dropout from
   * @param mode Analysis mode (full field or visible area)
   * @return Request ID for matching response
   */
  uint64_t requestDropoutData(const orc::NodeID& node_id,
                              orc::DropoutAnalysisMode mode);

  /**
   * @brief Request SNR analysis data for all fields (async)
   *
   * Reads what the node's last trigger produced; emits resultsNotAvailable
   * when it has not been triggered. Result via snrDataReady.
   *
   * @param node_id Node to analyze SNR from
   * @param mode Analysis mode (white, black, or both)
   * @return Request ID for matching response
   */
  uint64_t requestSNRData(const orc::NodeID& node_id,
                          orc::SNRAnalysisMode mode);

  /**
   * @brief Request burst level analysis data for all fields (async)
   *
   * Reads what the node's last trigger produced; emits resultsNotAvailable
   * when it has not been triggered. Result via burstLevelDataReady.
   *
   * @param node_id Node to analyze burst level from
   * @return Request ID for matching response
   */
  uint64_t requestBurstLevelData(const orc::NodeID& node_id);

  /**
   * @brief Request the browsable catalogue of a stage (async)
   *
   * A read, never a run: serves the catalogue the node's last trigger left
   * behind via catalogueDataReady, or emits resultsNotAvailable when the node
   * has not been triggered since the DAG was last built.
   *
   * Asking for a view option or a toggle is still a read: the stage draws the
   * items it already has a second way, and nothing upstream of it runs.
   *
   * @param node_id Node whose stage offers orc::ICatalogueResults
   * @param view_option One of the schema's view options, or empty for the view
   *        the stage's own settings give
   * @param active_toggles Ids of the schema's toggles the reader has on, or
   *        nothing where the reader has not been asked yet — which is the
   *        first read, and is answered with the toggles as the stage has them
   * @return Request ID
   */
  uint64_t requestCatalogueData(
      const orc::NodeID& node_id, const std::string& view_option = {},
      const std::optional<std::vector<std::string>>& active_toggles = {});

  /**
   * @brief Request available outputs for a node (async)
   *
   * Result will be emitted via availableOutputsReady signal.
   *
   * @param node_id Node to query
   * @return Request ID for matching response
   */
  uint64_t requestAvailableOutputs(const orc::NodeID& node_id);

  /**
   * @brief Request the node's audio channel pairs (async)
   *
   * Enumeration resolves the node's representation, which executes the DAG, so
   * it is routed through the worker like every other query. The result is
   * emitted via audioChannelPairsReady; an empty list means the node carries no
   * usable audio, which is the normal case for most projects.
   *
   * Only the newest request is answered — the viewed node changes faster than
   * enumeration completes on a heavy DAG.
   *
   * @param node_id Node to enumerate
   * @return Request ID for matching response
   */
  uint64_t requestAudioChannelPairs(const orc::NodeID& node_id);

  /**
   * @brief Request a playback reader for one audio channel pair (async)
   *
   * Creation executes the DAG and so must happen on the worker thread; the
   * reader is delivered via audioStreamReaderReady and may then be primed and
   * read from the playback thread. A null reader in the response means the pair
   * is not usable (no audio, unknown video system, or pair out of range).
   *
   * Only the newest request is answered, so a rapid pair reselection cannot
   * deliver a superseded reader.
   *
   * @param node_id Node whose representation supplies the audio
   * @param pair    Channel-pair index
   * @return Request ID for matching response
   */
  uint64_t requestAudioStreamReader(const orc::NodeID& node_id, size_t pair);

  /**
   * @brief Request line samples from a field (async)
   *
   * Result will be emitted via lineSamplesReady signal.
   *
   * @param node_id Node to get samples from
   * @param output_type Type of output (field/frame)
   * @param output_index Which field/frame
   * @param line_number Line number to retrieve (0-based)
   * @param sample_x Sample X position that was clicked (in preview image
   * coordinates)
   * @param preview_image_width Width of the preview image for coordinate
   * mapping
   * @return Request ID for matching response
   */
  uint64_t requestLineSamples(const orc::NodeID& node_id,
                              orc::PreviewOutputType output_type,
                              uint64_t output_index, int line_number,
                              int sample_x, int preview_image_width);

  /**
   * @brief Request one frame's samples for the timing and waveform dialogues
   *
   * Both dialogues plot the same extraction, so one request serves whichever
   * are open and the response is delivered to both. Result arrives via
   * frameSamplesReady.
   *
   * @param node_id Node to sample
   * @param output_type Preview output type the index is expressed in
   * @param output_index Frame or field index
   * @param for_frame_timing True when the frame timing dialogue wants it
   * @param for_waveform_monitor True when the waveform monitor wants it
   * @return Request ID for matching the response
   */
  uint64_t requestFrameSamples(const orc::NodeID& node_id,
                               orc::PreviewOutputType output_type,
                               uint64_t output_index, bool for_frame_timing,
                               bool for_waveform_monitor);

  /**
   * @brief Map preview image coordinates to field coordinates (synchronous)
   *
   * This is a synchronous call that returns immediately with the mapping.
   * No async request needed since it's just a calculation.
   *
   * @param node_id The node being displayed
   * @param output_type The output type (Field, Frame, etc.)
   * @param output_index The output index (0-based)
   * @param image_y Y coordinate in the preview image
   * @param image_height Total height of the image (for split mode)
   * @param option_id The preview option being displayed; frame outputs may be
   *        weaved or field-sequential and only the option id says which
   * @return Mapping result (field_index, field_line)
   */
  orc::ImageToFieldMappingResult mapImageToField(
      const orc::NodeID& node_id, orc::PreviewOutputType output_type,
      uint64_t output_index, int image_y, int image_height,
      const std::string& option_id = "");

  /**
   * @brief Map field coordinates back to preview image coordinates
   * (synchronous)
   *
   * This is a synchronous call that returns immediately with the mapping.
   * No async request needed since it's just a calculation.
   *
   * @param node_id The node being displayed
   * @param output_type The output type (Field, Frame, etc.)
   * @param output_index The output index (0-based)
   * @param field_index The field index
   * @param field_line The line within the field
   * @param image_height Total height of the image (for split mode)
   * @param option_id The preview option being displayed (see mapImageToField)
   * @return Mapping result (image_y)
   */
  orc::FieldToImageMappingResult mapFieldToImage(
      const orc::NodeID& node_id, orc::PreviewOutputType output_type,
      uint64_t output_index, uint64_t field_index, int field_line,
      int image_height, const std::string& option_id = "");

  /**
   * @brief Get the field indices that make up a frame (synchronous)
   *
   * Returns which two fields comprise the given frame, accounting for field
   * ordering.
   *
   * @param node_id The node being displayed
   * @param frame_index The frame index (0-based)
   * @return Result with first_field and second_field indices
   */
  orc::FrameFieldsResult getFrameFields(const orc::NodeID& node_id,
                                        uint64_t frame_index);

  /**
   * @brief Get registry-driven preview views for node/data type (synchronous).
   */
  std::vector<orc::PreviewViewDescriptor> getAvailablePreviewViews(
      const orc::NodeID& node_id, orc::VideoDataType data_type);

  /**
   * @brief Video data types the node's stage declares it can preview
   * (synchronous).
   *
   * The stage's own declaration, which is what decides view applicability —
   * as opposed to a data type inferred from the selected preview output type.
   */
  std::vector<orc::VideoDataType> getStageDataTypes(const orc::NodeID& node_id);

  /**
   * @brief Request preview-view data through presenter registry contract
   * (synchronous).
   */
  orc::PreviewViewDataResult requestPreviewViewData(
      const orc::NodeID& node_id, const std::string& view_id,
      orc::VideoDataType data_type, const orc::PreviewCoordinate& coordinate);

  uint64_t requestSavePNG(const orc::NodeID& node_id,
                          orc::PreviewOutputType output_type,
                          uint64_t output_index, const std::string& filename,
                          const std::string& option_id = "",
                          double aspect_correction = 1.0);

  /**
   * @brief Trigger a stage for batch processing (async)
   *
   * Progress updates emitted via triggerProgress signal.
   * Completion emitted via triggerComplete signal.
   *
   * @param node_id Node to trigger
   * @return Request ID for matching response
   */
  uint64_t requestTrigger(const orc::NodeID& node_id);

  /**
   * @brief Cancel ongoing trigger operation
   */
  void cancelTrigger();

  /**
   * @brief Set whether to render dropout regions onto images
   *
   * Thread-safe - can be called from GUI thread.
   *
   * @param show True to render dropouts, false to hide
   */
  void setShowDropouts(bool show);

  /**
   * @brief Tell the render presenter whether a preview is playing
   *
   * While playing, the presenter's background scheduler stops dequeuing
   * whole-node sweeps, which would otherwise occupy half the machine's cores
   * for as long as a node has unobserved frames - in competition with the very
   * worker that has to deliver the next frame. Nothing queued is discarded.
   *
   * Thread-safe - can be called from the GUI thread. Remembered, so a
   * presenter created later starts in the right state.
   *
   * @param active True while the preview is playing
   */
  void setPlaybackActive(bool active);

 signals:
  /**
   * @brief Emitted when a preview render completes
   *
   * @param request_id The request ID from requestPreview()
   * @param delivery The render result and any scope payloads extracted from
   *        the same carrier, shared rather than copied
   */
  void previewReady(uint64_t request_id, PreviewRenderDeliveryPtr delivery);

  /**
   * @brief Emitted (on the GUI thread) when a requestVBIData() response is
   *        ready
   *
   * Carries the decoded view for the requested field; a default-constructed
   * view means no VBI was available. Marshalled from the worker/scheduler
   * thread via a queued connection, so responses to concurrent requests may
   * arrive in any order - match them by @p request_id.
   */
  void vbiDataReady(uint64_t request_id,
                    orc::presenters::VBIFieldInfoView info);

  /**
   * @brief Emitted (on the GUI thread) when a requestClosedCaptionData()
   *        response is ready
   *
   * Carries the recovered EIA-608 byte pairs for both fields of the requested
   * frame, in temporal order (field1 precedes field2).
   *
   * @param request_id      Id returned by requestClosedCaptionData()
   * @param available       True when the frame's observations were produced
   * @param field1_id_value First field of the frame (FieldID::value())
   * @param field1          Caption data for the first field
   * @param field2_id_value Second field of the frame
   * @param field2          Caption data for the second field
   *
   * Marshalled from the worker/scheduler thread via a queued connection.
   */
  void closedCaptionDataReady(
      uint64_t request_id, bool available, qulonglong field1_id_value,
      orc::presenters::ClosedCaptionFieldDataView field1,
      qulonglong field2_id_value,
      orc::presenters::ClosedCaptionFieldDataView field2);

  /**
   * @brief Emitted when dropout analysis data is ready
   */
  void dropoutDataReady(uint64_t request_id,
                        orc::presenters::DropoutDisplaySeries series);

  /**
   * @brief Emitted when SNR analysis data is ready
   */
  void snrDataReady(uint64_t request_id,
                    orc::presenters::SNRDisplaySeries series);

  /**
   * @brief Emitted when burst level analysis data is ready
   */
  void burstLevelDataReady(uint64_t request_id,
                           orc::presenters::BurstLevelDisplaySeries series);

  /**
   * @brief Emitted when a stage's catalogue is ready
   */
  void catalogueDataReady(uint64_t request_id, orc::CatalogueDataset data);

  /**
   * @brief Emitted when a results read found nothing to read
   *
   * The node's stage has not been triggered since the DAG was last built, so
   * there is nothing to show. Distinct from error(): nothing has gone wrong,
   * the reader simply needs to trigger the stage first.
   */
  void resultsNotAvailable(uint64_t request_id);

  /**
   * @brief Emitted when available outputs query completes
   */
  void availableOutputsReady(uint64_t request_id,
                             std::vector<orc::PreviewOutputInfo> outputs);

  /**
   * @brief Emitted when an audio channel-pair enumeration completes
   *
   * An empty @p pairs list is the normal "this node has no audio" answer.
   * Marshalled from the worker thread via a queued connection.
   */
  void audioChannelPairsReady(uint64_t request_id,
                              std::vector<orc::AudioPairView> pairs);

  /**
   * @brief Emitted when a requested audio playback reader has been created
   *
   * @p reader is null when the pair turned out to be unusable. Marshalled from
   * the worker thread via a queued connection; the reader must be primed off
   * the GUI thread (an EFM decode can take minutes).
   */
  void audioStreamReaderReady(
      uint64_t request_id,
      std::shared_ptr<orc::presenters::IAudioStreamReader> reader);

  /**
   * @brief Emitted when line samples are ready
   */
  void lineSamplesReady(uint64_t request_id, LineSamplesDeliveryPtr delivery);

  /**
   * @brief Emitted when a frame's samples are ready
   *
   * Carries everything the timing and waveform dialogues need, including the
   * video parameters, shared rather than copied per consumer.
   */
  void frameSamplesReady(uint64_t request_id, FrameSamplesDeliveryPtr delivery);

  /**
   * @brief Emitted during trigger progress
   */
  void triggerProgress(size_t current, size_t total, QString message);

  /**
   * @brief Emitted when trigger completes
   */
  void triggerComplete(uint64_t request_id, bool success, QString status);

  /**
   * @brief Emitted when frame line navigation result is ready
   */
  void frameLineNavigationReady(uint64_t request_id,
                                orc::FrameLineNavigationResult result);

  /**
   * @brief Emitted on any error
   */
  void error(uint64_t request_id, QString message);

  /**
   * @brief Emitted (on the GUI thread) when a project edit invalidates stored
   *        observations
   *
   * Carries the ids of the nodes whose stored observations became stale
   * (edited node plus downstream descendants). Marshalled from the worker
   * thread via Qt's queued connection.
   */
  void observationsInvalidated(QVector<int> changed_node_ids);

  /**
   * @brief Emitted (on the GUI thread) when a requestObservations() response is
   *        ready
   *
   * @param request_id     Id returned by requestObservations()
   * @param available      True when the frame's observations were produced
   * @param field_id_value Field the observations are for (FieldID::value())
   * @param video_params   Video-parameter observer view model (empty if absent)
   * @param ntsc           NTSC observer view model (empty if absent)
   *
   * Marshalled from the worker/scheduler thread via a queued connection.
   */
  void observationDataReady(
      uint64_t request_id, bool available, qulonglong field_id_value,
      orc::presenters::VideoParameterObservationView video_params,
      orc::presenters::NtscFieldObservationsView ntsc);

  /**
   * @brief Emitted (on the GUI thread) when the background observation workload
   *        changes (Task 5.4)
   *
   * @param active            True while background observation work is running
   * @param percent_complete  Overall completion, 0..100
   * @param computing         True when the batch has actually computed frames;
   *                          false while it only verifies stored coverage
   * @param outstanding_nodes Distinct nodes with pending work
   * @param sweep_paused      True while queued whole-node sweep work is being
   *                          held back (see setPlaybackActive())
   *
   * Marshalled from the scheduler's worker thread via a queued connection.
   */
  void observationProgress(bool active, int percent_complete, bool computing,
                           qulonglong outstanding_nodes, bool sweep_paused);

  /**
   * @brief Emitted (on the GUI thread) as each node of an on-demand preview
   *        execution starts
   *
   * A preview query (available outputs, render) executes the DAG up to the
   * queried node. Opening a large source takes many seconds inside a single
   * node, so the view uses these events to report what the worker is doing
   * instead of leaving the window silent.
   *
   * @param node_id_value NodeID::value() of the node about to run
   * @param current       1-based position in the execution order
   * @param total         Number of nodes in the execution order
   *
   * Marshalled from the worker thread via a queued connection.
   */
  void executionProgress(int node_id_value, qulonglong current,
                         qulonglong total);

 private:
  // ========================================================================
  // Worker thread methods (run on worker thread only)
  // ========================================================================

  /**
   * @brief Main worker thread loop
   */
  void workerLoop();

  /**
   * @brief Process a single request
   */
  void processRequest(std::unique_ptr<RenderRequest> request);

  /**
   * @brief Handle UpdateDAG request
   */
  void handleUpdateDAG(const UpdateDAGRequest& req);

  /**
   * @brief Handle RenderPreview request
   */
  void handleRenderPreview(const RenderPreviewRequest& req);

  /**
   * @brief Handle GetObservations request
   */
  void handleGetObservations(const GetObservationsRequest& req);

  /**
   * @brief Handle GetVBIData request
   */
  void handleGetVBIData(const GetVBIDataRequest& req);

  /**
   * @brief Handle GetClosedCaptionData request
   */
  void handleGetClosedCaptionData(const GetClosedCaptionDataRequest& req);

  /**
   * @brief Handle GetDropoutData request
   */
  void handleGetDropoutData(const GetDropoutDataRequest& req);

  /**
   * @brief Handle GetSNRData request
   */
  void handleGetSNRData(const GetSNRDataRequest& req);

  /**
   * @brief Handle GetBurstLevelData request
   */
  void handleGetBurstLevelData(const GetBurstLevelDataRequest& req);

  /**
   * @brief Handle GetCatalogueData request
   */
  void handleGetCatalogueData(const GetCatalogueDataRequest& req);

  /**
   * @brief Handle GetAvailableOutputs request
   */
  void handleGetAvailableOutputs(const GetAvailableOutputsRequest& req);

  /**
   * @brief Handle GetAudioChannelPairs request
   */
  void handleGetAudioChannelPairs(const GetAudioChannelPairsRequest& req);

  /**
   * @brief Handle CreateAudioStreamReader request
   */
  void handleCreateAudioStreamReader(const CreateAudioStreamReaderRequest& req);

  /**
   * @brief Handle GetLineSamples request
   */
  void handleGetLineSamples(const GetLineSamplesRequest& req);

  /**
   * @brief Handle GetFrameSamples request
   */
  void handleGetFrameSamples(const GetFrameSamplesRequest& req);

  /**
   * @brief Handle NavigateFrameLine request
   */
  void handleNavigateFrameLine(const NavigateFrameLineRequest& req);

  void handleSavePNG(const SavePNGRequest& req);

  /**
   * @brief Handle TriggerStage request
   */
  void handleTriggerStage(const TriggerStageRequest& req);

  /**
   * @brief Enqueue a request (thread-safe)
   *
   * A request carrying a RequestCoalesceKey first sweeps the queue of anything
   * of its own type and key, so a consumer that re-asks per frame leaves at
   * most one outstanding question behind.
   */
  void enqueueRequest(std::unique_ptr<RenderRequest> request);

  /**
   * @brief Drop every queued (not yet started) preview render
   *
   * Caller must hold queue_mutex_. Returns how many were removed, for logging.
   * The request being processed is already off the queue and is unaffected; it
   * still finishes and is dropped by the existing stale-response check.
   */
  size_t discardQueuedPreviewsLocked();

  /**
   * @brief Drop every queued (not yet started) request of one type and key
   *
   * Caller must hold queue_mutex_. Returns how many were removed, for logging.
   * As with previews, a request the worker has already taken is unaffected;
   * its response is dropped by the consumer's own stale check.
   */
  size_t discardQueuedCoalescedLocked(RenderRequestType type,
                                      const RequestCoalesceKey& key);

  /**
   * @brief Get next request ID (thread-safe)
   */
  uint64_t nextRequestId();

  // ========================================================================
  // Thread synchronization
  // ========================================================================

  std::thread worker_thread_;
  std::atomic<bool> shutdown_requested_{false};

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  // A deque rather than a queue: superseded previews are removed from the
  // middle of the pending work (discardQueuedPreviewsLocked()).
  std::deque<std::unique_ptr<RenderRequest>> request_queue_;

  std::atomic<uint64_t> next_request_id_{1};
  std::atomic<uint64_t> latest_preview_request_id_{0};

  // Whether a preview is playing. Read by the worker when it creates the
  // presenter, so a playback session that began before the first DAG build
  // still suppresses sweeps. Guarded by queue_mutex_, like worker_project_.
  bool playback_active_ = false;

  // Newest-only audio queries: the viewed node and the selected pair both
  // change faster than a heavy DAG can answer, so superseded responses are
  // dropped rather than delivered to a dialogue that has moved on.
  std::atomic<uint64_t> latest_audio_pairs_request_id_{0};
  std::atomic<uint64_t> latest_audio_reader_request_id_{0};

  // ========================================================================
  // Worker thread state (owned by worker thread, never accessed from GUI)
  // ========================================================================

  std::shared_ptr<const void> worker_dag_;
  std::shared_ptr<orc::presenters::IRenderPresenter> worker_render_presenter_;
  void* worker_project_{nullptr};  // Non-owning opaque handle for presenter
  RenderPresenterFactory presenter_factory_;

  // Phase 3: invalidation subscription held on the worker presenter (0 = none).
  uint64_t worker_invalidation_subscription_{0};

  // Phase 5: workload-progress subscription on the worker presenter (0 = none).
  uint64_t worker_progress_subscription_{0};

  // Phase 2.7: Trigger state now managed by RenderPresenter
  // Removed: trigger_cancel_requested_ and current_trigger_stage_
};

/**
 * @brief Create an IRenderPresenter backed by a concrete RenderPresenter
 *
 * Used by GUI components (e.g. the dropout editor) that render through the
 * presenter interface so tests can substitute a fake implementation.
 *
 * @param project_handle Opaque core project handle (must be non-null and
 * outlive the returned presenter)
 */
std::shared_ptr<orc::presenters::IRenderPresenter> makeRenderPresenterAdapter(
    void* project_handle);

#endif  // RENDER_COORDINATOR_H
