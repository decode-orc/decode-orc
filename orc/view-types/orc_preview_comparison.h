/*
 * File:        orc_preview_comparison.h
 * Module:      orc-view-types
 * Purpose:     A/B preview comparison requests, results, view and export
 *              settings, as the GUI and CLI see them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>
#include <orc/stage/node_id.h>
#include <orc/stage/preview/orc_rendering.h>

#include <cstdint>
#include <string>
#include <vector>

namespace orc {

// The defaults, repeated from core (which these types cannot include); the
// comparison runner checks at compile time that the two agree.
inline constexpr uint32_t kPreviewCompareStretchWindow = 15;
inline constexpr double kPreviewCompareStretchGain = 4.0;
inline constexpr double kPreviewCompareDifferenceGain = 4.0;
inline constexpr uint32_t kPreviewCompareAutoCropWidth = 320;
inline constexpr uint32_t kPreviewCompareAutoCropHeight = 240;

/// The name `orc-cli preview --output-type` takes for one output type.
struct PreviewOutputTypeCommandName {
  const char* name;
  PreviewOutputType type;
};

/// Every output type the CLI can name, in the order its help lists them.
/// orc-gui uses the same names when it shows the command that repeats a
/// comparison, so the two cannot drift apart.
inline constexpr PreviewOutputTypeCommandName kPreviewOutputTypeCommandNames[] =
    {
        {"field1", PreviewOutputType::Frame_Field1},
        {"field2", PreviewOutputType::Frame_Field2},
        {"frame", PreviewOutputType::Frame_Field1_First},
        {"frame-reversed", PreviewOutputType::Frame_Reversed},
        {"split", PreviewOutputType::Split},
        {"luma", PreviewOutputType::Luma},
        {"chroma", PreviewOutputType::Chroma},
        {"composite", PreviewOutputType::Composite},
};

/// The CLI name of @p type, or nullptr when the CLI has none for it.
inline const char* preview_output_type_command_name(PreviewOutputType type) {
  for (const auto& entry : kPreviewOutputTypeCommandNames) {
    if (entry.type == type) {
      return entry.name;
    }
  }
  return nullptr;
}

/**
 * @brief Two renders to compare: the same output, option and index of two
 *        nodes.
 *
 * The index is each node's own. No mapping between the two paths is
 * attempted, so a stage that renumbers frames on one path only can make the
 * same index show different pictures.
 */
struct PreviewCompareRequest {
  NodeID node_a;
  NodeID node_b;
  PreviewOutputType output_type = PreviewOutputType::Frame_Field1_First;
  std::string option_id;
  uint64_t index = 0;  ///< 0-based

  bool operator==(const PreviewCompareRequest& other) const {
    return node_a == other.node_a && node_b == other.node_b &&
           output_type == other.output_type && option_id == other.option_id &&
           index == other.index;
  }
  bool operator!=(const PreviewCompareRequest& other) const {
    return !(*this == other);
  }
};

/**
 * @brief The raw pair a comparison is made from.
 *
 * When success is false, error says why (a missing output or option, an index
 * out of range, a failed render or images of different sizes) and the images
 * may be empty.
 */
struct PreviewCompareResult {
  bool success = false;
  std::string error;
  PreviewCompareRequest request;
  PreviewImage image_a;  ///< Native size, no dropout highlighting
  PreviewImage image_b;
  uint64_t count_a = 0;  ///< Outputs of the requested type at each node
  uint64_t count_b = 0;
  /// Things that do not stop the comparison but may explain what it shows,
  /// such as the two nodes having different output counts.
  std::vector<std::string> warnings;
};

/// What a comparison view shows.
enum class PreviewCompareViewMode {
  Normal,              ///< A and B as rendered
  Stretched,           ///< A and B stretched about A's local median
  NormalAndStretched,  ///< Each side normal, then stretched, side by side
  Difference           ///< One image: B − A about mid grey
};

/// Settings for deriving the images a comparison view shows.
struct PreviewCompareViewSettings {
  PreviewCompareViewMode mode = PreviewCompareViewMode::Normal;
  uint32_t stretch_window = kPreviewCompareStretchWindow;  ///< Odd, at least 1
  double stretch_gain = kPreviewCompareStretchGain;
  double difference_gain = kPreviewCompareDifferenceGain;
};

/**
 * @brief The images a comparison view shows.
 *
 * For the Difference mode only difference is set; for the other modes
 * image_a and image_b are set and the view flickers between them.
 */
struct PreviewCompareViewImages {
  bool success = false;
  std::string error;
  PreviewImage image_a;
  PreviewImage image_b;
  PreviewImage difference;
};

/// How an export's crop is chosen.
enum class PreviewCompareCropMode {
  None,      ///< The whole image
  Explicit,  ///< PreviewCompareExportSettings::crop_*
  Auto       ///< The window where A and B differ most
};

/// Settings for an animated WebP export of a comparison.
struct PreviewCompareExportSettings {
  PreviewCompareCropMode crop_mode = PreviewCompareCropMode::None;
  /// The crop in native samples, used when crop_mode is Explicit.
  uint32_t crop_x = 0;
  uint32_t crop_y = 0;
  uint32_t crop_width = 0;
  uint32_t crop_height = 0;
  /// The window size, in native samples, when crop_mode is Auto.
  uint32_t auto_crop_width = kPreviewCompareAutoCropWidth;
  uint32_t auto_crop_height = kPreviewCompareAutoCropHeight;

  bool stretch = true;  ///< Add a stretched half beside the normal one
  uint32_t stretch_window = kPreviewCompareStretchWindow;
  double stretch_gain = kPreviewCompareStretchGain;

  uint32_t scale = 1;          ///< Nearest-neighbour factor, 1 – 8
  uint32_t interval_ms = 800;  ///< How long each of A and B is shown
  uint32_t loop_count = 0;     ///< 0 loops forever

  /// When set, exportComparison() also writes B − A about mid grey as a PNG
  /// here, cut to the same crop and enlarged by the same scale. Not used by
  /// an in-memory build.
  std::string difference_path;
  double difference_gain = kPreviewCompareDifferenceGain;
};

/// The result of building or writing an export.
struct PreviewCompareExportResult {
  bool success = false;
  std::string error;
  std::vector<uint8_t> bytes;  ///< The .webp file (empty when written to disk)
  /// The crop used, in native samples, so an auto crop can be repeated.
  uint32_t crop_x = 0;
  uint32_t crop_y = 0;
  uint32_t crop_width = 0;
  uint32_t crop_height = 0;
  uint32_t frame_width = 0;  ///< Size of each animation frame
  uint32_t frame_height = 0;
  /// A and B are the same inside the crop; the frames differ only in their
  /// A and B labels.
  bool identical = false;
  std::vector<std::string> warnings;  ///< From the comparison
};

}  // namespace orc
