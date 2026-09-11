/*
 * File:        preview_renderer.h
 * Module:      orc-core
 * Purpose:     Preview rendering for GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

// Stage and plugin implementations may use this header for preview rendering.
// GUI/CLI code must NOT include this directly; use RenderPresenter instead.

#include <orc/stage/common_types.h>  // For PreviewOutputType, AspectRatioMode
#include <orc/stage/frame_id.h>
#include <orc/stage/node_id.h>
#include <orc/stage/preview/orc_rendering.h>  // For public API types (DropoutRegion, PreviewImage)
#include <orc/stage/preview/preview_stage_types.h>
#include <orc/stage/preview/stage_custom_preview_renderer.h>
#include <orc/stage/preview/stage_preview_capability.h>
#include <orc/stage/video_frame_representation.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../analysis/vectorscope/vectorscope_data.h"
#include "dag_frame_renderer.h"
#include "preview_frame_layout.h"

namespace orc {

// PreviewOutputType and AspectRatioMode now defined in common_types.h

// Use public API DropoutRegion in core (already aliased in dropout_decision.h,
// but include here for clarity)
using DropoutRegion = orc::DropoutRegion;

// Use view-type structs (defined in orc_rendering.h)
using SuggestedViewNode = orc::SuggestedViewNode;
using PreviewOutputInfo = orc::PreviewOutputInfo;
using PreviewItemDisplayInfo = orc::PreviewItemDisplayInfo;

// Note: PreviewImage and PreviewRenderResult are used directly from view-types
// Use public API mapping result types in core
using FrameLineNavigationResult = orc::FrameLineNavigationResult;
using ImageToFieldMappingResult = orc::ImageToFieldMappingResult;
using FieldToImageMappingResult = orc::FieldToImageMappingResult;
using FrameFieldsResult = orc::FrameFieldsResult;

/**
 * @brief Preview renderer for GUI
 *
 * This class handles ALL rendering logic for the GUI:
 * - Queries available output types at a node
 * - Renders specific outputs (field N, frame N, etc.) to RGB888
 * - Handles field weaving for frames
 * - Handles sample scaling (CVBS_U10_4FSC -> 8-bit RGB)
 * - Future: chroma decoding, composite generation
 *
 * The GUI is responsible ONLY for:
 * - Displaying the RGB888 data
 * - User interaction (selecting node, output type, index)
 * - Aspect ratio correction for display
 *
 * Thread safety: Not thread-safe. Use from single thread only.
 */
class PreviewRenderer {
 public:
  /**
   * @brief Construct a preview renderer
   * @param dag The DAG to render previews from
   */
  explicit PreviewRenderer(std::shared_ptr<const DAG> dag);

  ~PreviewRenderer() = default;

  // Prevent copying
  PreviewRenderer(const PreviewRenderer&) = delete;
  PreviewRenderer& operator=(const PreviewRenderer&) = delete;

  // Move operations deleted - internal renderer contains non-movable components
  PreviewRenderer(PreviewRenderer&&) = delete;
  PreviewRenderer& operator=(PreviewRenderer&&) = delete;

  // ========================================================================
  // Query API
  // ========================================================================

  /**
   * @brief Get available output types for a node
   *
   * @param node_id The node to query
   * @return Vector of output info, or empty if node doesn't exist
   *
   * Example output:
   * - Field: 400 fields available
   * - Frame (Even-Odd): 200 frames available
   * - Frame (Odd-Even): 200 frames available
   * - Luma: 400 fields available
   */
  std::vector<PreviewOutputInfo> get_available_outputs(const NodeID& node_id);

  /**
   * @brief Get the count of outputs for a specific type
   *
   * @param node_id The node to query
   * @param type The output type
   * @return Number of outputs, or 0 if type not available
   */
  uint64_t get_output_count(const NodeID& node_id, PreviewOutputType type);

  // ========================================================================
  // Render API
  // ========================================================================

  /**
   * @brief What the last render_output() call cost on the render worker.
   *
   * Preview rendering runs the upstream chain with the artifact cache
   * disabled, so a stage's execute() is called again for every displayed
   * frame. Whether that is where a playing preview's time goes is a question
   * about a particular project's graph, not one inspection can answer, so the
   * renderer reports it and the profiler in the view decides what to say.
   *
   * Microseconds spent inside DAG execution, summed over however many times
   * the last render had to execute. Zero before the first render. Read on the
   * thread that called render_output().
   */
  std::int64_t last_execution_us() const { return last_execution_us_; }

  /**
   * @brief Render a specific output
   *
   * @param node_id The node to render from
   * @param type The output type (field, frame, etc.)
   * @param index The output index (0-based)
   * @param delivery Which pixel representation to produce.  Planes are
   *        available only from the colour-carrier path; every other path
   *        produces the RGB image whatever is asked for, since there is no
   *        conversion there worth moving (the signal-domain mapping is a
   *        table lookup).
   * @return Rendered image result
   *
   * Examples:
   * - render_output("node_1", PreviewOutputType::Frame_Field1, 100) -> field
   * 100
   * - render_output("node_1", PreviewOutputType::Frame_EvenOdd, 50) -> frame 50
   * (even first)
   */
  PreviewRenderResult render_output(
      const NodeID& node_id, PreviewOutputType type, uint64_t index,
      const std::string& option_id = "",
      PreviewNavigationHint hint = PreviewNavigationHint::Random,
      PreviewPixelDelivery delivery = PreviewPixelDelivery::Rgb);

