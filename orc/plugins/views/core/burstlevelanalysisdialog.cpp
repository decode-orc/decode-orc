/*
 * File:        burstlevelanalysisdialog.cpp
 * Module:      orc-view_core
 * Purpose:     Burst level analysis dialog implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "burstlevelanalysisdialog.h"

#include <QLabel>
#include <QPen>
#include <QStackedLayout>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>

// Burst amplitude is an AC peak value (distance from the DC mean of the burst
// region), not an absolute sample level. Converting to display units means
// scaling by the signal range rather than translating from blanking.
static double burstAmplitudeToDisplay(double amplitude_10bit, int32_t blanking,
                                      int32_t white, orc::view::VideoSystem sys,
                                      orc::view::AmplitudeUnit unit) {
  const double range = static_cast<double>(white - blanking);
  if (range <= 0.0) return amplitude_10bit;
  switch (unit) {
    case orc::view::AmplitudeUnit::Millivolts:
      return amplitude_10bit / range * orc::view::active_video_mv(sys);
    case orc::view::AmplitudeUnit::Samples10Bit:
      return amplitude_10bit;
    default:  // IRE
      return amplitude_10bit / range * 100.0;
  }
}

BurstLevelAnalysisDialog::BurstLevelAnalysisDialog(QWidget* parent)
    : AnalysisDialogBase(parent),
      plot_(nullptr),
      burstSeries_(nullptr),
      plotMarker_(nullptr),
      maxY_(0.0),
      minY_(0.0),
      numberOfFrames_(0) {
  setWindowTitle("Burst Level Analysis");
  setWindowFlags(Qt::Window);
  setAttribute(Qt::WA_DeleteOnClose, false);

  // Create main layout
  auto* mainLayout = new QVBoxLayout(this);

  // Create plot widget
  plot_ = new orc::view::PlotWidget(this);
  plot_->updateTheme();

  // Set up "No data available" overlay (from base class)
  setupNoDataOverlay(mainLayout, plot_);

  // Bucket-info status line (from base class)
  setupBucketInfoLabel(mainLayout);

  // Set up series for Burst Level
  burstSeries_ = plot_->addSeries("Burst Level");
  burstSeries_->setPen(QPen(Qt::yellow, 2));
  burstSeries_->setStyle(orc::view::PlotSeries::Lines);

  // Set up frame marker
  plotMarker_ = plot_->addMarker();
  plotMarker_->setStyle(orc::view::PlotMarker::VLine);
  plotMarker_->setPen(QPen(Qt::blue, 2));

  // Set up update throttling timer (from base class)
  setupUpdateTimer();

  // Connect to plot area changed signal
  connect(plot_, &orc::view::PlotWidget::plotAreaChanged, this,
          &BurstLevelAnalysisDialog::onPlotAreaChanged);
  connect(plot_, &orc::view::PlotWidget::plotClicked, this,
          &BurstLevelAnalysisDialog::onPlotClicked);

  // Set default size
  resize(800, 600);
}

BurstLevelAnalysisDialog::~BurstLevelAnalysisDialog() { removeChartContents(); }

void BurstLevelAnalysisDialog::startUpdate(int32_t numberOfFrames,
                                           bool decimated) {
  removeChartContents();
  numberOfFrames_ = numberOfFrames;
  decimated_ = decimated;
  burstPoints_.reserve(numberOfFrames);

  // Hide the "No data available" label and show the plot
  if (noDataLabel_) {
    noDataLabel_->hide();
  }
  plot_->show();
}

void BurstLevelAnalysisDialog::removeChartContents() {
  maxY_ = 0.0;
  minY_ = 1023.0;  // Initialize high for 10-bit domain
  burstPoints_.clear();
  bucketStart_.clear();
  bucketEnd_.clear();
  plot_->replot();
}

void BurstLevelAnalysisDialog::setVideoLevels(orc::view::VideoSystem system,
                                              int32_t blankingLevel,
                                              int32_t whiteLevel) {
  video_system_ = system;
  blanking_level_ = blankingLevel;
  white_level_ = whiteLevel;
}

void BurstLevelAnalysisDialog::addDataPoint(int32_t frameNumber,
                                            double burstLevel10bit,
                                            int32_t frameStart,
                                            int32_t frameEnd) {
  if (!std::isnan(burstLevel10bit)) {
    // Store raw 10-bit value; display conversion happens in finishUpdate().
    burstPoints_.append(QPointF(static_cast<qreal>(frameNumber),
                                static_cast<qreal>(burstLevel10bit)));
    bucketStart_.append(frameStart);
    bucketEnd_.append(frameEnd);
    if (burstLevel10bit > maxY_) maxY_ = burstLevel10bit;
    if (burstLevel10bit < minY_) minY_ = burstLevel10bit;
  }
}

void BurstLevelAnalysisDialog::finishUpdate(int32_t currentFrameNumber) {
  current_frame_number_ = currentFrameNumber;

  plot_->updateTheme();
  plot_->setGridEnabled(true);
  plot_->setZoomEnabled(true);
  plot_->setPanEnabled(true);
  plot_->setYAxisIntegerLabels(false);

  plot_->setAxisTitle(Qt::Horizontal, "Frame number");
  plot_->setAxisTitle(
      Qt::Vertical,
      QString("Burst Level (%1)")
          .arg(QString::fromStdString(
              orc::view::amplitude_unit_suffix(amplitude_unit_))));

  // Resolve video levels for unit conversion.
  int32_t blanking = 256, white = 844;
  orc::view::VideoSystem sys = orc::view::VideoSystem::PAL;
  if (blanking_level_ >= 0 && white_level_ > blanking_level_) {
    blanking = blanking_level_;
    white = white_level_;
    sys = video_system_ == orc::view::VideoSystem::Unknown
              ? orc::view::VideoSystem::PAL
              : video_system_;
  }
  // Cache for the click readout so it uses the same conversion.
  conv_blanking_ = blanking;
  conv_white_ = white;
  conv_sys_ = sys;

  // X-axis range from actual data.
  double xMin = 0;
  double xMax = static_cast<double>(numberOfFrames_);
  if (!burstPoints_.isEmpty()) {
    double dataMin = std::numeric_limits<double>::max();
    double dataMax = 0;
    for (const auto& pt : burstPoints_) {
      if (pt.x() < dataMin) dataMin = pt.x();
      if (pt.x() > dataMax) dataMax = pt.x();
    }
    if (dataMax > 0) {
      xMin = std::floor(dataMin);
      xMax = std::ceil(dataMax);
    }
  }
  plot_->setAxisRange(Qt::Horizontal, xMin, xMax);

  double xRange = xMax - xMin;
  double xTickStep = 1.0;
  if (xRange > 0) {
    double idealStep = xRange / 10.0;
    double magnitude = std::pow(10.0, std::floor(std::log10(idealStep)));
    double normalized = idealStep / magnitude;
    if (normalized < 1.5) {
      xTickStep = 1.0 * magnitude;
    } else if (normalized < 3.0) {
      xTickStep = 2.0 * magnitude;
    } else if (normalized < 7.0) {
      xTickStep = 5.0 * magnitude;
    } else {
      xTickStep = 10.0 * magnitude;
    }
  }
  plot_->setAxisTickStep(Qt::Horizontal, xTickStep, 0.0);

  // Convert stored 10-bit Y values to display units.
  QVector<QPointF> displayPoints;
  double yMin = 0.0, yMax = 0.0;

  if (!burstPoints_.isEmpty()) {
    double dispMin = std::numeric_limits<double>::max();
    double dispMax = std::numeric_limits<double>::lowest();
    displayPoints.reserve(burstPoints_.size());
    for (const auto& pt : burstPoints_) {
      double yDisp = burstAmplitudeToDisplay(pt.y(), blanking, white, sys,
                                             amplitude_unit_);
      displayPoints.append(QPointF(pt.x(), yDisp));
      if (yDisp < dispMin) dispMin = yDisp;
      if (yDisp > dispMax) dispMax = yDisp;
    }
    double padding = (dispMax - dispMin) * 0.1;
    if (padding < 1.0) padding = 1.0;
    yMin = std::floor(dispMin - padding);
    yMax = std::ceil(dispMax + padding);
    if (yMin < 0) yMin = 0;
  } else {
    const auto [r_min, r_max] = orc::view::amplitude_display_range(
        0, blanking, white, 1023, sys, amplitude_unit_);
    yMin = r_min;
    yMax = r_max;
  }

  display_y_min_ = yMin;
  display_y_max_ = yMax;
  plot_->setAxisRange(Qt::Vertical, yMin, yMax);

  // Compute a sensible Y tick step from the visible range (same algorithm as
  // the X-axis). amplitude_major_tick() assumes a full-range signal display
  // (e.g. 0-100 IRE) and is too coarse for the narrow burst amplitude range.
  double yTickStep = orc::view::amplitude_major_tick(amplitude_unit_);
  double yRange = yMax - yMin;
  if (yRange > 0.0) {
    double idealStep = yRange / 8.0;
    double magnitude = std::pow(10.0, std::floor(std::log10(idealStep)));
    double normalized = idealStep / magnitude;
    if (normalized < 1.5) {
      yTickStep = 1.0 * magnitude;
    } else if (normalized < 3.0) {
      yTickStep = 2.0 * magnitude;
    } else if (normalized < 7.0) {
      yTickStep = 5.0 * magnitude;
    } else {
      yTickStep = 10.0 * magnitude;
    }
  }
  plot_->setAxisTickStep(Qt::Vertical, yTickStep, 0.0);

  if (!displayPoints.isEmpty()) {
    std::sort(displayPoints.begin(), displayPoints.end(),
              [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
    QColor burstColor =
        orc::view::PlotWidget::isDarkTheme() ? Qt::yellow : QColor(180, 140, 0);
    burstSeries_->setPen(QPen(burstColor, 2));
    burstSeries_->setData(displayPoints);
    burstSeries_->setVisible(true);
  }

  plotMarker_->setPosition(QPointF(static_cast<double>(currentFrameNumber),
                                   (display_y_max_ + display_y_min_) / 2));

  // Report whether this is a per-frame or a decimated (bucketed) view.
  setDecimationSummary(decimated_, numberOfFrames_,
                       static_cast<int>(burstPoints_.size()));

  plot_->replot();
}

void BurstLevelAnalysisDialog::updateFrameMarker(int32_t currentFrameNumber) {
  // Use base class throttling implementation
  updateFrameMarkerThrottled(currentFrameNumber);
}

void BurstLevelAnalysisDialog::showNoDataMessage(const QString& reason) {
  removeChartContents();

  // Use base class implementation
  showNoDataMessageImpl(reason, plot_);
}

void BurstLevelAnalysisDialog::calculateMarkerPosition(int32_t frameNumber) {
  plotMarker_->setPosition(QPointF(static_cast<double>(frameNumber),
                                   (display_y_max_ + display_y_min_) / 2));
}

void BurstLevelAnalysisDialog::setAmplitudeUnit(orc::view::AmplitudeUnit unit) {
  if (amplitude_unit_ == unit) return;
  amplitude_unit_ = unit;
  if (!burstPoints_.isEmpty()) {
    finishUpdate(current_frame_number_);
  }
}

void BurstLevelAnalysisDialog::onPlotAreaChanged() {
  // The PlotWidget handles zoom/pan internally.
}

void BurstLevelAnalysisDialog::onPlotClicked(const QPointF& dataPoint) {
  if (burstPoints_.isEmpty()) return;

  // Find the bucket nearest the click on the x (frame) axis.
  int nearest = 0;
  double bestDist = std::numeric_limits<double>::max();
  for (int i = 0; i < burstPoints_.size(); ++i) {
    double dist = std::abs(burstPoints_[i].x() - dataPoint.x());
    if (dist < bestDist) {
      bestDist = dist;
      nearest = i;
    }
  }

  const int32_t start = bucketStart_.value(nearest);
  const int32_t end = bucketEnd_.value(nearest);
  activateFrameInBucket(dataPoint.x(), start, end);
  const QString range = (start == end)
                            ? QString("Frame %1").arg(start)
                            : QString("Frames %1–%2").arg(start).arg(end);
  const double display =
      burstAmplitudeToDisplay(burstPoints_[nearest].y(), conv_blanking_,
                              conv_white_, conv_sys_, amplitude_unit_);
  setBucketReadout(QString("%1 — burst level: %2 %3")
                       .arg(range)
                       .arg(QString::number(display, 'f', 2))
                       .arg(QString::fromStdString(
                           orc::view::amplitude_unit_suffix(amplitude_unit_))));
}
