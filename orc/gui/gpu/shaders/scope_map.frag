// File:        scope_map.frag
// Module:      orc-gui
// Purpose:     Fragment stage for the scope map pass
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// Turns accumulated counts into the plot: brightness from the count, colour
// from one of the three mappings the two scopes between them use, composited
// between the graticule painted behind the trace and the one painted over it.
//
// scales  = (primary weight, secondary weight, brightness bias, colorize)
// params  = (ramp from background, canvas pixels per U/V unit, U/V full
//            scale, flip the accumulation sampling)
// centre  = (canvas position of U=V=0, canvas width, canvas height)
// dwell   = (dwell mode, gain, per-line anchor cap, transit weight)
// blend   = (add the trace to what is behind it, unused)

#version 440

layout(location = 0) in vec2 v_texcoord;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 trace_color;
    vec4 background_color;
    vec4 scales;
    vec4 params;
    vec4 centre;
    vec4 dwell;
    vec4 blend;
} ubuf;

layout(binding = 1) uniform sampler2D accumulation;
layout(binding = 2) uniform sampler2D underlay;
layout(binding = 3) uniform sampler2D overlay;
layout(binding = 4) uniform sampler2D anchor;

// The colour a signal at this chrominance would produce: the ITU-R BT.470-6
// §1.1.2 / EBU Tech. 3280-E §2.1 inverse at Y=0.5, normalised to its largest
// component so every direction is vivid and the origin comes out white.
vec3 chromaHue(vec2 canvas_position) {
    float u = (canvas_position.x - ubuf.centre.x) / ubuf.params.y;
    float v = -(canvas_position.y - ubuf.centre.y) / ubuf.params.y;
    float un = u / ubuf.params.z;
    float vn = v / ubuf.params.z;

    float red = 0.5 + vn / 0.877283;
    float blue = 0.5 + un / 0.492111;
    float green = (0.5 - 0.299 * red - 0.114 * blue) / 0.587;

    vec3 rgb = clamp(vec3(red, green, blue), 0.0, 1.0);
    float largest = max(max(rgb.r, rgb.g), rgb.b);
    return (largest > 0.001) ? (rgb / largest) : vec3(1.0);
}

void main() {
    vec2 accumulation_uv =
        vec2(v_texcoord.x, mix(v_texcoord.y, 1.0 - v_texcoord.y, ubuf.params.w));
    vec2 counts = texture(accumulation, accumulation_uv).rg;

    float raw;
    if (ubuf.dwell.x > 0.5) {
        // Intensity linear in dwell. Full brightness is given at whichever
        // anchor makes the trace brighter: the dwell a vector reaches landing
        // on the same pixel on every line, or the level above which the beam
        // spends half its resting time, which the canvas reduced out of the
        // plot. Taking the lower of the two also means the origin, where the
        // beam rests through blanking, cannot starve the rest of the plot.
        float reduced = texture(anchor, vec2(0.5)).r;
        float level = ubuf.dwell.z;
        if (reduced > 0.0) {
            level = min(level, reduced);
        }
        float dwell_scale = (level > 0.0) ? (ubuf.dwell.y / level) : 0.0;
        raw = (counts.r * dwell_scale) + (counts.g * ubuf.dwell.w);
    } else {
        raw = counts.r * ubuf.scales.x + counts.g * ubuf.scales.y;
    }
    // A pixel nothing reached keeps whatever is painted behind it; the bias
    // lifts the first count clear of the background, so it must not apply
    // where there is no count at all.
    float lit = step(1e-6, raw);
    float brightness = min(1.0, raw + ubuf.scales.z) * lit;

    vec3 trace;
    if (ubuf.scales.w > 0.5) {
        trace = chromaHue(v_texcoord * ubuf.centre.zw) * brightness;
    } else if (ubuf.params.x > 0.5) {
        trace = mix(ubuf.background_color.rgb, ubuf.trace_color.rgb, brightness);
    } else {
        trace = ubuf.trace_color.rgb * brightness;
    }

    vec4 behind = texture(underlay, v_texcoord);
    // A graticule painted under the trace has to be added to, not replaced:
    // overwriting punches a dark hole in it wherever the beam passed dimly,
    // which reads as the trace being darker than the graticule it crosses.
    vec3 composed = (ubuf.blend.x > 0.5) ? min(behind.rgb + trace, vec3(1.0))
                                         : mix(behind.rgb, trace, lit);

    vec4 front = texture(overlay, v_texcoord);
    fragColor = vec4(mix(composed, front.rgb, front.a), 1.0);
}
