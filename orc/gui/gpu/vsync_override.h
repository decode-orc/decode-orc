/*
 * File:        vsync_override.h
 * Module:      orc-gui
 * Purpose:     Opt-in --no-vsync: present GPU frames without waiting for vsync
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_GPU_VSYNC_OVERRIDE_H
#define ORC_GUI_GPU_VSYNC_OVERRIDE_H

namespace orc::gui::gpu {

/// The command-line option that asks for presentation without vsync.
const char* noVsyncFlag();

/**
 * @brief Whether this run was started with --no-vsync.
 *
 * Read straight from argv because the decision has to be made before
 * QApplication exists: the drivers read their sync settings when they
 * initialise, and Qt takes the default surface format when it creates the
 * first surface. The option is also registered with the command-line parser,
 * so --help lists it and the parser accepts it.
 */
bool isNoVsyncRequested(int argc, char** argv);

/**
 * @brief Stop the GPU path waiting for the display's vertical sync this run.
 *
 * A widget window that holds a render surface is presented through an OpenGL
 * swapchain, and on some driver/compositor combinations a swap that waits for
 * vsync can block the GUI thread for as long as the compositor withholds
 * frames from the window. This is the escape hatch for such a machine, not
 * the default: without vsync, playback in the preview tears.
 *
 * Three things have a say and all three are told: Qt's swap interval on every
 * surface it creates, Mesa's vblank_mode and the NVIDIA proprietary driver's
 * __GL_SYNC_TO_VBLANK (on Linux, each only when the user has not already set
 * it). Must be called before QApplication is constructed. Harmless when the
 * run ends up on the CPU: no GL context is ever created.
 *
 * Not thread-safe; call from main() before any other thread exists.
 */
void disableVsyncForRun();

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_GPU_VSYNC_OVERRIDE_H
