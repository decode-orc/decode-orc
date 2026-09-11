// File:        scope_map.vert
// Module:      orc-gui
// Purpose:     Vertex stage for the scope map pass
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// One quad covering the part of the widget the canvas is drawn into.
// Positions are in render-target pixels; texture coordinates run 0-1 across
// the canvas.

#version 440

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;

layout(location = 0) out vec2 v_texcoord;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 trace_color;
    vec4 background_color;
    vec4 scales;
    vec4 params;
    vec4 centre;
} ubuf;

void main() {
    v_texcoord = texcoord;
    gl_Position = ubuf.mvp * vec4(position, 0.0, 1.0);
}
