/*
 * File:        analysisdialogbase.cpp
 * Module:      orc-view_core
 * Purpose:     Base class for analysis dialogs implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "analysisdialogbase.h"

#include <QFont>
#include <QStackedLayout>
#include <algorithm>
#include <cmath>

AnalysisDialogBase::AnalysisDialogBase(QWidget* parent)
    : QDialog(parent),
      noDataLabel_(nullptr),
      updateTimer_(nullptr),
      pendingFrameNumber_(0),
      hasPendingUpdate_(false) {
  // Base initialization only - derived classes handle specific setup
}

void AnalysisDialogBase::setupUpdateTimer() {
  updateTimer_ = new QTimer(this);
  updateTimer_->setSingleShot(true);
  updateTimer_->setInterval(16);  // ~60fps max update rate
  connect(updateTimer_, &QTimer::timeout, this,
          &AnalysisDialogBase::onUpdateTimerTimeout);
}

void AnalysisDialogBase::setupNoDataOverlay(QVBoxLayout* mainLayout,
                                            orc::view::PlotWidget* plot) {
  // Create "No data available" label (initially hidden)
  noDataLabel_ = new QLabel("No data available", this);
  noDataLabel_->setAlignment(Qt::AlignCenter);
  QFont font = noDataLabel_->font();
  font.setPointSize(14);
  noDataLabel_->setFont(font);

  // Use a stacked layout to overlay label on plot
  auto* plotContainer = new QWidget(this);
  auto* plotLayout = new QStackedLayout(plotContainer);
  plotLayout->setStackingMode(QStackedLayout::StackAll);
  plotLayout->addWidget(plot);
  plotLayout->addWidget(noDataLabel_);

  mainLayout->addWidget(plotContainer);

  // Start with plot visible, label hidden
  noDataLabel_->hide();
}

void AnalysisDialogBase::setupBucketInfoLabel(QVBoxLayout* mainLayout) {
  bucketInfoLabel_ = new QLabel(this);
  bucketInfoLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  bucketInfoLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  QFont font = bucketInfoLabel_->font();
  font.setPointSize(font.pointSize() > 1 ? font.pointSize() - 1
                                         : font.pointSize());
  bucketInfoLabel_->setFont(font);
  mainLayout->addWidget(bucketInfoLabel_);
}

void AnalysisDialogBase::setDecimationSummary(bool decimated,
                                              int32_t totalFrames,
                                              int pointCount) {
  QString summary;
  if (decimated && pointCount > 0) {
    // Average bucket width; individual buckets vary by ±1 frame.
    int perBucket = (totalFrames + pointCount - 1) / pointCount;
    summary = QString(
                  "Decimated view: %1 bars over %2 frames "
                  "(~%3 frames per bar). Click a bar for its exact range and "
                  "totals.")
                  .arg(pointCount)
                  .arg(totalFrames)
                  .arg(perBucket);
  } else {
    summary = QString("Per-frame view: %1 frame%2.")
                  .arg(totalFrames)
                  .arg(totalFrames == 1 ? "" : "s");
  }
  decimationSummary_ = summary;
  if (bucketInfoLabel_) {
    bucketInfoLabel_->setText(summary);
  }
}

void AnalysisDialogBase::setBucketReadout(const QString& text) {
  if (bucketInfoLabel_) {
    bucketInfoLabel_->setText(text);
  }
}

void AnalysisDialogBase::activateFrameInBucket(double clickX,
                                               int32_t frameStart,
                                               int32_t frameEnd) {
  if (frameEnd < frameStart) {
    std::swap(frameStart, frameEnd);
  }
  const double clicked = std::round(clickX);
  int32_t frame = frameStart;
  if (std::isfinite(clicked)) {
    frame = static_cast<int32_t>(std::clamp(clicked,
                                            static_cast<double>(frameStart),
                                            static_cast<double>(frameEnd)));
  }
  emit frameActivated(frame);
}

void AnalysisDialogBase::updateFrameMarkerThrottled(
    int32_t currentFrameNumber) {
  // Always store the pending frame number
  pendingFrameNumber_ = currentFrameNumber;
  hasPendingUpdate_ = true;

  // Skip timer start if dialog is not visible - update will happen on show
  if (!isVisible()) return;

  // Start or restart the timer
  if (!updateTimer_->isActive()) {
    updateTimer_->start();
  }
}

void AnalysisDialogBase::showNoDataMessageImpl(const QString& reason,
                                               orc::view::PlotWidget* plot) {
  // Hide the plot and show the "No data available" label
  plot->hide();
  if (noDataLabel_) {
    QString message = reason.isEmpty() ? "No data available" : reason;
    noDataLabel_->setText(message);
    noDataLabel_->show();
  }
}

void AnalysisDialogBase::onUpdateTimerTimeout() {
  if (!hasPendingUpdate_) return;

  // Let derived class calculate and set marker position
  calculateMarkerPosition(pendingFrameNumber_);

  hasPendingUpdate_ = false;
}

void AnalysisDialogBase::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);

  // Force immediate marker update if we have a pending position
  if (hasPendingUpdate_) {
    onUpdateTimerTimeout();
  }
}
