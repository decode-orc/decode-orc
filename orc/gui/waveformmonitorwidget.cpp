/*
 * File:        waveformmonitorwidget.cpp
 * Module:      orc-gui
 * Purpose:     Waveform monitor raster widget
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "waveformmonitorwidget.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QRect>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QTimer>
#include <algorithm>
#include <cmath>

#include "gpu/gpu_surface_policy.h"
#include "gpu/scope_surface_factory.h"
#include "gpu/scope_vertex_builder.h"
#include "plotwidget.h"  // PlotWidget::isDarkTheme()
#include "theme_color_tokens.h"

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

WaveformMonitorWidget::WaveformMonitorWidget(QWidget* parent)
    : QWidget(parent) {
  setMinimumSize(400, 300);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

  surface_ = orc::gui::gpu::createScopeSurface(this);
}

QRect WaveformMonitorWidget::plotArea() const {
  return QRect(kLeftMargin, kTopMargin, width() - kLeftMargin - kRightMargin,
               height() - kTopMargin - kBottomMargin);
}

// ---------------------------------------------------------------------------
// Data ingestion
// ---------------------------------------------------------------------------

void WaveformMonitorWidget::setData(
    const std::vector<int16_t>& composite_samples, int first_field_height,
    int second_field_height,
    const std::optional<orc::presenters::VideoParametersView>& video_params) {
  video_params_ = video_params;

  // Derive Y-axis range (always in mV — display unit only affects rendering).
  y_min_mv_ = -350.0;
  y_max_mv_ = 950.0;
  if (y_only_mode_) {
    const double amv =
        video_params.has_value()
            ? orc::active_video_mv(toOrcVideoSystem(video_params->system))
            : 700.0;
    y_min_mv_ = -100.0;
    y_max_mv_ = amv + 100.0;
  } else if (video_params.has_value()) {
    const auto& vp = *video_params;
    if (vp.blanking_level >= 0 && vp.white_level > vp.blanking_level &&
        vp.sync_tip_level >= 0 && vp.peak_level >= 0) {
      const orc::VideoSystem sys = toOrcVideoSystem(vp.system);
      const double sync_tip_mv = orc::samples10_to_mv(
          vp.sync_tip_level, vp.blanking_level, vp.white_level, sys);
      const double peak_mv = orc::samples10_to_mv(
          vp.peak_level, vp.blanking_level, vp.white_level, sys);
      const double span = peak_mv - sync_tip_mv;
      if (span > 0.0) {
        y_min_mv_ = sync_tip_mv - span * 0.05;
        y_max_mv_ = peak_mv + span * 0.05;
      }
    }
  }
  y_bins_ = static_cast<int>((y_max_mv_ - y_min_mv_) / kBinWidthMv) + 1;

  const int total_lines =
      first_field_height + (second_field_height > 0 ? second_field_height : 0);
  if (total_lines <= 0 || composite_samples.empty()) {
    count_grid_.reset(0, 0);
    x_samples_ = 0;
    active_video_start_ = 0;
    line_count_ = 0;
    image_dirty_ = true;
    update();
    return;
  }

  const int samples_per_line =
      static_cast<int>(composite_samples.size()) / total_lines;
  if (samples_per_line <= 0) {
    count_grid_.reset(0, 0);
    x_samples_ = 0;
    active_video_start_ = 0;
    line_count_ = 0;
    image_dirty_ = true;
    update();
    return;
  }

  // Active video range — clip to samples_per_line to handle edge cases.
  int active_start = 0;
  int active_end = samples_per_line - 1;  // inclusive
  int32_t blanking_level = -1;
  int32_t white_level = -1;
  orc::VideoSystem sys = orc::VideoSystem::Unknown;

  if (video_params.has_value()) {
    const auto& vp = *video_params;
    if (vp.active_video_start >= 0 &&
        vp.active_video_end > vp.active_video_start) {
      active_start = std::min(vp.active_video_start, samples_per_line - 1);
      active_end = std::min(vp.active_video_end, samples_per_line - 1);
    }
    blanking_level = vp.blanking_level;
    white_level = vp.white_level;
    sys = toOrcVideoSystem(vp.system);
  }

  active_video_start_ = active_start;

  // µs per sample for the X-axis time labels.  Rates are 4FSC standards:
  // NTSC/PAL-M: 4 × 3.579545 MHz = 14 318 181.8 Hz
  // PAL:        4 × 4.433618 MHz = 17 734 475.0 Hz
  if (video_params.has_value()) {
    switch (video_params->system) {
      case orc::presenters::VideoSystem::PAL:
        us_per_sample_ = 1000000.0 / 17734475.0;
        break;
      case orc::presenters::VideoSystem::NTSC:
      case orc::presenters::VideoSystem::PAL_M:
      default:
        us_per_sample_ = 1000000.0 / 14318181.8;
        break;
    }
  }

  accumulate(composite_samples, total_lines, active_start, active_end,
             blanking_level, white_level, sys);
  line_count_ = total_lines;
  image_dirty_ = true;
  update();
}

void WaveformMonitorWidget::accumulate(const std::vector<int16_t>& samples,
                                       int total_lines, int active_start,
                                       int active_end, int32_t blanking_level,
                                       int32_t white_level,
                                       orc::VideoSystem sys) {
  const int active_width = active_end - active_start + 1;
  if (active_width <= 0 || total_lines <= 0) return;

  const int samples_per_line = static_cast<int>(samples.size()) / total_lines;
  if (samples_per_line <= 0) return;

  x_samples_ = active_width;
  count_grid_.reset(x_samples_, y_bins_);

  const bool have_levels =
      (blanking_level >= 0 && white_level > blanking_level);

  for (int line = 0; line < total_lines; ++line) {
    const int line_start = line * samples_per_line;
    // Guard against truncated data (non-orthogonal PAL lines).
    if (line_start + active_end >= static_cast<int>(samples.size())) break;

    for (int x = 0; x < active_width; ++x) {
      const int16_t raw =
          samples[static_cast<size_t>(line_start) +
                  static_cast<size_t>(active_start) + static_cast<size_t>(x)];

      const double mv = have_levels ? orc::samples10_to_mv(raw, blanking_level,
                                                           white_level, sys)
                                    : static_cast<double>(raw);

      const int y_bin = static_cast<int>((mv - y_min_mv_) / kBinWidthMv);
      count_grid_.increment(x, y_bin);
    }
  }
}

// ---------------------------------------------------------------------------
// Gain and display mode
// ---------------------------------------------------------------------------

void WaveformMonitorWidget::setGain(double gain) {
  gain_ = std::clamp(gain, 0.1, 10.0);
  image_dirty_ = true;
  update();
}

void WaveformMonitorWidget::setPhosphorMode(bool enabled) {
  if (phosphor_mode_ == enabled) return;
  phosphor_mode_ = enabled;
  image_dirty_ = true;
  update();
}

void WaveformMonitorWidget::setYOnlyMode(bool y_only) {
  if (y_only_mode_ == y_only) return;
  y_only_mode_ = y_only;
  image_dirty_ = true;
  update();
}

// ---------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------

QColor WaveformMonitorWidget::displayBackground() const {
  if (phosphor_mode_) return QColor(0, 0, 0);
  return palette().color(QPalette::Base);
}

QColor WaveformMonitorWidget::displayTrace() const {
  if (phosphor_mode_) return QColor(0, 230, 0);
  return theme_tokens::plotColor(theme_tokens::PlotColorToken::CompositePrimary,
                                 PlotWidget::isDarkTheme());
}

QColor WaveformMonitorWidget::displayAxis() const {
  if (phosphor_mode_) return Qt::white;
  return palette().color(QPalette::WindowText);
}

QColor WaveformMonitorWidget::displayGrid() const {
  if (phosphor_mode_) return QColor(55, 55, 55);
  return theme_tokens::gridLine(palette());
}

// ---------------------------------------------------------------------------
// Qt events
// ---------------------------------------------------------------------------

void WaveformMonitorWidget::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  image_dirty_ = true;
}

void WaveformMonitorWidget::paintEvent(QPaintEvent*) {
  downgradeIfCanvasFailed();

  QPainter painter(this);

  painter.fillRect(rect(), displayBackground());

  const QRect pa = plotArea();
  if (pa.isEmpty()) return;

  const bool have_data =
      !count_grid_.empty() && x_samples_ != 0 && y_bins_ != 0;

  if (surface_ && !downgrade_pending_) {
    // The canvas covers the plot area, so everything drawn inside it reaches
    // the window through the furniture images rather than this painter. The
    // axis labels and ticks live in the margins and are still drawn here.
    positionCanvas(pa);
    if (image_dirty_ || canvas_plot_size_ != pa.size()) {
      if (have_data) {
        updateCanvas(pa);
      } else {
        showEmptyCanvas(pa);
      }
      canvas_plot_size_ = pa.size();
      image_dirty_ = false;
    }
    drawYAxis(painter, pa);
    drawXAxis(painter, pa);
    return;
  }

  if (!have_data) {
    painter.setPen(displayAxis());
    painter.drawText(pa, Qt::AlignCenter, "No data");
    drawYAxis(painter, pa);
    drawXAxis(painter, pa);
    return;
  }

  if (image_dirty_ || cached_image_.size() != pa.size()) {
    rebuildImage(pa);
    image_dirty_ = false;
  }

  drawGrid(painter, pa);
  painter.drawImage(pa.topLeft(), cached_image_);

  drawLevelMarkers(painter, pa);
  drawYAxis(painter, pa);
  drawXAxis(painter, pa);
}

// ---------------------------------------------------------------------------
// Canvas path
// ---------------------------------------------------------------------------

bool WaveformMonitorWidget::FurnitureKey::matches(
    const FurnitureKey& other) const {
  return size == other.size && y_min_mv == other.y_min_mv &&
         y_max_mv == other.y_max_mv && x_samples == other.x_samples &&
         active_video_start == other.active_video_start &&
         us_per_sample == other.us_per_sample && phosphor == other.phosphor &&
         have_data == other.have_data && unit == other.unit &&
         background == other.background && trace == other.trace &&
         axis == other.axis && grid == other.grid &&
         have_params == other.have_params && system == other.system &&
         sync_tip == other.sync_tip && blanking == other.blanking &&
         black == other.black && white == other.white && peak == other.peak;
}

void WaveformMonitorWidget::downgradeIfCanvasFailed() {
  if (!surface_ || downgrade_pending_ ||
      orc::gui::gpu::GpuSurfacePolicy::instance().useGpuSurface()) {
    return;
  }

  // This is reached from paintEvent, and deleting a child widget while its
  // parent is painting cuts Qt's paint traversal out from under it. The
  // canvas is hidden now, which uncovers the plot area, and dropped on the
  // next turn of the event loop.
  downgrade_pending_ = true;
  surface_->widget()->hide();
  QTimer::singleShot(0, this, [this]() {
    downgrade_pending_ = false;
    if (orc::gui::gpu::dropScopeSurfaceIfFailed(surface_)) {
      // The widget's own renderer has drawn nothing yet.
      image_dirty_ = true;
      furniture_valid_ = false;
      update();
    }
  });
}

void WaveformMonitorWidget::positionCanvas(const QRect& plot_area) {
  QWidget* canvas = surface_->widget();
  if (canvas->geometry() != plot_area) {
    canvas->setGeometry(plot_area);
  }
  if (!canvas->isVisible()) {
    canvas->show();
  }
}

void WaveformMonitorWidget::refreshCanvasFurniture(const QRect& plot_area,
                                                   bool have_data) {
  FurnitureKey key;
  key.size = plot_area.size();
  key.y_min_mv = y_min_mv_;
  key.y_max_mv = y_max_mv_;
  key.x_samples = x_samples_;
  key.active_video_start = active_video_start_;
  key.us_per_sample = us_per_sample_;
  key.phosphor = phosphor_mode_;
  key.have_data = have_data;
  key.unit = amplitude_unit_;
  key.background = displayBackground().rgba();
  key.trace = displayTrace().rgba();
  key.axis = displayAxis().rgba();
  key.grid = displayGrid().rgba();
  if (video_params_.has_value()) {
    const auto& vp = *video_params_;
    key.have_params = true;
    key.system = static_cast<int>(vp.system);
    key.sync_tip = vp.sync_tip_level;
    key.blanking = vp.blanking_level;
    key.black = vp.black_level;
    key.white = vp.white_level;
    key.peak = vp.peak_level;
  }
  if (furniture_valid_ && furniture_key_.matches(key)) {
    return;
  }

  // The furniture is drawn in widget coordinates, so the painter is moved to
  // where the plot area starts and the same code draws into the image. What
  // falls outside the plot area - the axis labels and tick stubs - is clipped
  // away here and drawn by the widget itself in the margins instead.
  const QPoint origin = plot_area.topLeft();

  QImage under(plot_area.size(), QImage::Format_ARGB32_Premultiplied);
  under.fill(displayBackground());
  if (have_data) {
    QPainter painter(&under);
    painter.translate(-origin);
    drawGrid(painter, plot_area);
  }

  QImage over(plot_area.size(), QImage::Format_ARGB32_Premultiplied);
  over.fill(Qt::transparent);
  {
    QPainter painter(&over);
    painter.translate(-origin);
    if (have_data) {
      drawLevelMarkers(painter, plot_area);
    } else {
      painter.setPen(displayAxis());
      painter.drawText(plot_area, Qt::AlignCenter, "No data");
    }
    // The axis lines run along the edges of the plot area, so they are inside
    // the canvas and have to be drawn on it.
    drawYAxis(painter, plot_area);
    drawXAxis(painter, plot_area);
  }

  // The canvas composites with straight alpha, as the shader's source-over
  // does, so the premultiplied painting surface is undone here.
  canvas_underlay_ = under.convertToFormat(QImage::Format_RGBA8888);
  canvas_overlay_ = over.convertToFormat(QImage::Format_RGBA8888);
  furniture_key_ = key;
  furniture_valid_ = true;
}

void WaveformMonitorWidget::updateCanvas(const QRect& plot_area) {
  refreshCanvasFurniture(plot_area, true);

  const QSize canvas =
      orc::gui::gpu::waveformCanvasSize(count_grid_, plot_area.size());

  orc::gui::gpu::ScopeFrame frame;
  frame.canvas_size = canvas;
  frame.points = orc::gui::gpu::buildWaveformVertices(count_grid_, canvas);
  // One vertex per counted cell, carrying that cell's count: where several
  // cells fall under one canvas pixel the largest wins, which is the area-max
  // reduction the widget's own renderer performs.
  frame.blend = orc::gui::gpu::ScopeBlend::kMax;
  frame.preserve_aspect = false;
  // The canvas is never finer than the grid, so scaling it up to the plot
  // area replicates cells rather than interpolating between them - again what
  // the widget's own renderer does.
  frame.smooth = false;
  frame.underlay = canvas_underlay_;
  frame.overlay = canvas_overlay_;
  frame.map = orc::gui::gpu::waveformMapUniforms(
      gain_, kBrightnessBias, displayBackground(), displayTrace());

  surface_->setFrame(std::move(frame));
  surface_->refresh();
}

void WaveformMonitorWidget::showEmptyCanvas(const QRect& plot_area) {
  refreshCanvasFurniture(plot_area, false);

  orc::gui::gpu::ScopeFrame frame;
  frame.canvas_size = plot_area.size();
  frame.preserve_aspect = false;
  frame.smooth = false;
  frame.underlay = canvas_underlay_;
  frame.overlay = canvas_overlay_;

  surface_->setFrame(std::move(frame));
  surface_->refresh();
}

// ---------------------------------------------------------------------------
// Image rebuild — area-max accumulation + separable Gaussian blur
// ---------------------------------------------------------------------------

void WaveformMonitorWidget::rebuildImage(const QRect& pa) {
  if (pa.isEmpty() || x_samples_ == 0 || y_bins_ == 0) return;

  // ARGB32 with transparent background so grid lines drawn before drawImage()
  // remain visible through unpopulated pixels.
  cached_image_ = QImage(pa.size(), QImage::Format_ARGB32_Premultiplied);
  cached_image_.fill(Qt::transparent);

  const int img_w = pa.width();
  const int img_h = pa.height();

  // For each output pixel, find the maximum count across all grid cells that
  // map into its fractional range.  This avoids missed-bin aliasing when
  // x_samples_ >> img_w.
  //
  // Brightness mapping adapted from the Color Tools VirtualDub plugin
  // reference:
  //   clamp[0] = 0
  //   clamp[j] = min(j + kBrightnessBias, 255)   for j >= 1
  //   brightness = clamp[count * 5 * gain] / 255
  //
  // Full saturation is reached after ~(255 - kBrightnessBias) / (5*gain)
  // occurrences (ITU-R BT.601 broadcast norm).
  // The gain control moves the saturation knee: higher gain saturates faster.
  const float k = 5.0f * static_cast<float>(gain_);

  // Background → trace colour, interpolated by brightness.
  const QColor back_color = displayBackground();
  const QColor plot_color = displayTrace();
  const float br = static_cast<float>(back_color.redF());
  const float bg = static_cast<float>(back_color.greenF());
  const float bb = static_cast<float>(back_color.blueF());
  const float pr = static_cast<float>(plot_color.redF());
  const float pg = static_cast<float>(plot_color.greenF());
  const float pb = static_cast<float>(plot_color.blueF());

  // Written a scanline at a time: setPixel() re-derives the row address and
  // re-checks the format for every one of the ~half-million pixels here, and
  // this runs on the GUI thread on every displayed frame.
  for (int py = 0; py < img_h; ++py) {
    const int yi_lo =
        static_cast<int>(static_cast<double>(img_h - py - 1) / img_h * y_bins_);
    const int yi_hi = std::min(
        static_cast<int>(static_cast<double>(img_h - py) / img_h * y_bins_),
        y_bins_ - 1);

    auto* row = reinterpret_cast<QRgb*>(cached_image_.scanLine(py));

    for (int px = 0; px < img_w; ++px) {
      const int xi_lo =
          static_cast<int>(static_cast<double>(px) / img_w * x_samples_);
      const int xi_hi = std::min(
          static_cast<int>(static_cast<double>(px + 1) / img_w * x_samples_),
          x_samples_ - 1);

      const uint32_t max_count = count_grid_.maxIn(xi_lo, xi_hi, yi_lo, yi_hi);
      if (max_count == 0) continue;

      const float b = std::min(
          1.0f, (static_cast<float>(max_count) * k + kBrightnessBias) / 255.0f);
      const int cr = static_cast<int>((br + (pr - br) * b) * 255.0f);
      const int cg = static_cast<int>((bg + (pg - bg) * b) * 255.0f);
      const int cb = static_cast<int>((bb + (pb - bb) * b) * 255.0f);
      row[px] = qRgb(cr, cg, cb);
    }
  }
}

// ---------------------------------------------------------------------------
// Axis and markers
// ---------------------------------------------------------------------------

int WaveformMonitorWidget::mvToPixelY(double mv, const QRect& pa) const {
  const double range = y_max_mv_ - y_min_mv_;
  if (range <= 0.0) return pa.center().y();
  const double t = (mv - y_min_mv_) / range;
  return pa.bottom() - static_cast<int>(t * pa.height());
}

// Helper: resolve blanking/white/sys with PAL fallback.
static void resolveVideoLevels(
    const std::optional<orc::presenters::VideoParametersView>& vp,
    int32_t& blanking, int32_t& white, orc::VideoSystem& sys) {
  blanking = 256;
  white = 844;
  sys = orc::VideoSystem::PAL;
  if (vp.has_value() && vp->blanking_level >= 0 &&
      vp->white_level > vp->blanking_level) {
    blanking = vp->blanking_level;
    white = vp->white_level;
    sys = toOrcVideoSystem(vp->system);
  }
}

void WaveformMonitorWidget::drawYAxis(QPainter& painter,
                                      const QRect& pa) const {
  const QColor axis_color = displayAxis();
  painter.setPen(axis_color);

  // Axis line.
  painter.drawLine(pa.left(), pa.top(), pa.left(), pa.bottom());

  const QFontMetrics fm(painter.font());

  int32_t blanking, white;
  orc::VideoSystem sys;
  resolveVideoLevels(video_params_, blanking, white, sys);

  const double tick_step = orc::amplitude_major_tick(amplitude_unit_);
  double min_dv, max_dv;
  if (amplitude_unit_ == orc::AmplitudeDisplayUnit::Millivolts) {
    min_dv = y_min_mv_;
    max_dv = y_max_mv_;
  } else {
    min_dv = orc::samples10_to_display(
        orc::mv_to_samples10(y_min_mv_, blanking, white, sys), blanking, white,
        sys, amplitude_unit_);
    max_dv = orc::samples10_to_display(
        orc::mv_to_samples10(y_max_mv_, blanking, white, sys), blanking, white,
        sys, amplitude_unit_);
  }

  const double first_tick = orc::snap_ceil(min_dv, tick_step);
  for (double dv = first_tick; dv <= max_dv + tick_step * 0.01;
       dv += tick_step) {
    double mv;
    if (amplitude_unit_ == orc::AmplitudeDisplayUnit::Millivolts) {
      mv = dv;
    } else {
      mv = orc::samples10_to_mv(
          orc::display_to_samples10(dv, blanking, white, sys, amplitude_unit_),
          blanking, white, sys);
    }
    const int py = mvToPixelY(mv, pa);
    if (py < pa.top() || py > pa.bottom()) continue;

    painter.drawLine(pa.left() - 4, py, pa.left(), py);

    const QString label = QString::number(static_cast<int>(std::round(dv)));
    const QRect lr(0, py - fm.height() / 2, kLeftMargin - 6, fm.height());
    painter.drawText(lr, Qt::AlignRight | Qt::AlignVCenter, label);
  }

  // Y-axis title.
  painter.save();
  painter.setPen(axis_color);
  painter.translate(12, pa.center().y());
  painter.rotate(-90);
  painter.drawText(
      QRect(-50, -10, 100, 20), Qt::AlignCenter,
      QString::fromStdString(orc::amplitude_axis_title(amplitude_unit_)));
  painter.restore();
}

void WaveformMonitorWidget::drawGrid(QPainter& painter, const QRect& pa) const {
  painter.setPen(QPen(displayGrid(), 1, Qt::SolidLine));

  // Horizontal lines at display-unit major tick positions.
  int32_t blanking, white;
  orc::VideoSystem sys;
  resolveVideoLevels(video_params_, blanking, white, sys);

  const double y_tick_step = orc::amplitude_major_tick(amplitude_unit_);
  double min_dv, max_dv;
  if (amplitude_unit_ == orc::AmplitudeDisplayUnit::Millivolts) {
    min_dv = y_min_mv_;
    max_dv = y_max_mv_;
  } else {
    min_dv = orc::samples10_to_display(
        orc::mv_to_samples10(y_min_mv_, blanking, white, sys), blanking, white,
        sys, amplitude_unit_);
    max_dv = orc::samples10_to_display(
        orc::mv_to_samples10(y_max_mv_, blanking, white, sys), blanking, white,
        sys, amplitude_unit_);
  }
  const double first_y = orc::snap_ceil(min_dv, y_tick_step);
  for (double dv = first_y; dv <= max_dv + y_tick_step * 0.01;
       dv += y_tick_step) {
    double mv;
    if (amplitude_unit_ == orc::AmplitudeDisplayUnit::Millivolts) {
      mv = dv;
    } else {
      mv = orc::samples10_to_mv(
          orc::display_to_samples10(dv, blanking, white, sys, amplitude_unit_),
          blanking, white, sys);
    }
    const int py = mvToPixelY(mv, pa);
    if (py < pa.top() || py > pa.bottom()) continue;
    painter.drawLine(pa.left(), py, pa.right(), py);
  }

  if (x_samples_ <= 0) return;

  // Vertical lines at the same µs tick positions as the X-axis labels.
  const double start_us = active_video_start_ * us_per_sample_;
  const double end_us = (active_video_start_ + x_samples_) * us_per_sample_;
  const double total_us = end_us - start_us;
  const double raw_step = total_us / 7.0;
  const double mag = std::pow(10.0, std::floor(std::log10(raw_step)));
  double tick_us = mag;
  if (raw_step / mag >= 5.0) {
    tick_us = mag * 5.0;
  } else if (raw_step / mag >= 2.0) {
    tick_us = mag * 2.0;
  }
  const double first_tick = std::ceil(start_us / tick_us) * tick_us;
  for (double t = first_tick; t <= end_us + tick_us * 0.01; t += tick_us) {
    const int x_buf =
        static_cast<int>(std::round(t / us_per_sample_)) - active_video_start_;
    if (x_buf < 0 || x_buf >= x_samples_) continue;
    const int px = pa.left() + x_buf * pa.width() / x_samples_;
    if (px < pa.left() || px > pa.right()) continue;
    painter.drawLine(px, pa.top(), px, pa.bottom());
  }
}

void WaveformMonitorWidget::drawXAxis(QPainter& painter,
                                      const QRect& pa) const {
  const QColor axis_color = displayAxis();
  painter.setPen(axis_color);

  // Axis baseline.
  painter.drawLine(pa.left(), pa.bottom(), pa.right(), pa.bottom());

  if (x_samples_ <= 0) return;

  const QFontMetrics fm(painter.font());

  // Tick positions in µs, snapped to a nice interval for ~7 ticks.
  const double start_us = active_video_start_ * us_per_sample_;
  const double end_us = (active_video_start_ + x_samples_) * us_per_sample_;
  const double total_us = end_us - start_us;
  const double raw_step = total_us / 7.0;
  const double mag = std::pow(10.0, std::floor(std::log10(raw_step)));
  double tick_us = mag;
  if (raw_step / mag >= 5.0) {
    tick_us = mag * 5.0;
  } else if (raw_step / mag >= 2.0) {
    tick_us = mag * 2.0;
  }
  const double first_tick = std::ceil(start_us / tick_us) * tick_us;
  for (double t = first_tick; t <= end_us + tick_us * 0.01; t += tick_us) {
    const int x_buf =
        static_cast<int>(std::round(t / us_per_sample_)) - active_video_start_;
    if (x_buf < 0 || x_buf >= x_samples_) continue;
    const int px = pa.left() + x_buf * pa.width() / x_samples_;
    if (px < pa.left() || px > pa.right()) continue;

    painter.drawLine(px, pa.bottom(), px, pa.bottom() + 4);

    const QString label = QString::number(t, 'f', 1);
    const int label_w = fm.horizontalAdvance(label);
    const QRect lr(px - label_w / 2, pa.bottom() + 5, label_w + 2, fm.height());
    painter.drawText(lr, Qt::AlignHCenter | Qt::AlignTop, label);
  }

  // X-axis title centered below the tick labels.
  const QString title = QString::fromUtf8("Time (µs)");
  const int title_y = pa.bottom() + 5 + fm.height() + 2;
  painter.drawText(QRect(pa.left(), title_y, pa.width(), fm.height()),
                   Qt::AlignHCenter | Qt::AlignTop, title);
}

void WaveformMonitorWidget::drawLevelMarkers(QPainter& painter,
                                             const QRect& pa) const {
  if (!video_params_.has_value()) return;
  const auto& vp = *video_params_;
  if (vp.blanking_level < 0 || vp.white_level <= vp.blanking_level) return;

  const orc::VideoSystem sys = toOrcVideoSystem(vp.system);

  struct LevelSpec {
    int32_t raw;
    const char* name;
    Qt::PenStyle style;
    qreal alpha;
  };

  const LevelSpec levels[] = {
      {vp.sync_tip_level, "Sync tip", Qt::DashLine, 0.60},
      {vp.blanking_level, "Blanking", Qt::DashLine, 0.35},
      {vp.black_level == vp.blanking_level ? -1 : vp.black_level, "Black",
       Qt::DashDotLine, 0.50},
      {vp.white_level, "White", Qt::DashLine, 0.70},
      {vp.peak_level, "Peak", Qt::DashLine, 0.55},
  };

  for (const auto& lv : levels) {
    if (lv.raw < 0) continue;

    const double mv =
        orc::samples10_to_mv(lv.raw, vp.blanking_level, vp.white_level, sys);
    const int py = mvToPixelY(mv, pa);
    if (py < pa.top() || py > pa.bottom()) continue;

    QColor lc;
    if (phosphor_mode_) {
      lc = QColor(255, 255, 255, static_cast<int>(255.0 * lv.alpha));
    } else {
      lc = theme_tokens::neutralLine(palette(), lv.alpha);
    }
    painter.setPen(QPen(lc, 1, lv.style));
    painter.drawLine(pa.left(), py, pa.right(), py);

    const std::string amp = orc::format_amplitude(
        lv.raw, vp.blanking_level, vp.white_level, sys, amplitude_unit_);
    painter.setPen(lc);
    painter.drawText(
        pa.left() + 5, py - 3,
        QString::fromStdString(std::string(lv.name) + " (" + amp + ")"));
  }
}

void WaveformMonitorWidget::setAmplitudeUnit(orc::AmplitudeDisplayUnit unit) {
  if (amplitude_unit_ == unit) return;
  amplitude_unit_ = unit;
  image_dirty_ = true;
  update();
}
