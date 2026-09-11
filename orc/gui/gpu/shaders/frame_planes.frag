// File:        frame_planes.frag
// Module:      orc-gui
// Purpose:     Colour-carrier planes to display sRGB, on the device
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// The pixel loop of render_preview_from_colour_carrier(), one fragment per
// sample. Everything it divides by is a per-frame constant, so the uniform
// block carries reciprocals and this does no division at all.
//
// Source and destination are the same texel: the pass covers a target the
// size of the frame and addresses its planes by gl_FragCoord, so the mapping
// holds whichever way up the backend's framebuffer is - the same reason the
// scope canvas's spread and reduce passes fetch rather than interpolate.

#version 440

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 levels;  // picture black, 1/y range, 1/(uv range * kU), .. * kV
    vec4 matrix;  // kr, kb, 1/kg, unused
    vec4 table;   // transfer table width, rows, intervals, unused
} ubuf;

layout(binding = 1) uniform sampler2D y_plane;
layout(binding = 2) uniform sampler2D u_plane;
layout(binding = 3) uniform sampler2D v_plane;
layout(binding = 4) uniform sampler2D transfer_table;

// The composed transfer decode and sRGB encode, read from the table the CPU
// conversion interpolates between so that both paths apply the same curve
// sampled at the same nodes.
float transferNode(int index) {
    int width = int(ubuf.table.x);
    return texelFetch(transfer_table, ivec2(index % width, index / width), 0).r;
}

float encodeTransfer(float non_linear) {
    int intervals = int(ubuf.table.z);
    float scaled = non_linear * ubuf.table.z;
    int index = int(scaled);
    if (index >= intervals) {
        return transferNode(intervals);
    }
    float fraction = scaled - float(index);
    float low = transferNode(index);
    float high = transferNode(index + 1);
    return low + fraction * (high - low);
}

void main() {
    ivec2 here = ivec2(gl_FragCoord.xy);

    float y = (texelFetch(y_plane, here, 0).r - ubuf.levels.x) * ubuf.levels.y;

    // Un-weight the modulated U/V back into colour-difference signals, then
    // reconstruct R'G'B' from Y' and the two differences:
    //   R' = Y' + (R' - Y')
    //   B' = Y' + (B' - Y')
    //   G' = Y' - (kb (B' - Y') + kr (R' - Y')) / kg   [from the Y' equation]
    float b_minus_y = texelFetch(u_plane, here, 0).r * ubuf.levels.z;
    float r_minus_y = texelFetch(v_plane, here, 0).r * ubuf.levels.w;

    vec3 non_linear = vec3(
        y + r_minus_y,
        y - (((ubuf.matrix.y * b_minus_y) + (ubuf.matrix.x * r_minus_y)) *
             ubuf.matrix.z),
        y + b_minus_y);
    non_linear = clamp(non_linear, 0.0, 1.0);

    fragColor = vec4(encodeTransfer(non_linear.r), encodeTransfer(non_linear.g),
                     encodeTransfer(non_linear.b), 1.0);
}
