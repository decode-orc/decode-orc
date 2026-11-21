/*
 * File:        analysisdialogbase.h
 * Module:      orc-view_core
 * Purpose:     Base class for analysis dialogs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ANALYSISDIALOGBASE_H
#define ANALYSISDIALOGBASE_H

#include <orc/view-support/plot_widget.h>

#include <QDialog>
#include <QLabel>
#include <QShowEvent>
#include <QStackedLayout>
#include <QTimer>
#include <QVBoxLayout>

/**
 * @brief Base class for analysis dialogs with common update throttling and UI
 * patterns
 *
 * This base class provides:
 * - Update throttling timer (~60fps max update rate)
 * - "No data available" overlay pattern
 * - Frame marker update handling
 * - Show event handling for deferred updates
 */
class AnalysisDialogBase : public QDialog {
  Q_OBJECT

 signals:
  /**
   * @brief The user clicked the graph at a frame
   * @param frameNumber The frame clicked (1-based), within the clicked bucket
   */
  void frameActivated(int32_t frameNumber);

 protected:
  explicit AnalysisDialogBase(QWidget* parent = nullptr);
  virtual ~AnalysisDialogBase() = default;

  /**
   * @brief Set up the update throttling timer
   * Should be called in derived class constructor after this base constructor
   */
  void setupUpdateTimer();

  /**
   * @brief Set up the "No data available" overlay
   * @param mainLayout The main layout to add the plot container to
   * @param plot The plot widget to overlay the label on
   */
  void setupNoDataOverlay(QVBoxLayout* mainLayout, orc::view::PlotWidget* plot);

  /**
   * @brief Set up the bucket-info status line shown below the plot
   *
   * The label reports whether the graph is a per-frame or a decimated
   * (bucketed) view and, after a click, the frame range and aggregate values of
   * the selected bucket. Call from a derived constructor after the plot has
   * been added to @p mainLayout.
   *
   * @param mainLayout The main layout to append the status line to
   */
  void setupBucketInfoLabel(QVBoxLayout* mainLayout);

  /**
   * @brief Set the decimation summary shown when a new dataset is rendered
   *
   * When @p decimated is true the graph aggregates multiple analysed frames per
   * point; the summary states the average bucket width so the interactive view
   * is not mistaken for per-frame data. When false the view is per-frame.
   *
   * @param decimated True when points aggregate more than one analysed frame
   * @param totalFrames Total analysed frames represented by the series
   * @param pointCount Number of display points (buckets) plotted
   */
  void setDecimationSummary(bool decimated, int32_t totalFrames,
                            int pointCount);

  /**
   * @brief Replace the bucket-info line with a per-bucket readout
   * @param text Readout describing the selected bucket
   */
  void setBucketReadout(const QString& text);

  /**
   * @brief Emit frameActivated for a click at @p clickX on a bucket
   *
   * The frame is the one under the click, kept within the bucket's range
   * [@p frameStart, @p frameEnd] so a click on a wide bar never lands outside
   * it.
   */
  void activateFrameInBucket(double clickX, int32_t frameStart,
                             int32_t frameEnd);

  /**
   * @brief Update the frame marker position (with throttling)
   * @param currentFrameNumber Current frame being viewed
   */
  void updateFrameMarkerThrottled(int32_t currentFrameNumber);

  /**
   * @brief Show "No data available" message
   * @param reason Optional explanation for why no data is available
   * @param plot The plot widget to hide
   */
  void showNoDataMessageImpl(const QString& reason,
                             orc::view::PlotWidget* plot);

  /**
   * @brief Handle show event - triggers deferred updates
   */
  void showEvent(QShowEvent* event) override;

  /**
   * @brief Pure virtual method for derived classes to implement marker position
   * calculation Called by onUpdateTimerTimeout when update is ready
   */
  virtual void calculateMarkerPosition(int32_t frameNumber) = 0;

 protected slots:
  /**
   * @brief Timer timeout handler - calls calculateMarkerPosition
   */
  void onUpdateTimerTimeout();

 protected:
  // Common UI elements
  QLabel* noDataLabel_;
  QLabel* bucketInfoLabel_ =
      nullptr;                 ///< View-mode / bucket-readout status line
  QString decimationSummary_;  ///< Cached summary restored when readout clears

  // Update throttling state
  QTimer* updateTimer_;
  int32_t pendingFrameNumber_;
  bool hasPendingUpdate_;
};

#endif  // ANALYSISDIALOGBASE_H
