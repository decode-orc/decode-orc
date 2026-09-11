/*
 * File:        orc_rendering.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Public API for rendering and preview types
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

// SDK TIER: stage/preview — stage contract type crossing the plugin boundary.
// A layout change here bumps the host ABI version.

#include <orc/stage/common_types.h>  // For PreviewOutputType, AspectRatioMode
#include <orc/stage/field_id.h>
#include <orc/stage/node_id.h>
#include <orc/stage/preview/orc_preview_types.h>  // Colorimetry enums
#include <orc/stage/preview/orc_vectorscope.h>    // For VectorscopeData

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orc {

/**
 * @brief Represents a dropout region in a field
 *
 * A dropout is a region where the video signal was lost or corrupted during
 * capture. This structure identifies the location and extent of the dropout.
 */
struct DropoutRegion {
  enum class DetectionBasis {
    SAMPLE_DERIVED,  ///< Detected from signal analysis
    HINT_DERIVED,    ///< From decoder hints
    CORROBORATED     ///< Both sample and hint agree
  };

  uint32_t line;          ///< Line number (0-based)
  uint32_t start_sample;  ///< Start sample within line
  uint32_t end_sample;    ///< End sample within line (exclusive)
  DetectionBasis basis;   ///< How this dropout was detected

  DropoutRegion()
      : line(0),
        start_sample(0),
        end_sample(0),
        basis(DetectionBasis::HINT_DERIVED) {}
};

/**
 * @brief Rendered preview image data
 *
 * Simple RGB888 image format for GUI display.
 * All rendering logic (sample scaling, field weaving, etc.) is done in core.
 */
struct PreviewImage {
  uint32_t width;
  uint32_t height;
  std::vector<uint8_t> rgb_data;  ///< RGB888 format (width * height * 3 bytes)
  std::vector<DropoutRegion> dropout_regions;  ///< Dropout regions to highlight
  std::optional<VectorscopeData>
      vectorscope_data;  ///< Optional vectorscope data for chroma analysis

  bool is_valid() const {
    return !rgb_data.empty() &&
           rgb_data.size() == static_cast<size_t>(width) * height * 3;
  }
};

/**
 * @brief Which pixel representation a render was asked to produce.
 *
 * The RGB image is what every caller has always received, and is what the
 * PNG export, the raster preview path and the golden-image tests consume.
 * A caller that will hand the frame to a graphics device asks for planes
 * instead: the conversion to display RGB is then the device's fragment
 * shader rather than a pass over four million samples on the render worker.
 *
 * The two are exclusive.  Producing both would pay the conversion this exists
 * to avoid, so a request for planes produces no image and vice versa.
 */
enum class PreviewPixelDelivery {
  Rgb,     ///< Display-ready RGB888 in PreviewRenderResult::image
  Planes,  ///< Unconverted component planes in PreviewRenderResult::planes
};

/// Which set of planes PreviewPlanes carries.
enum class PreviewPlaneDomain {
  None,    ///< No planes were produced
  Colour,  ///< Decoder-domain Y/U/V from a ColourFrameCarrier
  Signal,  ///< Composite samples from a VideoFrameRepresentation
};

/**
 * @brief A rectangle of the frame to be dimmed, in display coordinates.
 *
 * The region outside the active picture is shown at 30% so that the full
 * frame stays visible at its normal size while the un-dimmed area shows
 * exactly what the exported output will crop to.  Carried as rectangles
 * rather than applied to the samples because the consumer draws it, and
 * because in the sequential layout there is one active band per field.
 */
struct PreviewPlaneBand {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

/**
 * @brief A preview frame before its conversion to display RGB.
 *
 * Carries the decoder-domain component planes together with every constant
 * render_preview_from_colour_carrier() would have applied to them, so that a
 * consumer can reproduce that conversion exactly rather than approximate it.
 * Nothing here is display-referred: the rows are in the display order the RGB
 * image would have had (the field weave and any sequential re-ordering are
 * already applied), but the samples are still the decoder's.
 *
 * The planes are float rather than the carrier's double.  A graphics device
 * has no double-precision sampled format, and the conversion's output is an
 * 8-bit code, so the narrowing is below the visible result by six orders of
 * magnitude while halving what crosses the thread boundary.
 *
 * A signal-domain payload fills y_plane alone, with the composite samples in
 * their own 10-bit domain and the levels the greyscale mapping scales them
 * by.  The two domains share the plane because they share the machinery that
 * uploads it.
 */
struct PreviewPlanes {
  PreviewPlaneDomain domain = PreviewPlaneDomain::None;
  uint32_t width = 0;
  uint32_t height = 0;

