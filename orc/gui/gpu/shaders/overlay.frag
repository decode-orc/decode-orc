// File:        overlay.frag
// Module:      orc-gui
// Purpose:     Fragment stage for overlay quads and lines
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// Colours are interpolated straight through; the pipeline blends them with
// source-alpha over the frame already in the render target, matching the
// QPainter composition the raster path uses.

#version 440

layout(location = 0) in vec4 v_color;

layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = v_color;
}
