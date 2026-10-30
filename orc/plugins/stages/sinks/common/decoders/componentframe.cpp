/*
 * File:        componentframe.cpp
 * Module:      orc-core
 * Purpose:     Component frame buffer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2021 Adam Sampson
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "componentframe.h"

#include <algorithm>

ComponentFrame::ComponentFrame() : width(-1), height(-1) {}

void ComponentFrame::merge_luma_from(const ComponentFrame& luma_source) {
  assert(width == luma_source.width);
  assert(height == luma_source.height);
  yData = luma_source.yData;
}

void ComponentFrame::init(const ::orc::SourceParameters& videoParameters,
                          bool mono) {
  weave_ = false;
  width = videoParameters.frame_width_nominal;
  height = static_cast<int32_t>(
               ::orc::calculate_padded_field_height(videoParameters.system)) *
               2 -
           1;

  const int32_t size = width * height;

  yData.resize(size);
  std::fill(yData.begin(), yData.end(), 0.0);

  if (!mono) {
    uData.resize(size);
    std::fill(uData.begin(), uData.end(), 0.0);

    vData.resize(size);
    std::fill(vData.begin(), vData.end(), 0.0);
  } else {
    // Clear and deallocate U/V if they're not used.
    uData.clear();
    uData.shrink_to_fit();

    vData.clear();
    vData.shrink_to_fit();
  }
}
