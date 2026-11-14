/*
 * File:        framescopedialog.cpp
 * Module:      orc-gui
 * Purpose:     Frame scope dialog for viewing CVBS_U10_4FSC line samples
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "framescopedialog.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/view-support/theme_color_tokens.h>

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>
#include <cmath>

#include "line_numbering.h"
#include "logging.h"

// Convert the presenter-layer VideoSystem to the core orc::VideoSystem used by
// make_line_label().  The two enums are mirrors; this translation keeps the
// GUI layer decoupled from core types.
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

// QSettings key group for the line numbering mode preference.
static constexpr const char* kSettingsGroup = "FrameScopeDialog";
static constexpr const char* kNumberingModeKey = "NumberingMode";

// Combo-box index ↔ LineNumberingMode mapping (order must match setupUI())
static orc::LineNumberingMode indexToMode(int idx) {
  switch (idx) {
    case 1:
      return orc::LineNumberingMode::kFrameSequential1Based;
    case 2:
      return orc::LineNumberingMode::kFieldRelative;
    case 3:
      return orc::LineNumberingMode::kBroadcastInterlaced;
    default:
      return orc::LineNumberingMode::kFrameFlat0Based;
  }
}

static int modeToIndex(orc::LineNumberingMode mode) {
  switch (mode) {
    case orc::LineNumberingMode::kFrameSequential1Based:
      return 1;
    case orc::LineNumberingMode::kFieldRelative:
      return 2;
    case orc::LineNumberingMode::kBroadcastInterlaced:
      return 3;
    default:
      return 0;
  }
}

// ============================================================================
// Construction / destruction
// ============================================================================

FrameScopeDialog::FrameScopeDialog(QWidget* parent)
    : QDialog(parent),
      line_series_(nullptr),
      y_series_(nullptr),
      c_series_(nullptr),
      sample_marker_(nullptr),
      is_yc_source_(false) {
  setupUI();
  setWindowTitle("Frame Scope");
  setWindowFlags(Qt::Window);
  setAttribute(Qt::WA_DeleteOnClose, false);
  resize(900, 500);
  loadNumberingModePreference();
}

FrameScopeDialog::~FrameScopeDialog() = default;

void FrameScopeDialog::closeEvent(QCloseEvent* event) {
  QDialog::closeEvent(event);
  emit dialogClosed();
}

// ============================================================================
// UI setup
// ============================================================================

void FrameScopeDialog::setupUI() {
  auto* mainLayout = new QVBoxLayout(this);

  plot_widget_ = new orc::view::PlotWidget(this);
  plot_widget_->setAxisTitle(Qt::Horizontal, "Time (µs)");
  plot_widget_->setAxisTitle(
      Qt::Vertical,
      QString::fromStdString(orc::amplitude_axis_title(amplitude_unit_)));
  const auto [y_min_init, y_max_init] = orc::amplitude_display_range(
      0, 256, 844, 1023, orc::VideoSystem::PAL, amplitude_unit_);
  plot_widget_->setAxisRange(Qt::Vertical, y_min_init, y_max_init);
  plot_widget_->setYAxisIntegerLabels(false);
  plot_widget_->setGridEnabled(true);
  plot_widget_->setLegendEnabled(true);
  plot_widget_->setZoomEnabled(true);
  plot_widget_->setPanEnabled(true);
  mainLayout->addWidget(plot_widget_, 1);

  auto* controlRow = new QHBoxLayout();

  // Channel selector (YC sources only)
  channel_selector_label_ = new QLabel("Channel:");
  channel_selector_ = new QComboBox(this);
  channel_selector_->addItem("Luma (Y)", 0);
  channel_selector_->addItem("Chroma (C)", 1);
  channel_selector_->addItem("Both (Y & C)", 2);
  channel_selector_->addItem("Y+C", 3);
  channel_selector_->setCurrentIndex(2);
  channel_selector_->setVisible(false);
  channel_selector_label_->setVisible(false);
  connect(channel_selector_,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &FrameScopeDialog::onChannelSelectionChanged);
  controlRow->addWidget(channel_selector_label_);
  controlRow->addWidget(channel_selector_);

  // Line numbering mode selector
  numbering_mode_label_ = new QLabel("Line numbering:");
  numbering_mode_combo_ = new QComboBox(this);
  numbering_mode_combo_->addItem(
      "Frame flat (0-based)",
      static_cast<int>(orc::LineNumberingMode::kFrameFlat0Based));
  numbering_mode_combo_->addItem(
      "Frame sequential (1-based)",
      static_cast<int>(orc::LineNumberingMode::kFrameSequential1Based));
  numbering_mode_combo_->addItem(
      "Field relative",
      static_cast<int>(orc::LineNumberingMode::kFieldRelative));
  numbering_mode_combo_->addItem(
      "Broadcast interlaced",
      static_cast<int>(orc::LineNumberingMode::kBroadcastInterlaced));
  connect(numbering_mode_combo_,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &FrameScopeDialog::onNumberingModeChanged);
  controlRow->addWidget(numbering_mode_label_);
  controlRow->addWidget(numbering_mode_combo_);

  controlRow->addStretch();

  // Navigation buttons
  line_up_button_ = new QPushButton("↑ Up", this);
  line_up_button_->setToolTip("Move to previous line");
  line_up_button_->setAutoRepeat(true);
  line_up_button_->setAutoRepeatDelay(500);
  line_up_button_->setAutoRepeatInterval(100);
  connect(line_up_button_, &QPushButton::clicked, this,
          &FrameScopeDialog::onLineUp);
  controlRow->addWidget(line_up_button_);

  line_down_button_ = new QPushButton("↓ Down", this);
  line_down_button_->setToolTip("Move to next line");
  line_down_button_->setAutoRepeat(true);
  line_down_button_->setAutoRepeatDelay(500);
  line_down_button_->setAutoRepeatInterval(100);
  connect(line_down_button_, &QPushButton::clicked, this,
          &FrameScopeDialog::onLineDown);
  controlRow->addWidget(line_down_button_);

  controlRow->addSpacing(20);

  // Sample info readout
  sample_info_label_ = new QLabel(this);
  sample_info_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  QFont monoFont("Monospace");
  monoFont.setStyleHint(QFont::TypeWriter);
  sample_info_label_->setFont(monoFont);
  controlRow->addWidget(sample_info_label_);

  controlRow->addStretch();
  mainLayout->addLayout(controlRow);

  // Add series for composite/line data
  line_series_ = plot_widget_->addSeries("Composite");

  connect(plot_widget_, &orc::view::PlotWidget::plotClicked, this,
          &FrameScopeDialog::onPlotClicked);
  connect(plot_widget_, &orc::view::PlotWidget::plotDragged, this,
          &FrameScopeDialog::onPlotClicked);
}

// ============================================================================
// QSettings persistence
// ============================================================================

void FrameScopeDialog::saveNumberingModePreference() const {
  QSettings settings;
  settings.beginGroup(kSettingsGroup);
  settings.setValue(kNumberingModeKey, numbering_mode_combo_->currentIndex());
  settings.endGroup();
}

void FrameScopeDialog::loadNumberingModePreference() {
  QSettings settings;
  settings.beginGroup(kSettingsGroup);
  int idx = settings.value(kNumberingModeKey, 0).toInt();
  settings.endGroup();
  const QSignalBlocker blocker(numbering_mode_combo_);
  numbering_mode_combo_->setCurrentIndex(
      qBound(0, idx, numbering_mode_combo_->count() - 1));
}

// ============================================================================
// Line label formatting
// ============================================================================

QString FrameScopeDialog::formatLineLabel() const {
  if (!current_video_params_.has_value()) {
    return QString::number(current_frame_line_);
  }
  const orc::VideoSystem sys = toOrcVideoSystem(current_video_params_->system);
  const orc::LineNumberingMode mode =
      indexToMode(numbering_mode_combo_->currentIndex());
  const orc::LineLabel label =
      orc::make_line_label(current_frame_line_, sys, mode);
  return QString::fromStdString(label.display);
}

// ============================================================================
// Primary data entry point
// ============================================================================

void FrameScopeDialog::setFrameLineSamples(
    const QString& node_id, int stage_index, uint64_t frame_id,
    size_t frame_line, int sample_x, const std::vector<int16_t>& samples,
    const std::optional<orc::presenters::VideoParametersView>& video_params,
    int preview_image_width, int original_sample_x, int original_image_y,
    const std::vector<int16_t>& y_samples,
    const std::vector<int16_t>& c_samples) {
  const QSignalBlocker blocker(channel_selector_);

  current_node_id_ = node_id;
  current_stage_index_ = stage_index;
  current_frame_id_ = frame_id;
  current_frame_line_ = frame_line;
  current_sample_x_ = sample_x;
  original_sample_x_ = original_sample_x;
  original_image_y_ = original_image_y;
  preview_image_width_ = preview_image_width;
  current_samples_ = samples;
  current_y_samples_ = y_samples;
  current_c_samples_ = c_samples;
  current_video_params_ = video_params;

  is_yc_source_ = !y_samples.empty() && !c_samples.empty();
  channel_selector_->setVisible(is_yc_source_);
  channel_selector_label_->setVisible(is_yc_source_);

  // Window title: "Stage N – Frame M, Line L (SYSTEM)"
  QString system_suffix;
  if (video_params.has_value()) {
    switch (video_params->system) {
      case orc::presenters::VideoSystem::PAL:
        system_suffix = " (PAL)";
        break;
      case orc::presenters::VideoSystem::NTSC:
        system_suffix = " (NTSC)";
        break;
      case orc::presenters::VideoSystem::PAL_M:
        system_suffix = " (PAL-M)";
        break;
      default:
        break;
    }
  }
  const QString yc_suffix = is_yc_source_ ? " (YC)" : "";
  setWindowTitle(QString("Stage %1 – Frame %2, Line %3%4%5")
                     .arg(stage_index)
                     .arg(frame_id + 1)  // 1-based frame number for display
                     .arg(formatLineLabel())
                     .arg(system_suffix)
                     .arg(yc_suffix));

  ORC_LOG_DEBUG("FrameScopeDialog: frameID={}, frameLine={}, line_label={}",
                frame_id, frame_line, formatLineLabel().toStdString());

  if (samples.empty() && !is_yc_source_) {
    plot_widget_->showNoDataMessage("No data available for this line");
    line_series_ = nullptr;
    y_series_ = nullptr;
    c_series_ = nullptr;
    sample_marker_ = nullptr;
    sample_info_label_->setText("");
    line_up_button_->setEnabled(false);
    line_down_button_->setEnabled(false);
    return;
  }

  line_up_button_->setEnabled(true);
  line_down_button_->setEnabled(true);
  plot_widget_->clearNoDataMessage();

  updatePlotData();

  // Clamp sample_x to valid range
  int max_sample_index = static_cast<int>(samples.size()) - 1;
  if (is_yc_source_ && !y_samples.empty()) {
    max_sample_index = static_cast<int>(y_samples.size()) - 1;
  }
  if (max_sample_index >= 0) {
    current_sample_x_ = qBound(0, sample_x, max_sample_index);
  }

  updateSampleMarker(current_sample_x_);
  plot_widget_->replot();
}

// ============================================================================
// Plot data update
// ============================================================================

void FrameScopeDialog::updatePlotData() {
  if (!is_yc_source_) {
    if (current_samples_.empty()) return;
  } else {
    if (current_y_samples_.empty() && current_c_samples_.empty()) return;
  }

  int channel_mode = is_yc_source_ ? channel_selector_->currentIndex() : -1;

  // Get the single-channel pointer (null for dual/combined modes)
  const std::vector<int16_t>* display_samples = nullptr;
  if (is_yc_source_) {
    if (channel_mode == 0 && !current_y_samples_.empty()) {
      display_samples = &current_y_samples_;
    } else if (channel_mode == 1 && !current_c_samples_.empty()) {
      display_samples = &current_c_samples_;
    }
  } else {
    if (!current_samples_.empty()) {
      display_samples = &current_samples_;
    }
  }

  // Resolve signal-level parameters for amplitude conversion
  int32_t blanking_level = -1;
  int32_t white_level = -1;
  orc::VideoSystem vc_sys = orc::VideoSystem::Unknown;
  if (current_video_params_.has_value()) {
    const auto& vp = current_video_params_.value();
    blanking_level = vp.blanking_level;
    white_level = vp.white_level;
    vc_sys = toOrcVideoSystem(vp.system);
  }

  const bool have_10bit_levels =
      (blanking_level >= 0 && white_level > blanking_level);

  // Lambda: convert int16_t sample vector → mV QVector<QPointF> with µs X-axis
  auto convertSamplesToPoints =
      [&](const std::vector<int16_t>& samps) -> QVector<QPointF> {
    QVector<QPointF> points;
    points.reserve(static_cast<qsizetype>(samps.size()));

    double us_per_sample = 1.0;
    if (current_video_params_.has_value()) {
      const double sr = orc::sample_rate_from_system(
          toOrcVideoSystem(current_video_params_->system));
      if (sr > 0.0) us_per_sample = 1000000.0 / sr;
    }

    for (size_t i = 0; i < samps.size(); ++i) {
      double display_val;
      if (have_10bit_levels) {
        display_val = orc::samples10_to_display(
            samps[i], blanking_level, white_level, vc_sys, amplitude_unit_);
      } else {
        display_val = static_cast<double>(samps[i]);
      }
      points.append(
          QPointF(static_cast<double>(i) * us_per_sample, display_val));
    }
    return points;
  };

  const bool is_dark = orc::view::PlotWidget::isDarkTheme();

  if (is_yc_source_ && channel_mode == 3 && !current_y_samples_.empty() &&
      !current_c_samples_.empty()) {
    // Y+C combined: raw chroma is centred at its DC offset in CVBS domain
    // (512 for CVBS YC sources; TBC YC sources compute it from 32768 uint16
    // midpoint). Subtract chroma_dc_offset before adding luma and chroma so
    // the result sits at the correct blanking level.
    const int32_t chroma_dc = (current_video_params_.has_value() &&
                               current_video_params_->chroma_dc_offset >= 0)
                                  ? current_video_params_->chroma_dc_offset
                                  : 0;
    std::vector<int16_t> combined;
    combined.reserve(current_y_samples_.size());
    for (size_t i = 0; i < current_y_samples_.size(); ++i) {
      const int32_t val = static_cast<int32_t>(current_y_samples_[i]) +
                          static_cast<int32_t>(current_c_samples_[i]) -
                          chroma_dc;
      combined.push_back(static_cast<int16_t>(std::clamp(val, -32768, 32767)));
    }
    plot_widget_->setLegendEnabled(false);
    if (!line_series_) {
      line_series_ = plot_widget_->addSeries("Y+C");
    } else {
      line_series_->setTitle("Y+C");
    }
    if (y_series_) {
      plot_widget_->removeSeries(y_series_);
      y_series_ = nullptr;
    }
    if (c_series_) {
      plot_widget_->removeSeries(c_series_);
      c_series_ = nullptr;
    }
    line_series_->setPen(QPen(
        orc::view::theme_tokens::plotColor(
            orc::view::theme_tokens::PlotColorToken::CompositePrimary, is_dark),
        1));
    line_series_->setData(convertSamplesToPoints(combined));

  } else if (is_yc_source_ && channel_mode == 2 &&
             !current_y_samples_.empty() && !current_c_samples_.empty()) {
    plot_widget_->setLegendEnabled(true);
    if (!y_series_) y_series_ = plot_widget_->addSeries("Luma (Y)");
    if (!c_series_) c_series_ = plot_widget_->addSeries("Chroma (C)");
    if (line_series_) {
      plot_widget_->removeSeries(line_series_);
      line_series_ = nullptr;
    }
    y_series_->setPen(
        QPen(orc::view::theme_tokens::plotColor(
                 orc::view::theme_tokens::PlotColorToken::LumaPrimary, is_dark),
             1));
    c_series_->setPen(QPen(
        orc::view::theme_tokens::plotColor(
            orc::view::theme_tokens::PlotColorToken::ChromaPrimary, is_dark),
        1));
    y_series_->setData(convertSamplesToPoints(current_y_samples_));
    c_series_->setData(convertSamplesToPoints(current_c_samples_));

  } else if (display_samples && !display_samples->empty()) {
    plot_widget_->setLegendEnabled(false);
    QString label = "Composite";
    if (is_yc_source_) {
      label = (channel_mode == 0) ? "Luma (Y)" : "Chroma (C)";
    }
    if (!line_series_) {
      line_series_ = plot_widget_->addSeries(label);
    } else {
      line_series_->setTitle(label);
    }
    if (y_series_) {
      plot_widget_->removeSeries(y_series_);
      y_series_ = nullptr;
    }
    if (c_series_) {
      plot_widget_->removeSeries(c_series_);
      c_series_ = nullptr;
    }

    QColor line_color;
    if (is_yc_source_ && channel_mode == 0) {
      line_color = orc::view::theme_tokens::plotColor(
          orc::view::theme_tokens::PlotColorToken::LumaPrimary, is_dark);
    } else if (is_yc_source_ && channel_mode == 1) {
      line_color = orc::view::theme_tokens::plotColor(
          orc::view::theme_tokens::PlotColorToken::ChromaPrimary, is_dark);
    } else {
      line_color = orc::view::theme_tokens::plotColor(
          orc::view::theme_tokens::PlotColorToken::CompositePrimary, is_dark);
    }
    line_series_->setPen(QPen(line_color, 1));
    line_series_->setData(convertSamplesToPoints(*display_samples));

  } else {
    if (line_series_) {
      plot_widget_->removeSeries(line_series_);
      line_series_ = nullptr;
    }
    if (y_series_) {
      plot_widget_->removeSeries(y_series_);
      y_series_ = nullptr;
    }
    if (c_series_) {
      plot_widget_->removeSeries(c_series_);
      c_series_ = nullptr;
    }
    return;
  }

  // ---- Y-axis range: sync_tip … peak in selected display unit
  auto [y_min, y_max] = orc::amplitude_display_range(
      0, 256, 844, 1023, orc::VideoSystem::PAL, amplitude_unit_);

  if (have_10bit_levels && current_video_params_.has_value()) {
    const auto& vp = current_video_params_.value();
    std::tie(y_min, y_max) = orc::amplitude_display_range(
        vp.sync_tip_level, blanking_level, white_level, vp.peak_level, vc_sys,
        amplitude_unit_);
    // Add 5% headroom
    const double span = y_max - y_min;
    y_min -= span * 0.05;
    y_max += span * 0.05;
  }

  // ---- X-axis range
  size_t sample_count = 0;
  if (display_samples) {
    sample_count = display_samples->size();
  } else if (is_yc_source_) {
    sample_count = current_y_samples_.size();
  }
  double us_per_sample = 1.0;
  if (current_video_params_.has_value()) {
    const double sr = orc::sample_rate_from_system(
        toOrcVideoSystem(current_video_params_->system));
    if (sr > 0.0) us_per_sample = 1000000.0 / sr;
  }
  const double max_time_us =
      static_cast<double>(sample_count > 0 ? sample_count - 1 : 0) *
      us_per_sample;

  plot_widget_->setAxisRange(Qt::Horizontal, 0, max_time_us);
  plot_widget_->setAxisRange(Qt::Vertical, y_min, y_max);
  plot_widget_->setAxisTitle(
      Qt::Vertical,
      QString::fromStdString(orc::amplitude_axis_title(amplitude_unit_)));
  plot_widget_->setAxisAutoScale(Qt::Horizontal, false);
  plot_widget_->setAxisAutoScale(Qt::Vertical, false);
  plot_widget_->setAxisTickStep(Qt::Horizontal, 2.0, 0.0);  // 2 µs ticks
  plot_widget_->setAxisTickStep(
      Qt::Vertical, orc::amplitude_major_tick(amplitude_unit_), 0.0);

  // No secondary IRE axis — 10-bit domain renders in selected unit directly.
  plot_widget_->setSecondaryYAxisEnabled(false);

  // ---- Markers
  plot_widget_->clearMarkers();
  sample_marker_ = nullptr;

  if (current_video_params_.has_value()) {
    const auto& vp = current_video_params_.value();
    const double ups = us_per_sample;

    // Color burst region (cyan dashed verticals)
    if (vp.color_burst_start >= 0 && vp.color_burst_end >= 0) {
      for (int s : {vp.color_burst_start, vp.color_burst_end}) {
        auto* m = plot_widget_->addMarker();
        m->setStyle(orc::view::PlotMarker::VLine);
        m->setPosition(QPointF(static_cast<double>(s) * ups, 0));
        m->setPen(QPen(
            orc::view::theme_tokens::plotColor(
                orc::view::theme_tokens::PlotColorToken::RegionBurst, is_dark),
            1, Qt::DashLine));
      }
    }

    // Active video region (yellow dashed verticals)
    if (vp.active_video_start >= 0 && vp.active_video_end >= 0) {
      for (int s : {vp.active_video_start, vp.active_video_end}) {
        auto* m = plot_widget_->addMarker();
        m->setStyle(orc::view::PlotMarker::VLine);
        m->setPosition(QPointF(static_cast<double>(s) * ups, 0));
        m->setPen(
            QPen(orc::view::theme_tokens::plotColor(
                     orc::view::theme_tokens::PlotColorToken::RegionActiveVideo,
                     is_dark),
                 1, Qt::DashLine));
      }
    }

    // Five normative CVBS_U10_4FSC level markers (horizontal).
    // ITU-R BT.1700-1 / EBU Tech. 3280-E Table 1 / SMPTE 170M-2004 §11.4.
    if (have_10bit_levels) {
      struct LevelSpec {
        int32_t raw;
        const char* name;
        Qt::PenStyle style;
        double alpha;
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
        const double dv = orc::samples10_to_display(
            lv.raw, blanking_level, white_level, vc_sys, amplitude_unit_);
        const std::string amp = orc::format_amplitude(
            lv.raw, blanking_level, white_level, vc_sys, amplitude_unit_);
        auto* m = plot_widget_->addMarker();
        m->setStyle(orc::view::PlotMarker::HLine);
        m->setPosition(QPointF(0, dv));
        m->setLabel(
            QString::fromStdString(std::string(lv.name) + " (" + amp + ")"));
        m->setPen(
            QPen(orc::view::theme_tokens::neutralLine(palette(), lv.alpha), 1,
                 lv.style));
      }
    }
  }
}

// ============================================================================
// Sample marker update
// ============================================================================

void FrameScopeDialog::updateSampleMarker(int sample_x) {
  // Choose which sample vector to read the value from
  const std::vector<int16_t>* samples_for_marker = &current_samples_;
  if (is_yc_source_) {
    const int ch = channel_selector_->currentIndex();
    samples_for_marker = (ch == 1) ? &current_c_samples_ : &current_y_samples_;
  }

  if (samples_for_marker->empty()) return;

  if (sample_marker_) {
    plot_widget_->removeMarker(sample_marker_);
    sample_marker_ = nullptr;
  }

  if (sample_x < 0 ||
      sample_x >= static_cast<int>(samples_for_marker->size())) {
    sample_info_label_->setText("");
    return;
  }

  current_sample_x_ = sample_x;

  double us_per_sample = 1.0;
  if (current_video_params_.has_value()) {
    const double sr = orc::sample_rate_from_system(
        toOrcVideoSystem(current_video_params_->system));
    if (sr > 0.0) us_per_sample = 1000000.0 / sr;
  }
  const double time_us = static_cast<double>(sample_x) * us_per_sample;

  sample_marker_ = plot_widget_->addMarker();
  sample_marker_->setStyle(orc::view::PlotMarker::VLine);
  sample_marker_->setPosition(QPointF(time_us, 0));
  sample_marker_->setPen(
      QPen(orc::view::theme_tokens::plotColor(
               orc::view::theme_tokens::PlotColorToken::MarkerSelection,
               orc::view::PlotWidget::isDarkTheme()),
           2));

  // Info label — time + mV value(s)
  const int16_t sample_val = (*samples_for_marker)[sample_x];
  QString info = QString("Time: %1 µs  (Sample: %2)")
                     .arg(time_us, 0, 'f', 3)
                     .arg(sample_x);

  const bool have_levels = current_video_params_.has_value() &&
                           current_video_params_->blanking_level >= 0 &&
                           current_video_params_->white_level >
                               current_video_params_->blanking_level;

  if (is_yc_source_ && channel_selector_->currentIndex() == 2) {
    // Both mode: show Y and C in selected display unit
    if (sample_x < static_cast<int>(current_y_samples_.size()) &&
        sample_x < static_cast<int>(current_c_samples_.size())) {
      if (have_levels) {
        const auto& vp = current_video_params_.value();
        const orc::VideoSystem sys = toOrcVideoSystem(vp.system);
        const std::string y_str = orc::format_amplitude(
            current_y_samples_[sample_x], vp.blanking_level, vp.white_level,
            sys, amplitude_unit_);
        const std::string c_str = orc::format_amplitude(
            current_c_samples_[sample_x], vp.blanking_level, vp.white_level,
            sys, amplitude_unit_);
        info += QString::fromStdString("\nY: " + y_str + "  C: " + c_str);
      } else {
        info += QString("\nY: %1  C: %2")
                    .arg(current_y_samples_[sample_x])
                    .arg(current_c_samples_[sample_x]);
      }
    }
  } else if (have_levels) {
    const auto& vp = current_video_params_.value();
    const std::string amp =
        orc::format_amplitude(sample_val, vp.blanking_level, vp.white_level,
                              toOrcVideoSystem(vp.system), amplitude_unit_);
    info += QString::fromStdString(
        "\n" + amp +
        "  (10-bit: " + std::to_string(static_cast<int>(sample_val)) + ")");
  } else {
    info += QString("\n10-bit: %1").arg(static_cast<int>(sample_val));
  }

  sample_info_label_->setText(info);
  plot_widget_->replot();
}

void FrameScopeDialog::updateOriginalSampleXFromSampleIndex(int sample_index) {
  size_t sample_count = current_samples_.size();
  if (is_yc_source_ && !current_y_samples_.empty()) {
    sample_count = current_y_samples_.size();
  }
  if (preview_image_width_ > 0 && sample_count > 0) {
    original_sample_x_ =
        (sample_index * preview_image_width_) / static_cast<int>(sample_count);
  }
}

// ============================================================================
// Slot implementations
// ============================================================================

void FrameScopeDialog::onChannelSelectionChanged(int /*index*/) {
  if (!is_yc_source_) return;
  updatePlotData();
  updateSampleMarker(current_sample_x_);
  plot_widget_->replot();
}

