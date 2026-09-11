// File:        scope_histogram.frag
// Module:      orc-gui
// Purpose:     Deposits one pixel's charge in its histogram bucket
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns

#version 440

layout(location = 0) in float v_charge;

layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = vec4(v_charge, 0.0, 0.0, 0.0);
}
