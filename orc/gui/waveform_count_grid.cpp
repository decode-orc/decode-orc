/*
 * File:        waveform_count_grid.cpp
 * Module:      orc-gui
 * Purpose:     Retained hit-count grid behind the waveform monitor's trace
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "waveform_count_grid.h"

#include <algorithm>

namespace orc::gui {

void WaveformCountGrid::reset(int x_samples, int y_bins) {
  x_samples_ = std::max(0, x_samples);
  y_bins_ = std::max(0, y_bins);

  const std::size_t cells =
      static_cast<std::size_t>(x_samples_) * static_cast<std::size_t>(y_bins_);

  if (counts_.size() == cells) {
    // The common case once a preview is playing: same picture, same shape.
    std::fill(counts_.begin(), counts_.end(), 0u);
    return;
  }
  // assign() keeps the capacity when shrinking and grows at most once.
  counts_.assign(cells, 0u);
}

std::uint32_t WaveformCountGrid::maxIn(int x_lo, int x_hi, int y_lo,
                                       int y_hi) const {
  if (counts_.empty()) {
    return 0;
  }
  x_lo = std::max(0, x_lo);
  y_lo = std::max(0, y_lo);
  x_hi = std::min(x_hi, x_samples_ - 1);
  y_hi = std::min(y_hi, y_bins_ - 1);

  std::uint32_t largest = 0;
  for (int x = x_lo; x <= x_hi; ++x) {
    const std::size_t column =
        static_cast<std::size_t>(x) * static_cast<std::size_t>(y_bins_);
    for (int y = y_lo; y <= y_hi; ++y) {
      largest =
          std::max(largest, counts_[column + static_cast<std::size_t>(y)]);
    }
  }
  return largest;
}

}  // namespace orc::gui
