/*
 * File:        render_presenter.h
 * Module:      orc-presenters
 * Purpose:     Rendering and preview presenter - MVP architecture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>  // PreviewOutputType
#include <orc/stage/field_id.h>
#include <orc/stage/node_id.h>
#include <orc/stage/orc_source_parameters.h>   // Public API SourceParameters
#include <orc/stage/params/parameter_types.h>  // ParameterValue
#include <orc/stage/preview/orc_rendering.h>   // Public API rendering types
#include <orc/stage/preview/preview_stage_types.h>  // PreviewNavigationHint
#include <orc/stage/tooling/catalogue_results.h>    // CatalogueDataset
#include <orc_analysis_series.h>  // Analysis display-series view types
#include <orc_audio_views.h>      // AudioPairView
#include <orc_preview_views.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio_stream_reader.h"            // IAudioStreamReader
#include "dag_execution_progress_view.h"    // DagExecutionProgressCallback
#include "observation_invalidation_view.h"  // ObservationInvalidationEvent
#include "observation_progress_view.h"  // ObservationProgressEvent, ObservationDataReadyCallback
#include "preview_render_cost_view.h"  // PreviewRenderCostView

// Forward declare core types
namespace orc {
class Project;
class DAG;
}  // namespace orc

namespace orc::presenters {

// Forward declarations
struct QualityMetrics;  // From metrics_presenter.h
using ProgressCallback = std::function<void(
    size_t, size_t, const std::string&)>;  // From project_presenter.h

/**
 * @brief Export format options
 */
enum class ExportFormat { PNG, TIFF, FFV1, ProRes };

/**
 * @brief Export options for sequence rendering
 */
struct ExportOptions {
  std::string output_path;  ///< Output file/directory path
  ExportFormat format;      ///< Export format
  int start_field;          ///< First field to export (-1 for all)
  int end_field;            ///< Last field to export (-1 for all)
  bool deinterlace;         ///< Whether to deinterlace
  int quality;              ///< Quality setting (0-100)
};

// Use public API types
using RenderProgress = orc::RenderProgress;

/**
 * @brief RenderPresenter - Manages preview and export rendering
 *
 * This presenter extracts rendering logic from the GUI layer.
 * It provides a clean interface for:
 * - Rendering preview images for specific nodes/fields
 * - Batch rendering with progress callbacks
 * - Analysis data requests (dropout, SNR, burst level)
 * - Managing render cache
 *
 * The presenter uses the core rendering pipeline but provides
 * a simplified interface suitable for GUI consumption.
 *
 * Thread safety: Methods are thread-safe when explicitly noted.
 * Preview rendering should be done from a worker thread.
 */
class RenderPresenter {
 public:
  /**
   * @brief Construct presenter for a project
   * @param project_handle Opaque handle to project
   */
  explicit RenderPresenter(void* project_handle);

  /**
   * @brief Destructor
   */
  ~RenderPresenter();

  // Disable copy, enable move
  RenderPresenter(const RenderPresenter&) = delete;
  RenderPresenter& operator=(const RenderPresenter&) = delete;
  RenderPresenter(RenderPresenter&&) noexcept;
  RenderPresenter& operator=(RenderPresenter&&) noexcept;

  // === DAG Management ===

  /**
   * @brief Adopt a DAG and rebuild the renderers and observation pipeline.
   *
   * Call this whenever the project changes (nodes added/removed/modified).
   * The DAG is built and owned elsewhere — ProjectPresenter::buildDAG() /
   * getDAG() — so that one graph backs every presenter looking at the project.
   *
   * @param dag_handle Opaque handle to DAG
   */
  void setDAG(std::shared_ptr<void> dag_handle);

  // === Observation Invalidation (Phase 3) ===

  /**
   * @brief Subscribe to observation-invalidation notifications.
   *
   * The callback fires whenever a DAG rebuild changes one or more nodes'
   * provenance fingerprints (a parameter or topology edit). It is invoked
   * synchronously on the thread that calls setDAG(); subscribers
   * must marshal to their own thread. The first DAG build does not notify (it
   * populates rather than invalidates).
   *
   * @param callback Invoked with the changed-node set (view types only).
   * @return Subscription id for unsubscribeInvalidation().
   *
   * Thread-safe: the subscriber registry is mutex-guarded.
   */
  uint64_t subscribeInvalidation(
      orc::presenters::ObservationInvalidationCallback callback);

