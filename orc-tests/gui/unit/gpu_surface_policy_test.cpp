/*
 * File:        gpu_surface_policy_test.cpp
 * Module:      orc-gui-tests
 * Purpose:     Unit tests for the GPU render surface availability policy
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "gpu/gpu_surface_policy.h"

#include <gtest/gtest.h>

using orc::gui::gpu::decideSurface;
using orc::gui::gpu::describeDecision;
using orc::gui::gpu::parseGpuRenderOverride;
using orc::gui::gpu::SurfaceInputs;
using orc::gui::gpu::SurfaceKind;
using orc::gui::gpu::SurfaceReason;

namespace {

/// Everything permitting the GPU path, for tests that then withdraw one thing.
SurfaceInputs everythingAvailable() {
  SurfaceInputs inputs;
  inputs.built_with_gpu_render = true;
  inputs.user_preference_enabled = true;
  inputs.runtime_failed = false;
  return inputs;
}

TEST(GpuSurfacePolicy, UsesTheGpuWhenNothingObjects) {
  const auto decision = decideSurface(everythingAvailable());
  EXPECT_EQ(decision.kind, SurfaceKind::kGpu);
  EXPECT_EQ(decision.reason, SurfaceReason::kGpuAvailable);
}

TEST(GpuSurfacePolicy, ABuildWithoutTheCodeCannotUseIt) {
  SurfaceInputs inputs = everythingAvailable();
  inputs.built_with_gpu_render = false;
  // Even asked for explicitly: there is nothing compiled in to ask for.
  inputs.environment_override = QStringLiteral("1");

  const auto decision = decideSurface(inputs);
  EXPECT_EQ(decision.kind, SurfaceKind::kRaster);
  EXPECT_EQ(decision.reason, SurfaceReason::kNotBuilt);
}

TEST(GpuSurfacePolicy, TheEnvironmentsOffBeatsTheSavedPreference) {
  SurfaceInputs inputs = everythingAvailable();
  inputs.environment_override = QStringLiteral("0");

  const auto decision = decideSurface(inputs);
  EXPECT_EQ(decision.kind, SurfaceKind::kRaster);
  EXPECT_EQ(decision.reason, SurfaceReason::kDisabledByEnvironment);
}

TEST(GpuSurfacePolicy, TheEnvironmentsOnBeatsTheSavedPreference) {
  SurfaceInputs inputs = everythingAvailable();
  inputs.user_preference_enabled = false;
  inputs.environment_override = QStringLiteral("on");

  EXPECT_EQ(decideSurface(inputs).kind, SurfaceKind::kGpu);
}

TEST(GpuSurfacePolicy, AFailureThisSessionBeatsTheEnvironmentsOn) {
  // Forcing it back on cannot help: the GPU path has already been tried on
  // this machine and did not work.
  SurfaceInputs inputs = everythingAvailable();
  inputs.environment_override = QStringLiteral("1");
  inputs.runtime_failed = true;

  const auto decision = decideSurface(inputs);
  EXPECT_EQ(decision.kind, SurfaceKind::kRaster);
  EXPECT_EQ(decision.reason, SurfaceReason::kRuntimeFailure);
}

TEST(GpuSurfacePolicy, TurningItOffInSettingsIsRespected) {
  SurfaceInputs inputs = everythingAvailable();
  inputs.user_preference_enabled = false;

  const auto decision = decideSurface(inputs);
  EXPECT_EQ(decision.kind, SurfaceKind::kRaster);
  EXPECT_EQ(decision.reason, SurfaceReason::kDisabledBySetting);
}

TEST(GpuSurfacePolicy, AnUnreadableOverrideLeavesTheDecisionToTheRest) {
  SurfaceInputs inputs = everythingAvailable();
  inputs.environment_override = QStringLiteral("maybe");
  EXPECT_EQ(decideSurface(inputs).kind, SurfaceKind::kGpu);

  inputs.user_preference_enabled = false;
  EXPECT_EQ(decideSurface(inputs).reason, SurfaceReason::kDisabledBySetting);
}

TEST(GpuSurfacePolicy, ReadsTheUsualSpellingsOfOnAndOff) {
  for (const QString& off : {QStringLiteral("0"), QStringLiteral("off"),
                             QStringLiteral("FALSE"), QStringLiteral(" no ")}) {
    EXPECT_EQ(parseGpuRenderOverride(off), std::optional<bool>(false))
        << off.toStdString();
  }
  for (const QString& on : {QStringLiteral("1"), QStringLiteral("ON"),
                            QStringLiteral("true"), QStringLiteral("yes")}) {
    EXPECT_EQ(parseGpuRenderOverride(on), std::optional<bool>(true))
        << on.toStdString();
  }
  EXPECT_FALSE(parseGpuRenderOverride(QString()).has_value());
  EXPECT_FALSE(parseGpuRenderOverride(QStringLiteral("2")).has_value());
}

TEST(GpuSurfacePolicy, SaysWhyItChoseTheCpu) {
  // The About line and the log record have to distinguish a build without the
  // code from a GPU that failed; a bug report is read from these.
  EXPECT_NE(
      describeDecision({SurfaceKind::kRaster, SurfaceReason::kNotBuilt}),
      describeDecision({SurfaceKind::kRaster, SurfaceReason::kRuntimeFailure}));
  EXPECT_NE(describeDecision(
                {SurfaceKind::kRaster, SurfaceReason::kDisabledByEnvironment}),
            describeDecision(
                {SurfaceKind::kRaster, SurfaceReason::kDisabledBySetting}));
}

TEST(GpuSurfacePolicy, NamesTheLiveBackendWhenThereIsOne) {
  const orc::gui::gpu::SurfaceDecision decision{SurfaceKind::kGpu,
                                                SurfaceReason::kGpuAvailable};
  EXPECT_TRUE(describeDecision(decision, QStringLiteral("Vulkan"))
                  .contains(QStringLiteral("Vulkan")));
  // Before a surface has come up there is no backend to name, and the line
  // must still read sensibly.
  EXPECT_FALSE(describeDecision(decision).isEmpty());
}

}  // namespace
