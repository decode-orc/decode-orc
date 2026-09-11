// File:        frame_preview.vert
// Module:      orc-gui
// Purpose:     Textured-quad vertex stage for the frame preview surface
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// Positions arrive in render-target pixels (y down, origin top-left), which is
// the same space FrameViewGeometry works in. The uniform matrix is the
// orthographic projection of that space composed with the backend's
// clip-space correction, so nothing here needs to know which graphics API is
// in use.

#version 440

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;

layout(location = 0) out vec2 v_texcoord;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
} ubuf;

void main() {
    v_texcoord = texcoord;
    gl_Position = ubuf.mvp * vec4(position, 0.0, 1.0);
}