  /**
   * @brief Cancel a subscription created by subscribeInvalidation().
   *
   * A no-op for an unknown id. Thread-safe.
   */
  void unsubscribeInvalidation(uint64_t subscription_id);

  // === Async Observations (Phase 5) ===

  /**
   * @brief Request a frame's observations without blocking on a render.
   *
   * Answered immediately (the callback fires synchronously before this returns)
   * when the provenance-keyed store already holds every observer's record for
   * the frame. Otherwise the frame is enqueued on the background scheduler at
   * interactive priority and @p callback fires later, on the scheduler's worker
   * thread, once the frame has been observed (or once its observation attempt
   * fails). No synchronous DAG execution happens on the calling thread.
   *
   * The delivered ObservationContext is valid only for the duration of the
   * callback; extract value-type view models inside it (see
   * ObservationDataReadyCallback).
   *
   * @param node_id  Node whose output frame is observed.
   * @param field_id Field of interest; both fields of its parent frame are
   *                 covered.
   * @param callback Delivery callback carrying the returned request id.
   * @return Request id echoed to @p callback for stale-response suppression.
   *
   * Thread-safe.
   */
  uint64_t requestObservations(
      NodeID node_id, FieldID field_id,
      orc::presenters::ObservationDataReadyCallback callback);

  /**
   * @brief Subscribe to background-observation workload snapshots (Task 5.4).
   *
   * The callback fires whenever the outstanding workload changes and returns to
   * an idle snapshot when the queue drains. Invoked on the scheduler's worker
   * thread; subscribers marshal to their own thread. Payload is view types
   * only.
   *
   * @return Subscription id for unsubscribeObservationProgress().
   *
   * Thread-safe.
   */
  uint64_t subscribeObservationProgress(
      orc::presenters::ObservationProgressCallback callback);

  /**
   * @brief Cancel a subscription created by subscribeObservationProgress().
   *
   * A no-op for an unknown id. Thread-safe.
   */
  void unsubscribeObservationProgress(uint64_t subscription_id);

  // === Preview Rendering ===

  /**
   * @brief Render a preview image for a specific output
   *
   * @param node_id Node to render from
   * @param output_type Type of output (Field, Frame, Luma, etc.)
   * @param output_index Index of the output (0-based)
   * @param option_id Optional rendering option ID
   * @param hint How the frame is being navigated to. Sequential tells stages
   *        that playback is running and adjacent frames are worth pre-fetching;
   *        scrubbing and one-off renders stay Random.
   * @return Preview render result with RGB image
   *
   * Thread-safe: Yes (uses internal DAG)
   */
  orc::PreviewRenderResult renderPreview(
      NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
      const std::string& option_id = "",
      orc::PreviewNavigationHint hint = orc::PreviewNavigationHint::Random);

  /**
   * @brief Get available output types for a node
   *
   * @param node_id Node to query
   * @return Vector of available output info
   *
   * Thread-safe: Yes
   */
  std::vector<orc::PreviewOutputInfo> getAvailableOutputs(NodeID node_id);

  /**
   * @brief Save a preview as PNG file
   *
   * @param node_id Node to render from
   * @param output_type Type of output
   * @param output_index Index of output
   * @param filename Path to save PNG
   * @param option_id Optional rendering option ID
   * @return true on success
   */
  bool savePNG(NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, const std::string& filename,
               const std::string& option_id = "",
               double aspect_correction = 1.0);

  /**
   * @brief Get registry-driven preview views applicable to a node/data type.
   */
  std::vector<orc::PreviewViewDescriptor> getAvailablePreviewViews(
      NodeID node_id, orc::VideoDataType data_type);

  /**
   * @brief Request preview-view data via the Phase 3 registry contract.
   */
  /**
   * @brief Video data types a node's stage declares it can preview.
   *
   * The stage's own declaration, not an inference from the preview output
   * type: it is what decides which registry-driven views apply to the node.
   * Empty when the node has no preview capability.
   *
   * @param node_id Node whose stage is inspected
   * @return Declared data types, in the stage's own preference order
   */
  std::vector<orc::VideoDataType> getStageDataTypes(NodeID node_id);

  orc::PreviewViewDataResult requestPreviewViewData(
      NodeID node_id, const std::string& view_id, orc::VideoDataType data_type,
      const orc::PreviewCoordinate& coordinate);

