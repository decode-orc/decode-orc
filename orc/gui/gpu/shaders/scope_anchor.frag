// File:        scope_anchor.frag
// Module:      orc-gui
// Purpose:     Reads the brightness anchor off the charge histogram
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// Full brightness is given at the dwell above which the beam spends half its
// resting time. On a real capture noise spreads each vector over a disc,
// dividing its per-pixel dwell by the area of that disc, which would leave
// the trace dim at any gain; this anchor follows the spreading instead of
// fighting it.
//
// Walking the buckets down from the top and stopping at half the charge is
// the same answer a sorted copy of the plot would give, to within a bucket's
// width, for one pass over a thousand texels instead of a million.

#version 440

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 config;  // bucket count, half-charge fraction, unused, unused
} ubuf;

layout(binding = 1) uniform sampler2D histogram;
layout(binding = 2) uniform sampler2D reduction;  // 1x1: peak, total

void main() {
    float peak = texelFetch(reduction, ivec2(0), 0).r;
    float total = texelFetch(reduction, ivec2(0), 0).g;
    int buckets = int(ubuf.config.x);

    if (peak <= 0.0 || total <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }

    float wanted = ubuf.config.y * total;
    float accumulated = 0.0;
    float anchor = 0.0;
    for (int bucket = buckets - 1; bucket >= 0; --bucket) {
        accumulated += texelFetch(histogram, ivec2(bucket, 0), 0).r;
        if (accumulated >= wanted) {
            // The bucket's lower edge, so rounding can only leave the trace
            // brighter than the exact half-charge point rather than dimmer.
            anchor = (float(bucket) * peak) / float(buckets);
            break;
        }
    }
    fragColor = vec4(anchor, 0.0, 0.0, 0.0);
}
