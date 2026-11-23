/*
 * File:        preview_comparison_runner.h
 * Module:      orc-presenters
 * Purpose:     Renders, checks, derives and exports A/B preview comparisons
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc_preview_comparison.h>

#include <optional>
#include <string>

#include "preview_image_source.h"

namespace orc::presenters {

/**
 * @brief Turns a comparison request into a checked pair of images, and the
 *        pair into view images or an animated WebP.
 *
 * compare() keeps the last successful pair, keyed by its request, so a change
 * to how the pair is viewed or exported (stretch, difference, crop, scale)
 * does not render either node again. invalidate() drops it; call it whenever
 * the graph the source renders from changes.
 *
 * Thread safety: not thread-safe. Use it from the thread the image source is
 * used on (for RenderPresenter, the render worker). derive() and
 * buildExport() are static and touch no state, so any thread may call them.
 */
class PreviewComparisonRunner {
 public:
  /// @param source Must outlive the runner.
  explicit PreviewComparisonRunner(IPreviewImageSource& source);

  /**
   * @brief Render and check the pair a request names.
   *
   * Both nodes must offer the requested output type with the requested option
   * id, and the index must be within each node's count; otherwise the error
   * says which node falls short and, for a missing output, which outputs it
   * does offer. Images of different sizes are refused with both sizes. Two
   * nodes with different output counts give a warning, not an error.
   *
   * A request equal to the last successful one returns the kept pair without
   * rendering. A failed request is never kept.
   */
  orc::PreviewCompareResult compare(const orc::PreviewCompareRequest& request);

  /// Drop the kept pair, so the next compare() renders again.
  void invalidate();

  /**
   * @brief The images a comparison view shows.
   *
   * Stretched and difference images are computed from the result's native
   * images: O(samples × window) for a stretch, O(samples) for a difference.
   * The result must be a successful one.
   */
  static orc::PreviewCompareViewImages derive(
      const orc::PreviewCompareResult& result,
      const orc::PreviewCompareViewSettings& settings);

  /**
   * @brief Encode a comparison as a lossless animated WebP, in memory.
   *
   * The two frames are A and B, cropped, optionally with their stretched
   * halves, enlarged and labelled. The crop used is reported, so an auto crop
   * can be repeated.
   */
  static orc::PreviewCompareExportResult buildExport(
      const orc::PreviewCompareResult& result,
      const orc::PreviewCompareExportSettings& settings);

  /**
   * @brief Compare, encode and write the WebP to @p path.
   *
   * The returned bytes are left empty; everything else is as buildExport()
   * gives it, plus the comparison's warnings. When the settings name a
   * difference_path, B − A is written there too, at the same crop and scale;
   * failing to write it fails the export (the WebP is then already written).
   */
  orc::PreviewCompareExportResult exportComparison(
      const orc::PreviewCompareRequest& request,
      const orc::PreviewCompareExportSettings& settings,
      const std::string& path);

  /**
   * @brief Write an image as a PNG, enlarged nearest-neighbour by @p scale.
   * @return Empty on success, otherwise why the file was not written.
   */
  static std::string writePng(const orc::PreviewImage& image, uint32_t scale,
                              const std::string& path);

 private:
  IPreviewImageSource& source_;
  std::optional<orc::PreviewCompareResult> kept_;
};

}  // namespace orc::presenters
