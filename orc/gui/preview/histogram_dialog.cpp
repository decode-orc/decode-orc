/*
 * File:        histogram_dialog.cpp
 * Module:      orc-gui
 * Purpose:     Video histogram visualization dialog implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "histogram_dialog.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "../logging.h"
#include "../plotwidget.h"
#include "../theme_color_tokens.h"

namespace {

constexpr double kNtscBlackPedestalPercent = 7.5;
constexpr double kZoneHeight = 1.0e9;
constexpr int kZoneAlpha = 35;
constexpr int kPedestalZoneAlpha = 25;
constexpr int kSeriesFillAlpha = 70;
constexpr int kSeriesPenAlpha = 180;
constexpr double kYHeadroomFactor = 1.20;
constexpr double kXTickStep = 10.0;

constexpr int kComboIndexY = 0;
constexpr int kComboIndexYUV = 1;
constexpr int kComboIndexYIQ = 2;

QVector<QPointF> buildHistogramPath(
    const std::array<uint32_t, orc::VideoHistogramData::kBinCount>& bins,
    double range_min, double range_max) {
  constexpr size_t n = orc::VideoHistogramData::kBinCount;
  const double bin_width = (range_max - range_min) / static_cast<double>(n);

  QVector<QPointF> pts;
  pts.reserve(static_cast<int>(n) * 2 + 4);
  pts.append(QPointF(range_min, 0.0));
  for (size_t i = 0; i < n; ++i) {
    const double x0 = range_min + static_cast<double>(i) * bin_width;
    pts.append(QPointF(x0, static_cast<double>(bins[i])));
    pts.append(QPointF(x0 + bin_width, static_cast<double>(bins[i])));
  }
  pts.append(QPointF(range_max, 0.0));
  pts.append(QPointF(range_min, 0.0));
  return pts;
}

QVector<QPointF> buildZonePath(double x_start, double x_end) {
  return {
      QPointF(x_start, 0.0),       QPointF(x_start, kZoneHeight),
      QPointF(x_end, kZoneHeight), QPointF(x_end, 0.0),
      QPointF(x_start, 0.0),
  };
}

PlotWidget* makePlot(QWidget* parent) {
  auto* p = new PlotWidget(parent);
  p->setAxisTitle(Qt::Horizontal, "Signal Level (%)");
  p->setAxisRange(Qt::Horizontal, orc::VideoHistogramData::kRangeMin,
                  orc::VideoHistogramData::kRangeMax);
  p->setAxisAutoScale(Qt::Horizontal, false);
  p->setAxisAutoScale(Qt::Vertical, false);
  p->setAxisTickStep(Qt::Horizontal, kXTickStep, 0.0);
  p->setYAxisIntegerLabels(true);
  p->setYAxisAbbreviatedLabels(true);
  p->setGridEnabled(true);
  p->setLegendEnabled(true);
  p->setZoomEnabled(true);
  p->setPanEnabled(true);
  return p;
}

}  // namespace

// ============================================================================

HistogramDialog::HistogramDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("Video Histogram");
  setWindowFlags(Qt::Window);
  setAttribute(Qt::WA_DeleteOnClose, false);
  resize(900, 650);
  setupUI();
}

HistogramDialog::~HistogramDialog() = default;

void HistogramDialog::setupUI() {
  auto* main_layout = new QVBoxLayout(this);

  info_label_ = new QLabel(this);
  info_label_->setStyleSheet("font-weight: bold;");
  main_layout->addWidget(info_label_);

  primary_plot_ = makePlot(this);
  primary_plot_->setAxisTitle(Qt::Vertical, "Y — Pixel Count");
  main_layout->addWidget(primary_plot_, 1);

  secondary_plot_ = makePlot(this);
  main_layout->addWidget(secondary_plot_, 1);

  tertiary_plot_ = makePlot(this);
  main_layout->addWidget(tertiary_plot_, 1);

  // Control row — label + combo, matches line-scope style.
  auto* control_row = new QHBoxLayout();
  auto* channel_label = new QLabel("Channels:", this);
  channel_combo_ = new QComboBox(this);
  channel_combo_->addItem("Y");
  channel_combo_->addItem("YUV");
  channel_combo_->addItem("YIQ");
  channel_combo_->setCurrentIndex(kComboIndexYUV);
  control_row->addWidget(channel_label);
  control_row->addWidget(channel_combo_);
  control_row->addStretch();
  main_layout->addLayout(control_row);

  connect(channel_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &HistogramDialog::onChannelSelectionChanged);

  // Start in YUV mode (three plots visible).
  setThreePlotMode(true);
}

HistogramDialog::ChannelMode HistogramDialog::currentChannelMode() const {
  switch (channel_combo_->currentIndex()) {
    case kComboIndexY:
      return ChannelMode::Y;
    case kComboIndexYIQ:
      return ChannelMode::YIQ;
    default:
      return ChannelMode::YUV;
  }
}

void HistogramDialog::setThreePlotMode(bool three_plots) {
  secondary_plot_->setVisible(three_plots);
  tertiary_plot_->setVisible(three_plots);
}

// ----------------------------------------------------------------------------

void HistogramDialog::updateHistogram(const orc::VideoHistogramData& data) {
  last_data_ = data;

  const bool is_ntsc = (data.system == orc::VideoSystem::NTSC);

  // Disable the YIQ entry for PAL sources.
  auto* model = qobject_cast<QStandardItemModel*>(channel_combo_->model());
  if (model) {
    auto* yiq_item = model->item(kComboIndexYIQ);
    if (yiq_item) {
      Qt::ItemFlags flags = yiq_item->flags();
      if (is_ntsc) {
        flags |= Qt::ItemIsEnabled;
      } else {
        flags &= ~Qt::ItemIsEnabled;
      }
      yiq_item->setFlags(flags);
    }
  }

  // If a PAL frame arrives while YIQ is selected, fall back to YUV.
  // The index change will trigger onChannelSelectionChanged → rebuildPlots.
  if (!is_ntsc && channel_combo_->currentIndex() == kComboIndexYIQ) {
    channel_combo_->setCurrentIndex(kComboIndexYUV);
    return;
  }

  const QString system_str = is_ntsc ? "NTSC" : "PAL";
  info_label_->setText(QString("Field %1 — %2\xC3\x97%3 — %4")
                           .arg(data.field_number + 1)
                           .arg(data.width)
                           .arg(data.height)
                           .arg(system_str));

  rebuildPlots();
}

void HistogramDialog::clearDisplay() {
  last_data_.reset();
  for (auto* p : {primary_plot_, secondary_plot_, tertiary_plot_}) {
    p->clearSeries();
    p->clearMarkers();
    p->showNoDataMessage("No histogram data available");
    p->replot();
  }
  // The plots have just deleted every item, so the remembered trace pointers
  // are dangling: the next update must build its furniture again.
  primary_furniture_ = PlotFurniture{};
  secondary_furniture_ = PlotFurniture{};
  tertiary_furniture_ = PlotFurniture{};
  info_label_->setText("No data");
}

// ----------------------------------------------------------------------------

void HistogramDialog::rebuildPlots() {
  if (!last_data_.has_value()) return;

  const auto& data = *last_data_;
  const bool is_ntsc = (data.system == orc::VideoSystem::NTSC);
  const bool dark = PlotWidget::isDarkTheme();
  const ChannelMode mode = currentChannelMode();

  const bool three_plots = (mode != ChannelMode::Y);
  setThreePlotMode(three_plots);

  // Always populate the primary (Y / luma) plot.
  QColor y_color =
      theme_tokens::plotColor(theme_tokens::PlotColorToken::LumaPrimary, dark);
  primary_plot_->setAxisTitle(Qt::Vertical, "Y — Pixel Count");
  populatePlot(primary_plot_, primary_furniture_, "Y (Luma)", y_color,
               data.y_bins, is_ntsc, /*is_chroma=*/false, dark);

  if (mode == ChannelMode::YUV) {
    QColor u_color = theme_tokens::plotColor(
        theme_tokens::PlotColorToken::ChromaPrimary, dark);
    secondary_plot_->setAxisTitle(Qt::Vertical, "U — Pixel Count");
    populatePlot(secondary_plot_, secondary_furniture_, "U (Cb)", u_color,
                 data.u_bins, is_ntsc, /*is_chroma=*/true, dark);

    QColor v_color = dark ? QColor(255, 90, 90) : QColor(200, 30, 30);
    tertiary_plot_->setAxisTitle(Qt::Vertical, "V — Pixel Count");
    populatePlot(tertiary_plot_, tertiary_furniture_, "V (Cr)", v_color,
                 data.v_bins, is_ntsc, /*is_chroma=*/true, dark);

  } else if (mode == ChannelMode::YIQ && is_ntsc) {
    QColor i_color = dark ? QColor(255, 160, 50) : QColor(200, 100, 0);
    secondary_plot_->setAxisTitle(Qt::Vertical, "I — Pixel Count");
    populatePlot(secondary_plot_, secondary_furniture_, "I", i_color,
                 data.i_bins, is_ntsc, /*is_chroma=*/true, dark);

    QColor q_color = dark ? QColor(200, 100, 255) : QColor(130, 0, 200);
    tertiary_plot_->setAxisTitle(Qt::Vertical, "Q — Pixel Count");
    populatePlot(tertiary_plot_, tertiary_furniture_, "Q", q_color, data.q_bins,
                 is_ntsc, /*is_chroma=*/true, dark);
  }

  ORC_LOG_DEBUG(
      "HistogramDialog: rebuilt plots for field {} ({}x{}, system={}, mode={})",
      data.field_number, data.width, data.height, static_cast<int>(data.system),
      static_cast<int>(mode));
}

