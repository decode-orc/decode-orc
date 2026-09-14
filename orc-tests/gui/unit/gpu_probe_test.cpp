/*
 * File:        gpu_probe_test.cpp
 * Module:      orc-gui-tests
 * Purpose:     Unit tests for the out-of-process graphics-stack probe
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "gpu/gpu_probe.h"

#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

using orc::gui::gpu::gpuProbeFlag;
using orc::gui::gpu::interpretProbeExit;
using orc::gui::gpu::isGpuProbeInvocation;
using orc::gui::gpu::kProbeExitUnusable;
using orc::gui::gpu::kProbeExitUsable;
using orc::gui::gpu::platformNeedsGpuProbe;
using orc::gui::gpu::ProbeOutcome;

namespace {

// gtest cannot hand a char** straight from string literals, and the probe is
// read from argv before anything Qt exists to parse it.
class Argv {
 public:
  explicit Argv(std::initializer_list<const char*> args) {
    for (const char* arg : args) {
      storage_.emplace_back(arg);
    }
    for (std::string& arg : storage_) {
      pointers_.push_back(arg.data());
    }
    pointers_.push_back(nullptr);
  }

  int argc() const { return static_cast<int>(storage_.size()); }
  char** argv() { return pointers_.data(); }

 private:
  std::vector<std::string> storage_;
  std::vector<char*> pointers_;
};

TEST(GpuProbe, RecognisesItsOwnInvocation) {
  Argv args({"orc-gui", gpuProbeFlag()});
  EXPECT_TRUE(isGpuProbeInvocation(args.argc(), args.argv()));
}

// The flag has to be found wherever it sits: the parent puts it first, but a
// user reproducing a report by hand will not.
TEST(GpuProbe, FindsTheFlagAmongOtherArguments) {
  Argv args({"orc-gui", "--log-level", "debug", gpuProbeFlag(), "file.orcprj"});
  EXPECT_TRUE(isGpuProbeInvocation(args.argc(), args.argv()));
}

// An ordinary run must not be diverted into the probe, or it would exit
// instead of starting.
TEST(GpuProbe, AnOrdinaryRunIsNotAProbe) {
  Argv args({"orc-gui", "--log-level", "debug", "recording.orcprj"});
  EXPECT_FALSE(isGpuProbeInvocation(args.argc(), args.argv()));

  Argv none({"orc-gui"});
  EXPECT_FALSE(isGpuProbeInvocation(none.argc(), none.argv()));
  EXPECT_FALSE(isGpuProbeInvocation(0, nullptr));
}

// A flag that merely contains the probe's name is a different flag.
TEST(GpuProbe, DoesNotMatchOnAPrefix) {
  Argv args({"orc-gui", "--gpu-probe-timeout", "--gpu-probes"});
  EXPECT_FALSE(isGpuProbeInvocation(args.argc(), args.argv()));
}

// The probe is bought with a process, and it is only worth that where a
// graphics stack that will not start takes the application down with it.
// That is Qt's X11 GLX integration and nothing else: everywhere else Qt
// reports the failure and the surfaces fall back on their own.
TEST(GpuProbe, OnlyX11NeedsTheProbeByDefault) {
  EXPECT_TRUE(platformNeedsGpuProbe(QStringLiteral("xcb"), std::nullopt));
  EXPECT_FALSE(platformNeedsGpuProbe(QStringLiteral("wayland"), std::nullopt));
  EXPECT_FALSE(platformNeedsGpuProbe(QStringLiteral("cocoa"), std::nullopt));
  EXPECT_FALSE(platformNeedsGpuProbe(QStringLiteral("windows"), std::nullopt));
  EXPECT_FALSE(
      platformNeedsGpuProbe(QStringLiteral("offscreen"), std::nullopt));
}

TEST(GpuProbe, TheEnvironmentCanDemandOrSkipTheProbeAnywhere) {
  EXPECT_TRUE(
      platformNeedsGpuProbe(QStringLiteral("cocoa"), QStringLiteral("1")));
  EXPECT_FALSE(
      platformNeedsGpuProbe(QStringLiteral("xcb"), QStringLiteral("off")));
  // Anything unreadable leaves the platform to decide, as elsewhere.
  EXPECT_TRUE(
      platformNeedsGpuProbe(QStringLiteral("xcb"), QStringLiteral("perhaps")));
}

TEST(GpuProbe, AProbeThatExitsCleanlySaysTheGpuWorks) {
  EXPECT_EQ(interpretProbeExit(false, kProbeExitUsable), ProbeOutcome::kUsable);
}

TEST(GpuProbe, AProbeThatReportsFailureIsBelieved) {
  EXPECT_EQ(interpretProbeExit(false, kProbeExitUnusable),
            ProbeOutcome::kUnusable);
}

// The whole point of running it elsewhere: on X11 a GLX that cannot supply a
// visual makes Qt call qFatal, so the probe dying IS the answer, and it must
// not be mistaken for the probe itself being broken.
TEST(GpuProbe, AProbeThatDiesIsAnAnswerAndNotAnError) {
  EXPECT_EQ(interpretProbeExit(true, 0), ProbeOutcome::kUnusable);
  EXPECT_EQ(interpretProbeExit(true, 134), ProbeOutcome::kUnusable);
}

// Any exit status that is not the agreed "it worked" means it did not.
TEST(GpuProbe, AnUnrecognisedExitStatusIsNotTakenAsSuccess) {
  EXPECT_EQ(interpretProbeExit(false, 42), ProbeOutcome::kUnusable);
}

}  // namespace
