// File:        scope_reduce.frag
// Module:      orc-gui
// Purpose:     One level of the scope canvas's peak and total reduction
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// Reduces the spread landing channel by four in each direction, carrying the
// largest value seen in red and the sum of them in green. Run to a single
// texel it answers the two questions the brightness anchor needs - how bright
// the plot's brightest pixel is, and how much charge the whole plot holds -
// without ever reading the plot back to the processor.
//
// The sums reach the order of a hundred million, which is why these targets
// are full-precision floating point while the plot itself is half.

#version 440

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 config;  // first level, source width, source height, unused
} ubuf;

layout(binding = 1) uniform sampler2D source;

void main() {
    ivec2 base = ivec2(gl_FragCoord.xy) * 4;
    ivec2 size = ivec2(ubuf.config.yz);
    bool first = ubuf.config.x > 0.5;

    float peak = 0.0;
    float total = 0.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            ivec2 at = base + ivec2(x, y);
            if (any(greaterThanEqual(at, size))) {
                continue;
            }
            vec4 texel = texelFetch(source, at, 0);
            if (first) {
                // The plot itself: landings are in red, and a pixel the beam
                // never reached holds no charge and is not counted.
                peak = max(peak, texel.r);
                total += texel.r;
            } else {
                peak = max(peak, texel.r);
                total += texel.g;
            }
        }
    }
    fragColor = vec4(peak, total, 0.0, 0.0);
}
