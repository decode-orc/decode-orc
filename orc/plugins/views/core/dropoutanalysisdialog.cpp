/*
 * File:        dropoutanalysisdialog.cpp
 * Module:      orc-view_core
 * Purpose:     Dropout analysis dialog implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dropoutanalysisdialog.h"

#include <QLabel>
#include <QPen>
#include <QStackedLayout>
#include <QtMath>
#include <cmath>
#include <limits>

DropoutAnalysisDialog::DropoutAnalysisDialog(QWidget* parent)
    : AnalysisDialogBase(parent),
      plot_(nullptr),
      series_(nullptr),
      plotMarker_(nullptr),
      maxY_(0.0),
      numberOfFrames_(0) {
  setWindowTitle("Dropout Analysis");
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

  // Set up series and marker
  series_ = plot_->addSeries("Dropout Length");
  series_->setPen(QPen(Qt::red, 1));
  series_->setStyle(orc::view::PlotSeries::Bars);

  plotMarker_ = plot_->addMarker();
  plotMarker_->setStyle(orc::view::PlotMarker::VLine);
  plotMarker_->setPen(QPen(Qt::blue, 2));

  // Set up update throttling timer (from base class)
  setupUpdateTimer();

  // Connect to plot area changed signal
  connect(plot_, &orc::view::PlotWidget::plotAreaChanged, this,
          &DropoutAnalysisDialog::onPlotAreaChanged);
  connect(plot_, &orc::view::PlotWidget::plotClicked, this,
          &DropoutAnalysisDialog::onPlotClicked);

  // Set default size
  resize(800, 600);
}

DropoutAnalysisDialog::~DropoutAnalysisDialog() { removeChartContents(); }

void DropoutAnalysisDialog::startUpdate(int32_t numberOfFrames,
                                        bool decimated) {
  removeChartContents();
  numberOfFrames_ = numberOfFrames;
  decimated_ = decimated;
  points_.reserve(numberOfFrames);

  // Hide the "No data available" label and show the plot
  if (noDataLabel_) {
    noDataLabel_->hide();
  }
  plot_->show();
}

void DropoutAnalysisDialog::removeChartContents() {
  maxY_ = 0.0;
  points_.clear();
  bucketStart_.clear();
  bucketEnd_.clear();
  bucketCount_.clear();

  // Clear the series data
  if (series_) {
    series_->setData(QVector<QPointF>());
  }

  plot_->replot();
}

void DropoutAnalysisDialog::addDataPoint(int32_t frameNumber,
                                         double dropoutLength,
                                         int32_t frameStart, int32_t frameEnd,
                                         int32_t dropoutCount) {
  points_.append(QPointF(static_cast<qreal>(frameNumber),
                         static_cast<qreal>(dropoutLength)));
  bucketStart_.append(frameStart);
  bucketEnd_.append(frameEnd);
  bucketCount_.append(dropoutCount);

  // Keep track of the maximum Y value
  if (dropoutLength > maxY_) {
    maxY_ = dropoutLength;
  }
}

void DropoutAnalysisDialog::finishUpdate(int32_t currentFrameNumber) {
  // Set up plot properties
  plot_->updateTheme();  // Auto-detect theme and set appropriate background
  plot_->setGridEnabled(true);
  plot_->setZoomEnabled(true);
  plot_->setPanEnabled(true);
  plot_->setXAxisIntegerLabels(true);  // Frame numbers are whole numbers
  plot_->setYAxisIntegerLabels(true);  // Dropouts should be whole numbers

  // Set axis titles and ranges
  plot_->setAxisTitle(Qt::Horizontal, "Frame number");
  plot_->setAxisTitle(Qt::Vertical, "Dropout length (in samples)");
  plot_->setAxisRange(Qt::Horizontal, 0, numberOfFrames_);

  // Calculate appropriate Y-axis range (dropout lengths should always be >= 0)
  // Round to whole numbers since fractions of dropouts aren't meaningful
  double yMax =
      (maxY_ < 10)
          ? 10
          : ceil(maxY_ + (maxY_ * 0.1));  // Add 10% padding and round up
  plot_->setAxisRange(Qt::Vertical, 0, yMax);

  // Set the dropout curve data with theme-aware color
  QColor dataColor =
      orc::view::PlotWidget::isDarkTheme() ? Qt::yellow : Qt::darkMagenta;
  series_->setPen(QPen(dataColor, 2));
  series_->setData(points_);

  // Set the frame marker position
  plotMarker_->setPosition(
      QPointF(static_cast<double>(currentFrameNumber), yMax / 2));

  // Report whether this is a per-frame or a decimated (bucketed) view.
  setDecimationSummary(decimated_, numberOfFrames_,
                       static_cast<int>(points_.size()));

  // Render the plot
  plot_->replot();
}

void DropoutAnalysisDialog::updateFrameMarker(int32_t currentFrameNumber) {
  // Use base class throttling implementation
  updateFrameMarkerThrottled(currentFrameNumber);
}

void DropoutAnalysisDialog::showNoDataMessage(const QString& reason) {
  removeChartContents();

  // Use base class implementation
  showNoDataMessageImpl(reason, plot_);
}

void DropoutAnalysisDialog::calculateMarkerPosition(int32_t frameNumber) {
  double yMax = (maxY_ < 10) ? 10 : ceil(maxY_ + (maxY_ * 0.1));
  plotMarker_->setPosition(QPointF(static_cast<double>(frameNumber), yMax / 2));
  // No need to call plot->replot() - marker update() handles the redraw
}

void DropoutAnalysisDialog::onPlotAreaChanged() {
  // Handle plot area changes if needed
  // The PlotWidget handles zoom/pan internally
}

void DropoutAnalysisDialog::onPlotClicked(const QPointF& dataPoint) {
  if (points_.isEmpty()) return;

  // Find the plotted bucket nearest the click on the x (frame) axis.
  int nearest = 0;
  double bestDist = std::numeric_limits<double>::max();
  for (int i = 0; i < points_.size(); ++i) {
    double dist = std::abs(points_[i].x() - dataPoint.x());
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
  setBucketReadout(QString("%1 — dropout length: %2 samples, dropouts: %3")
                       .arg(range)
                       .arg(static_cast<qint64>(points_[nearest].y()))
                       .arg(bucketCount_.value(nearest)));
}

// No need to call plot->replot() - marker update() handles the redraw