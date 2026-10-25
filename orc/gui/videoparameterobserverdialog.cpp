/*
 * File:        videoparameterobserverdialog.cpp
 * Module:      orc-gui
 * Purpose:     Video parameter observer dialog implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videoparameterobserverdialog.h"

#include <amplitude_conversion.h>

#include <QGridLayout>
#include <QStatusBar>
#include <QVBoxLayout>

// Burst level is an AC amplitude (peak burst excursion, not an absolute
// level), so IRE/mV conversion scales by the blanking-to-white range without
// translating from blanking.
static double burstAmplitudeToDisplay(double amplitude_10bit, int32_t blanking,
                                      int32_t white, orc::VideoSystem sys,
                                      orc::AmplitudeDisplayUnit unit) {
  const double range = static_cast<double>(white - blanking);
  if (range <= 0.0) return 0.0;
  if (unit == orc::AmplitudeDisplayUnit::Millivolts) {
    return amplitude_10bit / range * orc::active_video_mv(sys);
  }
  return amplitude_10bit / range * 100.0;
}

static orc::VideoSystem toOrcVideoSystem(orc::presenters::VideoSystem sys) {
  switch (sys) {
    case orc::presenters::VideoSystem::NTSC:
      return orc::VideoSystem::NTSC;
    case orc::presenters::VideoSystem::PAL_M:
      return orc::VideoSystem::PAL_M;
    default:
      return orc::VideoSystem::PAL;
  }
}

VideoParameterObserverDialog::VideoParameterObserverDialog(QWidget* parent)
    : QDialog(parent) {
  setupUI();
  setWindowTitle("Video Parameter Observer");
  setWindowFlags(Qt::Window);
  setAttribute(Qt::WA_DeleteOnClose, false);
  resize(480, 560);
  setMinimumSize(400, 480);
}

VideoParameterObserverDialog::~VideoParameterObserverDialog() = default;

void VideoParameterObserverDialog::setupUI() {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto* main = new QVBoxLayout();
  main->setContentsMargins(9, 9, 9, 9);
  outer->addLayout(main, /*stretch=*/1);

  // --- Signal Parameters group (once per frame) ---
  signal_group_ = new QGroupBox("Signal Parameters", this);
  auto* sg = new QGridLayout(signal_group_);
  sg->setColumnStretch(1, 1);
  sg->setVerticalSpacing(6);
  sg->setHorizontalSpacing(12);

  int row = 0;
  sg->addWidget(new QLabel("System:"), row, 0);
  system_label_ = new QLabel("-");
  sg->addWidget(system_label_, row++, 1);

  sg->addWidget(new QLabel("Frame width:"), row, 0);
  frame_width_label_ = new QLabel("-");
  sg->addWidget(frame_width_label_, row++, 1);

  sg->addWidget(new QLabel("Burst range:"), row, 0);
  burst_range_label_ = new QLabel("-");
  burst_range_label_->setToolTip("Colour burst sample range");
  sg->addWidget(burst_range_label_, row++, 1);

  sg->addWidget(new QLabel("Active range:"), row, 0);
  active_range_label_ = new QLabel("-");
  active_range_label_->setToolTip("Active video sample range");
  sg->addWidget(active_range_label_, row++, 1);

  sg->addWidget(new QLabel("Levels (sync/blk/wht):"), row, 0);
  levels_label_ = new QLabel("-");
  levels_label_->setToolTip(
      "Sync tip / black / white levels (in the project's amplitude unit)");
  sg->addWidget(levels_label_, row++, 1);

  main->addWidget(signal_group_);

  // --- Field 1 group ---
  field1_group_ = new QGroupBox("Field 1 Observations", this);
  auto* f1 = new QGridLayout(field1_group_);
  f1->setColumnStretch(1, 1);
  f1->setVerticalSpacing(6);
  f1->setHorizontalSpacing(12);

  row = 0;
  f1->addWidget(new QLabel("Colour frame index:"), row, 0);
  field1_colour_frame_label_ = new QLabel("-");
  field1_colour_frame_label_->setToolTip(
      "Position in colour subcarrier sequence (1–4 PAL/PAL-M, 0–1 NTSC)");
  f1->addWidget(field1_colour_frame_label_, row++, 1);

  f1->addWidget(new QLabel("Burst level:"), row, 0);
  field1_burst_label_ = new QLabel("-");
  field1_burst_label_->setToolTip(
      "Median colour burst amplitude (in the project's amplitude unit)");
  f1->addWidget(field1_burst_label_, row++, 1);

  f1->addWidget(new QLabel("White SNR:"), row, 0);
  field1_snr_label_ = new QLabel("-");
  field1_snr_label_->setToolTip("Luminance signal-to-noise ratio");
  f1->addWidget(field1_snr_label_, row++, 1);

  f1->addWidget(new QLabel("Black PSNR:"), row, 0);
  field1_psnr_label_ = new QLabel("-");
  field1_psnr_label_->setToolTip("Black level peak signal-to-noise ratio");
  f1->addWidget(field1_psnr_label_, row++, 1);

  f1->addWidget(new QLabel("Quality score:"), row, 0);
  field1_quality_label_ = new QLabel("-");
  field1_quality_label_->setToolTip(
      "Disc quality score for the whole frame (0.0 = worst, 1.0 = best)");
  f1->addWidget(field1_quality_label_, row++, 1);

  f1->addWidget(new QLabel("Dropouts:"), row, 0);
  field1_dropout_label_ = new QLabel("-");
  field1_dropout_label_->setToolTip("Dropout region count for the whole frame");
  f1->addWidget(field1_dropout_label_, row++, 1);

  main->addWidget(field1_group_);

  // --- Field 2 group ---
  field2_group_ = new QGroupBox("Field 2 Observations", this);
  auto* f2 = new QGridLayout(field2_group_);
  f2->setColumnStretch(1, 1);
  f2->setVerticalSpacing(6);
  f2->setHorizontalSpacing(12);

  row = 0;
  f2->addWidget(new QLabel("Colour frame index:"), row, 0);
  field2_colour_frame_label_ = new QLabel("-");
  f2->addWidget(field2_colour_frame_label_, row++, 1);

  f2->addWidget(new QLabel("Burst level:"), row, 0);
  field2_burst_label_ = new QLabel("-");
  f2->addWidget(field2_burst_label_, row++, 1);

  f2->addWidget(new QLabel("White SNR:"), row, 0);
  field2_snr_label_ = new QLabel("-");
  f2->addWidget(field2_snr_label_, row++, 1);

  f2->addWidget(new QLabel("Black PSNR:"), row, 0);
  field2_psnr_label_ = new QLabel("-");
  f2->addWidget(field2_psnr_label_, row++, 1);

  f2->addWidget(new QLabel("Quality score:"), row, 0);
  field2_quality_label_ = new QLabel("-");
  f2->addWidget(field2_quality_label_, row++, 1);

  f2->addWidget(new QLabel("Dropouts:"), row, 0);
  field2_dropout_label_ = new QLabel("-");
  f2->addWidget(field2_dropout_label_, row++, 1);

  field2_group_->setVisible(false);
  main->addWidget(field2_group_);
  main->addStretch();

  // The pending notice lives in the status bar, so showing or hiding it never
  // reflows the value groups above it.
  status_bar_ = new QStatusBar(this);
  status_bar_->setObjectName("observationStatusBar");
  status_bar_->setSizeGripEnabled(false);

  status_label_ = new QLabel(this);
  status_label_->setObjectName("observationStatusLabel");
  status_bar_->addWidget(status_label_);
  status_label_->setVisible(false);

  outer->addWidget(status_bar_);
}

