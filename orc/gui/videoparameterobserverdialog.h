/*
 * File:        videoparameterobserverdialog.h
 * Module:      orc-gui
 * Purpose:     Video parameter observer dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef VIDEOPARAMETEROBSERVERDIALOG_H
#define VIDEOPARAMETEROBSERVERDIALOG_H

#include <orc/stage/common_types.h>
#include <orc/stage/field_id.h>

#include <QDialog>
#include <QGroupBox>
#include <QLabel>
#include <QStatusBar>

#include "presenters/include/video_parameter_observation_view_models.h"

/**
 * @brief Dialog showing video parameter observer values for the current field.
 *
 * Displays only values derived from signal observation
 * (ColourFramePhaseObserver, BurstLevelObserver, WhiteSNRObserver,
 * BlackPSNRObserver, FrameQualityObserver) plus the base signal parameters from
 * get_video_parameters().
 *
 * No embedded TBC metadata is shown here. Values are always from the
 * observation context populated by the DAGFrameRenderer.
 */
class VideoParameterObserverDialog : public QDialog {
  Q_OBJECT

 public:
  explicit VideoParameterObserverDialog(QWidget* parent = nullptr);
  ~VideoParameterObserverDialog();

  /**
   * @brief Update displayed values for a single field.
   */
  void updateObservations(
      const orc::FieldID& field_id,
      const orc::presenters::VideoParameterObservationView& observations);

  /**
   * @brief Update displayed values for a frame (two fields).
   *
   * Shows field-1 observations and field-2 observations side by side.
   * The colour frame index (per-frame value) is shown once.
   */
  void updateObservationsForFrame(
      const orc::FieldID& field1_id,
      const orc::presenters::VideoParameterObservationView& field1_obs,
      const orc::FieldID& field2_id,
      const orc::presenters::VideoParameterObservationView& field2_obs);

  /**
   * @brief Clear all displayed values.
   */
  void clearObservations();

  /**
   * @brief Show a "computing" pending state while observations are fetched.
   *
   * Displayed in the status bar after an async request is issued and cleared
   * once values arrive. The previously shown values remain visible and keep
   * their position while the notice comes and goes.
   */
  void showPending();

  /**
   * @brief Set the amplitude display unit used for the burst level.
   *
   * Takes effect on the next update; the main window refreshes the dialog
   * after propagating a unit change.
   */
  void setAmplitudeUnit(orc::AmplitudeDisplayUnit unit);

 private:
  void setupUI();
  void updateFieldGroup(
      QGroupBox* group, QLabel* colour_frame_label, QLabel* burst_label,
      QLabel* snr_label, QLabel* psnr_label, QLabel* quality_label,
      QLabel* dropout_label,
      const orc::presenters::VideoParameterObservationView& obs);
  void updateSignalParams(
      const orc::presenters::VideoParameterObservationView& obs);

  QString formatBurstLevel(
      const orc::presenters::VideoParameterObservationView& obs) const;

  static QString systemName(orc::presenters::VideoSystem sys);
  static QString fmtOptDouble(const std::optional<double>& v,
                              const char* unit = "");

  // Status bar along the bottom edge; always present so that showing or hiding
  // the pending notice never changes the dialog's layout.
  QStatusBar* status_bar_ = nullptr;

  // Pending-state notice shown while an async observation request is in flight.
  QLabel* status_label_;

  // Signal parameters group (shown once — same for both fields)
  QGroupBox* signal_group_;
  QLabel* system_label_;
  QLabel* frame_width_label_;
  QLabel* burst_range_label_;
  QLabel* active_range_label_;
  QLabel* levels_label_;

  // Field 1 observations group
  QGroupBox* field1_group_;
  QLabel* field1_colour_frame_label_;
  QLabel* field1_burst_label_;
  QLabel* field1_snr_label_;
  QLabel* field1_psnr_label_;
  QLabel* field1_quality_label_;
  QLabel* field1_dropout_label_;

  // Field 2 observations group (hidden in field-view mode)
  QGroupBox* field2_group_;
  QLabel* field2_colour_frame_label_;
  QLabel* field2_burst_label_;
  QLabel* field2_snr_label_;
  QLabel* field2_psnr_label_;
  QLabel* field2_quality_label_;
  QLabel* field2_dropout_label_;

  orc::AmplitudeDisplayUnit amplitude_unit_ = orc::AmplitudeDisplayUnit::IRE;
};

#endif  // VIDEOPARAMETEROBSERVERDIALOG_H
