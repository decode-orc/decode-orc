/*
 * File:        preview_helpers.cpp
 * Module:      orc-sdk-support
 * Purpose:     Helper functions for stage preview rendering
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace orc {
namespace PreviewHelpers {

StagePreviewCapability make_signal_preview_capability(
    const std::shared_ptr<const VideoFrameRepresentation>& vfr) {
  if (!vfr || vfr->frame_count() == 0) return {};
  auto params = vfr->get_video_parameters();
  if (!params || !params->is_valid()) return {};

  const bool is_yc = vfr->has_separate_channels();
  VideoDataType data_type;
  if (params->system == VideoSystem::NTSC ||
      params->system == VideoSystem::PAL_M) {
    data_type = is_yc ? VideoDataType::YC_NTSC : VideoDataType::CompositeNTSC;
  } else {
    data_type = is_yc ? VideoDataType::YC_PAL : VideoDataType::CompositePAL;
  }

  uint32_t active_width =
      (params->active_video_end > params->active_video_start)
          ? static_cast<uint32_t>(params->active_video_end -
                                  params->active_video_start)
          : static_cast<uint32_t>(params->frame_width_nominal);
  uint32_t active_height =
      (params->last_active_frame_line > params->first_active_frame_line)
          ? static_cast<uint32_t>(params->last_active_frame_line -
                                  params->first_active_frame_line)
          : static_cast<uint32_t>(params->frame_height);

  // Fixed per-system pixel aspect: must not depend on the (possibly overridden)
  // active-area values, or changing the active window would rescale the whole
  // preview instead of re-framing it.
  const double dar_correction = standard_dar_correction(params->system);

  StagePreviewCapability cap;
  cap.supported_data_types = {data_type};
  cap.navigation_extent.item_count = vfr->frame_count();
  cap.navigation_extent.item_label = "frame";
  cap.navigation_extent.granularity = 1;
  cap.geometry.active_width = active_width;
  cap.geometry.active_height = active_height;
  cap.geometry.display_aspect_ratio = 4.0 / 3.0;
  cap.geometry.dar_correction_factor = dar_correction;
  return cap;
}

// Scale a 10-bit CVBS_U10_4FSC int16_t sample to an 8-bit grayscale value.
// Clamped mode maps [black_level, white_level] → [0, 255].
// Raw mode maps [sync_tip_level (-300 mV), peak_level (1000 mV)] → [0, 255].
inline uint8_t scale_10bit_to_8bit(int16_t sample, bool apply_level_scaling,
                                   int32_t black_level, int32_t white_level,
                                   int32_t sync_tip_level, int32_t peak_level) {
  if (apply_level_scaling) {
    int32_t range = white_level - black_level;
    if (range <= 0) return 0;
    int32_t scaled =
        ((static_cast<int32_t>(sample) - black_level) * 255) / range;
    return static_cast<uint8_t>(std::max(0, std::min(255, scaled)));
  } else {
    // Raw: map [sync_tip_level, peak_level] → [0, 255] so the full analogue
    // signal range (-300 mV to 1000 mV) spans the display range.
    int32_t range = peak_level - sync_tip_level;
    if (range <= 0) return 0;
    int32_t scaled =
        ((static_cast<int32_t>(sample) - sync_tip_level) * 255) / range;
    return static_cast<uint8_t>(std::max(0, std::min(255, scaled)));
  }
}

/**
 * @brief The greyscale mapping above, tabulated over the 10-bit sample domain.
 *
 * scale_10bit_to_8bit() costs an integer divide, and a preview frame is around
 * 710 000 pixels; the levels behind it are fixed for the whole frame, and in
 * practice for the whole source. Tabulating the 1024 samples the domain can
 * hold turns the inner loop into a load, and remembering which levels the
 * table was built for means a playing preview builds it once rather than once
 * a frame.
 *
 * A sample outside the domain - which a malformed source can produce - is
 * still put through the formula, so the table is an optimisation and never a
 * change of result.
 */
/// Which of a YC source's channels a preview option asked for.
enum class YCChannel { Composite, Luma, Chroma };

