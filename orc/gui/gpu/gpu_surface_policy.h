/*
 * File:        gpu_surface_policy.h
 * Module:      orc-gui
 * Purpose:     Decides whether render surfaces draw through the GPU
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_GPU_SURFACE_POLICY_H
#define ORC_GUI_GPU_SURFACE_POLICY_H

#include <QString>
#include <atomic>
#include <mutex>
#include <optional>

namespace orc::gui::gpu {

/// Which of the two drawing paths a surface uses.
enum class SurfaceKind {
  kRaster,  ///< QPainter, always available
  kGpu,     ///< Qt RHI through QRhiWidget
};

/**
 * @brief Why the policy settled on the path it did.
 *
 * Recorded so the About dialogue and the one-off log record can say what
 * happened rather than only what is in use: "the GPU was not compiled in" and
 * "the GPU failed and was dropped" are very different reports to receive with
 * a bug.
 */
enum class SurfaceReason {
  kGpuAvailable,           ///< Nothing objected
  kNotBuilt,               ///< Built with ORC_GUI_GPU_RENDER=OFF
  kDisabledByEnvironment,  ///< ORC_GUI_GPU_RENDER=0 in the environment
  kDisabledBySetting,      ///< Turned off in the settings dialogue
  kRuntimeFailure,         ///< QRhiWidget::renderFailed fired this session
};

/// Everything the decision depends on, so the decision itself is pure.
struct SurfaceInputs {
  /// True when the GPU surfaces were compiled in.
  bool built_with_gpu_render = false;
  /// Raw value of the ORC_GUI_GPU_RENDER environment variable, when set.
  std::optional<QString> environment_override;
  /// The persisted user preference; true unless the user turned it off.
  bool user_preference_enabled = true;
  /// True once a surface has reported a run-time render failure.
  bool runtime_failed = false;
};

struct SurfaceDecision {
  SurfaceKind kind = SurfaceKind::kRaster;
  SurfaceReason reason = SurfaceReason::kNotBuilt;

  bool operator==(const SurfaceDecision& other) const {
    return kind == other.kind && reason == other.reason;
  }
  bool operator!=(const SurfaceDecision& other) const {
    return !(*this == other);
  }
};

/**
 * @brief Read an ORC_GUI_GPU_RENDER value as a tri-state.
 *
 * `0`, `off`, `false` and `no` mean off; `1`, `on`, `true` and `yes` mean on;
 * anything else - including an empty string - means the variable has nothing
 * to say and the rest of the policy decides. Case-insensitive, surrounding
 * whitespace ignored.
 */
std::optional<bool> parseGpuRenderOverride(const QString& value);

/**
 * @brief The decision table.
 *
 * The order matters and is the point of the function: a build without the
 * code cannot use it; an explicit "off" in the environment beats everything
 * that follows, which is what makes it usable to get a broken machine
 * running; a run-time failure beats an explicit "on", because by then the
 * GPU path has already been tried and did not work; and the persisted
 * setting is the last word before the default.
 */
SurfaceDecision decideSurface(const SurfaceInputs& inputs);

/// One-line description of a decision, naming @p backend_name when the GPU
/// path is live and the backend has identified itself.
QString describeDecision(const SurfaceDecision& decision,
                         const QString& backend_name = QString());

/**
 * @brief Session-wide state behind the decision.
 *
 * Holds the impure inputs - the environment, the persisted preference and
 * whether a surface has failed - so that widgets can ask one question and get
 * a consistent answer. The run-time failure latch is deliberately
 * session-long and process-wide: a machine whose RHI backend fails once will
 * fail again, and switching back and forth mid-session would mean rebuilding
 * surfaces repeatedly while the user watches.
 *
 * Thread safety: readable from any thread (the render worker asks whether to
 * expand frames to RGBA); the mutators are GUI thread only.
 */
class GpuSurfacePolicy {
 public:
  static GpuSurfacePolicy& instance();

  /// True when the GPU surfaces were compiled into this build.
  static bool builtWithGpuRender();

  /// The persisted user preference, as edited in the settings dialogue.
  bool userPreferenceEnabled() const;
  void setUserPreferenceEnabled(bool enabled);

  /// Latch a run-time render failure for the rest of the session. Logs once.
  void noteRenderFailure(const QString& context);
  bool renderFailed() const;

  /**
   * @brief Latch that no surface can convert component planes. Logs once.
   *
   * A narrower failure than noteRenderFailure(): the GPU surfaces still draw,
   * they just cannot finish the colour conversion themselves, so the render
   * worker goes back to producing display RGB for them. Separate because a
   * device that lacks floating-point sampled textures still benefits from
   * every other thing a surface does.
   */
  void notePlaneConversionUnavailable(const QString& context);
  bool planeConversionAvailable() const;

  /// Backend the live RHI reported (`QRhi::backendName()`), when one is up.
  QString backendName() const;
  void setBackendName(const QString& name);

  SurfaceDecision decision() const;

  /// Convenience: true when new surfaces should be built on the GPU path.
  bool useGpuSurface() const;

  /// Line for the About dialogue.
  QString aboutText() const;

  /// Drop the session state, for tests that need a clean policy.
  void resetForTesting();

 private:
  GpuSurfacePolicy();

  std::atomic<bool> user_preference_enabled_{true};
  std::atomic<bool> runtime_failed_{false};
  std::atomic<bool> failure_logged_{false};
  std::atomic<bool> plane_conversion_unavailable_{false};
  std::atomic<bool> plane_failure_logged_{false};
  mutable std::mutex backend_name_mutex_;
  QString backend_name_;
  std::optional<QString> environment_override_;
};

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_GPU_SURFACE_POLICY_H
