/*
 * File:        frame_plane_uniforms.h
 * Module:      orc-gui
 * Purpose:     Uniform packing for the frame surface's plane conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_GPU_FRAME_PLANE_UNIFORMS_H
#define ORC_GUI_GPU_FRAME_PLANE_UNIFORMS_H

#include <orc/stage/preview/orc_rendering.h>

#include <array>
#include <vector>

namespace orc::gui::gpu {

/// Floats in the plane-conversion uniform block: three vec4s.
constexpr int kFramePlaneUniformFloats = 12;

/**
 * @brief The transfer table's texture width.
 *
 * The table has more entries than the 2048-texel minimum GLES 3.0 guarantees
 * a one-dimensional texture, so it is stored as rows of this width and
 * addressed by (index & (width - 1), index >> log2(width)).  A power of two
 * keeps that addressing to a mask and a shift.
 */
constexpr int kFrameTransferLutWidth = 256;

/// Rows needed to hold @p entries at kFrameTransferLutWidth per row.
int frameTransferLutRows(int entries);

/**
 * @brief The transfer table padded out to a whole rectangle of texels.
 *
 * Entries past the table's end repeat its last value, so a fragment that
 * addresses one - which only an index past the final node can - reads the
 * curve's endpoint rather than whatever the allocation happened to contain.
 */
std::vector<float> packFrameTransferLut(const std::vector<float>& lut);

/**
 * @brief The levels the greyscale mapping scales a signal frame by.
 *
 * Layout, in the same block the colour conversion uses:
 *   [0..3]   range floor, range, unused, unused
 *   [4..11]  unused
 *
 * A clamped preview maps [black, white] onto the display range; a raw one
 * maps [sync tip, peak] so the full analogue excursion is visible. Which of
 * the two, and the levels themselves, come from the payload.
 */
std::array<float, kFramePlaneUniformFloats> packSignalPlaneUniforms(
    const orc::PreviewPlanes& planes);

/**
 * @brief Every constant the conversion needs, in the shader's block order.
 *
 * The reciprocals are formed here rather than in the shader because they are
 * per-frame constants: the shader would otherwise divide four million times
 * for a value that never changes within the frame.  What they are reciprocals
 * of comes from the payload, which carries the same numbers the CPU
 * conversion applies, so the two paths normalise by the same excursions.
 *
 * Layout:
 *   [0..3]   cvbs_black, 1/y_range, 1/(uv_range*kU), 1/(uv_range*kV)
 *   [4..7]   kr, kb, 1/kg, unused
 *   [8..11]  table width, table rows, table intervals, unused
 */
std::array<float, kFramePlaneUniformFloats> packFramePlaneUniforms(
    const orc::PreviewPlanes& planes);

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_GPU_FRAME_PLANE_UNIFORMS_H