void HistogramDialog::populatePlot(
    PlotWidget* plot, PlotFurniture& furniture, const QString& series_title,
    const QColor& color,
    const std::array<uint32_t, orc::VideoHistogramData::kBinCount>& bins,
    bool is_ntsc, bool is_chroma, bool dark) {
  plot->clearNoDataMessage();

  const double range_min = is_chroma ? orc::VideoHistogramData::kChromaRangeMin
                                     : orc::VideoHistogramData::kRangeMin;
  const double range_max = is_chroma ? orc::VideoHistogramData::kChromaRangeMax
                                     : orc::VideoHistogramData::kRangeMax;

  // The zones, guide lines and the trace item depend on none of what changes
  // from frame to frame, so they are built once and kept. Only a channel that
  // has become a different kind, a change of video system, or a theme switch
  // (the zone and guide colours are drawn from the palette) needs them again.
  const bool furniture_stale =
      !furniture.built || furniture.is_chroma != is_chroma ||
      furniture.is_ntsc != is_ntsc || furniture.dark != dark;

  if (furniture_stale) {
    plot->clearSeries();
    plot->clearMarkers();

    plot->setAxisRange(Qt::Horizontal, range_min, range_max);

    if (is_chroma) {
      // Chroma overrange zones — signal outside ±100 % is clipped.
      addChromaOverrangeZones(plot);
    } else {
      // EBU R103 luma tolerance zones.
      addEbuR103Zones(plot, is_ntsc);
    }

    // Histogram series — closed staircase polygon with transparent fill.
    furniture.series = plot->addSeries(series_title);
    furniture.series->setStyle(PlotSeries::Lines);

    auto add_vline = [&](double x, Qt::PenStyle style, QColor line_color) {
      auto* m = plot->addMarker();
      m->setStyle(PlotMarker::VLine);
      m->setPosition(QPointF(x, 0.0));
      m->setPen(QPen(line_color, 1, style));
    };

    if (is_chroma) {
      // 0 % = neutral (no colour), ±100 % = full legal swing.
      add_vline(0.0, Qt::DashLine,
                theme_tokens::neutralLine(plot->palette(), 0.8));
      add_vline(-100.0, Qt::DashLine,
                theme_tokens::neutralLine(plot->palette(), 0.4));
      add_vline(100.0, Qt::DashLine,
                theme_tokens::neutralLine(plot->palette(), 0.4));
    } else {
      // 0 % = black, 100 % = white.
      add_vline(0.0, Qt::DashLine,
                theme_tokens::neutralLine(plot->palette(), 0.6));
      add_vline(100.0, Qt::DashLine,
                theme_tokens::neutralLine(plot->palette(), 0.6));
      if (is_ntsc) {
        add_vline(kNtscBlackPedestalPercent, Qt::DashDotLine,
                  QColor(255, 160, 50, 160));
      }
    }

    plot->setLegendEnabled(true);

    furniture.built = true;
    furniture.is_chroma = is_chroma;
    furniture.is_ntsc = is_ntsc;
    furniture.dark = dark;
  }

  // Per-frame work: the title and pen follow the selected channel (Y/U/V or
  // Y/I/Q reuse the same three plots), and the bins and vertical range are the
  // measurement itself.
  QColor pen_color = color;
  pen_color.setAlpha(kSeriesPenAlpha);
  QColor fill_color = color;
  fill_color.setAlpha(kSeriesFillAlpha);
  furniture.series->setTitle(series_title);
  furniture.series->setPen(QPen(pen_color, 1));
  furniture.series->setBrush(QBrush(fill_color));
  furniture.series->setData(buildHistogramPath(bins, range_min, range_max));

  // Auto-scale Y axis to the peak bin with headroom.
  uint32_t max_count = 1;
  for (uint32_t v : bins) {
    if (v > max_count) max_count = v;
  }
  const double y_max =
      std::ceil(static_cast<double>(max_count) * kYHeadroomFactor);
  plot->setAxisRange(Qt::Vertical, 0.0, y_max);

  plot->replot();
}