void FrameScopeDialog::onNumberingModeChanged(int /*index*/) {
  saveNumberingModePreference();
  // Re-generate title with new numbering mode
  if (current_video_params_.has_value()) {
    QString system_suffix;
    switch (current_video_params_->system) {
      case orc::presenters::VideoSystem::PAL:
        system_suffix = " (PAL)";
        break;
      case orc::presenters::VideoSystem::NTSC:
        system_suffix = " (NTSC)";
        break;
      case orc::presenters::VideoSystem::PAL_M:
        system_suffix = " (PAL-M)";
        break;
      default:
        break;
    }
    const QString yc_suffix = is_yc_source_ ? " (YC)" : "";
    setWindowTitle(QString("Stage %1 – Frame %2, Line %3%4%5")
                       .arg(current_stage_index_)
                       .arg(current_frame_id_ + 1)
                       .arg(formatLineLabel())
                       .arg(system_suffix)
                       .arg(yc_suffix));
  }
}

void FrameScopeDialog::onPlotClicked(const QPointF& dataPoint) {
  double us_per_sample = 1.0;
  if (current_video_params_.has_value()) {
    const double sr = orc::sample_rate_from_system(
        toOrcVideoSystem(current_video_params_->system));
    if (sr > 0.0) us_per_sample = 1000000.0 / sr;
  }
  int new_sample_x = qRound(dataPoint.x() / us_per_sample);

  const std::vector<int16_t>* samples_to_check = &current_samples_;
  if (is_yc_source_) {
    const int ch = channel_selector_->currentIndex();
    samples_to_check = (ch == 1) ? &current_c_samples_ : &current_y_samples_;
  }
  if (samples_to_check->empty()) return;

  new_sample_x =
      qBound(0, new_sample_x, static_cast<int>(samples_to_check->size()) - 1);
  updateSampleMarker(new_sample_x);
  updateOriginalSampleXFromSampleIndex(new_sample_x);
  emit sampleMarkerMoved(new_sample_x);
}

