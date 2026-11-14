/*
 * File:        snranalysisdialog.cpp
 * Module:      orc-view_core
 * Purpose:     SNR analysis dialog implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "snranalysisdialog.h"

#include <QLabel>
#include <QPen>
#include <QStackedLayout>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>

SNRAnalysisDialog::SNRAnalysisDialog(QWidget* parent)
    : AnalysisDialogBase(parent),
      plot_(nullptr),
      whiteSNRSeries_(nullptr),
      blackPSNRSeries_(nullptr),
      plotMarker_(nullptr),
      displayModeCombo_(nullptr),
      maxWhiteY_(0.0),
      maxBlackY_(0.0),
      numberOfFrames_(0) {
  setWindowTitle("SNR Analysis");
  setWindowFlags(Qt::Window);
  setAttribute(Qt::WA_DeleteOnClose, false);

  // Create main layout
  auto* mainLayout = new QVBoxLayout(this);

  // Create display mode combo box
  displayModeCombo_ = new QComboBox(this);
  displayModeCombo_->addItem("White", QVariant::fromValue(DisplayMode::White));
  displayModeCombo_->addItem("Black", QVariant::fromValue(DisplayMode::Black));
  displayModeCombo_->addItem("Both", QVariant::fromValue(DisplayMode::Both));
  displayModeCombo_->setCurrentIndex(2);  // Default to "Both"
  displayModeCombo_->setToolTip("Select which SNR metrics to display");
  connect(displayModeCombo_,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &SNRAnalysisDialog::onDisplayModeChanged);
  mainLayout->addWidget(displayModeCombo_);

  // Create plot widget
  plot_ = new orc::view::PlotWidget(this);
  plot_->updateTheme();

  // Set up "No data available" overlay (from base class)
  setupNoDataOverlay(mainLayout, plot_);

  // Bucket-info status line (from base class)
  setupBucketInfoLabel(mainLayout);

  // Set up series for White SNR
  whiteSNRSeries_ = plot_->addSeries("White SNR");
  whiteSNRSeries_->setPen(QPen(Qt::green, 2));
  whiteSNRSeries_->setStyle(orc::view::PlotSeries::Lines);

  // Set up series for Black PSNR
  blackPSNRSeries_ = plot_->addSeries("Black PSNR");
  blackPSNRSeries_->setPen(QPen(Qt::cyan, 2));
  blackPSNRSeries_->setStyle(orc::view::PlotSeries::Lines);

  // Set up frame marker
  plotMarker_ = plot_->addMarker();
  plotMarker_->setStyle(orc::view::PlotMarker::VLine);
  plotMarker_->setPen(QPen(Qt::blue, 2));

  // Set up update throttling timer (from base class)
  setupUpdateTimer();

  // Connect to plot area changed signal
  connect(plot_, &orc::view::PlotWidget::plotAreaChanged, this,
          &SNRAnalysisDialog::onPlotAreaChanged);
  connect(plot_, &orc::view::PlotWidget::plotClicked, this,
          &SNRAnalysisDialog::onPlotClicked);

  // Set default size
  resize(800, 600);
}

SNRAnalysisDialog::~SNRAnalysisDialog() { removeChartContents(); }

void SNRAnalysisDialog::startUpdate(int32_t numberOfFrames, bool decimated) {
  removeChartContents();
  numberOfFrames_ = numberOfFrames;
  decimated_ = decimated;
  whitePoints_.reserve(numberOfFrames);
  blackPoints_.reserve(numberOfFrames);

  // Hide the "No data available" label and show the plot
  if (noDataLabel_) {
    noDataLabel_->hide();
  }
  plot_->show();
}

void SNRAnalysisDialog::removeChartContents() {
  maxWhiteY_ = 0.0;
  maxBlackY_ = 0.0;
  whitePoints_.clear();
  blackPoints_.clear();
  bucketLabel_.clear();
  bucketStart_.clear();
  bucketEnd_.clear();
  bucketWhite_.clear();
  bucketBlack_.clear();
  plot_->replot();
}

void SNRAnalysisDialog::addDataPoint(int32_t frameNumber, double whiteSNR,
                                     double blackPSNR, int32_t frameStart,
                                     int32_t frameEnd) {
  // Record the bucket for the click readout (one entry per call).
  bucketLabel_.append(frameNumber);
  bucketStart_.append(frameStart);
  bucketEnd_.append(frameEnd);
  bucketWhite_.append(whiteSNR);
  bucketBlack_.append(blackPSNR);

  // Add white SNR point if valid
  if (!std::isnan(whiteSNR)) {
    whitePoints_.append(
        QPointF(static_cast<qreal>(frameNumber), static_cast<qreal>(whiteSNR)));

    // Keep track of the maximum Y value
    if (whiteSNR > maxWhiteY_) {
      maxWhiteY_ = whiteSNR;
    }
  }

  // Add black PSNR point if valid
  if (!std::isnan(blackPSNR)) {
    blackPoints_.append(QPointF(static_cast<qreal>(frameNumber),
                                static_cast<qreal>(blackPSNR)));

    // Keep track of the maximum Y value
    if (blackPSNR > maxBlackY_) {
      maxBlackY_ = blackPSNR;
    }
  }
}

void SNRAnalysisDialog::finishUpdate(int32_t currentFrameNumber) {
  // Set up plot properties
  plot_->updateTheme();  // Auto-detect theme and set appropriate background
  plot_->setGridEnabled(true);
  plot_->setZoomEnabled(true);
  plot_->setPanEnabled(true);
  plot_->setYAxisIntegerLabels(false);  // SNR values can be decimal

  // Set axis titles and ranges
  plot_->setAxisTitle(Qt::Horizontal, "Frame number");
  plot_->setAxisTitle(Qt::Vertical, "SNR (dB)");

  // Set X-axis range based on actual data points
  double xMin = 0;
  double xMax = static_cast<double>(numberOfFrames_);

  // Find the actual min and max frame numbers from the data
  if (!whitePoints_.isEmpty() || !blackPoints_.isEmpty()) {
    double dataMin = std::numeric_limits<double>::max();
    double dataMax = 0;

    for (const auto& pt : whitePoints_) {
      if (pt.x() < dataMin) dataMin = pt.x();
      if (pt.x() > dataMax) dataMax = pt.x();
    }
    for (const auto& pt : blackPoints_) {
      if (pt.x() < dataMin) dataMin = pt.x();
      if (pt.x() > dataMax) dataMax = pt.x();
    }

    // Use the actual data range
    if (dataMax > 0) {
      xMin = std::floor(dataMin);  // Round down to integer
      xMax = std::ceil(dataMax);   // Round up to integer
    }
  }

  plot_->setAxisRange(Qt::Horizontal, xMin, xMax);

  // Calculate appropriate tick step for nice round numbers
  double xRange = xMax - xMin;
  double xTickStep = 1.0;
  if (xRange > 0) {
    // Determine tick step based on range to show ~10 ticks
    double idealStep = xRange / 10.0;

    // Round to nice numbers: 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, etc.
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

  // Calculate appropriate Y-axis range
  // SNR values are typically in the range of 20-60 dB
  double maxY = std::max(maxWhiteY_, maxBlackY_);
  double yMax = (maxY < 10) ? 10 : ceil(maxY + 5);  // Add some padding
  double yMin =
      0;  // SNR can theoretically be 0 or negative, but typically positive

  // If we have data, adjust the min to show the range better
  if (maxY > 0) {
    double minWhite = whitePoints_.isEmpty() ? yMax : whitePoints_[0].y();
    double minBlack = blackPoints_.isEmpty() ? yMax : blackPoints_[0].y();

    for (const auto& pt : whitePoints_) {
      if (pt.y() < minWhite) minWhite = pt.y();
    }
    for (const auto& pt : blackPoints_) {
      if (pt.y() < minBlack) minBlack = pt.y();
    }

    double minY = std::min(minWhite, minBlack);
    yMin = floor(minY - 5);  // Add some padding below
    if (yMin < 0) yMin = 0;
  }

  plot_->setAxisRange(Qt::Vertical, yMin, yMax);

  // Set the data for both series with theme-aware colors
  if (!whitePoints_.isEmpty()) {
    // Sort points by X-coordinate (frame number) to ensure proper line drawing
    std::sort(whitePoints_.begin(), whitePoints_.end(),
              [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });

    QColor whiteColor =
        orc::view::PlotWidget::isDarkTheme() ? Qt::green : Qt::darkGreen;
    whiteSNRSeries_->setPen(QPen(whiteColor, 2));
    whiteSNRSeries_->setData(whitePoints_);
  }

  if (!blackPoints_.isEmpty()) {
    // Sort points by X-coordinate (frame number) to ensure proper line drawing
    std::sort(blackPoints_.begin(), blackPoints_.end(),
              [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });

    QColor blackColor =
        orc::view::PlotWidget::isDarkTheme() ? Qt::cyan : Qt::darkBlue;
    blackPSNRSeries_->setPen(QPen(blackColor, 2));
    blackPSNRSeries_->setData(blackPoints_);
  }

  // Update series visibility based on current mode
  updateSeriesVisibility();

  // Set the frame marker position
  plotMarker_->setPosition(
      QPointF(static_cast<double>(currentFrameNumber), (yMax + yMin) / 2));

  // Report whether this is a per-frame or a decimated (bucketed) view.
  setDecimationSummary(decimated_, numberOfFrames_,
                       static_cast<int>(bucketLabel_.size()));

  // Render the plot
  plot_->replot();
}

void SNRAnalysisDialog::updateFrameMarker(int32_t currentFrameNumber) {
  // Use base class throttling implementation
  updateFrameMarkerThrottled(currentFrameNumber);
}

void SNRAnalysisDialog::showNoDataMessage(const QString& reason) {
  removeChartContents();

  // Use base class implementation
  showNoDataMessageImpl(reason, plot_);
}

SNRAnalysisDialog::DisplayMode SNRAnalysisDialog::getCurrentMode() const {
  return displayModeCombo_->currentData().value<DisplayMode>();
}

void SNRAnalysisDialog::onDisplayModeChanged(int index) {
  Q_UNUSED(index);

  // Update series visibility
  updateSeriesVisibility();

  // Emit mode changed signal
  emit modeChanged(getCurrentMode());
}

void SNRAnalysisDialog::updateSeriesVisibility() {
  auto mode = getCurrentMode();

  switch (mode) {
    case DisplayMode::White:
      whiteSNRSeries_->setVisible(true);
      blackPSNRSeries_->setVisible(false);
      break;

    case DisplayMode::Black:
      whiteSNRSeries_->setVisible(false);
      blackPSNRSeries_->setVisible(true);
      break;

    case DisplayMode::Both:
      whiteSNRSeries_->setVisible(true);
      blackPSNRSeries_->setVisible(true);
      break;
  }

  plot_->replot();
}

void SNRAnalysisDialog::calculateMarkerPosition(int32_t frameNumber) {
  // Calculate the Y position for the marker (middle of the visible range)
  double maxY = std::max(maxWhiteY_, maxBlackY_);
  double yMax = (maxY < 10) ? 10 : ceil(maxY + 5);
  double yMin = 0;

  if (maxY > 0) {
    double minWhite = whitePoints_.isEmpty() ? yMax : whitePoints_[0].y();
    double minBlack = blackPoints_.isEmpty() ? yMax : blackPoints_[0].y();

    for (const auto& pt : whitePoints_) {
      if (pt.y() < minWhite) minWhite = pt.y();
    }
    for (const auto& pt : blackPoints_) {
      if (pt.y() < minBlack) minBlack = pt.y();
    }

    double minY = std::min(minWhite, minBlack);
    yMin = floor(minY - 5);
    if (yMin < 0) yMin = 0;
  }

  plotMarker_->setPosition(
      QPointF(static_cast<double>(frameNumber), (yMax + yMin) / 2));
  // No need to call plot->replot() - marker update() handles the redraw
}

void SNRAnalysisDialog::onPlotAreaChanged() {
  // Handle plot area changes if needed
  // The PlotWidget handles zoom/pan internally
}

void SNRAnalysisDialog::onPlotClicked(const QPointF& dataPoint) {
  if (bucketLabel_.isEmpty()) return;

  // Find the bucket nearest the click on the x (frame) axis.
  int nearest = 0;
  double bestDist = std::numeric_limits<double>::max();
  for (int i = 0; i < bucketLabel_.size(); ++i) {
    double dist =
        std::abs(static_cast<double>(bucketLabel_[i]) - dataPoint.x());
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
  auto fmt = [](double v) {
    return std::isnan(v) ? QString("n/a") : QString::number(v, 'f', 2) + " dB";
  };
  setBucketReadout(QString("%1 — white SNR: %2, black PSNR: %3")
                       .arg(range)
                       .arg(fmt(bucketWhite_.value(nearest)))
                       .arg(fmt(bucketBlack_.value(nearest))));
}