  /// Decoder-domain component planes, width * height samples each.
  std::vector<float> y_plane;
  std::vector<float> u_plane;
  std::vector<float> v_plane;

  /// Picture-black floor and the excursions Y and U/V are normalised over.
  /// See render_preview_from_colour_carrier() for why both use the picture
  /// excursion rather than blanking-to-white.
  float cvbs_black = 0.0F;
  float y_range = 1.0F;
  float uv_range = 1.0F;

  /// Composite modulation factors divided out of U/V before the matrix.
  float composite_u = 1.0F;
  float composite_v = 1.0F;

  /// Luma coefficients of the matrix named by the carrier's colorimetry.
  float matrix_kr = 0.0F;
  float matrix_kb = 0.0F;

  /// The transfer decode and sRGB encode, composed and tabulated.  Shared
  /// rather than copied: the table depends only on the characteristic, so a
  /// playing preview hands out the same one every frame and a consumer can
  /// tell it has already uploaded this table by its identity alone.
  ColorimetricTransferCharacteristics transfer =
      ColorimetricTransferCharacteristics::Unspecified;
  std::shared_ptr<const std::vector<float>> transfer_lut;

  /// Signal domain: the levels the greyscale mapping scales the samples by.
  /// A clamped preview maps [black, white] onto the display range; a raw one
  /// maps [sync tip, peak], so that the full analogue excursion is visible.
  bool apply_level_scaling = false;
  float black_level = 0.0F;
  float white_level = 0.0F;
  float sync_tip_level = 0.0F;
  float peak_level = 0.0F;

  /// Dropout regions in display-row coordinates, as PreviewImage carries
  /// them.  Always populated, because the display draws its own markers from
  /// them whatever the setting says.
  std::vector<DropoutRegion> dropout_regions;

  /// True when the renderer would have burned the dropouts into the image.
  /// The consumer does it instead, over the converted frame and before it is
  /// rescaled, which is where the burn-in happens on the image path.
  bool burn_in_dropouts = false;

  /// Regions outside the active picture, in display coordinates.  Empty
  /// unless the stage asked for the inactive area to be masked.
  std::vector<PreviewPlaneBand> dimmed_bands;

  bool is_valid() const {
    if (domain == PreviewPlaneDomain::None || width == 0 || height == 0) {
      return false;
    }
    const size_t expected =
        static_cast<size_t>(width) * static_cast<size_t>(height);
    if (y_plane.size() != expected) {
      return false;
    }
    if (domain == PreviewPlaneDomain::Signal) {
      return true;
    }
    return u_plane.size() == expected && v_plane.size() == expected &&
           transfer_lut != nullptr && !transfer_lut->empty();
  }
};

/**
 * @brief Result of rendering a preview
 */
struct PreviewRenderResult {
  PreviewImage image;
  bool success;
  std::string error_message;
  NodeID node_id;
  PreviewOutputType output_type;
  uint64_t
      output_index;  ///< Which output was rendered (field N, frame N, etc.)

  /// Populated instead of |image| when the render was asked for planes.
  PreviewPlanes planes;

  // Vectorscope data (if rendering from a VideoSinkStage)
  std::optional<VectorscopeData> vectorscope_data;

  bool is_valid() const {
    return success && (image.is_valid() || planes.is_valid());
  }
};

/**
 * @brief Progress information for batch rendering operations
 */
struct RenderProgress {
  int current_field;
  int total_fields;
  std::string status_message;
  bool is_complete;
  bool has_error;
  std::string error_message;

