/*
 * File:        burstlevelanalysisdialog.h
 * Module:      orc-view_core
 * Purpose:     Burst level analysis dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef BURSTLEVELANALYSISDIALOG_H
#define BURSTLEVELANALYSISDIALOG_H

#include <orc/view-support/amplitude_units.h>
#include <orc/view-support/plot_widget.h>

#include <QPointF>
#include <QVBoxLayout>
#include <QVector>

#include "analysisdialogbase.h"

/**
 * @brief Dialog for displaying burst level analysis graphs
 *
 * This dialog shows graphs of color burst median IRE levels across all fields
 * in the source. This is useful for tracking signal strength variations and
 * detecting tape/capture issues.
 *
 * Data and business logic is handled by the BurstLevelObserver and
 * BurstLevelAnalysisDecoder in orc-core. This GUI component only handles
 * rendering the graphs.
 */
class BurstLevelAnalysisDialog : public AnalysisDialogBase {
  Q_OBJECT

 public:
  explicit BurstLevelAnalysisDialog(QWidget* parent = nullptr);
  ~BurstLevelAnalysisDialog();

  /**
   * @brief Start a new update cycle
   * @param numberOfFrames Total number of frames in the source
   * @param decimated True when each plotted point aggregates >1 analysed frame
   */
  void startUpdate(int32_t numberOfFrames, bool decimated);

  /**
   * @brief Set the analysed source's levels, used to convert burst amplitudes
   * to the display unit
   *
   * Without levels (blanking < 0 or white <= blanking) the nominal PAL levels
   * are assumed.
   */
  void setVideoLevels(orc::view::VideoSystem system, int32_t blankingLevel,
                      int32_t whiteLevel);

  /**
   * @brief Add a data point (display bucket) to the graph
   * @param frameNumber Representative frame number for the bucket (1-based)
   * @param burstLevel10bit Burst level in 10-bit sample domain
   * @param frameStart First analysed frame in the bucket
   * @param frameEnd Last analysed frame in the bucket
   */
  void addDataPoint(int32_t frameNumber, double burstLevel10bit,
                    int32_t frameStart, int32_t frameEnd);

  /**
   * @brief Update the amplitude display unit and replot
   */
  void setAmplitudeUnit(orc::view::AmplitudeUnit unit);

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
  orc::view::PlotSeries* burstSeries_;
  orc::view::PlotMarker* plotMarker_;

  double maxY_;
  double minY_;
  int32_t numberOfFrames_;
  int32_t current_frame_number_{1};
  bool decimated_ = false;
  QVector<QPointF> burstPoints_;
  // Per-bucket detail parallel to burstPoints_, for the click readout.
  QVector<int32_t> bucketStart_;
  QVector<int32_t> bucketEnd_;
  orc::view::AmplitudeUnit amplitude_unit_ = orc::view::AmplitudeUnit::IRE;
  orc::view::VideoSystem video_system_ = orc::view::VideoSystem::Unknown;
  int32_t blanking_level_ = -1;
  int32_t white_level_ = -1;
  double display_y_min_{0.0};
  double display_y_max_{40.0};
  // Conversion inputs resolved during finishUpdate(), reused by the readout.
  int32_t conv_blanking_{256};
  int32_t conv_white_{844};
  orc::view::VideoSystem conv_sys_{orc::view::VideoSystem::PAL};
};

#endif  // BURSTLEVELANALYSISDIALOG_H
