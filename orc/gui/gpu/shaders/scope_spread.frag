// File:        scope_spread.frag
// Module:      orc-gui
// Purpose:     One direction of the scope canvas's separable beam spot
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// Taps that fall outside the canvas are skipped rather than clamped to its
// edge, which is what the CPU renderer does: charge the beam laid down at the
// rim of the plot is lost there, not reflected back in.
//
// Both trace channels are spread together - landings in red, beam transits in
// green - because the kernel is the same for each.

#version 440

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 config;      // step x, step y, radius, unused
    vec4 canvas;      // width, height, unused, unused
    vec4 weights[4];  // half-kernel, index 0 the centre
} ubuf;

layout(binding = 1) uniform sampler2D source;

float weightAt(int index) {
    return ubuf.weights[index >> 2][index & 3];
}

void main() {
    ivec2 here = ivec2(gl_FragCoord.xy);
    ivec2 step = ivec2(ubuf.config.xy);
    ivec2 size = ivec2(ubuf.canvas.xy);
    int radius = int(ubuf.config.z);

    vec2 total = weightAt(0) * texelFetch(source, here, 0).rg;
    for (int t = 1; t <= radius; ++t) {
        float weight = weightAt(t);
        ivec2 before = here - (step * t);
        ivec2 after = here + (step * t);
        if (all(greaterThanEqual(before, ivec2(0)))) {
            total += weight * texelFetch(source, before, 0).rg;
        }
        if (all(lessThan(after, size))) {
            total += weight * texelFetch(source, after, 0).rg;
        }
    }
    fragColor = vec4(total, 0.0, 0.0);
}
