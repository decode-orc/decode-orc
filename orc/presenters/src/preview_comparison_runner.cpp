/*
 * File:        preview_comparison_runner.cpp
 * Module:      orc-presenters
 * Purpose:     Renders, checks, derives and exports A/B preview comparisons
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_comparison_runner.h"

#include <exception>
#include <sstream>
#include <utility>

#include "animated_webp_writer.h"
#include "preview_comparison.h"
#include "preview_renderer.h"

namespace orc::presenters {

namespace {

// The view types cannot include core headers, so they repeat the defaults;
// these keep the two in step.
static_assert(orc::kPreviewCompareStretchWindow == orc::kDefaultStretchWindow);
static_assert(orc::kPreviewCompareStretchGain == orc::kDefaultStretchGain);
static_assert(orc::kPreviewCompareDifferenceGain ==
              orc::kDefaultDifferenceGain);
static_assert(orc::kPreviewCompareAutoCropWidth == orc::kDefaultAutoCropWidth);
static_assert(orc::kPreviewCompareAutoCropHeight ==
              orc::kDefaultAutoCropHeight);

const char* output_type_name(orc::PreviewOutputType type) {
  switch (type) {
    case orc::PreviewOutputType::Frame_Field1:
      return "Field 1";
    case orc::PreviewOutputType::Frame_Field2:
      return "Field 2";
    case orc::PreviewOutputType::Frame_Field1_First:
      return "Frame";
    case orc::PreviewOutputType::Frame_Reversed:
      return "Frame (Reversed)";
    case orc::PreviewOutputType::Split:
      return "Split";
    case orc::PreviewOutputType::Luma:
      return "Luma";
    case orc::PreviewOutputType::Chroma:
      return "Chroma";
    case orc::PreviewOutputType::Composite:
      return "Composite";
  }
  return "Unknown";
}

std::string describe_output(orc::PreviewOutputType type,
                            const std::string& option_id) {
  std::string text = output_type_name(type);
  text +=
      option_id.empty() ? " (no option id)" : " (option '" + option_id + "')";
  return text;
}

/// "Field 1 'field', Frame 'frame'" — what a node offers, for an error.
std::string list_outputs(const std::vector<orc::PreviewOutputInfo>& outputs) {
  std::string text;
  for (const auto& output : outputs) {
    if (!output.is_available) {
      continue;
    }
    if (!text.empty()) {
      text += ", ";
    }
    text += output.display_name.empty() ? output_type_name(output.type)
                                        : output.display_name;
    text += " '" + output.option_id + "'";
  }
  return text.empty() ? "none" : text;
}

const orc::PreviewOutputInfo* find_output(
    const std::vector<orc::PreviewOutputInfo>& outputs,
    orc::PreviewOutputType type, const std::string& option_id) {
  for (const auto& output : outputs) {
    if (output.is_available && output.type == type &&
        output.option_id == option_id) {
      return &output;
    }
  }
  return nullptr;
}

std::string side_name(char side, NodeID node) {
  return std::string("node ") + side + " (" + node.to_string() + ")";
}

orc::PreviewComparisonSettings to_core(
    const orc::PreviewCompareExportSettings& settings) {
  orc::PreviewComparisonSettings core;
  switch (settings.crop_mode) {
    case orc::PreviewCompareCropMode::None:
      core.crop_mode = orc::ComparisonCropMode::None;
      break;
    case orc::PreviewCompareCropMode::Explicit:
      core.crop_mode = orc::ComparisonCropMode::Explicit;
      break;
    case orc::PreviewCompareCropMode::Auto:
      core.crop_mode = orc::ComparisonCropMode::Auto;
      break;
  }
  core.crop_rect = {settings.crop_x, settings.crop_y, settings.crop_width,
                    settings.crop_height};
  core.auto_crop_width = settings.auto_crop_width;
  core.auto_crop_height = settings.auto_crop_height;
  core.stretch = settings.stretch;
  core.stretch_window = settings.stretch_window;
  core.stretch_gain = settings.stretch_gain;
  core.enlarge = settings.scale;
  return core;
}

}  // namespace

PreviewComparisonRunner::PreviewComparisonRunner(IPreviewImageSource& source)
    : source_(source) {}

orc::PreviewCompareResult PreviewComparisonRunner::compare(
    const orc::PreviewCompareRequest& request) {
  if (kept_ && kept_->request == request) {
    return *kept_;
  }

  orc::PreviewCompareResult result;
  result.request = request;

  const auto outputs_a = source_.outputs(request.node_a);
  const auto outputs_b = source_.outputs(request.node_b);
  const auto* output_a =
      find_output(outputs_a, request.output_type, request.option_id);
  const auto* output_b =
      find_output(outputs_b, request.output_type, request.option_id);
  const std::string wanted =
      describe_output(request.output_type, request.option_id);
  if (output_a == nullptr) {
    result.error = side_name('A', request.node_a) + " has no " + wanted +
                   " output; it offers " + list_outputs(outputs_a);
    return result;
  }
  if (output_b == nullptr) {
    result.error = side_name('B', request.node_b) + " has no " + wanted +
                   " output; it offers " + list_outputs(outputs_b);
    return result;
  }
  result.count_a = output_a->count;
  result.count_b = output_b->count;

  const auto check_index = [&](char side, NodeID node, uint64_t count) {
    if (request.index < count) {
      return true;
    }
    // Numbered from 1, as both front ends show frames.
    std::ostringstream text;
    text << "frame " << request.index + 1 << " is beyond "
         << side_name(side, node) << ", which has ";
    if (count == 0) {
      text << "none";
    } else {
      text << count << " (frames 1 to " << count << ")";
    }
    text << ", so the two cannot be compared at this frame";
    result.error = text.str();
    return false;
  };
  if (!check_index('A', request.node_a, result.count_a) ||
      !check_index('B', request.node_b, result.count_b)) {
    return result;
  }

  if (result.count_a != result.count_b) {
    std::ostringstream text;
    text << side_name('A', request.node_a) << " has " << result.count_a
         << " and " << side_name('B', request.node_b) << " has "
         << result.count_b << " " << output_type_name(request.output_type)
         << " outputs; each side's index is its own, so the two may show "
            "different pictures";
    result.warnings.push_back(text.str());
  }

  auto render_side = [&](char side, NodeID node,
                         orc::PreviewImage& image) -> bool {
    auto rendered = source_.render(node, request.output_type, request.index,
                                   request.option_id);
    if (!rendered.success || !rendered.image.is_valid()) {
      result.error = "rendering " + side_name(side, node) + " failed";
      if (!rendered.error_message.empty()) {
        result.error += ": " + rendered.error_message;
      }
      return false;
    }
    image = std::move(rendered.image);
    return true;
  };
  if (!render_side('A', request.node_a, result.image_a) ||
      !render_side('B', request.node_b, result.image_b)) {
    return result;
  }

  const auto check =
      orc::check_comparison_compatible(result.image_a, result.image_b);
  if (!check.compatible) {
    result.error = check.message;
    return result;
  }

  result.success = true;
  kept_ = result;
  return result;
}

void PreviewComparisonRunner::invalidate() { kept_.reset(); }

orc::PreviewCompareViewImages PreviewComparisonRunner::derive(
    const orc::PreviewCompareResult& result,
    const orc::PreviewCompareViewSettings& settings) {
  orc::PreviewCompareViewImages view;
  if (!result.success) {
    view.error =
        result.error.empty() ? "the comparison has no images" : result.error;
    return view;
  }

  try {
    const auto& a = result.image_a;
    const auto& b = result.image_b;
    switch (settings.mode) {
      case orc::PreviewCompareViewMode::Normal:
        view.image_a = a;
        view.image_b = b;
        break;
      case orc::PreviewCompareViewMode::Stretched:
        view.image_a = orc::stretch_about_local_median(
            a, a, settings.stretch_window, settings.stretch_gain);
        view.image_b = orc::stretch_about_local_median(
            a, b, settings.stretch_window, settings.stretch_gain);
        break;
      case orc::PreviewCompareViewMode::NormalAndStretched:
        view.image_a = orc::compose_side_by_side(
            a, orc::stretch_about_local_median(a, a, settings.stretch_window,
                                               settings.stretch_gain));
        view.image_b = orc::compose_side_by_side(
            b, orc::stretch_about_local_median(a, b, settings.stretch_window,
                                               settings.stretch_gain));
        break;
      case orc::PreviewCompareViewMode::Difference:
        view.difference = orc::difference_image(a, b, settings.difference_gain);
        break;
    }
  } catch (const std::exception& error) {
    view.error = error.what();
    return view;
  }

  view.success = true;
  return view;
}

orc::PreviewCompareExportResult PreviewComparisonRunner::buildExport(
    const orc::PreviewCompareResult& result,
    const orc::PreviewCompareExportSettings& settings) {
  orc::PreviewCompareExportResult exported;
  exported.warnings = result.warnings;
  if (!result.success) {
    exported.error =
        result.error.empty() ? "the comparison has no images" : result.error;
    return exported;
  }

  try {
    const auto frames = orc::build_comparison_frames(
        result.image_a, result.image_b, to_core(settings));
    exported.bytes =
        orc::encode_animated_webp({frames.frame_a, frames.frame_b},
                                  settings.interval_ms, settings.loop_count);
    exported.crop_x = frames.crop.x;
    exported.crop_y = frames.crop.y;
    exported.crop_width = frames.crop.width;
    exported.crop_height = frames.crop.height;
    exported.frame_width = frames.frame_a.width;
    exported.frame_height = frames.frame_a.height;
    // The labels always differ, so compare what was cut from A and B.
    exported.identical = orc::crop(result.image_a, frames.crop).rgb_data ==
                         orc::crop(result.image_b, frames.crop).rgb_data;
  } catch (const std::exception& error) {
    exported.bytes.clear();
    exported.error = error.what();
    return exported;
  }

  exported.success = true;
  return exported;
}

orc::PreviewCompareExportResult PreviewComparisonRunner::exportComparison(
    const orc::PreviewCompareRequest& request,
    const orc::PreviewCompareExportSettings& settings,
    const std::string& path) {
  const auto compared = compare(request);
  auto exported = buildExport(compared, settings);
  if (!exported.success) {
    return exported;
  }
  try {
    orc::write_file(path, exported.bytes);
  } catch (const std::exception& error) {
    exported.success = false;
    exported.error = error.what();
  }
  exported.bytes.clear();
  if (!exported.success || settings.difference_path.empty()) {
    return exported;
  }

  std::string problem;
  try {
    const auto difference =
        orc::crop(orc::difference_image(compared.image_a, compared.image_b,
                                        settings.difference_gain),
                  {exported.crop_x, exported.crop_y, exported.crop_width,
                   exported.crop_height});
    problem = writePng(difference, settings.scale, settings.difference_path);
  } catch (const std::exception& error) {
    problem = error.what();
  }
  if (!problem.empty()) {
    exported.success = false;
    exported.error = "the difference image was not written: " + problem;
  }
  return exported;
}

std::string PreviewComparisonRunner::writePng(const orc::PreviewImage& image,
                                              uint32_t scale,
                                              const std::string& path) {
  if (!image.is_valid()) {
    return "the image is empty";
  }
  try {
    const bool written = scale == 1
                             ? orc::PreviewRenderer::save_png(image, path)
                             : orc::PreviewRenderer::save_png(
                                   orc::enlarge_nearest(image, scale), path);
    if (!written) {
      return "could not write " + path;
    }
  } catch (const std::exception& error) {
    return error.what();
  }
  return {};
}

}  // namespace orc::presenters
