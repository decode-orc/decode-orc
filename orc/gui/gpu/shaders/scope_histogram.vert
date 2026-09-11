// File:        scope_histogram.vert
// Module:      orc-gui
// Purpose:     Scatters the spread plot into a charge histogram
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// One point per canvas pixel, positioned by how bright that pixel is rather
// than by where it is: the rasteriser drops each pixel's charge into the
// bucket for its level, and additive blending sums them. That is the same
// sweep the CPU renderer makes over the plot, done by the part of the device
// built to scatter.
//
// Pixels the beam never reached carry no charge and are sent off-screen to be
// clipped rather than counted into the lowest bucket.

#version 440

layout(location = 0) out float v_charge;

layout(std140, binding = 0) uniform buf {
    vec4 config;  // canvas width, canvas height, bucket count, unused
} ubuf;

layout(binding = 1) uniform sampler2D plot;
layout(binding = 2) uniform sampler2D reduction;  // 1x1: peak, total

void main() {
    int width = int(ubuf.config.x);
    ivec2 at = ivec2(gl_VertexIndex % width, gl_VertexIndex / width);

    float charge = texelFetch(plot, at, 0).r;
    float peak = texelFetch(reduction, ivec2(0), 0).r;
    v_charge = charge;
    gl_PointSize = 1.0;

    if (charge <= 0.0 || peak <= 0.0) {
        gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
        return;
    }

    float buckets = ubuf.config.z;
    float bucket = min(buckets - 1.0, floor(charge * buckets / peak));
    gl_Position = vec4((((bucket + 0.5) / buckets) * 2.0) - 1.0, 0.0, 0.0, 1.0);
}
