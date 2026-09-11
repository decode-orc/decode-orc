// File:        overlay.vert
// Module:      orc-gui
// Purpose:     Vertex stage for overlay quads and lines
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// Shared by the quad (triangle list) and line (line list) pipelines: the only
// difference between them is the topology, so one shader pair serves both.

#version 440

layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;

layout(location = 0) out vec4 v_color;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
} ubuf;

void main() {
    v_color = color;
    gl_Position = ubuf.mvp * vec4(position, 0.0, 1.0);
}