void FrameScopeDialog::onLineUp() {
  const bool has_data = !current_samples_.empty() ||
                        !current_y_samples_.empty() ||
                        !current_c_samples_.empty();
  if (!has_data) return;
  emit lineNavigationRequested(-1, current_frame_id_, current_frame_line_,
                               original_sample_x_, preview_image_width_);
}

void FrameScopeDialog::onLineDown() {
  const bool has_data = !current_samples_.empty() ||
                        !current_y_samples_.empty() ||
                        !current_c_samples_.empty();
  if (!has_data) return;
  emit lineNavigationRequested(+1, current_frame_id_, current_frame_line_,
                               original_sample_x_, preview_image_width_);
}

void FrameScopeDialog::refreshSamples() {
  emit refreshRequested(original_sample_x_, original_image_y_);
}

void FrameScopeDialog::refreshSamplesAtCurrentPosition() {
  if (preview_image_width_ > 0) {
    emit refreshRequested(original_sample_x_, original_image_y_);
  }
}

void FrameScopeDialog::setAmplitudeUnit(orc::AmplitudeDisplayUnit unit) {
  if (amplitude_unit_ == unit) return;
  amplitude_unit_ = unit;
  const bool has_data = !current_samples_.empty() ||
                        !current_y_samples_.empty() ||
                        !current_c_samples_.empty();
  if (has_data) {
    updatePlotData();
    updateSampleMarker(current_sample_x_);
    plot_widget_->replot();
  } else {
    plot_widget_->setAxisTitle(
        Qt::Vertical,
        QString::fromStdString(orc::amplitude_axis_title(amplitude_unit_)));
    const auto [y_min, y_max] = orc::amplitude_display_range(
        0, 256, 844, 1023, orc::VideoSystem::PAL, amplitude_unit_);
    plot_widget_->setAxisRange(Qt::Vertical, y_min, y_max);
  }
}
