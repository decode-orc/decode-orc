/*
 * File:        waveformmonitordialog.cpp
 * Module:      orc-gui
 * Purpose:     Waveform monitor dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "waveformmonitordialog.h"

#include <orc/stage/cvbs_signal_constants.h>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdint>
#include <utility>

#include "waveformmonitorwidget.h"

// Intensity slider: range 1–100 maps to gain 0.1–10.0 (divide by 10).
static constexpr int kSliderMin = 1;
static constexpr int kSliderMax = 100;
static constexpr int kSliderDefault = 30;  // 3.0×
static constexpr double kSliderScale = 10.0;

// Combo box item indices must match WaveformChannel enum order.
static constexpr int kChannelIndexYPlusC = 0;
static constexpr int kChannelIndexYOnly = 1;

// Range combo box item indices.
static constexpr int kRangeIndexActiveVideo = 0;
static constexpr int kRangeIndexWholeFrame = 1;

// Field combo box item indices must match WaveformFieldSelection enum order.
static constexpr int kFieldIndexFrame = 0;
static constexpr int kFieldIndexField1 = 1;
static constexpr int kFieldIndexField2 = 2;

WaveformMonitorDialog::WaveformMonitorDialog(QWidget* parent)
    : QDialog(parent),
      monitor_widget_(nullptr),
      channel_combo_(nullptr),
      range_combo_(nullptr),
      field_combo_(nullptr),
      phosphor_check_(nullptr),
      gain_slider_(nullptr),
      gain_value_label_(nullptr) {
  setWindowFlags(Qt::Window);
  setModal(false);
  setAttribute(Qt::WA_DeleteOnClose, false);
  setWindowTitle("Waveform Monitor");

  setupUI();

  QSettings settings;
  const QByteArray geom =
      settings.value("WaveformMonitorDialog/geometry").toByteArray();
  if (!geom.isEmpty()) {
    restoreGeometry(geom);
  } else {
    resize(900, 500);
  }

  const bool phosphor =
      settings.value("WaveformMonitorDialog/phosphorMode", false).toBool();
  phosphor_check_->setChecked(phosphor);
  monitor_widget_->setPhosphorMode(phosphor);
}

WaveformMonitorDialog::~WaveformMonitorDialog() {
  QSettings settings;
  settings.setValue("WaveformMonitorDialog/geometry", saveGeometry());
  if (phosphor_check_) {
    settings.setValue("WaveformMonitorDialog/phosphorMode",
                      phosphor_check_->isChecked());
  }
}

void WaveformMonitorDialog::setupUI() {
  auto* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(4, 4, 4, 4);
  main_layout->setSpacing(4);

  // Raster widget fills the available space.
  monitor_widget_ = new WaveformMonitorWidget(this);
  main_layout->addWidget(monitor_widget_, 1);

  // Controls row at the bottom of the dialog.
  auto* controls = new QHBoxLayout();

  // Channel selector: Y+C (composite) or Y-only (luma).
  controls->addWidget(new QLabel("Channel:"));
  channel_combo_ = new QComboBox();
  channel_combo_->addItem("Y+C (Composite)", kChannelIndexYPlusC);
  channel_combo_->addItem("Y (Luma only)", kChannelIndexYOnly);
  channel_combo_->setCurrentIndex(kChannelIndexYOnly);
  channel_combo_->setToolTip(
      "Y+C: full composite signal (composite source) or summed Y+C (Y/C "
      "source).\n"
      "Y: luma only — separate Y channel when available, otherwise the "
      "composite signal low-pass filtered to remove the colour subcarrier.");
  controls->addWidget(channel_combo_);
  controls->addSpacing(12);

  // Range selector: visible (active video) region or whole frame.
  controls->addWidget(new QLabel("Range:"));
  range_combo_ = new QComboBox();
  range_combo_->addItem("Active video", kRangeIndexActiveVideo);
  range_combo_->addItem("Whole frame", kRangeIndexWholeFrame);
  range_combo_->setCurrentIndex(kRangeIndexActiveVideo);
  range_combo_->setToolTip(
      "Active video: accumulate only the visible portion of each line "
      "(blanking excluded).\n"
      "Whole frame: accumulate all samples including sync and blanking.");
  controls->addWidget(range_combo_);
  controls->addSpacing(12);

  // Field selector: whole frame or a single field.  The colour subcarrier
  // phase alternates between fields, so a per-field trace tells field-
  // correlated interference apart from random noise.
  controls->addWidget(new QLabel("Field:"));
  field_combo_ = new QComboBox();
  field_combo_->setObjectName("waveform_field_combo");
  field_combo_->addItem("Frame", kFieldIndexFrame);
  field_combo_->addItem("Field 1", kFieldIndexField1);
  field_combo_->addItem("Field 2", kFieldIndexField2);
  field_combo_->setCurrentIndex(kFieldIndexFrame);
  field_combo_->setToolTip(
      "Frame: accumulate the lines of both fields together.\n"
      "Field 1 / Field 2: accumulate one field only — the colour subcarrier "
      "phase alternates between fields, so comparing the two separates "
      "field-correlated interference from random noise.");
  controls->addWidget(field_combo_);
  controls->addSpacing(12);

  // Phosphor mode — green trace on black background.
  phosphor_check_ = new QCheckBox("Phosphor");
  phosphor_check_->setToolTip(
      "Display green trace on black background, emulating a classic "
      "analogue oscilloscope phosphor screen.");
  controls->addWidget(phosphor_check_);
  controls->addSpacing(12);

  controls->addWidget(new QLabel("Intensity:"));
  gain_slider_ = new QSlider(Qt::Horizontal);
  gain_slider_->setRange(kSliderMin, kSliderMax);
  gain_slider_->setValue(kSliderDefault);
  gain_slider_->setTickPosition(QSlider::NoTicks);
  gain_slider_->setToolTip(
      "Intensity gain — higher values brighten sparse signals and reach "
      "saturation sooner; lower values reduce saturation in uniform scenes.");
  controls->addWidget(gain_slider_, 1);
  gain_value_label_ = new QLabel();
  gain_value_label_->setMinimumWidth(40);
  gain_value_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  controls->addWidget(gain_value_label_);

  main_layout->addLayout(controls);

  // Initialise gain from the default slider position.
  const double initial_gain = kSliderDefault / kSliderScale;
  monitor_widget_->setGain(initial_gain);
  gain_value_label_->setText(QString::number(initial_gain, 'f', 1) +
                             QString::fromUtf8("×"));

  connect(gain_slider_, &QSlider::valueChanged, this, [this](int v) {
    const double gain = v / kSliderScale;
    monitor_widget_->setGain(gain);
    gain_value_label_->setText(QString::number(gain, 'f', 1) +
                               QString::fromUtf8("×"));
  });

  connect(channel_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, [this](int) { updateWidgetForCurrentChannel(); });

  connect(range_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, [this](int) { updateWidgetForCurrentChannel(); });

  connect(field_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, [this](int) { updateWidgetForCurrentChannel(); });

  updateFieldComboAvailability();

  connect(phosphor_check_, &QCheckBox::toggled, this,
          [this](bool checked) { monitor_widget_->setPhosphorMode(checked); });
}

// ---------------------------------------------------------------------------
// Data ingestion
// ---------------------------------------------------------------------------

void WaveformMonitorDialog::setData(
    std::vector<int16_t> composite_samples, std::vector<int16_t> y_samples,
    std::vector<int16_t> c_samples, int first_field_height,
    int second_field_height,
    const std::optional<orc::presenters::VideoParametersView>& video_params) {
  composite_samples_ = std::move(composite_samples);
  y_samples_ = std::move(y_samples);
  c_samples_ = std::move(c_samples);
  first_field_height_ = first_field_height;
  second_field_height_ = second_field_height;
  video_params_ = video_params;

  updateFieldComboAvailability();
  updateWidgetForCurrentChannel();
}

// ---------------------------------------------------------------------------
// Channel switching
// ---------------------------------------------------------------------------

void WaveformMonitorDialog::updateWidgetForCurrentChannel() {
  const int ch_idx =
      channel_combo_ ? channel_combo_->currentIndex() : kChannelIndexYOnly;
  const WaveformChannel ch = (ch_idx == kChannelIndexYOnly)
                                 ? WaveformChannel::YOnly
                                 : WaveformChannel::YPlusC;

  const bool clip_to_active =
      !range_combo_ || range_combo_->currentIndex() == kRangeIndexActiveVideo;

  // Build video params for this render pass.  When "whole frame" is selected
  // the active-video sample bounds are cleared so the widget accumulates all
  // samples across each full line including sync and blanking.  Signal levels
  // and system are preserved for Y-axis mV mapping and level markers.
  std::optional<orc::presenters::VideoParametersView> display_params =
      video_params_;
  if (display_params.has_value() && !clip_to_active) {
    display_params->active_video_start = -1;
    display_params->active_video_end = -1;
  }

  // Determine the first active picture line (0-based field-line index) so
  // that VBI lines are excluded when "active video" range is selected.
  int first_active_line = 0;
  if (clip_to_active && video_params_.has_value()) {
    switch (video_params_->system) {
      case orc::presenters::VideoSystem::PAL:
      case orc::presenters::VideoSystem::SECAM:
        first_active_line = orc::kPalFirstActiveLine;
        break;
      case orc::presenters::VideoSystem::PAL_M:
        first_active_line = orc::kPalMFirstActiveLine;
        break;
      case orc::presenters::VideoSystem::NTSC:
      case orc::presenters::VideoSystem::Unknown:
        first_active_line = orc::kNtscFirstActiveLine;
        break;
    }
  }

  // Select and prepare the sample data according to the channel mode.
  std::vector<int16_t> raw_samples;

  if (ch == WaveformChannel::YPlusC) {
    if (!composite_samples_.empty()) {
      raw_samples = composite_samples_;
    } else if (!y_samples_.empty()) {
      if (!c_samples_.empty() && c_samples_.size() == y_samples_.size()) {
        // Y/C source — reconstruct composite by summing luma and chroma.
        raw_samples.resize(y_samples_.size());
        for (size_t i = 0; i < y_samples_.size(); ++i) {
          raw_samples[i] = static_cast<int16_t>(
              std::clamp(static_cast<int32_t>(y_samples_[i]) + c_samples_[i],
                         static_cast<int32_t>(INT16_MIN),
                         static_cast<int32_t>(INT16_MAX)));
        }
      } else {
        raw_samples = y_samples_;
      }
    }
  } else {
    if (!y_samples_.empty()) {
      raw_samples = y_samples_;
    } else if (!composite_samples_.empty()) {
      raw_samples = extractYFromComposite(composite_samples_);
    }
  }

  if (raw_samples.empty()) return;

  // Apply vertical line slicing: strip VBI lines from each field when in
  // active-video range mode.
  int f1 = first_field_height_;
  int f2 = second_field_height_;
  std::vector<int16_t> display_samples = std::move(raw_samples);
  if (first_active_line > 0) {
    display_samples =
        sliceToActiveLines(display_samples, f1, f2, first_active_line);
  }

  // Then reduce to a single field when one is selected.
  const WaveformFieldSelection field = currentFieldSelection();
  if (field != WaveformFieldSelection::Frame) {
    display_samples = sliceToField(display_samples, f1, f2, field);
  }

  monitor_widget_->setYOnlyMode(ch == WaveformChannel::YOnly);
  monitor_widget_->setData(display_samples, f1, f2, display_params);
}

// ---------------------------------------------------------------------------
// Field selection
// ---------------------------------------------------------------------------

WaveformFieldSelection WaveformMonitorDialog::currentFieldSelection() const {
  // Single-field data has no second field to pick from, so it always reads
  // as Frame regardless of what the (disabled) combo still shows.
  if (!field_combo_ || first_field_height_ <= 0 || second_field_height_ <= 0) {
    return WaveformFieldSelection::Frame;
  }
  switch (field_combo_->currentIndex()) {
    case kFieldIndexField1:
      return WaveformFieldSelection::Field1;
    case kFieldIndexField2:
      return WaveformFieldSelection::Field2;
    default:
      return WaveformFieldSelection::Frame;
  }
}

// Single-field preview output arrives with second_field_height == 0; there is
// no second field to choose from, so the selector is disabled and reset to
// Frame rather than silently showing the same trace for all three items.
void WaveformMonitorDialog::updateFieldComboAvailability() {
  if (!field_combo_) return;

  const bool both_fields_present =
      first_field_height_ > 0 && second_field_height_ > 0;
  if (!both_fields_present &&
      field_combo_->currentIndex() != kFieldIndexFrame) {
    // Blocked so the reset does not trigger a render with stale data; the
    // caller re-renders immediately afterwards.
    const QSignalBlocker blocker(field_combo_);
    field_combo_->setCurrentIndex(kFieldIndexFrame);
  }
  field_combo_->setEnabled(both_fields_present);
}

// Keep the selected field and drop the other.  The returned buffer describes
// one field, so field1_height becomes the selected field's line count and
// field2_height becomes zero.
std::vector<int16_t> WaveformMonitorDialog::sliceToField(
    const std::vector<int16_t>& samples, int& field1_height, int& field2_height,
    WaveformFieldSelection selection) {
  if (selection == WaveformFieldSelection::Frame) return samples;

  const int total_lines = field1_height + field2_height;
  if (total_lines <= 0 || samples.empty()) return samples;

  const int spl = static_cast<int>(samples.size()) / total_lines;
  if (spl <= 0) return samples;

  const int kept_height = (selection == WaveformFieldSelection::Field1)
                              ? field1_height
                              : field2_height;
  if (kept_height <= 0) return samples;

  const int line_offset =
      (selection == WaveformFieldSelection::Field1) ? 0 : field1_height;

  const auto* base = samples.data();
  std::vector<int16_t> result(
      base + static_cast<ptrdiff_t>(line_offset) * spl,
      base + static_cast<ptrdiff_t>(line_offset + kept_height) * spl);

  field1_height = kept_height;
  field2_height = 0;
  return result;
}

// ---------------------------------------------------------------------------
// Vertical line slicing — strip VBI lines from each field
// ---------------------------------------------------------------------------

// Remove the first |first_active_line| lines from field 1 and (if present)
// from field 2 in the flat sample buffer.  field1_height and field2_height are
// updated to reflect the reduced counts.  Returns the sliced sample vector;
// returns |samples| unchanged when first_active_line <= 0.
std::vector<int16_t> WaveformMonitorDialog::sliceToActiveLines(
    const std::vector<int16_t>& samples, int& field1_height, int& field2_height,
    int first_active_line) {
  if (first_active_line <= 0) return samples;

  const int total_lines = field1_height + field2_height;
  if (total_lines <= 0 || samples.empty()) return samples;

  const int spl = static_cast<int>(samples.size()) / total_lines;
  if (spl <= 0) return samples;

  const int skip1 = std::min(first_active_line, field1_height);
  const int new_f1 = field1_height - skip1;
  const int skip2 =
      (field2_height > 0) ? std::min(first_active_line, field2_height) : 0;
  const int new_f2 = (field2_height > 0) ? field2_height - skip2 : 0;

  if (new_f1 + new_f2 <= 0) return {};

  std::vector<int16_t> result;
  result.reserve(static_cast<size_t>(new_f1 + new_f2) *
                 static_cast<size_t>(spl));

  // Field 1 active lines.
  const auto* base = samples.data();
  result.insert(result.end(), base + static_cast<ptrdiff_t>(skip1) * spl,
                base + static_cast<ptrdiff_t>(field1_height) * spl);

  // Field 2 active lines (if present).
  if (field2_height > 0) {
    result.insert(
        result.end(),
        base + static_cast<ptrdiff_t>(field1_height + skip2) * spl,
        base + static_cast<ptrdiff_t>(field1_height + field2_height) * spl);
  }

  field1_height = new_f1;
  field2_height = new_f2;
  return result;
}

// ---------------------------------------------------------------------------
// Chroma extraction — 4-tap FIR (notch at fs/4)
// ---------------------------------------------------------------------------

// For 4FSC-sampled data the colour subcarrier sits exactly at fs/4.
// A 4-point moving average [0.25, 0.25, 0.25, 0.25] has a null at fs/4 and
// at all harmonics thereof, giving clean luma separation without a separate
// decode pass.
std::vector<int16_t> WaveformMonitorDialog::extractYFromComposite(
    const std::vector<int16_t>& composite) {
  const int n = static_cast<int>(composite.size());
  std::vector<int16_t> result(static_cast<size_t>(n), 0);
  // Leave the first and last two samples at zero to avoid boundary wrap.
  for (int i = 2; i < n - 1; ++i) {
    const int32_t sum = static_cast<int32_t>(composite[i - 2]) +
                        composite[i - 1] + composite[i] + composite[i + 1];
    result[static_cast<size_t>(i)] = static_cast<int16_t>(sum / 4);
  }
  return result;
}

void WaveformMonitorDialog::setAmplitudeUnit(orc::AmplitudeDisplayUnit unit) {
  if (monitor_widget_) {
    monitor_widget_->setAmplitudeUnit(unit);
  }
}
