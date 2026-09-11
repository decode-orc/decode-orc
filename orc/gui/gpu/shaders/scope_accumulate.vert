// File:        scope_accumulate.vert
// Module:      orc-gui
// Purpose:     Vertex stage for the scope accumulation pass
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// Positions arrive in canvas pixels with y down, the space the scope's plot
// geometry works in. Shared by the point and line pipelines: the only
// difference between them is the topology, so one shader pair serves both.

#version 440

layout(location = 0) in vec2 position;
layout(location = 1) in float weight;

layout(location = 0) out vec2 v_weight;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    // Which trace channel this draw deposits in: landings in the first,
    // beam transits in the second. A property of the draw rather than of the
    // vertex, so one buffer of samples serves both.
    vec4 channel;
} ubuf;

void main() {
    v_weight = weight * ubuf.channel.xy;
    // One canvas pixel per sample: the spot the beam leaves is the map pass's
    // business, not the rasteriser's.
    gl_PointSize = 1.0;
    gl_Position = ubuf.mvp * vec4(position, 0.0, 1.0);
}