  /// Calculate percentage complete (0-100)
  int percentage() const {
    if (total_fields == 0) return 0;
    return static_cast<int>((current_field * 100) / total_fields);
  }
};

/**
 * @brief Information about an available output type for preview
 */
struct PreviewOutputInfo {
  PreviewOutputType type;
  std::string display_name;  ///< Human-readable name
  uint64_t
      count;  ///< Number of outputs available (e.g., 100 fields, 50 frames)
  bool is_available;  ///< Whether this type is available for this node
  double dar_aspect_correction;  ///< Width scaling factor for 4:3 DAR (e.g.,
                                 ///< 0.7 for PAL/NTSC)
  std::string option_id;  ///< Option ID for IStageCustomPreviewRenderer outputs
  bool dropouts_available;  ///< Whether dropout highlighting is available for
                            ///< this output type
  bool has_separate_channels;   ///< Whether source has separate Y/C channels
                                ///< (for signal dropdown)
  uint64_t first_field_offset;  ///< Field offset for frame-based outputs (0 or
                                ///< 1, indicates first field of frame 0)
};

/**
 * @brief Detailed information for displaying an item in preview
 *
 * Provides all components needed for GUI to arrange labels as desired.
 */
struct PreviewItemDisplayInfo {
  std::string
      type_name;  ///< Type name (e.g., "Field", "Frame", "Frame (Reversed)")
  uint64_t current_number;       ///< Current item number (1-based)
  uint64_t total_count;          ///< Total number of items available
  uint64_t first_field_number;   ///< First field number (1-based, 0 if N/A)
  uint64_t second_field_number;  ///< Second field number (1-based, 0 if N/A)
  bool has_field_info;           ///< True if field numbers are relevant
};

/**
 * @brief Information about an aspect ratio mode option
 */
struct AspectRatioModeInfo {
  AspectRatioMode mode;
  std::string display_name;  ///< Human-readable name for GUI
  double
      correction_factor;  ///< Width scaling factor (1.0 for SAR, 0.7 for DAR)
};

/**
 * @brief Result of querying for suggested view node
 */
struct SuggestedViewNode {
  NodeID node_id;       ///< Node to view (invalid if none available)
  bool has_nodes;       ///< True if DAG has any nodes at all
  std::string message;  ///< User-facing message explaining the situation

  /// Helper to check if a valid node was suggested
  bool is_valid() const { return node_id.is_valid(); }
};

/**
 * @brief VBI data decoded from a field
 */
struct VBIData {
  bool has_data;  ///< True if VBI data was successfully decoded
  std::optional<int32_t> frame_number;  ///< Frame number if available
  std::string raw_data;  ///< Raw VBI data as string (for display)

  bool is_valid() const { return has_data; }
};

/**
 * @brief Observation data for a specific field
 *
 * Contains metadata and analysis results for a rendered field.
 */
struct ObservationData {
  bool has_data;           ///< True if observation data is available
  std::string stage_name;  ///< Name of the stage that provided this data
  std::map<std::string, std::string> metadata;  ///< Key-value metadata

  bool is_valid() const { return has_data; }
};

/**
 * @brief Result of navigating to next/previous line in frame mode
 *
 * When displaying a frame with two interlaced fields, moving up/down navigates
 * between alternating fields. This structure tells you which field and line to
 * fetch next.
 */
struct FrameLineNavigationResult {
  bool is_valid;             ///< True if navigation succeeded (within bounds)
  uint64_t new_field_index;  ///< Field index to render next
  int new_line_number;       ///< Line number to render next (within the field)
};

/**
 * @brief Result of mapping image coordinates to field coordinates
 *
 * Converts preview image coordinates (x, y) to field-space coordinates,
 * accounting for output type (field/frame/split) and field ordering.
 *
 * Note: field_line uses 0-based indexing (0 to field_height-1) to match
 * the core API (get_line, etc.). GUI code should convert to 1-based for
 * display.
 */
struct ImageToFieldMappingResult {
  bool is_valid;         ///< True if mapping succeeded
  uint64_t field_index;  ///< Field index for this position
  int field_line;        ///< Line number within the field (0-based: 0 to
                         ///< field_height-1)
};

/**
 * @brief Result of mapping field coordinates to image coordinates
 *
 * Converts field-space coordinates back to preview image coordinates.
 * Used for positioning UI elements like cross-hairs.
 */
struct FieldToImageMappingResult {
  bool is_valid;  ///< True if mapping succeeded
  int image_y;    ///< Y coordinate in the preview image
};

/**
 * @brief Result of querying which fields make up a frame
 *
 * Returns the two field indices that comprise a given frame,
 * accounting for field ordering (parity hint).
 */
struct FrameFieldsResult {
  bool is_valid;          ///< True if query succeeded
  uint64_t first_field;   ///< Index of first field in frame
  uint64_t second_field;  ///< Index of second field in frame
};

}  // namespace orc