  /**
   * @brief Update the DAG reference
   *
   * Call this when the DAG changes (nodes added/removed/modified).
   * This will invalidate any cached render results.
   *
   * @param dag The new DAG to use
   */
  void update_dag(std::shared_ptr<const DAG> dag);

  /**
   * @brief Set whether to render dropout regions onto the image
   * @param show True to render dropouts, false to hide
   */
  void set_show_dropouts(bool show);

  /**
   * @brief Get whether dropout rendering is enabled
   * @return True if dropouts are rendered, false otherwise
   */
  bool get_show_dropouts() const;

  /**
   * @brief Observe on-demand DAG execution driven by preview queries
   *
   * Preview queries (available outputs, renders) execute the DAG up to the
   * queried node on the calling thread. Opening a large source can take many
   * seconds inside a single node, so callers that need to report "still
   * working" install a callback here; it is invoked once per node, immediately
   * before that node executes, with the node's 1-based position in the
   * execution order and the order's length.
   *
   * Thread-safety: the callback is invoked synchronously on whichever thread
   * calls into the renderer, so it must not assume any particular thread.
   *
   * @param callback Progress sink, or an empty function to stop observing
   */
  void set_execution_progress_callback(DAGExecutor::ProgressCallback callback);

  /**
   * @brief Get the frame representation at a node
   *
   * This allows direct access to the underlying CVBS_U10_4FSC frame data
   * for operations like line scope display.
   *
   * @param node_id The node to get representation from
   * @return Shared pointer to the frame representation, or nullptr if not
   * available
   */
  VideoFrameRepresentationPtr get_representation_at_node(
      const NodeID& node_id) const;

  /**
   * @brief Navigate to next or previous line within a frame
   *
   * In frame mode with interlaced fields, this handles the complex logic of
   * toggling between fields and advancing lines. It accounts for the field
   * order (whether field 0 or field 1 is the first field in the frame).
   *
   * @param node_id The node being displayed
   * @param output_type The current output type (must be Frame or
   * Frame_Reversed)
   * @param current_field The current field index being displayed
   * @param current_line The current line within the field (0-based)
   * @param direction +1 to go down, -1 to go up
   * @param field_height The height of a single field in lines
   * @return Navigation result with new field/line or is_valid=false if out of
   * bounds
   *
   * Example usage:
   * - User clicks down arrow in line scope dialog
   * - Call navigate_frame_line(..., direction=1)
   * - If is_valid=true, fetch field at new_field_index, line new_line_number
   * - If is_valid=false, stay at current position (at boundary)
   */
  FrameLineNavigationResult navigate_frame_line(const NodeID& node_id,
                                                PreviewOutputType output_type,
                                                uint64_t current_field,
                                                int current_line, int direction,
                                                int field_height) const;

  /**
   * @brief Map preview image coordinates to field coordinates
   *
   * Converts an (x, y) position in the rendered preview image to the actual
   * field index and line number, accounting for:
   * - Output type (field/frame/split)
   * - Field ordering (parity hint)
   * - Reversed frame mode
   *
   * @param node_id The node being displayed
   * @param output_type The output type (Field, Frame, Frame_Reversed, Split)
   * @param output_index The current output index (field/frame number, 0-based)
   * @param image_y The Y coordinate in the preview image
   * @param image_height Total height of the preview image (for split mode)
   * @param option_id The preview option being displayed. Frame outputs may be
   *        rendered weaved or field-sequential (field 1 block above field 2
   *        block); only the option id distinguishes them, so it must be
   *        supplied for the row mapping to match what is on screen.
   * @return Mapping result with field_index and field_line, or is_valid=false
   */
  ImageToFieldMappingResult map_image_to_field(
      const NodeID& node_id, PreviewOutputType output_type,
      uint64_t output_index, int image_y, int image_height = 0,
      const std::string& option_id = "") const;

  /**
   * @brief Map field coordinates back to preview image coordinates
   *
   * Converts a (field_index, line_number) position back to the Y coordinate
   * in the rendered preview image. This is the reverse of map_image_to_field.
   * Used for positioning UI elements like cross-hairs.
   *
   * @param node_id The node being displayed
   * @param output_type The output type (Field, Frame, Frame_Reversed, Split)
   * @param output_index The current output index (field/frame number, 0-based)
   * @param field_index The field index to map
   * @param field_line The line within the field
   * @param image_height Total height of the image (for split mode)
   * @param option_id The preview option being displayed (see
   *        map_image_to_field; the frame layout depends on it)
   * @return Mapping result with image_y, or is_valid=false
   */
  FieldToImageMappingResult map_field_to_image(
      const NodeID& node_id, PreviewOutputType output_type,
      uint64_t output_index, uint64_t field_index, int field_line,
      int image_height = 0, const std::string& option_id = "") const;

