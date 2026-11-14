/*
 * File:        dropoutanalysisdialog.h
 * Module:      orc-view_core
 * Purpose:     Dropout analysis dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef DROPOUTANALYSISDIALOG_H
#define DROPOUTANALYSISDIALOG_H

#include <orc/view-support/plot_widget.h>

#include <QCheckBox>
#include <QPointF>
#include <QVBoxLayout>
#include <QVector>

#include "analysisdialogbase.h"

/**
 * @brief Dialog for displaying dropout analysis graphs
 *
 * This dialog shows a graph of dropout length across all fields in the source,
 * with options to view either:
 * - Full field dropout data
 * - Visible area only dropout data
 *
 * Data and business logic is handled by the DropoutAnalysisObserver in
 * orc-core. This GUI component only handles rendering the graph.
 */
class DropoutAnalysisDialog : public AnalysisDialogBase {
  Q_OBJECT

 public:
  explicit DropoutAnalysisDialog(QWidget* parent = nullptr);
  ~DropoutAnalysisDialog();

  /**
   * @brief Start a new update cycle
   * @param numberOfFrames Total number of frames in the source
   * @param decimated True when each plotted point aggregates >1 analysed frame
   */
  void startUpdate(int32_t numberOfFrames, bool decimated);

  /**
   * @brief Add a data point (display bucket) to the graph
   * @param frameNumber Representative frame number for the bucket (1-based)
   * @param dropoutLength Total dropout length in samples (summed over the
   * bucket)
   * @param frameStart First analysed frame in the bucket
   * @param frameEnd Last analysed frame in the bucket
   * @param dropoutCount Number of dropout runs (summed over the bucket)
   */
  void addDataPoint(int32_t frameNumber, double dropoutLength,
                    int32_t frameStart, int32_t frameEnd, int32_t dropoutCount);

  /**
   * @brief Finish the update and render the graph
   * @param currentFrameNumber Current frame being viewed
   */
  void finishUpdate(int32_t currentFrameNumber);

  /**
   * @brief Update the frame marker position
   * @param currentFrameNumber Current frame being viewed
   */
  void updateFrameMarker(int32_t currentFrameNumber);

  /**
   * @brief Show "No data available" message
   * @param reason Optional explanation for why no data is available
   */
  void showNoDataMessage(const QString& reason = QString());

 protected:
  /**
   * @brief Calculate and set marker position (implements base class pure
   * virtual)
   */
  void calculateMarkerPosition(int32_t frameNumber) override;

 private slots:
  void onPlotAreaChanged();
  void onPlotClicked(const QPointF& dataPoint);

 private:
  void removeChartContents();

  orc::view::PlotWidget* plot_;
  orc::view::PlotSeries* series_;
  orc::view::PlotMarker* plotMarker_;

  double maxY_;
  int32_t numberOfFrames_;
  bool decimated_ = false;
  QVector<QPointF> points_;
  // Per-bucket detail, parallel to points_, for the click readout.
  QVector<int32_t> bucketStart_;
  QVector<int32_t> bucketEnd_;
  QVector<int32_t> bucketCount_;
};

#endif  // DROPOUTANALYSISDIALOG_H
