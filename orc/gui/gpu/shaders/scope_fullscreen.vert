// File:        scope_fullscreen.vert
// Module:      orc-gui
// Purpose:     Vertex stage for the scope canvas's full-target passes
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Simon Inns
//
// One quad covering the whole render target, generated from the vertex index
// so the passes that use it need no vertex buffer at all. They address their
// source by integer texel through gl_FragCoord rather than by interpolated
// texture coordinate, which is why nothing is passed on: a fragment reads the
// texel it is about to write, so the mapping holds whichever way up the
// backend's framebuffer is.

#version 440

void main() {
    vec2 corner = vec2(((gl_VertexIndex & 1) == 0) ? -1.0 : 1.0,
                       ((gl_VertexIndex & 2) == 0) ? -1.0 : 1.0);
    gl_Position = vec4(corner, 0.0, 1.0);
}