  /**
   * @brief Vectorscope and histogram payloads for one frame, one decode.
   *
   * Both scopes plot the same decoded carrier, so asking for them separately
   * costs two chroma decodes of the same frame. This fetches the carrier once
   * and extracts whatever @p request asks for. Call it on the render worker
   * alongside the preview render, never on the GUI thread.
   *
   * A signal-domain request has no carrier to share and no histogram, so its
   * vectorscope goes through the view registry as before; what it gains is
   * running here rather than on the GUI thread.
   *
   * @param node_id  Node whose stage provides the carrier.
   * @param request  Which scopes to produce, in which domain, for which frame
   *                 and line selection. A request for nothing does no work.
   * @return The payloads asked for. Everything is disengaged when the node
   *         provides nothing to plot.
   */
  orc::PreviewScopePayloads getPreviewScopes(
      NodeID node_id, const orc::PreviewScopeRequest& request);

  // === Analysis Data Access ===

  /**
   * @brief Get the decimated dropout display series from a sink stage
   *
   * The node must be a DropoutAnalysisSinkStage with results. The sink's
   * full-resolution per-frame series is decimated to the display budget and
   * returned as a typed view series; this method abstracts DAG traversal from
   * the GUI layer.
   *
   * @param node_id Node to get data from
   * @return The decimated series, or std::nullopt if the node is not a
   * triggered dropout sink
   */
  std::optional<DropoutDisplaySeries> getDropoutAnalysisData(NodeID node_id);

  /**
   * @brief Get the decimated SNR display series from a sink stage
   *
   * @param node_id Node to get data from
   * @return The decimated series, or std::nullopt if the node is not a
   * triggered SNR sink
   */
  std::optional<SNRDisplaySeries> getSNRAnalysisData(NodeID node_id);

  /**
   * @brief Get the decimated burst-level display series from a sink stage
   *
   * @param node_id Node to get data from
   * @return The decimated series, or std::nullopt if the node is not a
   * triggered burst-level sink
   */
  std::optional<BurstLevelDisplaySeries> getBurstLevelAnalysisData(
      NodeID node_id);

  /**
   * @brief Get the browsable catalogue from a stage that offers one
   *
   * The dataset is bounded by whatever cap the stage applies rather than by
   * the frame range, so unlike the graph series it needs no decimation, and it
   * is handed over whole.
   *
   * @param node_id Node to get data from
   * @param view_option One of orc::CatalogueSchema::view_options, or empty for
   *        the view the stage's own settings give. Passed to the stage
   *        verbatim; one it does not know means the same as empty
   * @param active_toggles Ids of the orc::CatalogueSchema::toggles the reader
   *        has on. Passed to the stage verbatim; one it offers and this does
   *        not name is off. Absent is not the same as empty: an empty list is
   *        a reader who has turned everything off, and std::nullopt is a
   *        reader who has not been asked yet, which is answered with whatever
   *        the stage has the toggles at by default
   * @return The catalogue, or std::nullopt if the node's stage does not expose
   *         orc::ICatalogueResults or has not been triggered
   */
  std::optional<orc::CatalogueDataset> getCatalogueData(
      NodeID node_id, const std::string& view_option = {},
      const std::optional<std::vector<std::string>>& active_toggles = {});

  /**
   * @brief Request dropout analysis data from a sink node (deprecated - use
   * getDropoutAnalysisData)
   *
   * The node must be a DropoutAnalysisSinkStage that has been triggered.
   *
   * @param node_id Node to get data from
   * @param request_id Unique request ID for async tracking
   * @param callback Callback when data is ready
   * @return true if request was queued
   */
  bool requestDropoutData(
      NodeID node_id, uint64_t request_id,
      std::function<void(uint64_t id, bool success, const std::string& error)>
          callback);

  /**
   * @brief Request SNR analysis data from a sink node (deprecated - use
   * getSNRAnalysisData)
   *
   * @param node_id Node to get data from
   * @param request_id Unique request ID
   * @param callback Callback when data is ready
   * @return true if request was queued
   */
  bool requestSNRData(
      NodeID node_id, uint64_t request_id,
      std::function<void(uint64_t id, bool success, const std::string& error)>
          callback);

