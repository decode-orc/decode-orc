/*
 * File:        gpu_probe.cpp
 * Module:      orc-gui
 * Purpose:     Measures whether this machine's GPU stack can start, safely
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "gpu_probe.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSurfaceFormat>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "gpu_surface_policy.h"

#ifdef ORC_GUI_GPU_RENDER
#include <rhi/qrhi.h>
#endif

#ifndef Q_OS_WIN
#include <unistd.h>

#include <csignal>
#endif

namespace orc::gui::gpu {

namespace {

constexpr auto kProbeFlag = "--gpu-probe";
constexpr auto kProbeEnvironmentVariable = "ORC_GUI_GPU_PROBE";

// The one platform integration that ends the process instead of reporting a
// graphics stack that will not start.
constexpr auto kPlatformWithFatalGl = "xcb";

#ifndef Q_OS_WIN
// The probe is expected to die. Turning the death into an ordinary exit keeps
// it from reaching the things that watch for a process falling over: no core
// file, no operating-system crash reporter telling the user the application
// crashed when it did no such thing, and no bundle from our own handler (the
// probe never installs it). _exit(), not exit(): nothing that ran before the
// signal is in a fit state to be unwound.
extern "C" void probeSignalExit(int /*signal_number*/) {
  _exit(kProbeExitUnusable);
}

void reportDeathAsAnAnswer() {
  for (const int signal_number : {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE}) {
    std::signal(signal_number, probeSignalExit);
  }
}
#else
void reportDeathAsAnAnswer() {}
#endif

}  // namespace

const char* gpuProbeFlag() { return kProbeFlag; }

const char* gpuProbeEnvironmentVariable() { return kProbeEnvironmentVariable; }

bool platformNeedsGpuProbe(const QString& platform_name,
                           const std::optional<QString>& environment_override) {
  const std::optional<bool> forced =
      environment_override.has_value()
          ? parseGpuRenderOverride(*environment_override)
          : std::nullopt;
  if (forced.has_value()) {
    return *forced;
  }
  return platform_name.compare(QLatin1String(kPlatformWithFatalGl),
                               Qt::CaseInsensitive) == 0;
}

bool isGpuProbeInvocation(int argc, char** argv) {
  if (argv == nullptr) {
    return false;
  }
  for (int i = 1; i < argc; ++i) {
    if (argv[i] != nullptr && std::strcmp(argv[i], kProbeFlag) == 0) {
      return true;
    }
  }
  return false;
}

int runGpuProbe(int argc, char** argv) {
  reportDeathAsAnAnswer();

  // QGuiApplication, not QApplication: the probe needs a platform and a
  // display connection, and nothing the widget layer adds.
  QGuiApplication app(argc, argv);

  // Everything below is what the widget backingstore does when a window
  // holding a render surface is created, in the same order, so that what is
  // measured is what will be run.
  QOffscreenSurface surface;
  surface.setFormat(QSurfaceFormat::defaultFormat());
  surface.create();
  if (!surface.isValid()) {
    qWarning("gpu probe: no offscreen surface");
    return kProbeExitUnusable;
  }

  QOpenGLContext context;
  context.setFormat(surface.format());
  if (!context.create()) {
    qWarning("gpu probe: no OpenGL context");
    return kProbeExitUnusable;
  }
  if (!context.makeCurrent(&surface)) {
    qWarning("gpu probe: the context would not become current");
    return kProbeExitUnusable;
  }
  context.doneCurrent();

#ifdef ORC_GUI_GPU_RENDER
  // A context is the part that can take the process down; the QRhi on top of
  // it fails politely. It is still worth building, because a machine where it
  // fails should be on the CPU path from the start rather than after a
  // surface has been built and thrown away.
  QRhiGles2InitParams params;
  params.fallbackSurface = &surface;
  params.format = surface.format();
  const std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::OpenGLES2, &params));
  if (!rhi) {
    qWarning("gpu probe: no QRhi on the OpenGL backend");
    return kProbeExitUnusable;
  }
#endif

  return kProbeExitUsable;
}

ProbeOutcome interpretProbeExit(bool crashed, int exit_code) {
  // A probe that died is the answer this whole mechanism exists to collect,
  // so it is read as one rather than as a fault.
  if (crashed) {
    return ProbeOutcome::kUnusable;
  }
  return exit_code == kProbeExitUsable ? ProbeOutcome::kUsable
                                       : ProbeOutcome::kUnusable;
}

ProbeOutcome probeGpuInSeparateProcess(int timeout_ms, QString* diagnostics) {
  const QString program = QCoreApplication::applicationFilePath();
  if (program.isEmpty()) {
    return ProbeOutcome::kUnknown;
  }

  QProcess probe;
  probe.setProgram(program);
  probe.setArguments({QString::fromLatin1(kProbeFlag)});
  // Qt's own warnings on the way down are the most useful thing a report can
  // carry about a machine like this, and they go to stderr.
  probe.setProcessChannelMode(QProcess::MergedChannels);
  // ...but only while they are still going there. A Qt built against journald
  // - which the Linux distribution builds are - routes its default handler to
  // the journal whenever stderr is not a console, and the child then says
  // nothing at all: a bare exit code with no reason attached to it. The
  // probe's entire output is a few lines read by one parent, so putting it
  // back on stderr costs nothing, and it is the difference between a report
  // that can be diagnosed and one that cannot.
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"),
                     QStringLiteral("1"));
  probe.setProcessEnvironment(environment);
  probe.start(QIODevice::ReadOnly);

  if (!probe.waitForStarted(timeout_ms)) {
    if (diagnostics != nullptr) {
      *diagnostics = probe.errorString();
    }
    return ProbeOutcome::kUnknown;
  }

  if (!probe.waitForFinished(timeout_ms)) {
    probe.kill();
    probe.waitForFinished(1000);
    if (diagnostics != nullptr) {
      *diagnostics = QStringLiteral("the probe did not finish within %1 ms")
                         .arg(timeout_ms);
    }
    // A driver that hangs bringing a context up would hang the application in
    // exactly the same place, which is no better than dying in it.
    return ProbeOutcome::kUnusable;
  }

  if (diagnostics != nullptr) {
    *diagnostics = QString::fromLocal8Bit(probe.readAll()).trimmed();
  }
  return interpretProbeExit(probe.exitStatus() != QProcess::NormalExit,
                            probe.exitCode());
}

}  // namespace orc::gui::gpu
