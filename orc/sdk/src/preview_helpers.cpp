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

  if (!representation) {
    return result;
  }

  auto video_params = representation->get_video_parameters();
  if (!video_params || !video_params->is_valid()) {
    return result;
  }

  FrameID frame_id = representation->frame_range().first + index;
  if (!representation->has_frame(frame_id)) {
    return result;
  }

  auto descriptor = representation->get_frame_descriptor(frame_id);
  if (!descriptor) {
    return result;
  }

  // Strip YC channel suffix appended by the GUI for YC sources.
  // "_yc" = luma+chroma combined (display luma), "_y" = luma, "_c" = chroma.
  // Check "_yc" before "_y"/"_c" to avoid partial matches.
  enum class YCChannel { Composite, Luma, Chroma };
  YCChannel yc_channel = YCChannel::Composite;
  std::string base_option_id = option_id;
  if (representation->has_separate_channels()) {
    auto ends_with = [](const std::string& s, const std::string& suffix) {
      return s.size() >= suffix.size() &&
             s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (ends_with(base_option_id, "_yc")) {
      base_option_id.resize(base_option_id.size() - 3);
      yc_channel = YCChannel::Luma;
    } else if (ends_with(base_option_id, "_y")) {
      base_option_id.resize(base_option_id.size() - 2);
      yc_channel = YCChannel::Luma;
    } else if (ends_with(base_option_id, "_c")) {
      base_option_id.resize(base_option_id.size() - 2);
      yc_channel = YCChannel::Chroma;
    }
  }

  const bool apply_level_scaling = (base_option_id == "sequential_clamped" ||
                                    base_option_id == "interlaced_clamped");
  // The option alone selects the layout; masking no longer forces interlacing.
  // The mask below maps each display row back to its weaved frame-flat line, so
  // it lands on the correct lines in both the interlaced (weaved) and the
  // sequential (field-1 block over field-2 block) layouts.
  const bool do_interlace = (base_option_id == "interlaced_clamped" ||
                             base_option_id == "interlaced_raw");

  if (base_option_id != "sequential_clamped" &&
      base_option_id != "sequential_raw" &&
      base_option_id != "interlaced_clamped" &&
      base_option_id != "interlaced_raw") {
    ORC_LOG_WARN("PreviewHelpers (frame): Unknown preview option '{}'",
                 option_id);
    return result;
  }

  uint32_t width = static_cast<uint32_t>(descriptor->samples_per_line_nominal);
  uint32_t height = static_cast<uint32_t>(descriptor->height);
  int32_t black_level = video_params->black_level;
  int32_t white_level = video_params->white_level;
  int32_t sync_tip_level = video_params->sync_tip_level;
  int32_t peak_level = video_params->peak_level;

  // Determine field-line count and dominance for interlaced weaving.
  // VFR field 1 is always the top spatial field; all systems use even display
  // rows.
  //   PAL:   field 1 (313 lines, top) → even display rows.
  //   NTSC:  field 1 (263 lines, top) → even display rows.
  //   PAL_M: field 1 (263 lines, top) → even display rows.
  size_t field1_lines = static_cast<size_t>(height) / 2;
  bool field1_on_even_rows = true;
  if (do_interlace) {
    switch (video_params->system) {
      case VideoSystem::PAL:
        field1_lines = static_cast<size_t>(kPalField1Lines);
        field1_on_even_rows = true;
        break;
      case VideoSystem::NTSC:
        field1_lines = static_cast<size_t>(kNtscField1Lines);
        field1_on_even_rows = true;
        break;
      case VideoSystem::PAL_M:
        field1_lines = static_cast<size_t>(kPalMField1Lines);
        field1_on_even_rows = true;
        break;
      default:
        break;
    }
  }

  result.width = width;
  result.height = height;
  result.rgb_data.resize(static_cast<size_t>(width) * height * 3);

  // One table per rendering thread, carried between frames; configure() only
  // does work when the levels differ from the last frame's.
  thread_local GreyscaleLevelTable grey_table;
  grey_table.configure(apply_level_scaling, black_level, white_level,
                       sync_tip_level, peak_level);

  for (uint32_t display_row = 0; display_row < height; ++display_row) {
    size_t buf_line;
    if (do_interlace) {
      const bool use_field1 = (display_row % 2 == 0) == field1_on_even_rows;
      buf_line =
          use_field1 ? (display_row / 2) : (field1_lines + display_row / 2);
      if (buf_line >= static_cast<size_t>(height)) {
        buf_line = static_cast<size_t>(height) - 1;
      }
    } else {
      buf_line = display_row;
    }

    // Per-line accessors account for PAL's non-uniform line lengths (1135 or
    // 1136 samples); a fixed buf_line * width stride would drift on field 2.
    const VideoFrameRepresentation::sample_type* line = nullptr;
    switch (yc_channel) {
      case YCChannel::Luma:
        line = representation->get_line_luma(frame_id, buf_line);
        break;
      case YCChannel::Chroma:
        line = representation->get_line_chroma(frame_id, buf_line);
        break;
      case YCChannel::Composite:
        line = representation->get_line(frame_id, buf_line);
        break;
    }
    if (!line) continue;

    for (uint32_t x = 0; x < width; ++x) {
      uint8_t gray = grey_table(line[x]);
      size_t offset = (static_cast<size_t>(display_row) * width + x) * 3;
      result.rgb_data[offset + 0] = gray;
      result.rgb_data[offset + 1] = gray;
      result.rgb_data[offset + 2] = gray;
    }
  }

  // Convert DropoutRun (frame-flat offsets) to DropoutRegion (display-row
  // coordinates) using the same interlace/sequential layout as above.
  auto dropout_runs = representation->get_dropout_hints(frame_id);
  const size_t spl_nom = static_cast<size_t>(width);
  for (const auto& run : dropout_runs) {
    uint64_t offset = run.sample_start;
    uint32_t remaining = run.sample_count;
    while (remaining > 0) {
      auto [flat_line, sample_in_line] = frame_flat_offset_to_line_sample(
          video_params->system, spl_nom, offset);
      size_t line_len =
          frame_line_sample_count(video_params->system, spl_nom, flat_line);
      uint32_t samples_this_line = static_cast<uint32_t>(
          std::min<uint64_t>(remaining, line_len - sample_in_line));

      // Progress guard: a malformed/out-of-range dropout offset can make this
      // zero (sample_in_line == line_len, or line_len == 0), which would spin
      // the render worker forever. Bail out rather than hang (issue #209).
      if (samples_this_line == 0) break;

      int32_t field = 1;
      int32_t line_in_field = static_cast<int32_t>(flat_line);
      switch (video_params->system) {
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

      uint32_t display_row;
      if (do_interlace) {
        display_row = (field == 1)
                          ? (field1_on_even_rows
                                 ? static_cast<uint32_t>(line_in_field) * 2
                                 : static_cast<uint32_t>(line_in_field) * 2 + 1)
                          : (field1_on_even_rows
                                 ? static_cast<uint32_t>(line_in_field) * 2 + 1
                                 : static_cast<uint32_t>(line_in_field) * 2);
      } else {
        // Sequential layout: display rows ARE frame-flat lines. Do not
        // recompose from field1_lines, which is height/2 here (312 for PAL)
        // while the field split above uses the true field-1 line count (313
        // for PAL) — recomposing shifted every field-2 region up one line.
        display_row = static_cast<uint32_t>(flat_line);
      }

      if (display_row < height) {
        DropoutRegion region;
        region.line = display_row;
        region.start_sample = static_cast<uint32_t>(sample_in_line);
        region.end_sample =
            static_cast<uint32_t>(sample_in_line + samples_this_line);
        region.basis = DropoutRegion::DetectionBasis::HINT_DERIVED;
        result.dropout_regions.push_back(region);
      }

      offset += samples_this_line;
      remaining -= samples_this_line;
    }
  }

  // Dim the region outside the active picture so the full frame stays visible
  // at its normal size/aspect while the un-dimmed area shows exactly what the
  // exported output will crop to.  No cropping or rescaling — just a mask.
  //
  // The active-area line parameters [first,last) are expressed in weaved
  // (frame-flat) coordinates.  In the interlaced layout each display row
  // already IS its weaved line, so the mask is a single band.  In the
  // sequential layout the display stacks the field-1 buffer block above the
  // field-2 block, so we map each display row back to its weaved line —
  // producing one active band per field (twice the horizontal borders).
  if (mask_inactive_area) {
    const int32_t ax0 = video_params->active_video_start;
    const int32_t ax1 = video_params->active_video_end;  // exclusive
    const int32_t ay0 = video_params->first_active_frame_line;
    const int32_t ay1 = video_params->last_active_frame_line;  // exclusive
    // True field-1 line count (313 for PAL, not height/2); the sequential
    // block split is stored in the buffer using this count.  qualified to reach
    // the free helper past the local field1_lines shadowing it.
    const size_t seq_field1_lines = orc::field1_lines(video_params->system);
    if (ax1 > ax0 && ay1 > ay0) {
      for (uint32_t y = 0; y < height; ++y) {
        int32_t weaved_line;
        if (do_interlace) {
          weaved_line = static_cast<int32_t>(y);
        } else if (static_cast<size_t>(y) < seq_field1_lines) {
          // Field-1 block: buffer line y → weaved line (field 1 on even rows).
          const int32_t lif = static_cast<int32_t>(y);
          weaved_line = field1_on_even_rows ? lif * 2 : lif * 2 + 1;
        } else {
          // Field-2 block: buffer line y − field1_lines → weaved line.
          const int32_t lif = static_cast<int32_t>(y - seq_field1_lines);
          weaved_line = field1_on_even_rows ? lif * 2 + 1 : lif * 2;
        }
        const bool row_active = weaved_line >= ay0 && weaved_line < ay1;
        for (uint32_t x = 0; x < width; ++x) {
          const bool col_active =
              static_cast<int32_t>(x) >= ax0 && static_cast<int32_t>(x) < ax1;
          if (row_active && col_active) continue;  // inside output area
          const size_t o = (static_cast<size_t>(y) * width + x) * 3;
          // Dim to ~30% so the excluded content is still faintly visible.
          result.rgb_data[o + 0] =
              static_cast<uint8_t>(result.rgb_data[o] * 3 / 10);
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

}  // namespace PreviewHelpers
}  // namespace orc