  /**
   * @brief Request burst level analysis data from a sink node (deprecated - use
   * getBurstLevelAnalysisData)
   *
   * @param node_id Node to get data from
   * @param request_id Unique request ID
   * @param callback Callback when data is ready
   * @return true if request was queued
   */
  bool requestBurstLevelData(
      NodeID node_id, uint64_t request_id,
      std::function<void(uint64_t id, bool success, const std::string& error)>
          callback);

  // === Batch Rendering (Triggering) ===

  /**
   * @brief Trigger a triggerable stage (start batch processing)
   *
   * This method synchronously executes the trigger operation.
   * It should be called from a worker thread to avoid blocking the UI.
   *
   * @param node_id Node to trigger
   * @param callback Progress callback (called from the same thread)
   * @return Request ID for tracking
   * @throws std::runtime_error if triggering fails
   */
  uint64_t triggerStage(NodeID node_id, ProgressCallback callback);

  /**
   * @brief Trigger a stage with transient parameter overrides
   *
   * Like triggerStage() but merges the supplied overrides into the node's
   * stored parameters for this trigger only — the node's saved parameters
   * are not modified.
   *
   * @param node_id Node to trigger
   * @param callback Progress callback
   * @param parameter_overrides Key/value pairs that shadow the node's params
   * @return Request ID for tracking
   * @throws std::runtime_error if triggering fails
   */
  uint64_t triggerStage(
      NodeID node_id, ProgressCallback callback,
      std::map<std::string, ParameterValue> parameter_overrides);

  /**
   * @brief Cancel ongoing trigger operation
   *
   * This sets a cancellation flag that the trigger operation will check.
   * The operation may not stop immediately.
   */
  void cancelTrigger();

  /**
   * @brief Enable or disable this presenter's background observation pipeline.
   *
   * Enabled (the default), the first DAG build attaches the durable
   * observation sidecar and starts the worker-pool scheduler with its
   * background sweeps. Auxiliary presenters that only render frames or read
   * parameters (the dropout editor's, and MainWindow's short-lived helper
   * presenters) must disable this *before* the first setDAG():
   * they then run with a small in-memory store only — no sidecar attach, no
   * version purge/GC against a potentially multi-GB database, no scheduler,
   * no sweeps — keeping construction cheap enough for the GUI thread and
   * avoiding duplicate background pipelines (one per process is enough). No
   * effect once the store/scheduler have been created.
   */
  void setBackgroundObservationEnabled(bool enabled);

  /**
   * @brief What the last renderPreview() call cost on the render worker.
   *
   * Zero before the first render. Read on the thread that called
   * renderPreview(); the coordinator's worker does so immediately afterwards
   * and carries the numbers back with the frame.
   */
  orc::presenters::PreviewRenderCostView lastPreviewRenderCost() const;

  /**
   * @brief Tell the background pipeline that a preview is playing.
   *
   * While set, the scheduler holds back whole-node sweeps: they would otherwise
   * run on half the machine's cores for as long as a node has unobserved
   * frames, competing with the render thread that has to deliver a frame every
   * 40 ms. Queued sweep work is kept, not dropped, and resumes when playback
   * stops. Interactive and prefetch observations continue, so the frame in
   * front of the user is still observed.
   *
   * Safe from any thread. Remembered across DAG changes, so a scheduler
   * created while playback is running starts out paused.
   */
  void setPlaybackActive(bool active);

  /**
   * @brief Observe on-demand DAG execution driven by preview queries.
   *
   * getAvailableOutputs()/renderPreview() execute the DAG up to the queried
   * node on the calling thread; a single node (opening a large source) can take
   * many seconds. The callback fires once per node, immediately before that
   * node runs, so the view can report what the worker is doing. Pass an empty
   * function to stop observing.
   *
   * The callback survives DAG rebuilds — it is reinstalled on the renderer
   * every setDAG().
   *
   * Thread-safety: invoked synchronously on the thread driving the query; the
   * setter is expected to be called from that same thread (the render
   * coordinator's worker).
   */
  void setExecutionProgressCallback(
      orc::presenters::DagExecutionProgressCallback callback);

  // === Dropout Visualization ===

  /**
   * @brief Enable/disable dropout highlighting in previews
   * @param show Whether to show dropouts
   */
  void setShowDropouts(bool show);

  /**
   * @brief Get current dropout highlighting state
   * @return true if showing dropouts
   */
  bool getShowDropouts() const;

  // === Coordinate Mapping (for interactive features) ===

