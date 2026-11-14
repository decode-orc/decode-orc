/*
 * File:        analysis_session_controller.h
 * Module:      orc-view_core
 * Purpose:     Feed an analysis sink's graph window from its tool session:
 *              results after each trigger, the preview's frame, and frame
 *              clicks back to the preview
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/view/stage_tool_session.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class AnalysisDialogBase;
class BurstLevelAnalysisDialog;
class DropoutAnalysisDialog;
class SNRAnalysisDialog;

/**
 * @brief What every analysis graph window does with its session
 *
 * Asks for the stage's results when the window opens and again after each
 * trigger, keeps the frame marker on the frame the main preview shows, and
 * moves the preview to a frame the user clicks. A window with no results to
 * show (the stage has not been triggered) says why in place of the graph.
 *
 * Parented to its dialog, so it goes when the dialog does; the host closes
 * the session first, so no reply reaches it after that. GUI thread only.
 */
class AnalysisSessionController : public QObject {
  Q_OBJECT

 public:
  AnalysisSessionController(
      AnalysisDialogBase* dialog,
      std::shared_ptr<orc::view::IStageToolSession> session);

  /// Ask the stage for its results again.
  void requestResults();

 protected:
  /// Draw the stage's reply; @return false with @p error set when the bytes
  /// are not this analysis's results.
  virtual bool showResults(const std::vector<uint8_t>& data,
                           QString* error) = 0;
  /// Show @p reason in place of the graph.
  virtual void showNoResults(const QString& reason) = 0;
  /// Move the frame marker to @p frame_number (1-based).
  virtual void moveFrameMarker(int32_t frame_number) = 0;

  /// The frame (1-based) the main preview last showed, or 1.
  int32_t currentFrame() const;

  orc::view::IStageToolSession& session() const { return *session_; }

 private:
  std::shared_ptr<orc::view::IStageToolSession> session_;
  std::optional<uint64_t> preview_frame_;
};

/// The Dropout Analysis window.
class DropoutSessionController final : public AnalysisSessionController {
  Q_OBJECT

 public:
  DropoutSessionController(
      DropoutAnalysisDialog* dialog,
      std::shared_ptr<orc::view::IStageToolSession> session);

 protected:
  bool showResults(const std::vector<uint8_t>& data, QString* error) override;
  void showNoResults(const QString& reason) override;
  void moveFrameMarker(int32_t frame_number) override;

 private:
  QPointer<DropoutAnalysisDialog> dialog_;
};

/// The SNR Analysis window.
class SnrSessionController final : public AnalysisSessionController {
  Q_OBJECT

 public:
  SnrSessionController(SNRAnalysisDialog* dialog,
                       std::shared_ptr<orc::view::IStageToolSession> session);

 protected:
  bool showResults(const std::vector<uint8_t>& data, QString* error) override;
  void showNoResults(const QString& reason) override;
  void moveFrameMarker(int32_t frame_number) override;

 private:
  QPointer<SNRAnalysisDialog> dialog_;
};

/// The Burst Level Analysis window. It also follows the project's amplitude
/// unit.
class BurstLevelSessionController final : public AnalysisSessionController {
  Q_OBJECT

 public:
  BurstLevelSessionController(
      BurstLevelAnalysisDialog* dialog,
      std::shared_ptr<orc::view::IStageToolSession> session);

 protected:
  bool showResults(const std::vector<uint8_t>& data, QString* error) override;
  void showNoResults(const QString& reason) override;
  void moveFrameMarker(int32_t frame_number) override;

 private:
  QPointer<BurstLevelAnalysisDialog> dialog_;
};