  /**
   * @brief Get the field indices that make up a frame
   *
   * Returns which two fields comprise the given frame index,
   * accounting for field ordering (parity hint). This is needed
   * when the GUI wants to display metadata for both fields in a frame.
   *
   * @param node_id The node being displayed
   * @param frame_index The frame index (0-based)
   * @return Result with first_field and second_field indices
   */
  FrameFieldsResult get_frame_fields(const NodeID& node_id,
                                     uint64_t frame_index) const;

  // ========================================================================
  // Export API
  // ========================================================================

  /**
   * @brief Save rendered output to PNG file
   *
   * @param node_id The node to render from
   * @param type The output type (field, frame, etc.)
   * @param index The output index (0-based)
   * @param filename Path to PNG file to create
   * @param option_id Optional ID for custom preview renderer outputs (default:
   * "")
   * @return true if successful, false on error
   *
   * Example:
   * - save_png("node_1", PreviewOutputType::Frame_EvenOdd, 50,
   * "/tmp/frame50.png")
   */
  bool save_png(const NodeID& node_id, PreviewOutputType type, uint64_t index,
                const std::string& filename, const std::string& option_id = "",
                double aspect_correction = 1.0);

  /**
   * @brief Save a PreviewImage directly to PNG file
   *
   * @param image The rendered image to save
   * @param filename Path to PNG file to create
   * @return true if successful, false on error
   */
  bool save_png(const PreviewImage& image, const std::string& filename);

 private:
  /// DAG frame renderer for getting frame representations
  std::unique_ptr<DAGFrameRenderer> frame_renderer_;

  /// Current DAG reference
  std::shared_ptr<const DAG> dag_;

  /// DAG executor for on-demand execution
  mutable DAGExecutor dag_executor_;

  /// Whether to render dropout regions onto images
  bool show_dropouts_ = false;

  /// Microseconds the current render has spent in DAG execution. Reset at the
  /// top of render_output() and added to by ensure_node_executed(), which is
  /// const, hence mutable.
  mutable std::int64_t last_execution_us_ = 0;

  /// Cache of first_field_offset per node, computed once on the worker thread
  /// when available outputs are first queried or a frame is first rendered.
  /// This allows GUI-thread calls (getFrameFields, map_image_to_field, etc.) to
  /// determine field ordering without calling ensure_node_executed
  /// concurrently.
  mutable std::map<NodeID, uint64_t> first_field_offset_cache_;

  /// Ensure node has been executed (execute on-demand if needed). Returns the
  /// executor's node -> outputs map so callers that need the node's artifact
  /// can take it from here rather than rendering a frame to get at it.
  std::map<NodeID, std::vector<ArtifactPtr>> ensure_node_executed(
      const NodeID& node_id, bool disable_cache = false) const;

  /// The VideoFrameRepresentation at @p node_id, obtained by executing the DAG
  /// to that node and resolving its output artifact — no frame is rendered and
  /// no observer pass runs, because none of the callers need either. Sinks
  /// resolve to the upstream output their edge names (see resolve_node_vfr).
  /// Returns nullptr when the node has no representation, or when it has one
  /// with no frame 0 (which is what the previous render-based probe reported,
  /// and what makes get_representation_at_node() fall back upstream).
  VideoFrameRepresentationPtr representation_at(const NodeID& node_id) const;

  /// Field line counts of the frame being previewed at a node, or nullopt when
  /// the node has no representation to measure.
  std::optional<PreviewFieldGeometry> get_preview_field_geometry(
      const NodeID& node_id, uint64_t output_index) const;

  // ========================================================================
  // Internal rendering functions
  // ========================================================================

  /**
   * @brief Render dropout regions onto an image
   * @param image Image to render dropouts onto (modified in place)
   */
  void render_dropouts(PreviewImage& image) const;

  // ========================================================================
  // Stage preview support (new interface for sources/transforms)
  // ========================================================================

  /**
   * @brief Build preview output metadata from StagePreviewCapability.
   */
  std::vector<PreviewOutputInfo> get_capability_preview_outputs(
      const StagePreviewCapability& capability);

  /**
   * @brief Render a frame through the colour-carrier provider path.
   *
   * @param option_id Selects the display layout: kColourCarrierSequentialId
   *        stacks field 1 above field 2; any other value renders the
   *        interlaced (weaved) frame.
   */
  PreviewRenderResult render_colour_carrier_preview(
      const NodeID& stage_node_id, const class IColourPreviewProvider& provider,
      const StagePreviewCapability& capability, PreviewOutputType type,
      uint64_t index, const std::string& option_id,
      PreviewNavigationHint hint = PreviewNavigationHint::Random,
      PreviewPixelDelivery delivery = PreviewPixelDelivery::Rgb);

  /**
   * @brief Convert a vector of PreviewOptions to PreviewOutputInfo entries.
   *
   * Shared by the VFR path and the IStageCustomPreviewRenderer path.
   */
  std::vector<PreviewOutputInfo> build_outputs_from_options(
      const NodeID& stage_node_id, const DAGNode& stage_node,
      const std::vector<PreviewOption>& options);
};

}  // namespace orc
