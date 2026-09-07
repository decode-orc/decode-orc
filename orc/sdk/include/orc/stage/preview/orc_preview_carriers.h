/*
 * File:        orc_preview_carriers.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Typed preview carriers used by the Phase 2 preview pipeline.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: stage/preview — stage contract type crossing the plugin boundary.
// A layout change here bumps the host ABI version.

#include <orc/stage/preview/orc_preview_types.h>
#include <orc/stage/preview/orc_vectorscope.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace orc {

/**
 * @brief Colour-domain preview carrier with high-precision component planes.
 *
 * This carrier is intentionally display-agnostic. It preserves decoded Y/U/V
 * component values in a high-precision processing domain, along with explicit
 * colorimetric metadata. Conversion to display-target RGB is performed only at
 * the rendering boundary.
 */
struct ColourFrameCarrier {
  VideoDataType data_type{VideoDataType::ColourNTSC};
  ColorimetricMetadata colorimetry{};
  VideoSystem system{VideoSystem::Unknown};

  uint64_t frame_index{0};
  uint32_t width{0};
  uint32_t height{0};
  uint32_t active_x_start{0};
  uint32_t active_x_end{0};
  uint32_t active_y_start{0};
  uint32_t active_y_end{0};

  // Decoder-domain component planes. Each plane is width * height samples.
  std::vector<double> y_plane;
  std::vector<double> u_plane;
  std::vector<double> v_plane;

  std::optional<VectorscopeData> vectorscope_data;

  // CVBS_U10_4FSC anchor points used to normalize Y/U/V before matrix
  // conversion. Set by chroma_sink from SourceParameters. Values are in the
  // 10-bit CVBS domain (e.g. PAL: 256/256/844, NTSC: 240/282/800).
  // cvbs_black is the picture-black floor: blanking for PAL and for NTSC-J,
  // +7.5 IRE for standard NTSC/PAL_M. Y and U/V are both normalized over the
  // picture excursion cvbs_white - cvbs_black, which the setup pedestal
  // shortens; cvbs_blanking anchors instrument readouts, not the matrix.
  double cvbs_blanking{0.0};
  double cvbs_black{0.0};
  double cvbs_white{1023.0};

  bool is_valid() const {
    if (width == 0 || height == 0) {
      return false;
    }

    const size_t expected =
        static_cast<size_t>(width) * static_cast<size_t>(height);
    if (y_plane.size() != expected || u_plane.size() != expected ||
        v_plane.size() != expected) {
      return false;
    }

    return cvbs_white > cvbs_blanking;
  }
};

}  // namespace orc