class GreyscaleLevelTable {
 public:
  void configure(bool apply_level_scaling, int32_t black_level,
                 int32_t white_level, int32_t sync_tip_level,
                 int32_t peak_level) {
    const Levels levels{apply_level_scaling, black_level, white_level,
                        sync_tip_level, peak_level};
    if (built_ && levels == levels_) {
      return;
    }
    levels_ = levels;
    built_ = true;
    for (int sample = 0; sample < kDomainSize; ++sample) {
      table_[sample] = scale(static_cast<int16_t>(sample));
    }
  }

  uint8_t operator()(int16_t sample) const {
    if (sample >= 0 && sample < kDomainSize) {
      return table_[static_cast<size_t>(sample)];
    }
    return scale(sample);
  }

 private:
  static constexpr int kDomainSize = 1024;

  struct Levels {
    bool apply_level_scaling = false;
    int32_t black = 0;
    int32_t white = 0;
    int32_t sync_tip = 0;
    int32_t peak = 0;

    bool operator==(const Levels& other) const {
      return apply_level_scaling == other.apply_level_scaling &&
             black == other.black && white == other.white &&
             sync_tip == other.sync_tip && peak == other.peak;
    }
  };

  uint8_t scale(int16_t sample) const {
    return scale_10bit_to_8bit(sample, levels_.apply_level_scaling,
                               levels_.black, levels_.white, levels_.sync_tip,
                               levels_.peak);
  }

  std::array<uint8_t, kDomainSize> table_{};
  Levels levels_;
  bool built_ = false;
};

/**
 * @brief Everything a preview option and a frame settle before any pixel.
 *
 * Resolved once and used by both the image path below and the plane path
 * beside it, so the two cannot disagree about which buffer line a display row
 * comes from, which levels scale it, or where the active picture is.
 */
struct ResolvedPreviewLayout {
  bool valid = false;
  FrameID frame_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool do_interlace = false;
  bool field1_on_even_rows = true;
  size_t field1_lines = 0;
  bool apply_level_scaling = false;
  YCChannel yc_channel = YCChannel::Composite;
  /// A copy rather than a pointer: get_video_parameters() returns an
  /// optional by value, so there is nothing to point at.
  SourceParameters video_params{};
};

