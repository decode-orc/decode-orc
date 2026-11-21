/*
 * File:        frametimingwidget.cpp
 * Module:      orc-gui
 * Purpose:     Widget for rendering frame timing graphs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frametimingwidget.h"

#include <orc/view-support/plot_widget.h>
#include <orc/view-support/theme_color_tokens.h>

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

static orc::VideoSystem toOrcVideoSystem(orc::presenters::VideoSystem sys) {
  switch (sys) {
    case orc::presenters::VideoSystem::PAL:
      return orc::VideoSystem::PAL;
    case orc::presenters::VideoSystem::NTSC:
      return orc::VideoSystem::NTSC;
    case orc::presenters::VideoSystem::PAL_M:
      return orc::VideoSystem::PAL_M;
    default:
      return orc::VideoSystem::Unknown;
  }
}

namespace {
std::vector<int16_t> buildCombinedYPlusC(const std::vector<int16_t>& y_samples,
                                         const std::vector<int16_t>& c_samples,
                                         int32_t chroma_dc_offset) {
  if (y_samples.empty() || c_samples.empty()) {
    return {};
  }

  const size_t sample_count = std::min(y_samples.size(), c_samples.size());
  std::vector<int16_t> combined_samples;
  combined_samples.reserve(sample_count);

  for (size_t i = 0; i < sample_count; ++i) {
    const int32_t combined = static_cast<int32_t>(y_samples[i]) +
                             static_cast<int32_t>(c_samples[i]) -
                             chroma_dc_offset;
    combined_samples.push_back(static_cast<int16_t>(
        std::clamp(combined, static_cast<int32_t>(INT16_MIN),
                   static_cast<int32_t>(INT16_MAX))));
  }

  return combined_samples;
}
}  // namespace

double FrameTimingWidget::convertSampleToMV(int16_t sample) const {
  if (!video_params_.has_value()) {
    return sample / 100.0;
  }
  const auto& vp = video_params_.value();
  return orc::samples10_to_mv(sample, vp.blanking_level, vp.white_level,
                              toOrcVideoSystem(vp.system));
}

double FrameTimingWidget::getMVRange(double& min_mv, double& max_mv) const {
  if (!video_params_.has_value()) {
    min_mv = -400;
    max_mv = 1000;
    return max_mv - min_mv;
  }
  const auto& vp = video_params_.value();
  const orc::VideoSystem sys = toOrcVideoSystem(vp.system);
  if (vp.blanking_level >= 0 && vp.white_level > vp.blanking_level) {
    min_mv = orc::samples10_to_mv(vp.sync_tip_level, vp.blanking_level,
                                  vp.white_level, sys);
    max_mv = orc::samples10_to_mv(vp.peak_level, vp.blanking_level,
                                  vp.white_level, sys);
  } else {
    min_mv = -400;
    max_mv = 1000;
  }
  return max_mv - min_mv;
}

FrameTimingWidget::FrameTimingWidget(QWidget* parent)
    : QWidget(parent),
      scroll_offset_(0),
      zoom_factor_(1.0),
      is_dragging_(false),
      drag_start_scroll_value_(0) {
  // Create scroll bar
  scroll_bar_ = new QScrollBar(Qt::Horizontal, this);
  connect(scroll_bar_, &QScrollBar::valueChanged, [this](int value) {
    scroll_offset_ = value;
    update();
  });

  // Set minimum size
  setMinimumSize(600, 400);

  // Enable mouse tracking for interactive features
  setMouseTracking(true);
}

void FrameTimingWidget::setChannelMode(ChannelMode mode) {
  if (channel_mode_ == mode) {
    return;
  }
  channel_mode_ = mode;
  updateScrollBar();
  update();
}

void FrameTimingWidget::setFieldData(
    const std::vector<int16_t>& samples, const std::vector<int16_t>& samples_2,
    const std::vector<int16_t>& y_samples,
    const std::vector<int16_t>& c_samples,
    const std::vector<int16_t>& y_samples_2,
    const std::vector<int16_t>& c_samples_2,
    const std::optional<orc::presenters::VideoParametersView>& video_params,
    const std::optional<int>& marker_sample) {
  field1_samples_ = samples;
  field2_samples_ = samples_2;
  y1_samples_ = y_samples;
  c1_samples_ = c_samples;
  y2_samples_ = y_samples_2;
  c2_samples_ = c_samples_2;
  video_params_ = video_params;
  marker_sample_ = marker_sample;

  updateScrollBar();
  update();
}

void FrameTimingWidget::scrollToMarker() {
  if (!marker_sample_.has_value()) {
    return;
  }

  int marker_pos = marker_sample_.value();

  // Calculate the visible width in samples
  int visible_width =
      width() - 2 * MARGIN - 50;  // Account for left margin for labels
  double base_pixels_per_sample = getBasePixelsPerSample();
  double effective_pixels_per_sample = base_pixels_per_sample * zoom_factor_;
  int samples_per_view =
      static_cast<int>(visible_width / effective_pixels_per_sample);

  // Center the marker in the view
  int target_offset = marker_pos - (samples_per_view / 2);
  target_offset = std::max(0, target_offset);

  // Set scroll position
  if (scroll_bar_->isEnabled()) {
    scroll_bar_->setValue(target_offset);
  }
}

void FrameTimingWidget::scrollToLine(int line_number) {
  if (!video_params_.has_value() || video_params_->frame_width_nominal <= 0) {
    return;
  }

  // Convert line number (1-based) to sample position
  // Line 1 starts at sample 0
  int line_start_sample =
      (line_number - 1) * video_params_->frame_width_nominal;

  // Calculate the visible width in samples
  int visible_width =
      width() - 2 * MARGIN - 50;  // Account for left margin for labels
  double base_pixels_per_sample = getBasePixelsPerSample();
  double effective_pixels_per_sample = base_pixels_per_sample * zoom_factor_;
  int samples_per_view =
      static_cast<int>(visible_width / effective_pixels_per_sample);

  // Center the line in the view
  int target_offset = line_start_sample - (samples_per_view / 2);
  target_offset = std::max(0, target_offset);

  // Set scroll position
  if (scroll_bar_->isEnabled()) {
    scroll_bar_->setValue(target_offset);
  }
}

int FrameTimingWidget::getCenterSample() const {
  // Check if we have data
  size_t total_samples = std::max(
      {field1_samples_.size(), field2_samples_.size(), y1_samples_.size(),
       y2_samples_.size(), c1_samples_.size(), c2_samples_.size()});

  if (total_samples == 0) {
    return -1;
  }

  // Calculate the visible width in samples
  int visible_width = width() - 2 * MARGIN - 50;
  double base_pixels_per_sample = getBasePixelsPerSample();
  double effective_pixels_per_sample = base_pixels_per_sample * zoom_factor_;
  int samples_per_view =
      static_cast<int>(visible_width / effective_pixels_per_sample);

  // Get center of current view
  int center_sample = scroll_offset_ + (samples_per_view / 2);

  // Clamp to valid range
  if (center_sample < 0) center_sample = 0;
  if (center_sample >= static_cast<int>(total_samples)) {
    center_sample = static_cast<int>(total_samples) - 1;
  }

  return center_sample;
}

void FrameTimingWidget::setZoomFactor(double zoom_factor) {
  // Get the center sample before zoom change
  int center_sample = getCenterSample();

  zoom_factor_ = std::max(0.01, zoom_factor);  // Clamp to reasonable minimum
  updateScrollBar();

  // Recalculate scroll position to keep the center sample centered
  if (center_sample >= 0) {
    // Calculate new samples per view with updated zoom
    int visible_width = width() - 2 * MARGIN - 50;
    double base_pixels_per_sample = getBasePixelsPerSample();
    double effective_pixels_per_sample = base_pixels_per_sample * zoom_factor_;
    int samples_per_view =
        static_cast<int>(visible_width / effective_pixels_per_sample);

    // Calculate new scroll offset to keep center_sample in the center
    int new_scroll_offset = center_sample - (samples_per_view / 2);

    // Clamp to valid scroll range
    new_scroll_offset = std::max(0, new_scroll_offset);
    new_scroll_offset = std::min(new_scroll_offset, scroll_bar_->maximum());

    // Update scroll position
    scroll_bar_->setValue(new_scroll_offset);
  }

  update();
}

void FrameTimingWidget::setDraftRenderMode(bool enabled) {
  if (draft_render_mode_ == enabled) {
    return;
  }
  draft_render_mode_ = enabled;
  update();
}

double FrameTimingWidget::getBasePixelsPerSample() const {
  // Calculate pixels per sample needed to fit ALL samples horizontally at
  // zoom_factor = 1.0 This means at zoom 1.0, we show all available lines
  if (!video_params_.has_value() || video_params_->frame_width_nominal <= 0) {
    return PIXELS_PER_SAMPLE;  // Fallback to default
  }

  int visible_width =
      width() - 2 * MARGIN - 50;  // Account for margins and labels
  if (visible_width <= 0) {
    return PIXELS_PER_SAMPLE;
  }

  // Determine total samples (all lines in the field(s))
  size_t total_samples = std::max(
      {field1_samples_.size(), field2_samples_.size(), y1_samples_.size(),
       y2_samples_.size(), c1_samples_.size(), c2_samples_.size()});

  if (total_samples == 0) {
    return PIXELS_PER_SAMPLE;
  }

  // At zoom_factor = 1.0, all samples should fit in the visible width
  double base_pixels_per_sample =
      static_cast<double>(visible_width) / static_cast<double>(total_samples);

  return base_pixels_per_sample;
}

void FrameTimingWidget::updateScrollBar() {
  // Determine total sample count
  size_t total_samples = std::max(
      {field1_samples_.size(), field2_samples_.size(), y1_samples_.size(),
       y2_samples_.size(), c1_samples_.size(), c2_samples_.size()});

  if (total_samples == 0) {
    scroll_bar_->setRange(0, 0);
    scroll_bar_->setEnabled(false);
    return;
  }

  // Calculate visible width (excluding margins)
  int visible_width =
      width() - 2 * MARGIN - 50;  // Account for left margin for labels

  // Get base pixels per sample (to fit all samples at 100% zoom)
  double base_pixels_per_sample = getBasePixelsPerSample();
  double effective_pixels_per_sample = base_pixels_per_sample * zoom_factor_;
  int samples_per_view =
      static_cast<int>(visible_width / effective_pixels_per_sample);

  if (total_samples <= static_cast<size_t>(samples_per_view)) {
    // All samples fit in view
    scroll_bar_->setRange(0, 0);
    scroll_bar_->setEnabled(false);
  } else {
    // Need scrolling
    int max_offset = static_cast<int>(total_samples) - samples_per_view;
    scroll_bar_->setRange(0, max_offset);
    scroll_bar_->setPageStep(samples_per_view);
    scroll_bar_->setSingleStep(std::max(1, samples_per_view / 10));
    scroll_bar_->setEnabled(true);
  }
}

void FrameTimingWidget::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);

  // Position scroll bar at bottom
  int sb_height = scroll_bar_->sizeHint().height();
  scroll_bar_->setGeometry(MARGIN, height() - sb_height - 5,
                           width() - 2 * MARGIN, sb_height);

  updateScrollBar();
}

void FrameTimingWidget::wheelEvent(QWheelEvent* event) {
  // Horizontal scrolling with mouse wheel
  if (scroll_bar_->isEnabled()) {
    int delta = -event->angleDelta().y() / 8;  // Convert to scroll steps
    scroll_bar_->setValue(scroll_bar_->value() + delta);
    event->accept();
  } else {
    QWidget::wheelEvent(event);
  }
}

void FrameTimingWidget::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && scroll_bar_->isEnabled()) {
    is_dragging_ = true;
    drag_start_pos_ = event->pos();
    drag_start_scroll_value_ = scroll_bar_->value();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
  } else {
    QWidget::mousePressEvent(event);
  }
}

void FrameTimingWidget::mouseMoveEvent(QMouseEvent* event) {
  if (is_dragging_) {
    // Calculate how far we've dragged in pixels
    int dx = event->pos().x() - drag_start_pos_.x();

    // Convert pixel movement to sample movement
    double base_pixels_per_sample = getBasePixelsPerSample();
    double effective_pixels_per_sample = base_pixels_per_sample * zoom_factor_;

    // Invert the direction: dragging right should scroll left (see earlier
    // content)
    int sample_delta = static_cast<int>(-dx / effective_pixels_per_sample);

    // Update scroll position
    int new_value = drag_start_scroll_value_ + sample_delta;
    scroll_bar_->setValue(new_value);

    event->accept();
  } else {
    QWidget::mouseMoveEvent(event);
  }
}

void FrameTimingWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && is_dragging_) {
    is_dragging_ = false;
    setCursor(Qt::ArrowCursor);
    event->accept();
  } else {
    QWidget::mouseReleaseEvent(event);
  }
}

void FrameTimingWidget::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  // Fill background
  const QPalette palette = this->palette();
  painter.fillRect(rect(), palette.color(QPalette::Base));

  // Define graph area (leave room for margins and scroll bar)
  // Add extra left margin for Y-axis labels
  int sb_height = scroll_bar_->sizeHint().height();
  int left_margin = 50;  // Extra space for Y-axis labels
  QRect graph_area(MARGIN + left_margin, MARGIN,
                   width() - 2 * MARGIN - left_margin,
                   height() - 2 * MARGIN - sb_height - 10);

  if (field1_samples_.empty() && field2_samples_.empty() &&
      y1_samples_.empty() && y2_samples_.empty()) {
    // No data to display
    painter.setPen(orc::view::theme_tokens::mutedText(palette));
    painter.drawText(rect(), Qt::AlignCenter, "No field data available");
    return;
  }

  drawGraph(painter, graph_area);
}

void FrameTimingWidget::drawGraph(QPainter& painter, const QRect& graph_area) {
  const bool is_dark_theme = orc::view::PlotWidget::isDarkTheme();
  const QPalette palette = this->palette();
  const QColor axis_color = palette.color(QPalette::WindowText);
  const QColor grid_color = orc::view::theme_tokens::gridLine(palette);
  const QColor label_color = axis_color;
  const QColor marker_yellow = orc::view::theme_tokens::plotColor(
      orc::view::theme_tokens::PlotColorToken::FieldBoundary, is_dark_theme);

  // Draw graph border
  painter.setPen(axis_color);
  painter.drawRect(graph_area);

  // Get mV range
  double min_mv, max_mv;
  getMVRange(min_mv, max_mv);

  // Resolve video params for unit conversion (PAL defaults if unavailable).
  int32_t blanking = 256, white = 844;
  orc::VideoSystem sys = orc::VideoSystem::PAL;
  if (video_params_.has_value() && video_params_->blanking_level >= 0 &&
      video_params_->white_level > video_params_->blanking_level) {
    blanking = video_params_->blanking_level;
    white = video_params_->white_level;
    sys = toOrcVideoSystem(video_params_->system);
  }

  // Convert mV range to display units for tick iteration.
  double min_dv, max_dv;
  if (amplitude_unit_ == orc::AmplitudeDisplayUnit::Millivolts) {
    min_dv = min_mv;
    max_dv = max_mv;
  } else {
    min_dv = orc::samples10_to_display(
        orc::mv_to_samples10(min_mv, blanking, white, sys), blanking, white,
        sys, amplitude_unit_);
    max_dv = orc::samples10_to_display(
        orc::mv_to_samples10(max_mv, blanking, white, sys), blanking, white,
        sys, amplitude_unit_);
  }

  // Helper: convert display-unit tick value to mV for Y-coordinate mapping.
  auto displayTickToMv = [&](double dv) -> double {
    if (amplitude_unit_ == orc::AmplitudeDisplayUnit::Millivolts) return dv;
    return orc::samples10_to_mv(
        orc::display_to_samples10(dv, blanking, white, sys, amplitude_unit_),
        blanking, white, sys);
  };

  const double minor_step = orc::amplitude_minor_tick(amplitude_unit_);
  const double major_step = orc::amplitude_major_tick(amplitude_unit_);

  // Draw grid lines and labels
  QFont label_font = painter.font();
  label_font.setPointSize(8);
  painter.setFont(label_font);

  painter.setPen(grid_color);

  const double first_grid = std::floor(min_dv / minor_step) * minor_step;
  for (double dv = first_grid; dv <= max_dv; dv += minor_step) {
    const double mv = displayTickToMv(dv);
    const double normalized = (mv - min_mv) / (max_mv - min_mv);
    const int y = graph_area.bottom() -
                  static_cast<int>(normalized * graph_area.height());

    if (y >= graph_area.top() && y <= graph_area.bottom()) {
      painter.setPen(grid_color);
      painter.drawLine(graph_area.left(), y, graph_area.right(), y);
    }
  }

  painter.setPen(label_color);
  const double first_label = std::floor(min_dv / major_step) * major_step;
  for (double dv = first_label; dv <= max_dv; dv += major_step) {
    const double mv = displayTickToMv(dv);
    const double normalized = (mv - min_mv) / (max_mv - min_mv);
    const int y = graph_area.bottom() -
                  static_cast<int>(normalized * graph_area.height());

    if (y >= graph_area.top() && y <= graph_area.bottom()) {
      const QString label = QString::number(static_cast<int>(std::round(dv)));
      QRect label_rect(MARGIN, y - 6, graph_area.left() - MARGIN - 5, 12);
      painter.drawText(label_rect,
                       static_cast<int>(Qt::AlignRight | Qt::AlignVCenter),
                       label);
    }
  }

  // Y-axis title (rotated, in the outer left margin).
  painter.save();
  painter.setPen(label_color);
  painter.translate(15, graph_area.center().y());
  painter.rotate(-90);
  painter.drawText(
      QRect(-50, -10, 100, 20), Qt::AlignCenter,
      QString::fromStdString(orc::amplitude_axis_title(amplitude_unit_)));
  painter.restore();

  // Draw level indicator lines if we have video parameters
  if (video_params_.has_value()) {
    const auto& vp = video_params_.value();

    // Helper to draw horizontal level line
    auto drawLevelLine = [&](double mv, const QColor& color,
                             Qt::PenStyle style = Qt::DashLine) {
      // Map mV to Y coordinate
      double normalized = (mv - min_mv) / (max_mv - min_mv);
      int y = graph_area.bottom() -
              static_cast<int>(normalized * graph_area.height());

      if (y >= graph_area.top() && y <= graph_area.bottom()) {
        painter.setPen(QPen(color, 1, style));
        painter.drawLine(graph_area.left(), y, graph_area.right(), y);
      }
    };

    // Draw level lines
    if (vp.blanking_level >= 0 && vp.white_level >= 0) {
      double blanking_mv =
          convertSampleToMV(static_cast<int16_t>(vp.blanking_level));
      drawLevelLine(blanking_mv,
                    orc::view::theme_tokens::neutralLine(palette, 0.35),
                    Qt::DashLine);

      if (vp.black_level >= 0 && vp.black_level != vp.blanking_level) {
        double black_mv =
            convertSampleToMV(static_cast<int16_t>(vp.black_level));
        drawLevelLine(black_mv,
                      orc::view::theme_tokens::neutralLine(palette, 0.5),
                      Qt::DashDotLine);
      }

      double white_mv = convertSampleToMV(static_cast<int16_t>(vp.white_level));
      drawLevelLine(white_mv,
                    orc::view::theme_tokens::neutralLine(palette, 0.7),
                    Qt::DashLine);
    }
  }

  // Draw vertical field line markers
  if (video_params_.has_value()) {
    const auto& vp = video_params_.value();
    if (vp.frame_width_nominal > 0) {
      // Determine which samples to use for line count
      size_t total_samples =
          std::max({field1_samples_.size(), field2_samples_.size(),
                    y1_samples_.size(), y2_samples_.size()});

      int visible_width = graph_area.width();
      double base_pixels_per_sample = getBasePixelsPerSample();
      double effective_pixels_per_sample =
          base_pixels_per_sample * zoom_factor_;
      int samples_per_view =
          static_cast<int>(visible_width / effective_pixels_per_sample);
      int start_sample = scroll_offset_;
      int end_sample = std::min(start_sample + samples_per_view,
                                static_cast<int>(total_samples));

      // Adapt marker density to current zoom level so marker drawing cost
      // scales smoothly instead of jumping at an arbitrary visible-line
      // threshold.
      double pixels_per_line =
          vp.frame_width_nominal * effective_pixels_per_sample;
      int marker_interval = 1;
      int label_interval = 1;
      if (pixels_per_line > 0.0) {
        marker_interval =
            std::max(1, static_cast<int>(std::ceil(30.0 / pixels_per_line)));
        label_interval =
            std::max(marker_interval,
                     static_cast<int>(std::ceil(70.0 / pixels_per_line)));
      }

      QFont line_num_font = painter.font();
      line_num_font.setPointSize(8);
      painter.setFont(line_num_font);

      // Draw vertical markers at field line boundaries
      for (int line = 0; line * vp.frame_width_nominal < end_sample; ++line) {
        int sample_pos = line * vp.frame_width_nominal;
        if (sample_pos >= start_sample && sample_pos <= end_sample) {
          // Only draw markers at the specified interval.
          if (line % marker_interval == 0) {
            int x = graph_area.left() +
                    static_cast<int>((sample_pos - start_sample) *
                                     effective_pixels_per_sample);

            // Draw vertical line in yellow
            painter.setPen(QPen(marker_yellow, 1, Qt::DotLine));
            painter.drawLine(x, graph_area.top(), x, graph_area.bottom());

            // Draw line number labels less frequently than marker lines.
            if (line % label_interval == 0) {
              painter.setPen(axis_color);
              QString line_label = QString::number(line + 1);
              QRect text_rect(x - 15, graph_area.bottom() + 5, 30, 12);
              painter.drawText(text_rect, Qt::AlignCenter, line_label);
            }
          }
        }
      }
    }
  }

  // Draw selected position marker (green) if provided
  if (marker_sample_.has_value()) {
    // Determine visible sample range
    int visible_width = graph_area.width();
    double base_pixels_per_sample = getBasePixelsPerSample();
    double effective_pixels_per_sample = base_pixels_per_sample * zoom_factor_;
    int samples_per_view =
        static_cast<int>(visible_width / effective_pixels_per_sample);
    int start_sample = scroll_offset_;
    int end_sample = start_sample + samples_per_view;

    int sample_pos = marker_sample_.value();
    if (sample_pos >= start_sample && sample_pos <= end_sample) {
      int x = graph_area.left() + static_cast<int>((sample_pos - start_sample) *
                                                   effective_pixels_per_sample);
      painter.setPen(
          QPen(orc::view::theme_tokens::plotColor(
                   orc::view::theme_tokens::PlotColorToken::MarkerSelection,
                   is_dark_theme),
               2, Qt::SolidLine));
      painter.drawLine(x, graph_area.top(), x, graph_area.bottom());
    }
  }

  // Draw X-axis label
  painter.setPen(axis_color);
  painter.drawText(graph_area.center().x() - 50, height() - 5,
                   "Sample Position");

  // Determine which samples to draw and their colors
  // Priority: if Y/C samples exist, use those; otherwise use composite
  bool has_yc = !y1_samples_.empty() || !y2_samples_.empty() ||
                !c1_samples_.empty() || !c2_samples_.empty();
  bool has_two_fields = !field2_samples_.empty() || !y2_samples_.empty();

  if (has_yc) {
    const QColor y1_color = orc::view::theme_tokens::plotColor(
        orc::view::theme_tokens::PlotColorToken::LumaPrimary, is_dark_theme);
    const QColor c1_color = orc::view::theme_tokens::plotColor(
        orc::view::theme_tokens::PlotColorToken::ChromaPrimary, is_dark_theme);
    const QColor y2_color = orc::view::theme_tokens::plotColor(
        orc::view::theme_tokens::PlotColorToken::LumaSecondary, is_dark_theme);
    const QColor c2_color = orc::view::theme_tokens::plotColor(
        orc::view::theme_tokens::PlotColorToken::ChromaSecondary,
        is_dark_theme);
    const QColor composite1_color = orc::view::theme_tokens::plotColor(
        orc::view::theme_tokens::PlotColorToken::CompositePrimary,
        is_dark_theme);
    const QColor composite2_color = orc::view::theme_tokens::plotColor(
        orc::view::theme_tokens::PlotColorToken::CompositeSecondary,
        is_dark_theme);

    switch (channel_mode_) {
      case ChannelMode::YOnly:
        if (!y1_samples_.empty()) {
          drawSamples(painter, graph_area, y1_samples_, y1_color, 0);
        }
        if (has_two_fields && !y2_samples_.empty()) {
          drawSamples(painter, graph_area, y2_samples_, y2_color, 0);
        }
        break;

      case ChannelMode::COnly:
        if (!c1_samples_.empty()) {
          drawSamples(painter, graph_area, c1_samples_, c1_color, 0);
        }
        if (has_two_fields && !c2_samples_.empty()) {
          drawSamples(painter, graph_area, c2_samples_, c2_color, 0);
        }
        break;

      case ChannelMode::BothYC:
        if (!y1_samples_.empty()) {
          drawSamples(painter, graph_area, y1_samples_, y1_color, 0);
        }
        if (!c1_samples_.empty()) {
          drawSamples(painter, graph_area, c1_samples_, c1_color, 0);
        }
        if (has_two_fields) {
          if (!y2_samples_.empty()) {
            drawSamples(painter, graph_area, y2_samples_, y2_color, 0);
          }
          if (!c2_samples_.empty()) {
            drawSamples(painter, graph_area, c2_samples_, c2_color, 0);
          }
        }
        break;

      case ChannelMode::YPlusC: {
        const int32_t chroma_dc =
            (video_params_.has_value() && video_params_->chroma_dc_offset >= 0)
                ? video_params_->chroma_dc_offset
                : 0;
        const std::vector<int16_t> combined_1 =
            buildCombinedYPlusC(y1_samples_, c1_samples_, chroma_dc);
        const std::vector<int16_t> combined_2 =
            buildCombinedYPlusC(y2_samples_, c2_samples_, chroma_dc);

        if (!combined_1.empty()) {
          drawSamples(painter, graph_area, combined_1, composite1_color, 0);
        }
        if (has_two_fields && !combined_2.empty()) {
          drawSamples(painter, graph_area, combined_2, composite2_color, 0);
        }
        break;
      }
    }
  } else {
    // Draw composite samples
    const QColor field1_color = orc::view::theme_tokens::plotColor(
        orc::view::theme_tokens::PlotColorToken::CompositePrimary,
        is_dark_theme);
    const QColor field2_color = orc::view::theme_tokens::plotColor(
        orc::view::theme_tokens::PlotColorToken::CompositeSecondary,
        is_dark_theme);
    if (!field1_samples_.empty()) {
      drawSamples(painter, graph_area, field1_samples_, field1_color, 0);
    }
    if (!field2_samples_.empty()) {
      drawSamples(painter, graph_area, field2_samples_, field2_color, 0);
    }
  }
}

void FrameTimingWidget::drawSamples(QPainter& painter, const QRect& graph_area,
                                    const std::vector<int16_t>& samples,
                                    const QColor& color, int y_offset) {
  if (samples.empty()) return;

  painter.setPen(QPen(color, 1));

  // Calculate visible sample range
  int visible_width = graph_area.width();
  double base_pixels_per_sample = getBasePixelsPerSample();
  double effective_pixels_per_sample = base_pixels_per_sample * zoom_factor_;
  int samples_per_view =
      static_cast<int>(visible_width / effective_pixels_per_sample);
  int start_sample = scroll_offset_;
  int end_sample = std::min(start_sample + samples_per_view,
                            static_cast<int>(samples.size()));

  if (start_sample >= static_cast<int>(samples.size())) return;

  // Get mV range for normalization
  double min_mv, max_mv;
  getMVRange(min_mv, max_mv);

  int lines_visible = 0;
  if (video_params_.has_value() && video_params_->frame_width_nominal > 0) {
    lines_visible =
        std::max(1, samples_per_view / video_params_->frame_width_nominal);
  }

  if (draft_render_mode_) {
    // During active zoom interaction, render a lighter-weight but still
    // detailed preview.
    constexpr int kDraftBucketPixels = 3;
    int bucket_samples = std::max(
        1, static_cast<int>(std::ceil((1.0 / effective_pixels_per_sample) *
                                      kDraftBucketPixels)));
    bucket_samples = std::min(bucket_samples, 64);
    if (lines_visible > 0 && lines_visible <= 8) {
      // Preserve more structure when zoomed into just a few lines.
      bucket_samples = std::min(bucket_samples, 4);
    }

    QPainterPath path;
    bool first_point = true;

    for (int bucket_start = start_sample; bucket_start < end_sample;
         bucket_start += bucket_samples) {
      int bucket_end = std::min(bucket_start + bucket_samples, end_sample);
      if (bucket_start >= bucket_end) {
        continue;
      }

      int64_t sum = 0;
      int16_t min_sample = samples[bucket_start];
      int16_t max_sample = samples[bucket_start];
      for (int i = bucket_start; i < bucket_end; ++i) {
        sum += samples[i];
        min_sample = std::min(min_sample, samples[i]);
        max_sample = std::max(max_sample, samples[i]);
      }
      int16_t avg_sample = static_cast<int16_t>(
          sum / static_cast<int64_t>(bucket_end - bucket_start));

      int center_index = bucket_start + ((bucket_end - bucket_start) / 2);
      int x =
          graph_area.left() + static_cast<int>((center_index - start_sample) *
                                               effective_pixels_per_sample);

      double mv_value = convertSampleToMV(avg_sample);
      double normalized = (mv_value - min_mv) / (max_mv - min_mv);
      int y = graph_area.bottom() -
              static_cast<int>(normalized * graph_area.height()) + y_offset;

      // Preserve peaks/troughs that a pure average would hide.
      double min_mv_value = convertSampleToMV(min_sample);
      double max_mv_value = convertSampleToMV(max_sample);
      double min_normalized = (min_mv_value - min_mv) / (max_mv - min_mv);
      double max_normalized = (max_mv_value - min_mv) / (max_mv - min_mv);
      int y_top = graph_area.bottom() -
                  static_cast<int>(max_normalized * graph_area.height()) +
                  y_offset;
      int y_bottom = graph_area.bottom() -
                     static_cast<int>(min_normalized * graph_area.height()) +
                     y_offset;
      painter.drawLine(x, y_top, x, y_bottom);

      if (first_point) {
        path.moveTo(x, y);
        first_point = false;
      } else {
        path.lineTo(x, y);
      }
    }

    painter.drawPath(path);
    return;
  }

  // Use min/max per pixel optimization when zoomed out enough that detail loss
  // is acceptable. At low visible line counts, prefer the full path to preserve
  // waveform detail.
  constexpr int kAggregationLineThreshold = 12;
  if (effective_pixels_per_sample < 1.0 &&
      (lines_visible <= 0 || lines_visible > kAggregationLineThreshold)) {
    // Multiple samples map to each pixel - draw vertical line from min to max
    double samples_per_pixel = 1.0 / effective_pixels_per_sample;
    QPainterPath center_path;
    bool first_center_point = true;

    for (int px = 0; px < visible_width; ++px) {
      int x = graph_area.left() + px;

      // Calculate sample range for this pixel column. Use floor and enforce at
      // least one sample per bucket to avoid rendering gaps from rounding.
      int bucket_start =
          start_sample + static_cast<int>(std::floor(px * samples_per_pixel));
      int bucket_end =
          start_sample +
          static_cast<int>(std::floor((px + 1) * samples_per_pixel));
      bucket_end = std::max(bucket_end, bucket_start + 1);
      bucket_end = std::min(bucket_end, static_cast<int>(samples.size()));

      if (bucket_start >= bucket_end ||
          bucket_start >= static_cast<int>(samples.size())) {
        continue;
      }

      // Find min and max sample values in this pixel column
      int16_t min_sample = samples[bucket_start];
      int16_t max_sample = samples[bucket_start];
      int64_t sum_samples = 0;

      for (int i = bucket_start; i < bucket_end; ++i) {
        min_sample = std::min(min_sample, samples[i]);
        max_sample = std::max(max_sample, samples[i]);
        sum_samples += samples[i];
      }
      int16_t avg_sample = static_cast<int16_t>(
          sum_samples / static_cast<int64_t>(bucket_end - bucket_start));

      // Convert to Y coordinates
      double min_mv_value = convertSampleToMV(min_sample);
      double max_mv_value = convertSampleToMV(max_sample);
      double avg_mv_value = convertSampleToMV(avg_sample);

      double min_normalized = (min_mv_value - min_mv) / (max_mv - min_mv);
      double max_normalized = (max_mv_value - min_mv) / (max_mv - min_mv);
      double avg_normalized = (avg_mv_value - min_mv) / (max_mv - min_mv);

      int y_top = graph_area.bottom() -
                  static_cast<int>(max_normalized * graph_area.height()) +
                  y_offset;
      int y_bottom = graph_area.bottom() -
                     static_cast<int>(min_normalized * graph_area.height()) +
                     y_offset;
      int y_center = graph_area.bottom() -
                     static_cast<int>(avg_normalized * graph_area.height()) +
                     y_offset;

      // Draw vertical line from min to max (preserves peaks and troughs)
      painter.drawLine(x, y_top, x, y_bottom);

      if (first_center_point) {
        center_path.moveTo(x, y_center);
        first_center_point = false;
      } else {
        center_path.lineTo(x, y_center);
      }
    }

    painter.drawPath(center_path);
  } else {
    // Draw all individual samples as connected lines
    QPainterPath path;
    bool first_point = true;

    for (int i = start_sample; i < end_sample; ++i) {
      int x = graph_area.left() + static_cast<int>((i - start_sample) *
                                                   effective_pixels_per_sample);

      // Convert sample to mV and map to Y coordinate
      double mv_value = convertSampleToMV(samples[i]);
      double normalized = (mv_value - min_mv) / (max_mv - min_mv);
      int y = graph_area.bottom() -
              static_cast<int>(normalized * graph_area.height()) + y_offset;

      if (first_point) {
        path.moveTo(x, y);
        first_point = false;
      } else {
        path.lineTo(x, y);
      }
    }

    painter.drawPath(path);
  }
}

void FrameTimingWidget::setAmplitudeUnit(orc::AmplitudeDisplayUnit unit) {
  if (amplitude_unit_ == unit) return;
  amplitude_unit_ = unit;
  update();
}