  /**
   * @brief Map image coordinates to field coordinates
   *
   * Used for determining which field/line user clicked on in preview.
   *
   * @param node_id Node being previewed
   * @param output_type Output type being displayed
   * @param output_index Output index being displayed
   * @param image_y Y coordinate in preview image
   * @param image_height Height of preview image
   * @param option_id Preview option being displayed; frame outputs can be
   *        weaved or field-sequential and only the option id says which
   * @return Mapping result with field index and line number
   */
  struct ImageToFieldMapping {
    bool is_valid;
    uint64_t field_index;
    int field_line;
  };

  ImageToFieldMapping mapImageToField(NodeID node_id,
                                      orc::PreviewOutputType output_type,
                                      uint64_t output_index, int image_y,
                                      int image_height,
                                      const std::string& option_id = "");

  /**
   * @brief Map field coordinates to image coordinates
   *
   * @param node_id Node being previewed
   * @param output_type Output type being displayed
   * @param output_index Output index being displayed
   * @param field_index Field index
   * @param field_line Line within field
   * @param image_height Height of preview image
   * @param option_id Preview option being displayed (see mapImageToField)
   * @return Image Y coordinate (is_valid=false if out of bounds)
   */
  struct FieldToImageMapping {
    bool is_valid;
    int image_y;
  };

  FieldToImageMapping mapFieldToImage(NodeID node_id,
                                      orc::PreviewOutputType output_type,
                                      uint64_t output_index,
                                      uint64_t field_index, int field_line,
                                      int image_height,
                                      const std::string& option_id = "");

  /**
   * @brief Get which fields comprise a frame
   *
   * @param node_id Node being queried
   * @param frame_index Frame index
   * @return Field indices (is_valid=false if invalid)
   */
  struct FrameFields {
    bool is_valid;
    uint64_t first_field;
    uint64_t second_field;
  };

  FrameFields getFrameFields(NodeID node_id, uint64_t frame_index);

  /**
   * @brief Navigate to next/previous line in frame preview
   *
   * @param node_id Node being previewed
   * @param output_type Output type
   * @param current_field Current field index
   * @param current_line Current line in field
   * @param direction +1 for down, -1 for up
   * @param field_height Height of a single field
   * @return New field index and line number
   */
  struct FrameLineNavigation {
    bool is_valid;
    uint64_t new_field_index;
    int new_line_number;
  };

  FrameLineNavigation navigateFrameLine(NodeID node_id,
                                        orc::PreviewOutputType output_type,
                                        uint64_t current_field,
                                        int current_line, int direction,
                                        int field_height);

  // === Line Samples (for waveform display) ===

  /**
   * @brief Line sample data with optional Y/C separation
   *
   * For frame outputs (two fields), heights reflect the actual field heights
   * from the VFR descriptors, which may be different for first vs second field.
   * For single field outputs, only first_field_height is used.
   */
  struct LineSampleData {
    std::vector<int16_t>
        composite_samples;  ///< Composite/combined samples (always populated)
    std::vector<int16_t> y_samples;  ///< Luma samples (only for Y/C sources)
    std::vector<int16_t> c_samples;  ///< Chroma samples (only for Y/C sources)
    bool has_separate_channels;      ///< True if Y/C separation is available
    int first_field_height = 0;  ///< Height of first field from VFR descriptor
    int second_field_height =
        0;  ///< Height of second field (0 if single field)

    /// The composite trace to plot.
    ///
    /// A Y/C source has no composite signal of its own; its luma is what a
    /// composite display shows. Duplicating the luma buffer into
    /// composite_samples to say so cost a full frame copy per extraction, so
    /// the substitution is made here instead and composite_samples is left
    /// empty. Consumers that plot a composite trace must read this rather
    /// than the member.
    const std::vector<int16_t>& composite() const {
      if (composite_samples.empty() && has_separate_channels) {
        return y_samples;
      }
      return composite_samples;
    }

    /// True when neither a composite nor a luma trace was extracted.
    bool empty() const {
      return composite_samples.empty() && y_samples.empty();
    }
  };

