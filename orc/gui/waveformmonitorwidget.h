/*
 * File:        waveformmonitorwidget.h
 * Module:      orc-gui
 * Purpose:     Waveform monitor raster widget
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef WAVEFORMMONITORWIDGET_H
#define WAVEFORMMONITORWIDGET_H

#include <amplitude_conversion.h>
#include <orc/stage/common_types.h>

#include <QImage>
#include <QWidget>
#include <cstdint>
#include <optional>
#include <vector>

#include "presenters/include/hints_view_models.h"
#include "waveform_count_grid.h"

/**
 * @brief Waveform monitor widget — sample-luminance histogram across all lines
 *
 * Accumulates a 2D count buffer: for every active video line in the frame,
 * for every sample position x, it increments count[x][mv_bin].  The buffer
 * is rendered as a QImage using area-max mapping followed by a Gaussian blur
 * (phosphor-glow effect), coloured with the theme's CompositePrimary token.
 *
 * Five normative level markers (sync tip, blanking, black, white, peak) are
 * drawn on top of the raster image, matching the Frame-scope style exactly.
 * X-axis carries sample-position tick labels; Y-axis carries mV labels.
 *
 * The gain control affects only the render pass — no re-accumulation needed.
 */
class WaveformMonitorWidget : public QWidget {
  Q_OBJECT

 public:
  explicit WaveformMonitorWidget(QWidget* parent = nullptr);

  /**
   * @brief Load frame data and rebuild the accumulation buffer.
   *
   * @param composite_samples Flat concatenation of all field samples
   * @param first_field_height  Lines in the first field
   * @param second_field_height Lines in the second field (0 = single field)
   * @param video_params        Signal levels and active video range
   */
  void setData(
      const std::vector<int16_t>& composite_samples, int first_field_height,
      int second_field_height,
      const std::optional<orc::presenters::VideoParametersView>& video_params);

  void setGain(double gain);
  double gain() const { return gain_; }

  void setPhosphorMode(bool enabled);
  bool phosphorMode() const { return phosphor_mode_; }

  // Constrain the Y-axis to the legal luma range when displaying a Y-only
  // channel.  Must be called before setData() to take effect on the current
  // frame.
  void setYOnlyMode(bool y_only);

  void setAmplitudeUnit(orc::AmplitudeDisplayUnit unit);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  void accumulate(const std::vector<int16_t>& samples, int total_lines,
                  int active_start, int active_end, int32_t blanking_level,
                  int32_t white_level, orc::VideoSystem sys);
  void rebuildImage(const QRect& plot_area);
  void drawGrid(QPainter& painter, const QRect& plot_area) const;
  void drawYAxis(QPainter& painter, const QRect& plot_area) const;
  void drawXAxis(QPainter& painter, const QRect& plot_area) const;
  void drawLevelMarkers(QPainter& painter, const QRect& plot_area) const;
  int mvToPixelY(double mv, const QRect& plot_area) const;
  QRect plotArea() const;

  // Returns the appropriate background, trace, axis, and grid colors
  // depending on whether phosphor mode is active.
  QColor displayBackground() const;
  QColor displayTrace() const;
  QColor displayAxis() const;
  QColor displayGrid() const;

  // Hit counts behind the trace, retained across frames (see
  // WaveformCountGrid); x_samples_ mirrors its column count for the axis maths.
  orc::gui::WaveformCountGrid count_grid_;
  int x_samples_ = 0;
  int active_video_start_ = 0;
  double us_per_sample_ = 1000000.0 / 14318181.8;  // default: NTSC 4FSC
  int y_bins_ = 0;
  double y_min_mv_ = -350.0;
  double y_max_mv_ = 950.0;
  static constexpr double kBinWidthMv = 1.0;
  int line_count_ = 0;

  double gain_ = 1.0;
  bool phosphor_mode_ = false;
  bool y_only_mode_ = false;
  orc::AmplitudeDisplayUnit amplitude_unit_ = orc::AmplitudeDisplayUnit::IRE;
  bool image_dirty_ = true;
  QImage cached_image_;

  std::optional<orc::presenters::VideoParametersView> video_params_;

  // Additive brightness floor applied to every non-zero count before dividing
  // by 255. Lower values extend the low-intensity gradient range; the
  // VirtualDub Color Tools reference used 128 (~52 % minimum), but 64 gives a
  // more usable gradient (~27 % minimum).
  static constexpr float kBrightnessBias = 64.0f;

  static constexpr int kLeftMargin = 55;
  static constexpr int kRightMargin = 10;
  static constexpr int kTopMargin = 15;
  static constexpr int kBottomMargin = 50;
};

#endif  // WAVEFORMMONITORWIDGET_H
