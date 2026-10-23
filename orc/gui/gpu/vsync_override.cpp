/*
 * File:        vsync_override.cpp
 * Module:      orc-gui
 * Purpose:     Opt-in --no-vsync: present GPU frames without waiting for vsync
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vsync_override.h"

#include <QByteArray>
#include <QSurfaceFormat>
#include <QtGlobal>
#include <cstring>

namespace orc::gui::gpu {

namespace {
constexpr const char* kNoVsyncFlag = "--no-vsync";
}  // namespace

const char* noVsyncFlag() { return kNoVsyncFlag; }

bool isNoVsyncRequested(int argc, char** argv) {
  if (argv == nullptr) {
    return false;
  }
  for (int i = 1; i < argc; ++i) {
    if (argv[i] != nullptr && std::strcmp(argv[i], kNoVsyncFlag) == 0) {
      return true;
    }
  }
  return false;
}

void disableVsyncForRun() {
#ifdef Q_OS_LINUX
  // Read by the drivers when they initialise, so they must be in the
  // environment before the platform plugin brings them up. A value the user
  // has already chosen is left alone.
  if (!qEnvironmentVariableIsSet("vblank_mode")) {
    qputenv("vblank_mode", "0");
  }
  if (!qEnvironmentVariableIsSet("__GL_SYNC_TO_VBLANK")) {
    qputenv("__GL_SYNC_TO_VBLANK", "0");
  }
#endif
  QSurfaceFormat format = QSurfaceFormat::defaultFormat();
  format.setSwapInterval(0);
  QSurfaceFormat::setDefaultFormat(format);
}

}  // namespace orc::gui::gpu
