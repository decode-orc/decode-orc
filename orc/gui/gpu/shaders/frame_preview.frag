// File:        frame_preview.frag
// Module:      orc-gui
// Purpose:     Textured-quad fragment stage for the frame preview surface
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// The frame texture is opaque RGBA8 whose alpha channel is padding from the
// RGB888 expansion, so the alpha written here is a constant rather than the
// sampled one.

#version 440

layout(location = 0) in vec2 v_texcoord;

layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D frame_texture;

void main() {
    fragColor = vec4(texture(frame_texture, v_texcoord).rgb, 1.0);
}