ResolvedPreviewLayout resolve_preview_layout(
    const std::shared_ptr<const VideoFrameRepresentation>& representation,
    const std::string& option_id, uint64_t index) {
  ResolvedPreviewLayout layout{};

  if (!representation) {
    return layout;
  }

  const auto video_params = representation->get_video_parameters();
  if (!video_params || !video_params->is_valid()) {
    return layout;
  }

  layout.frame_id = representation->frame_range().first + index;
  if (!representation->has_frame(layout.frame_id)) {
    return layout;
  }

  const auto descriptor = representation->get_frame_descriptor(layout.frame_id);
  if (!descriptor) {
    return layout;
  }

  // Strip YC channel suffix appended by the GUI for YC sources.
  // "_yc" = luma+chroma combined (display luma), "_y" = luma, "_c" = chroma.
  // Check "_yc" before "_y"/"_c" to avoid partial matches.
  std::string base_option_id = option_id;
  if (representation->has_separate_channels()) {
    auto ends_with = [](const std::string& text, const std::string& suffix) {
      return text.size() >= suffix.size() &&
             text.compare(text.size() - suffix.size(), suffix.size(), suffix) ==
                 0;
    };
    if (ends_with(base_option_id, "_yc")) {
      base_option_id.resize(base_option_id.size() - 3);
      layout.yc_channel = YCChannel::Luma;
    } else if (ends_with(base_option_id, "_y")) {
      base_option_id.resize(base_option_id.size() - 2);
      layout.yc_channel = YCChannel::Luma;
    } else if (ends_with(base_option_id, "_c")) {
      base_option_id.resize(base_option_id.size() - 2);
      layout.yc_channel = YCChannel::Chroma;
    }
  }

  layout.apply_level_scaling = (base_option_id == "sequential_clamped" ||
                                base_option_id == "interlaced_clamped");
  // The option alone selects the layout; masking no longer forces interlacing.
  layout.do_interlace = (base_option_id == "interlaced_clamped" ||
                         base_option_id == "interlaced_raw");

  if (base_option_id != "sequential_clamped" &&
      base_option_id != "sequential_raw" &&
      base_option_id != "interlaced_clamped" &&
      base_option_id != "interlaced_raw") {
    ORC_LOG_WARN("PreviewHelpers (frame): Unknown preview option '{}'",
                 option_id);
    return layout;
  }

  layout.width = static_cast<uint32_t>(descriptor->samples_per_line_nominal);
  layout.height = static_cast<uint32_t>(descriptor->height);

  // Determine field-line count and dominance for interlaced weaving.
  // VFR field 1 is always the top spatial field; all systems use even display
  // rows.
  //   PAL:   field 1 (313 lines, top) → even display rows.
  //   NTSC:  field 1 (263 lines, top) → even display rows.
  //   PAL_M: field 1 (263 lines, top) → even display rows.
  layout.field1_lines = static_cast<size_t>(layout.height) / 2;
  layout.field1_on_even_rows = true;
  if (layout.do_interlace) {
    switch (video_params->system) {
      case VideoSystem::PAL:
        layout.field1_lines = static_cast<size_t>(kPalField1Lines);
        break;
      case VideoSystem::NTSC:
        layout.field1_lines = static_cast<size_t>(kNtscField1Lines);
        break;
      case VideoSystem::PAL_M:
        layout.field1_lines = static_cast<size_t>(kPalMField1Lines);
        break;
      default:
        break;
    }
  }

  layout.video_params = *video_params;
  layout.valid = layout.width > 0 && layout.height > 0;
  return layout;
}

/// The buffer line a display row is read from, under @p layout.
size_t buffer_line_for_row(const ResolvedPreviewLayout& layout,
                           uint32_t display_row) {
  if (!layout.do_interlace) {
    return display_row;
  }
  const bool use_field1 = (display_row % 2 == 0) == layout.field1_on_even_rows;
  size_t buf_line = use_field1 ? (display_row / 2)
                               : (layout.field1_lines + (display_row / 2));
  if (buf_line >= static_cast<size_t>(layout.height)) {
    buf_line = static_cast<size_t>(layout.height) - 1;
  }
  return buf_line;
}

/// The samples of one display row, or null when the line is not there.
const VideoFrameRepresentation::sample_type* line_for_row(
    const std::shared_ptr<const VideoFrameRepresentation>& representation,
    const ResolvedPreviewLayout& layout, uint32_t display_row) {
  // Per-line accessors account for PAL's non-uniform line lengths (1135 or
  // 1136 samples); a fixed buf_line * width stride would drift on field 2.
  const size_t buf_line = buffer_line_for_row(layout, display_row);
  switch (layout.yc_channel) {
    case YCChannel::Luma:
      return representation->get_line_luma(layout.frame_id, buf_line);
    case YCChannel::Chroma:
      return representation->get_line_chroma(layout.frame_id, buf_line);
    case YCChannel::Composite:
    default:
      return representation->get_line(layout.frame_id, buf_line);
  }
}

/**
 * @brief DropoutRun (frame-flat offsets) to DropoutRegion (display rows).
 *
 * Uses the same interlace/sequential layout the pixels do, so a dropout is
 * drawn on the line it actually fell on whichever way the frame is laid out.
 */