// ---------------------------------------------------------------------------
// Public update methods
// ---------------------------------------------------------------------------

void VideoParameterObserverDialog::showPending() {
  status_label_->setText(tr("Computing observations…"));
  status_label_->setVisible(true);
}

void VideoParameterObserverDialog::updateObservations(
    const orc::FieldID& field_id,
    const orc::presenters::VideoParameterObservationView& obs) {
  (void)field_id;
  status_label_->setVisible(false);
  updateSignalParams(obs);
  field1_group_->setTitle("Field Observations");
  updateFieldGroup(field1_group_, field1_colour_frame_label_,
                   field1_burst_label_, field1_snr_label_, field1_psnr_label_,
                   field1_quality_label_, field1_dropout_label_, obs);
  field2_group_->setVisible(false);
}

void VideoParameterObserverDialog::updateObservationsForFrame(
    const orc::FieldID& field1_id,
    const orc::presenters::VideoParameterObservationView& field1_obs,
    const orc::FieldID& field2_id,
    const orc::presenters::VideoParameterObservationView& field2_obs) {
  (void)field1_id;
  (void)field2_id;
  status_label_->setVisible(false);
  updateSignalParams(field1_obs);
  field1_group_->setTitle("Field 1 Observations");
  updateFieldGroup(field1_group_, field1_colour_frame_label_,
                   field1_burst_label_, field1_snr_label_, field1_psnr_label_,
                   field1_quality_label_, field1_dropout_label_, field1_obs);
  field2_group_->setVisible(true);
  updateFieldGroup(field2_group_, field2_colour_frame_label_,
                   field2_burst_label_, field2_snr_label_, field2_psnr_label_,
                   field2_quality_label_, field2_dropout_label_, field2_obs);
}

