// File:        frame_planes_signal.frag
// Module:      orc-gui
// Purpose:     Composite samples to display greyscale, on the device
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// scale_10bit_to_8bit(), one fragment per sample. The CPU mapping is integer
// throughout - a multiply by 255 and a truncating divide - so the code is
// formed with floor() here rather than left to the render target's rounding,
// which would land half a code away for most of the domain.
//
// Shares the uniform block and the vertex stage with the colour conversion,
// so the pass around it is the same; only what the block means differs.

#version 440

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 levels;  // range floor, range, unused, unused
    vec4 matrix;  // unused on this path
    vec4 table;   // unused on this path
} ubuf;

layout(binding = 1) uniform sampler2D samples;

void main() {
    if (ubuf.levels.y <= 0.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Divided rather than multiplied by a reciprocal: the numerator and the
    // range are both whole numbers well inside what a float represents
    // exactly, so a quotient that is an integer comes out as that integer -
    // which a multiply by a rounded 1/range would not guarantee, and one
    // code either way of a floor() is visible banding.
    float sample_value = texelFetch(samples, ivec2(gl_FragCoord.xy), 0).r;
    float code = floor((sample_value - ubuf.levels.x) * 255.0 / ubuf.levels.y);
    code = clamp(code, 0.0, 255.0);

    float grey = code / 255.0;
    fragColor = vec4(grey, grey, grey, 1.0);
}