std::vector<DropoutRegion> map_dropouts_to_display_rows(
    const std::shared_ptr<const VideoFrameRepresentation>& representation,
    const ResolvedPreviewLayout& layout) {
  std::vector<DropoutRegion> regions;

  const SourceParameters& params = layout.video_params;
  const size_t spl_nom = static_cast<size_t>(layout.width);
  for (const auto& run : representation->get_dropout_hints(layout.frame_id)) {
    uint64_t offset = run.sample_start;
    uint32_t remaining = run.sample_count;
    while (remaining > 0) {
      auto [flat_line, sample_in_line] =
          frame_flat_offset_to_line_sample(params.system, spl_nom, offset);
      const size_t line_len =
          frame_line_sample_count(params.system, spl_nom, flat_line);
      const uint32_t samples_this_line = static_cast<uint32_t>(
          std::min<uint64_t>(remaining, line_len - sample_in_line));

      // Progress guard: a malformed/out-of-range dropout offset can make this
      // zero (sample_in_line == line_len, or line_len == 0), which would spin
      // the render worker forever. Bail out rather than hang (issue #209).
      if (samples_this_line == 0) break;

      int32_t field = 1;
      int32_t line_in_field = static_cast<int32_t>(flat_line);
      switch (params.system) {
        case VideoSystem::PAL:
          if (flat_line >= static_cast<size_t>(kPalField1Lines)) {
            field = 2;
            line_in_field = static_cast<int32_t>(flat_line) - kPalField1Lines;
          }
          break;
        case VideoSystem::NTSC:
          if (flat_line >= static_cast<size_t>(kNtscField1Lines)) {
            field = 2;
            line_in_field = static_cast<int32_t>(flat_line) - kNtscField1Lines;
          }
          break;
        case VideoSystem::PAL_M:
          if (flat_line >= static_cast<size_t>(kPalMField1Lines)) {
            field = 2;
            line_in_field = static_cast<int32_t>(flat_line) - kPalMField1Lines;
          }
          break;
        default:
          break;
      }

      uint32_t display_row = 0;
      if (layout.do_interlace) {
        display_row = (field == 1)
                          ? (layout.field1_on_even_rows
                                 ? static_cast<uint32_t>(line_in_field) * 2
                                 : static_cast<uint32_t>(line_in_field) * 2 + 1)
                          : (layout.field1_on_even_rows
                                 ? static_cast<uint32_t>(line_in_field) * 2 + 1
                                 : static_cast<uint32_t>(line_in_field) * 2);
      } else {
        // Sequential layout: display rows ARE frame-flat lines. Do not
        // recompose from field1_lines, which is height/2 here (312 for PAL)
        // while the field split above uses the true field-1 line count (313
        // for PAL) — recomposing shifted every field-2 region up one line.
        display_row = static_cast<uint32_t>(flat_line);
      }

      if (display_row < layout.height) {
        DropoutRegion region;
        region.line = display_row;
        region.start_sample = static_cast<uint32_t>(sample_in_line);
        region.end_sample =
            static_cast<uint32_t>(sample_in_line + samples_this_line);
        region.basis = DropoutRegion::DetectionBasis::HINT_DERIVED;
        regions.push_back(region);
      }

      offset += samples_this_line;
      remaining -= samples_this_line;
    }
  }

  return regions;
}

/**
 * @brief The rectangles of the frame that fall outside the active picture.
 *
 * The active-area line parameters [first,last) are expressed in weaved
 * (frame-flat) coordinates.  In the interlaced layout each display row
 * already IS its weaved line, so the mask is a single band.  In the
 * sequential layout the display stacks the field-1 buffer block above the
 * field-2 block, so each display row maps back to its weaved line —
 * producing one active band per field (twice the horizontal borders).
 *
 * Returned as rectangles so that both the pixel dimming below and a consumer
 * that draws the mask itself work from one description of it.
 */