void VideoParameterObserverDialog::clearObservations() {
  status_label_->setVisible(false);
  system_label_->setText("-");
  frame_width_label_->setText("-");
  burst_range_label_->setText("-");
  active_range_label_->setText("-");
  levels_label_->setText("-");

  const auto clear = [](QLabel* l) { l->setText("-"); };
  clear(field1_colour_frame_label_);
  clear(field1_burst_label_);
  clear(field1_snr_label_);
  clear(field1_psnr_label_);
  clear(field1_quality_label_);
  clear(field1_dropout_label_);
  clear(field2_colour_frame_label_);
  clear(field2_burst_label_);
  clear(field2_snr_label_);
  clear(field2_psnr_label_);
  clear(field2_quality_label_);
  clear(field2_dropout_label_);
  field2_group_->setVisible(false);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void VideoParameterObserverDialog::updateSignalParams(
    const orc::presenters::VideoParameterObservationView& obs) {
  if (!obs.video_params.has_value()) {
    system_label_->setText("-");
    frame_width_label_->setText("-");
    burst_range_label_->setText("-");
    active_range_label_->setText("-");
    levels_label_->setText("-");
    return;
  }
  const auto& vp = *obs.video_params;
  system_label_->setText(systemName(vp.system));
  frame_width_label_->setText(vp.frame_width_nominal >= 0
                                  ? QString::number(vp.frame_width_nominal) +
                                        " samples/line"
                                  : "-");
  burst_range_label_->setText(
      (vp.color_burst_start >= 0 && vp.color_burst_end >= 0)
          ? QString("%1–%2").arg(vp.color_burst_start).arg(vp.color_burst_end)
          : "-");
  active_range_label_->setText(
      (vp.active_video_start >= 0 && vp.active_video_end >= 0)
          ? QString("%1–%2").arg(vp.active_video_start).arg(vp.active_video_end)
          : "-");
  // IRE/mV need valid blanking/white levels; fall back to raw 10-bit ADU.
  const orc::AmplitudeDisplayUnit level_unit =
      (vp.blanking_level >= 0 && vp.white_level > vp.blanking_level)
          ? amplitude_unit_
          : orc::AmplitudeDisplayUnit::Samples10Bit;
  const auto fmtLevel = [&](int32_t level) {
    return QString::fromStdString(
        orc::format_amplitude(level, vp.blanking_level, vp.white_level,
                              toOrcVideoSystem(vp.system), level_unit));
  };
  levels_label_->setText(
      (vp.sync_tip_level >= 0 && vp.black_level >= 0 && vp.white_level >= 0)
          ? QString("%1 / %2 / %3")
                .arg(fmtLevel(vp.sync_tip_level))
                .arg(fmtLevel(vp.black_level))
                .arg(fmtLevel(vp.white_level))
          : "-");
}

void VideoParameterObserverDialog::updateFieldGroup(
    QGroupBox* /*group*/, QLabel* colour_frame_label, QLabel* burst_label,
    QLabel* snr_label, QLabel* psnr_label, QLabel* quality_label,
    QLabel* dropout_label,
    const orc::presenters::VideoParameterObservationView& obs) {
  if (obs.colour_frame_index.has_value()) {
    const int idx = *obs.colour_frame_index;
    colour_frame_label->setText(idx >= 0 ? QString::number(idx) : "unknown");
  } else {
    colour_frame_label->setText("-");
  }

  burst_label->setText(formatBurstLevel(obs));
  snr_label->setText(fmtOptDouble(obs.white_snr_db, " dB"));
  psnr_label->setText(fmtOptDouble(obs.black_psnr_db, " dB"));

  if (obs.quality_score.has_value()) {
    quality_label->setText(QString::number(*obs.quality_score, 'f', 3));
  } else {
    quality_label->setText("-");
  }

  dropout_label->setText(obs.dropout_count.has_value()
                             ? QString::number(*obs.dropout_count)
                             : "-");
}

QString VideoParameterObserverDialog::formatBurstLevel(
    const orc::presenters::VideoParameterObservationView& obs) const {
  if (!obs.burst_level_10bit.has_value()) return "-";
  const double burst = *obs.burst_level_10bit;

  // IRE/mV need valid blanking/white levels; fall back to raw 10-bit ADU.
  if (amplitude_unit_ != orc::AmplitudeDisplayUnit::Samples10Bit &&
      obs.video_params.has_value() && obs.video_params->blanking_level >= 0 &&
      obs.video_params->white_level > obs.video_params->blanking_level) {
    const auto& vp = *obs.video_params;
    const double value =
        burstAmplitudeToDisplay(burst, vp.blanking_level, vp.white_level,
                                toOrcVideoSystem(vp.system), amplitude_unit_);
    return QString::number(value, 'f', 1) + " " +
           QString::fromStdString(orc::amplitude_unit_suffix(amplitude_unit_));
  }
  return QString::number(burst, 'f', 1) + " ADU";
}

void VideoParameterObserverDialog::setAmplitudeUnit(
    orc::AmplitudeDisplayUnit unit) {
  amplitude_unit_ = unit;
}

QString VideoParameterObserverDialog::systemName(
    orc::presenters::VideoSystem sys) {
  switch (sys) {
    case orc::presenters::VideoSystem::PAL:
      return "PAL";
    case orc::presenters::VideoSystem::NTSC:
      return "NTSC";
    case orc::presenters::VideoSystem::PAL_M:
      return "PAL-M";
    default:
      return "Unknown";
  }
}

QString VideoParameterObserverDialog::fmtOptDouble(
    const std::optional<double>& v, const char* unit) {
  if (!v.has_value()) return "-";
  return QString::number(*v, 'f', 1) + unit;
}
