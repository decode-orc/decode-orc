/*
 * File:        config_session_controller.h
 * Module:      orc-view_core
 * Purpose:     Connect a stage's configuration dialog to its tool session:
 *              the node's stored values in, what OK configures out through
 *              apply_parameters
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/view/stage_tool_session.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <memory>

class ConfigDialogBase;
class FFmpegPresetDialog;
class MaskLineConfigDialog;

/**
 * @brief What every configuration dialog does with its session
 *
 * OK writes the values the dialog configured to the node with
 * apply_parameters(). That rebuilds the graph, which closes the dialog; if
 * the host refuses the values the dialog stays open and says why.
 *
 * Parented to its dialog, so it goes when the dialog does. GUI thread only.
 */
class ConfigSessionController : public QObject {
  Q_OBJECT

 public:
  /// @p apply_label names the edit in the status bar.
  ConfigSessionController(ConfigDialogBase* dialog,
                          std::shared_ptr<orc::view::IStageToolSession> session,
                          QString apply_label);

 protected:
  /// Show the node's stored values in the dialog.
  void loadStoredParameters();

  orc::view::IStageToolSession& session() const { return *session_; }
  ConfigDialogBase* dialog() const { return dialog_; }

 private:
  void submit();

  QPointer<ConfigDialogBase> dialog_;
  std::shared_ptr<orc::view::IStageToolSession> session_;
  QString apply_label_;
};

/// Mask Line Config. The stored lines are numbered in the input's video
/// system, so they are shown (and OK is enabled) once the stage has said
/// which system that is; when it cannot, PAL numbering is assumed.
class MaskLineSessionController final : public ConfigSessionController {
  Q_OBJECT

 public:
  MaskLineSessionController(
      MaskLineConfigDialog* dialog,
      std::shared_ptr<orc::view::IStageToolSession> session);

 private:
  void showStored();

  QPointer<MaskLineConfigDialog> mask_dialog_;
  std::shared_ptr<orc::view::IToolJob> job_;
};

/// FFmpeg Preset Config. The hardware encoders on offer are the ones the
/// stage says its FFmpeg has.
class FfmpegPresetSessionController final : public ConfigSessionController {
  Q_OBJECT

 public:
  FfmpegPresetSessionController(
      FFmpegPresetDialog* dialog,
      std::shared_ptr<orc::view::IStageToolSession> session);

 private:
  QPointer<FFmpegPresetDialog> ffmpeg_dialog_;
};
