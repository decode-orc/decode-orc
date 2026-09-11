/*
 * File:        gpu_surface_policy.cpp
 * Module:      orc-gui
 * Purpose:     Decides whether render surfaces draw through the GPU
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "gpu_surface_policy.h"

#include <QSettings>

#include "../logging.h"

namespace orc::gui::gpu {

namespace {

constexpr auto kEnvironmentVariable = "ORC_GUI_GPU_RENDER";
constexpr auto kPreferenceKey = "rendering/gpu_acceleration_enabled";

}  // namespace

std::optional<bool> parseGpuRenderOverride(const QString& value) {
  const QString normalised = value.trimmed().toLower();
  if (normalised == QLatin1String("0") || normalised == QLatin1String("off") ||
      normalised == QLatin1String("false") ||
      normalised == QLatin1String("no")) {
    return false;
  }
  if (normalised == QLatin1String("1") || normalised == QLatin1String("on") ||
      normalised == QLatin1String("true") ||
      normalised == QLatin1String("yes")) {
    return true;
  }
  return std::nullopt;
}

SurfaceDecision decideSurface(const SurfaceInputs& inputs) {
  if (!inputs.built_with_gpu_render) {
    return {SurfaceKind::kRaster, SurfaceReason::kNotBuilt};
  }

  const std::optional<bool> override_value =
      inputs.environment_override.has_value()
          ? parseGpuRenderOverride(*inputs.environment_override)
          : std::nullopt;

  if (override_value.has_value() && !*override_value) {
    return {SurfaceKind::kRaster, SurfaceReason::kDisabledByEnvironment};
  }

  // Checked after the environment's "off" but before its "on": by the time a
  // surface has reported a failure the GPU path has been tried on this
  // machine and did not work, so forcing it back on would only fail again.
  if (inputs.runtime_failed) {
    return {SurfaceKind::kRaster, SurfaceReason::kRuntimeFailure};
  }

  if (override_value.has_value() && *override_value) {
    return {SurfaceKind::kGpu, SurfaceReason::kGpuAvailable};
  }

  if (!inputs.user_preference_enabled) {
    return {SurfaceKind::kRaster, SurfaceReason::kDisabledBySetting};
  }

  return {SurfaceKind::kGpu, SurfaceReason::kGpuAvailable};
}

QString describeDecision(const SurfaceDecision& decision,
                         const QString& backend_name) {
  switch (decision.reason) {
    case SurfaceReason::kGpuAvailable:
      return backend_name.isEmpty()
                 ? QStringLiteral("GPU (Qt RHI)")
                 : QStringLiteral("GPU (Qt RHI, %1)").arg(backend_name);
    case SurfaceReason::kNotBuilt:
      return QStringLiteral("CPU (built without GPU render support)");
    case SurfaceReason::kDisabledByEnvironment:
      return QStringLiteral("CPU (disabled by ORC_GUI_GPU_RENDER)");
    case SurfaceReason::kDisabledBySetting:
      return QStringLiteral("CPU (disabled in settings)");
    case SurfaceReason::kRuntimeFailure:
      return QStringLiteral("CPU (GPU rendering failed, fell back)");
  }
  return QStringLiteral("CPU");
}

GpuSurfacePolicy& GpuSurfacePolicy::instance() {
  static GpuSurfacePolicy policy;
  return policy;
}

bool GpuSurfacePolicy::builtWithGpuRender() {
#ifdef ORC_GUI_GPU_RENDER
  return true;
#else
  return false;
#endif
}

GpuSurfacePolicy::GpuSurfacePolicy() {
  if (qEnvironmentVariableIsSet(kEnvironmentVariable)) {
    environment_override_ = qEnvironmentVariable(kEnvironmentVariable);
  }

  QSettings settings;
  user_preference_enabled_.store(settings.value(kPreferenceKey, true).toBool(),
                                 std::memory_order_relaxed);
}

bool GpuSurfacePolicy::userPreferenceEnabled() const {
  return user_preference_enabled_.load(std::memory_order_relaxed);
}

void GpuSurfacePolicy::setUserPreferenceEnabled(bool enabled) {
  user_preference_enabled_.store(enabled, std::memory_order_relaxed);
  QSettings settings;
  settings.setValue(kPreferenceKey, enabled);
}

void GpuSurfacePolicy::noteRenderFailure(const QString& context) {
  runtime_failed_.store(true, std::memory_order_relaxed);
  if (!failure_logged_.exchange(true, std::memory_order_relaxed)) {
    ORC_LOG_WARN(
        "GPU rendering failed ({}); this session falls back to CPU drawing",
        context.toStdString());
  }
}

bool GpuSurfacePolicy::renderFailed() const {
  return runtime_failed_.load(std::memory_order_relaxed);
}

void GpuSurfacePolicy::notePlaneConversionUnavailable(const QString& context) {
  plane_conversion_unavailable_.store(true, std::memory_order_relaxed);
  if (!plane_failure_logged_.exchange(true, std::memory_order_relaxed)) {
    ORC_LOG_WARN(
        "GPU plane conversion unavailable ({}); frames will be converted on "
        "the render worker for the rest of this session",
        context.toStdString());
  }
}

bool GpuSurfacePolicy::planeConversionAvailable() const {
  return !plane_conversion_unavailable_.load(std::memory_order_relaxed);
}

QString GpuSurfacePolicy::backendName() const {
  const std::lock_guard<std::mutex> lock(backend_name_mutex_);
  return backend_name_;
}

void GpuSurfacePolicy::setBackendName(const QString& name) {
  const std::lock_guard<std::mutex> lock(backend_name_mutex_);
  backend_name_ = name;
}

SurfaceDecision GpuSurfacePolicy::decision() const {
  SurfaceInputs inputs;
  inputs.built_with_gpu_render = builtWithGpuRender();
  inputs.environment_override = environment_override_;
  inputs.user_preference_enabled = userPreferenceEnabled();
  inputs.runtime_failed = renderFailed();
  return decideSurface(inputs);
}

bool GpuSurfacePolicy::useGpuSurface() const {
  return decision().kind == SurfaceKind::kGpu;
}

QString GpuSurfacePolicy::aboutText() const {
  return describeDecision(decision(), backendName());
}

void GpuSurfacePolicy::resetForTesting() {
  runtime_failed_.store(false, std::memory_order_relaxed);
  failure_logged_.store(false, std::memory_order_relaxed);
  plane_conversion_unavailable_.store(false, std::memory_order_relaxed);
  plane_failure_logged_.store(false, std::memory_order_relaxed);
  setBackendName(QString());
}

}  // namespace orc::gui::gpu