void HistogramDialog::addEbuR103Zones(PlotWidget* plot, bool is_ntsc) {
  auto add_zone = [&](double x0, double x1, QColor color, double z) {
    PlotSeries* zone = plot->addSeries(QString{});
    zone->setStyle(PlotSeries::Lines);
    zone->setPen(QPen(Qt::NoPen));
    zone->setBrush(QBrush(color));
    zone->setZValue(z);
    zone->setData(buildZonePath(x0, x1));
  };

  add_zone(orc::VideoHistogramData::kRangeMin, 0.0,
           QColor(220, 60, 60, kZoneAlpha), -2.0);

  if (is_ntsc) {
    add_zone(0.0, kNtscBlackPedestalPercent,
             QColor(255, 160, 50, kPedestalZoneAlpha), -1.5);
  }

  add_zone(100.0, orc::VideoHistogramData::kRangeMax,
           QColor(220, 60, 60, kZoneAlpha), -2.0);
}

void HistogramDialog::addChromaOverrangeZones(PlotWidget* plot) {
  auto add_zone = [&](double x0, double x1) {
    PlotSeries* zone = plot->addSeries(QString{});
    zone->setStyle(PlotSeries::Lines);
    zone->setPen(QPen(Qt::NoPen));
    zone->setBrush(QBrush(QColor(220, 60, 60, kZoneAlpha)));
    zone->setZValue(-2.0);
    zone->setData(buildZonePath(x0, x1));
  };

  add_zone(orc::VideoHistogramData::kChromaRangeMin, -100.0);
  add_zone(100.0, orc::VideoHistogramData::kChromaRangeMax);
}

void HistogramDialog::onChannelSelectionChanged(int /*index*/) {
  if (last_data_.has_value()) rebuildPlots();
}

void HistogramDialog::closeEvent(QCloseEvent* event) {
  emit closed();
  QDialog::closeEvent(event);
}
