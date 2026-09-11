// File:        scope_accumulate.frag
// Module:      orc-gui
// Purpose:     Fragment stage for the scope accumulation pass
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// Two independent trace channels in one floating-point target: the pipeline's
// blend combines what lands on the same pixel, either by summing it (one
// point per sample) or by keeping the largest (one point per counted cell).

#version 440

layout(location = 0) in vec2 v_weight;

layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = vec4(v_weight, 0.0, 0.0);
}
