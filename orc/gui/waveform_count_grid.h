/*
 * File:        waveform_count_grid.h
 * Module:      orc-gui
 * Purpose:     Retained hit-count grid behind the waveform monitor's trace
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_WAVEFORM_COUNT_GRID_H
#define ORC_GUI_WAVEFORM_COUNT_GRID_H

#include <cstdint>
#include <vector>

namespace orc::gui {

/**
 * @brief How many samples of a frame fell in each (column, millivolt bin).
 *
 * The waveform monitor's trace is a hit count: every sample of the active
 * picture lands in one cell, and the brightness of a pixel is the largest
 * count under it. The grid is a single flat buffer rather than a column of
 * vectors, and it is kept between frames: its shape is fixed by the active
 * picture width and the millivolt range, so a playing preview re-uses the same
 * storage and only has to zero it. Building a vector-of-vectors instead cost
 * roughly one heap allocation per active sample column, every frame.
 *
 * Cells are addressed column-major - all of a column's bins are contiguous -
 * because the render pass walks a column's bins together when reducing them to
 * an output pixel.
 */
class WaveformCountGrid {
 public:
  /**
   * @brief Shape the grid and zero every cell.
   *
   * Re-uses the existing allocation whenever the cell count is unchanged,
   * which is the case for every frame of a playing preview. A negative extent
   * is treated as zero.
   */
  void reset(int x_samples, int y_bins);

  int xSamples() const { return x_samples_; }
  int yBins() const { return y_bins_; }
  bool empty() const { return counts_.empty(); }

  /// Count one sample into (@p x, @p y_bin). Out-of-range cells are ignored,
  /// so the caller need not filter a signal that leaves the displayed range.
  void increment(int x, int y_bin) {
    if (x < 0 || x >= x_samples_ || y_bin < 0 || y_bin >= y_bins_) {
      return;
    }
    ++counts_[index(x, y_bin)];
  }

  /// Count in one cell; zero for a cell outside the grid.
  std::uint32_t at(int x, int y_bin) const {
    if (x < 0 || x >= x_samples_ || y_bin < 0 || y_bin >= y_bins_) {
      return 0;
    }
    return counts_[index(x, y_bin)];
  }

  /**
   * @brief Largest count in an inclusive block of cells.
   *
   * The render pass reduces however many cells fall under one output pixel to
   * the brightest of them, so no occupied bin is lost when the grid is wider
   * than the widget.
   */
  std::uint32_t maxIn(int x_lo, int x_hi, int y_lo, int y_hi) const;

  /// The raw cells, for tests that need to see the storage itself.
  const std::vector<std::uint32_t>& cells() const { return counts_; }

 private:
  std::size_t index(int x, int y_bin) const {
    return static_cast<std::size_t>(x) * static_cast<std::size_t>(y_bins_) +
           static_cast<std::size_t>(y_bin);
  }

  std::vector<std::uint32_t> counts_;
  int x_samples_ = 0;
  int y_bins_ = 0;
};

}  // namespace orc::gui

#endif  // ORC_GUI_WAVEFORM_COUNT_GRID_H
