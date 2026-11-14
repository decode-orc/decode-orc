/*
 * File:        snranalysisdialog.h
 * Module:      orc-view_core
 * Purpose:     SNR analysis dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef SNRANALYSISDIALOG_H
#define SNRANALYSISDIALOG_H

#include <orc/view-support/plot_widget.h>

#include <QComboBox>
#include <QPointF>
#include <QVBoxLayout>
#include <QVector>

#include "analysisdialogbase.h"

/**
 * @brief Dialog for displaying SNR analysis graphs
 *
 * This dialog shows graphs of SNR (Signal-to-Noise Ratio) across all fields
 * in the source, with options to view:
 * - White SNR only
 * - Black PSNR only
 * - Both white SNR and black PSNR
 *
 * Data and business logic is handled by the WhiteSNRObserver and
 * BlackPSNRObserver in orc-core. This GUI component only handles rendering the
 * graphs.
 */
class SNRAnalysisDialog : public AnalysisDialogBase {
  Q_OBJECT

 public:
  /// Which series the graph shows. Display only: both are always measured.
  enum class DisplayMode { White, Black, Both };

  explicit SNRAnalysisDialog(QWidget* parent = nullptr);
  ~SNRAnalysisDialog();

  /**
   * @brief Start a new update cycle
   * @param numberOfFrames Total number of frames in the source
   * @param decimated True when each plotted point aggregates >1 analysed frame
   */
  void startUpdate(int32_t numberOfFrames, bool decimated);

  /**
   * @brief Add a data point (display bucket) to the graphs
   * @param frameNumber Representative frame number for the bucket (1-based)
   * @param whiteSNR White SNR value (dB), or NaN if not available
   * @param blackPSNR Black PSNR value (dB), or NaN if not available
   * @param frameStart First analysed frame in the bucket
   * @param frameEnd Last analysed frame in the bucket
   */
  void addDataPoint(int32_t frameNumber, double whiteSNR, double blackPSNR,
                    int32_t frameStart, int32_t frameEnd);

  /**
   * @brief Finish the update and render the graphs
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

  /**
   * @brief Get the series the graph shows
   */
  DisplayMode getCurrentMode() const;

 signals:
  /**
   * @brief Emitted when the user changes the series shown
   * @param mode The series now shown
   */
  void modeChanged(DisplayMode mode);

 protected:
  /**
   * @brief Calculate and set marker position (implements base class pure
   * virtual)
   */
  void calculateMarkerPosition(int32_t frameNumber) override;

 private slots:
  void onDisplayModeChanged(int index);
  void onPlotAreaChanged();
  void onPlotClicked(const QPointF& dataPoint);

 private:
  void removeChartContents();
  void updateSeriesVisibility();

  orc::view::PlotWidget* plot_;
  orc::view::PlotSeries* whiteSNRSeries_;
  orc::view::PlotSeries* blackPSNRSeries_;
  orc::view::PlotMarker* plotMarker_;
  QComboBox* displayModeCombo_;

  double maxWhiteY_;
  double maxBlackY_;
  int32_t numberOfFrames_;
  bool decimated_ = false;
  QVector<QPointF> whitePoints_;
  QVector<QPointF> blackPoints_;
  // Per-bucket detail (one entry per addDataPoint call) for the click readout.
  QVector<int32_t> bucketLabel_;
  QVector<int32_t> bucketStart_;
  QVector<int32_t> bucketEnd_;
  QVector<double> bucketWhite_;
  QVector<double> bucketBlack_;
};

#endif  // SNRANALYSISDIALOG_H
