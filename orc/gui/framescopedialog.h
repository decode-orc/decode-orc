/*
 * File:        framescopedialog.h
 * Module:      orc-gui
 * Purpose:     Frame scope dialog for viewing CVBS_U10_4FSC line samples
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef FRAMESCOPEDIALOG_H
#define FRAMESCOPEDIALOG_H

#include <amplitude_conversion.h>
#include <orc/stage/common_types.h>
#include <orc/view-support/plot_widget.h>

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPointF>
#include <QPushButton>
#include <QVector>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "presenters/include/hints_view_models.h"

/**
 * @brief Dialog for displaying frame scope — all samples in a selected line
 *
 * Displays CVBS_U10_4FSC int16_t samples converted to millivolts using the
 * spec-defined blanking and white levels from SourceParameters.  The Y-axis
 * spans [sync_tip_mv, peak_mv] by default and auto-expands when samples fall
 * outside that range (design §3.5).  Five normative level markers are drawn
 * at sync_tip, blanking (0 mV), black, white, and peak.
 *
 * Navigation is by FrameID + 0-based frame-flat line index.  The line
 * numbering display mode (frame-flat, sequential, field-relative, broadcast
 * interlaced) is user-selectable and stored as a per-system QSettings
 * preference shared with FrameTimingDialog.
 */
class FrameScopeDialog : public QDialog {
  Q_OBJECT

 public:
  explicit FrameScopeDialog(QWidget* parent = nullptr);
  ~FrameScopeDialog();

  /**
   * @brief Display samples for a frame line in CVBS_U10_4FSC domain
   *
   * @param node_id        Stage node identifier (for window title)
   * @param stage_index    1-based stage index (for window title)
   * @param frame_id       0-based frame index being displayed
   * @param frame_line     0-based frame-flat line index
   * @param sample_x       Sample position of the click marker
   * @param samples        int16_t composite samples for the line
   * @param video_params   Signal levels and geometry from SourceParameters
   * @param preview_image_width  Pixel width of the preview image
   * @param original_sample_x   Preview-space X coordinate (for cross-hair sync)
   * @param original_image_y    Preview-space Y coordinate (for refresh)
   * @param y_samples      Optional luma samples (YC sources)
   * @param c_samples      Optional chroma samples (YC sources)
   */
  void setFrameLineSamples(
      const QString& node_id, int stage_index, uint64_t frame_id,
      size_t frame_line, int sample_x, const std::vector<int16_t>& samples,
      const std::optional<orc::presenters::VideoParametersView>& video_params,
      int preview_image_width, int original_sample_x, int original_image_y,
      const std::vector<int16_t>& y_samples = {},
      const std::vector<int16_t>& c_samples = {});

  /**
   * @brief Emit refreshRequested using the stored preview-space coordinates.
   * Called by callers when the frame changes but the scope should stay at the
   * same visual position.
   */
  void refreshSamples();

  /**
   * @brief Re-emit refreshRequested at the current position.
   * Called when the preview frame advances so the scope gets fresh samples.
   */
  void refreshSamplesAtCurrentPosition();

  /**
   * @brief Set the amplitude display unit and re-render the current plot.
   */
  void setAmplitudeUnit(orc::AmplitudeDisplayUnit unit);

 Q_SIGNALS:
  /// Emitted when the user navigates up/down by one line.
  void lineNavigationRequested(int direction, uint64_t current_frame_id,
                               size_t current_frame_line, int sample_x,
                               int preview_image_width);
  /// Emitted when the sample marker is dragged on the plot (field-space coord).
  void sampleMarkerMoved(int sample_x);
  /// Emitted when a full refresh is needed (frame change).
  void refreshRequested(int image_x, int image_y);
  /// Emitted when the dialog is explicitly closed (not on transient WM hides).
  void dialogClosed();

 private slots:
  void onLineUp();
  void onLineDown();
  void onPlotClicked(const QPointF& dataPoint);
  void onChannelSelectionChanged(int index);
  void onNumberingModeChanged(int index);

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  void setupUI();
  void updateSampleMarker(int sample_x);
  void updatePlotData();
  void updateOriginalSampleXFromSampleIndex(int sample_index);
  QString formatLineLabel() const;  // Formats current_frame_line_ per mode
  void saveNumberingModePreference() const;
  void loadNumberingModePreference();

  orc::view::PlotWidget* plot_widget_;
  orc::view::PlotSeries* line_series_;
  orc::view::PlotSeries* y_series_;
  orc::view::PlotSeries* c_series_;
  QPushButton* line_up_button_;
  QPushButton* line_down_button_;
  QLabel* sample_info_label_;
  QLabel* channel_selector_label_;
  QComboBox* channel_selector_;
  QLabel* numbering_mode_label_;
  QComboBox* numbering_mode_combo_;

  // Navigation state (internal 0-based frame-flat coordinates)
  QString current_node_id_;
  int current_stage_index_ = 1;
  uint64_t current_frame_id_ = 0;
  size_t current_frame_line_ = 0;
  int current_sample_x_ = 0;
  int original_sample_x_ = 0;
  int original_image_y_ = 0;
  int preview_image_width_ = 0;

  // Sample storage (CVBS_U10_4FSC int16_t domain)
  std::vector<int16_t> current_samples_;
  std::vector<int16_t> current_y_samples_;
  std::vector<int16_t> current_c_samples_;
  std::optional<orc::presenters::VideoParametersView> current_video_params_;

  orc::view::PlotMarker* sample_marker_ = nullptr;
  bool is_yc_source_ = false;
  orc::AmplitudeDisplayUnit amplitude_unit_ = orc::AmplitudeDisplayUnit::IRE;
};

#endif  // FRAMESCOPEDIALOG_H