std::vector<PreviewPlaneBand> inactive_area_bands(
    const ResolvedPreviewLayout& layout) {
  std::vector<PreviewPlaneBand> bands;

  const SourceParameters& params = layout.video_params;
  const int32_t ax0 = params.active_video_start;
  const int32_t ax1 = params.active_video_end;  // exclusive
  const int32_t ay0 = params.first_active_frame_line;
  const int32_t ay1 = params.last_active_frame_line;  // exclusive
  if (ax1 <= ax0 || ay1 <= ay0) {
    return bands;
  }

  // True field-1 line count (313 for PAL, not height/2); the sequential block
  // split is stored in the buffer using this count.  Qualified to reach the
  // free helper past the layout member of the same name.
  const size_t seq_field1_lines = orc::field1_lines(params.system);

  const auto row_is_active = [&](uint32_t row) {
    int32_t weaved_line = 0;
    if (layout.do_interlace) {
      weaved_line = static_cast<int32_t>(row);
    } else if (static_cast<size_t>(row) < seq_field1_lines) {
      // Field-1 block: buffer line row → weaved line (field 1 on even rows).
      const int32_t lif = static_cast<int32_t>(row);
      weaved_line = layout.field1_on_even_rows ? lif * 2 : lif * 2 + 1;
    } else {
      // Field-2 block: buffer line row − field1_lines → weaved line.
      const int32_t lif = static_cast<int32_t>(row - seq_field1_lines);
      weaved_line = layout.field1_on_even_rows ? lif * 2 + 1 : lif * 2;
    }
    return weaved_line >= ay0 && weaved_line < ay1;
  };

  const uint32_t left = static_cast<uint32_t>(std::max(ax0, 0));
  const uint32_t right =
      std::min(static_cast<uint32_t>(std::max(ax1, 0)), layout.width);

  // Rows of the same kind are emitted as one band, so an ordinary frame
  // comes out as three rectangles rather than as one per line.
  uint32_t run_start = 0;
  while (run_start < layout.height) {
    const bool active = row_is_active(run_start);
    uint32_t run_end = run_start + 1;
    while (run_end < layout.height && row_is_active(run_end) == active) {
      ++run_end;
    }
    const uint32_t run_height = run_end - run_start;

    if (!active) {
      bands.push_back({0, run_start, layout.width, run_height});
    } else {
      if (left > 0) {
        bands.push_back({0, run_start, left, run_height});
      }
      if (right < layout.width) {
        bands.push_back({right, run_start, layout.width - right, run_height});
      }
    }
    run_start = run_end;
  }

  return bands;
}

std::vector<PreviewOption> get_standard_preview_options(
    const std::shared_ptr<const VideoFrameRepresentation>& representation) {
  std::vector<PreviewOption> options;

  if (!representation) {
    return options;
  }

  auto video_params = representation->get_video_parameters();
  if (!video_params || !video_params->is_valid()) {
    return options;
  }

  size_t frame_count = representation->frame_count();
  if (frame_count == 0) {
    return options;
  }

  uint32_t width = static_cast<uint32_t>(video_params->frame_width_nominal);
  uint32_t height = static_cast<uint32_t>(video_params->frame_height);

  // Fixed per-system pixel aspect (see standard_dar_correction): independent of
  // the active-area values so changing the active window re-frames rather than
  // rescales the preview.
  const double dar_correction = standard_dar_correction(video_params->system);

  options.push_back(PreviewOption{"interlaced_clamped", "Interlaced Clamped",
                                  false, width, height, frame_count,
                                  dar_correction});
  options.push_back(PreviewOption{"interlaced_raw", "Interlaced Raw", false,
                                  width, height, frame_count, dar_correction});
  options.push_back(PreviewOption{"sequential_clamped", "Sequential Clamped",
                                  false, width, height, frame_count,
                                  dar_correction});
  options.push_back(PreviewOption{"sequential_raw", "Sequential Raw", false,
                                  width, height, frame_count, dar_correction});

  return options;
}

