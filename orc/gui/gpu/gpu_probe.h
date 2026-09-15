/*
 * File:        gpu_probe.h
 * Module:      orc-gui
 * Purpose:     Measures whether this machine's GPU stack can start, safely
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_GPU_PROBE_H
#define ORC_GUI_GPU_PROBE_H

#include <QString>
#include <optional>

namespace orc::gui::gpu {

/**
 * @brief What asking the graphics stack to start told us.
 */
enum class ProbeOutcome {
  kUsable,    ///< A context came up and, where built, so did a QRhi
  kUnusable,  ///< It refused, died, or hung
  kUnknown,   ///< The question could not be put; nothing was learned
};

/// Exit statuses the probe process reports with. Values are a contract between
/// the two halves of this file and nothing else.
enum ProbeExitCode {
  kProbeExitUsable = 0,
  kProbeExitUnusable = 1,
};

/// The hidden argument that turns a run of this executable into the probe.
const char* gpuProbeFlag();

/// Environment variable forcing the probe on or off, read like
/// ORC_GUI_GPU_RENDER.
const char* gpuProbeEnvironmentVariable();

/**
 * @brief Whether this platform's graphics failures are worth a probe.
 *
 * Only where they are unsurvivable. Qt's RHI backends and the other platform
 * integrations report a graphics stack that will not start and let the caller
 * fall back — which the render surfaces already do. The X11 GLX integration
 * is the exception: it calls qFatal, so there the answer has to be collected
 * somewhere the answer can be fatal, and that costs a process.
 *
 * Spending that process everywhere would be paying for a fault the other
 * platforms do not have — and on macOS a second UI process, however brief,
 * is visible to the user.
 *
 * @param platform_name QGuiApplication::platformName()
 * @param environment_override Raw ORC_GUI_GPU_PROBE value, when set: "off"
 *        skips the probe on any platform, "on" demands it on any platform.
 */
bool platformNeedsGpuProbe(const QString& platform_name,
                           const std::optional<QString>& environment_override);

/**
 * @brief Whether this process was started to be the probe.
 *
 * Checked from the first line of main(), before anything is constructed: the
 * probe must not initialise logging (it would truncate the run's log file),
 * install the crash handler (its abort is an answer, not a crash to report)
 * or, above all, probe again.
 */
bool isGpuProbeInvocation(int argc, char** argv);

/**
 * @brief Be the probe: bring the graphics stack up and report by exit status.
 *
 * Runs in a process of its own, so it is free to do the one thing the
 * application cannot risk doing — ask Qt for an OpenGL context. On X11 that
 * call is not allowed to fail: when GLX can supply no visual, Qt's own
 * integration calls qFatal("Could not initialize GLX") and the process is
 * gone. Here that is simply the answer, and the exit status carries it back.
 *
 * @return A ProbeExitCode, to be returned straight out of main().
 */
int runGpuProbe(int argc, char** argv);

/**
 * @brief Read a finished probe process's result.
 *
 * Separated from running it so the mapping can be tested: a probe that was
 * killed by a signal (its qFatal, or a driver falling over) is as clear an
 * answer as one that exited saying so.
 *
 * @param crashed True when the process died on a signal rather than returning
 * @param exit_code Its exit status, meaningful only when it did return
 */
ProbeOutcome interpretProbeExit(bool crashed, int exit_code);

/**
 * @brief Ask a child process whether the GPU path can start here.
 *
 * Costs one short process. Run only when the policy would otherwise use the
 * GPU, and before the first render surface is built — after that the question
 * has already been answered the hard way.
 *
 * A probe that cannot be started at all reports kUnknown: not being able to
 * ask says nothing about the machine, and refusing the GPU on those grounds
 * would take it away from everyone whose environment merely makes spawning
 * awkward.
 *
 * @param timeout_ms How long to wait before treating the probe as hung
 * @param diagnostics When given, receives whatever the probe printed
 */
ProbeOutcome probeGpuInSeparateProcess(int timeout_ms = 15000,
                                       QString* diagnostics = nullptr);

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_GPU_PROBE_H