  /**
   * @brief Get line samples with Y/C separation for oscilloscope display
   *
   * For Y/C sources, returns separate Y and C samples in addition to composite.
   * For composite sources, only composite_samples is populated.
   *
   * @param node_id Node to get samples from
   * @param output_type Output type
   * @param output_index Output index
   * @param line_number Line number in the field/frame
   * @param sample_x X coordinate hint (for field selection in frames)
   * @param preview_width Width of preview image (for coordinate mapping)
   * @return LineSampleData with composite and optional Y/C samples
   */
  LineSampleData getLineSamplesWithYC(NodeID node_id,
                                      orc::PreviewOutputType output_type,
                                      uint64_t output_index, int line_number,
                                      int sample_x, int preview_width);

  /**
   * @brief Get all field samples for timing display
   *
   * Returns all samples from one or two fields (depending on output type).
   * For field output: returns samples from single field.
   * For frame outputs: returns samples from both fields in field order.
   *
   * @param node_id Node to get samples from
   * @param output_type Output type
   * @param output_index Output index
   * @return LineSampleData with all field samples concatenated
   */
  LineSampleData getFieldSamplesForTiming(NodeID node_id,
                                          orc::PreviewOutputType output_type,
                                          uint64_t output_index);

  /**
   * @brief Get video parameters for a node
   *
   * @param node_id Node to get parameters from
   * @return Video parameters if available
   */
  std::optional<orc::SourceParameters> getVideoParameters(NodeID node_id);

  /**
   * @brief Audio channel-pair descriptor names present at a node's output
   *
   * Resolves the representation produced at @p node_id and returns one entry
   * per audio channel pair — the pair's descriptor name, or an empty string
   * when it has none. Used by the GUI to build the audio_channel_map target
   * dropdown (index plus description) restricted to the pairs the node's input
   * actually carries. The pair count is the returned vector's size.
   *
   * @param node_id Node whose output representation is inspected
   * @return One name per channel pair; empty when no audio or unavailable
   */
  std::vector<std::string> getAudioChannelPairNames(NodeID node_id);

  /**
   * @brief Audio channel pairs available at a node's output
   *
   * Richer form of getAudioChannelPairNames() for callers that need the pair's
   * provenance as well as its name (the preview dialogue's audio selector).
   * Resolves the representation produced at @p node_id and returns one entry
   * per pair, in pair-index order.
   *
   * Returns an empty list — the normal case, not an error — when the node
   * carries no audio, when the representation cannot be resolved, or when the
   * video system is unknown (the SMPTE 272M-1994 §14.3 cadence is then
   * undefined, so no audio can be addressed).
   *
   * @param node_id Node whose output representation is inspected
   * @return One view per channel pair; empty when there is no usable audio
   */
  std::vector<orc::AudioPairView> getAudioChannelPairs(NodeID node_id);

  /**
   * @brief Create a frame-addressed reader for one audio channel pair
   *
   * Resolves the representation at @p node_id (which executes the DAG) and
   * wraps channel pair @p pair as an IAudioStreamReader. The reader holds the
   * representation alive for its own lifetime.
   *
   * @warning Must be called on the render worker thread: resolving the
   * representation executes the DAG, and the renderers are single-threaded by
   * contract. The returned reader may then be primed and read from one other
   * thread (see IAudioStreamReader).
   *
   * @param node_id Node whose output representation supplies the audio
   * @param pair    Channel-pair index, < getAudioChannelPairs().size()
   * @return Reader, or nullptr when the node has no such usable pair
   */
  std::shared_ptr<IAudioStreamReader> createAudioStreamReader(NodeID node_id,
                                                              size_t pair);

  /**
   * @brief Execute DAG to a specific node and return field representation
   *
   * This executes the DAG up to (not including) the specified node and returns
   * the field representation output. This is used for analysis tools that need
   * access to intermediate field data.
   *
   * @param node_id Node to execute to
   * @return Shared pointer to field representation (as void* for encapsulation)
   *
   * @note Returns core VideoFrameRepresentation. Analysis tools should
   * eventually migrate to presenter-based data access.
   */
  std::shared_ptr<const void> executeToNode(NodeID node_id);

  /**
   * @brief Get observation context after rendering a field
   *
   * This renders the field and returns the observation context which can be
   * used by presenters to extract various metrics and observations.
   *
   * @param node_id Node to render at
   * @param field_id Field to render
   * @return Pointer to observation context (as void* for encapsulation)
   *
   * @note Returns core ObservationContext. Metric presenters use this
   * to extract quality data without GUI having direct core access.
   */
  const void* getObservationContext(NodeID node_id, FieldID field_id);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orc::presenters