PreviewImage render_standard_preview(
    const std::shared_ptr<const VideoFrameRepresentation>& representation,
    const std::string& option_id, uint64_t index, PreviewNavigationHint hint,
    bool mask_inactive_area) {
  (void)hint;
  PreviewImage result{};

  const ResolvedPreviewLayout layout =
      resolve_preview_layout(representation, option_id, index);
  if (!layout.valid) {
    return result;
  }

  const SourceParameters& params = layout.video_params;
  const FrameID frame_id = layout.frame_id;
  const uint32_t width = layout.width;
  const uint32_t height = layout.height;
  const bool do_interlace = layout.do_interlace;
  const bool field1_on_even_rows = layout.field1_on_even_rows;
  const size_t field1_lines = layout.field1_lines;

  result.width = width;
  result.height = height;
  result.rgb_data.resize(static_cast<size_t>(width) * height * 3);

  // One table per rendering thread, carried between frames; configure() only
  // does work when the levels differ from the last frame's.
  thread_local GreyscaleLevelTable grey_table;
  grey_table.configure(layout.apply_level_scaling, params.black_level,
                       params.white_level, params.sync_tip_level,
                       params.peak_level);

  for (uint32_t display_row = 0; display_row < height; ++display_row) {
    const VideoFrameRepresentation::sample_type* line =
        line_for_row(representation, layout, display_row);
    if (!line) continue;

    for (uint32_t x = 0; x < width; ++x) {
      uint8_t gray = grey_table(line[x]);
      size_t offset = (static_cast<size_t>(display_row) * width + x) * 3;
      result.rgb_data[offset + 0] = gray;
      result.rgb_data[offset + 1] = gray;
      result.rgb_data[offset + 2] = gray;
    }
  }

  result.dropout_regions = map_dropouts_to_display_rows(representation, layout);

  // Dim the region outside the active picture so the full frame stays visible
  // at its normal size/aspect while the un-dimmed area shows exactly what the
  // exported output will crop to.  No cropping or rescaling — just a mask,
  // over the same rectangles a consumer drawing it for itself would be given.
  if (mask_inactive_area) {
    for (const PreviewPlaneBand& band : inactive_area_bands(layout)) {
      for (uint32_t y = band.y; y < band.y + band.height; ++y) {
        for (uint32_t x = band.x; x < band.x + band.width; ++x) {
          const size_t o = ((static_cast<size_t>(y) * width) + x) * 3;
          // Dim to ~30% so the excluded content is still faintly visible.
          result.rgb_data[o + 0] =
              static_cast<uint8_t>(result.rgb_data[o + 0] * 3 / 10);
          result.rgb_data[o + 1] =
              static_cast<uint8_t>(result.rgb_data[o + 1] * 3 / 10);
          result.rgb_data[o + 2] =
              static_cast<uint8_t>(result.rgb_data[o + 2] * 3 / 10);
        }
      }
    }
  }

  return result;
}

PreviewPlanes preview_planes_from_representation(
    const std::shared_ptr<const VideoFrameRepresentation>& representation,
    const std::string& option_id, uint64_t index, PreviewNavigationHint hint,
    bool mask_inactive_area) {
  (void)hint;
  PreviewPlanes planes{};

  const ResolvedPreviewLayout layout =
      resolve_preview_layout(representation, option_id, index);
  if (!layout.valid) {
    return planes;
  }

  const SourceParameters& params = layout.video_params;

  planes.domain = PreviewPlaneDomain::Signal;
  planes.width = layout.width;
  planes.height = layout.height;
  planes.apply_level_scaling = layout.apply_level_scaling;
  planes.black_level = static_cast<float>(params.black_level);
  planes.white_level = static_cast<float>(params.white_level);
  planes.sync_tip_level = static_cast<float>(params.sync_tip_level);
  planes.peak_level = static_cast<float>(params.peak_level);

  // The weave is applied here, as it is to the image: a display row is read
  // from the buffer line the layout says it comes from, so what reaches the
  // consumer is already in display order.
  planes.y_plane.resize(static_cast<size_t>(layout.width) * layout.height);
  for (uint32_t display_row = 0; display_row < layout.height; ++display_row) {
    const VideoFrameRepresentation::sample_type* line =
        line_for_row(representation, layout, display_row);
    if (!line) continue;
    float* out = planes.y_plane.data() +
                 (static_cast<size_t>(display_row) * layout.width);
    for (uint32_t x = 0; x < layout.width; ++x) {
      out[x] = static_cast<float>(line[x]);
    }
  }

  planes.dropout_regions = map_dropouts_to_display_rows(representation, layout);
  if (mask_inactive_area) {
    planes.dimmed_bands = inactive_area_bands(layout);
  }

  return planes;
}

}  // namespace PreviewHelpers
}  // namespace orc
