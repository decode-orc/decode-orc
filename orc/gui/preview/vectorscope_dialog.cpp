/*
 * File:        vectorscope_dialog.cpp
 * Module:      orc-gui
 * Purpose:     Vectorscope visualization dialog implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "vectorscope_dialog.h"

#include "../field_frame_presentation.h"
#include "../gpu/scope_surface_factory.h"
#include "../gpu/scope_vertex_builder.h"
#include "../logging.h"
#include "vectorscope_geometry.h"

// ============================================================================
// Private Implementation - Simple state, no core access
// ============================================================================
class VectorscopeDialogPrivate {
 public:
  orc::NodeID node_id;
  QString scope_label{"Vectorscope"};
  uint64_t current_field_number = 0;
  std::optional<orc::VectorscopeData> last_data;

  void drawColorZones(QPainter& painter, VectorscopeDialog* dialog,
                      orc::VideoSystem system, int32_t cvbs_white,
                      int32_t cvbs_blanking,
                      orc::VectorscopeAcquisitionMode mode);
  void drawGraticule(QPainter& painter, VectorscopeDialog* dialog,
                     orc::VideoSystem system, int32_t cvbs_white,
                     int32_t cvbs_blanking,
                     orc::VectorscopeAcquisitionMode mode);
  void drawBurstTargets(QPainter& painter,
                        const orc::gui::VectorscopePlotGeometry& geometry,
                        orc::VideoSystem system, bool switched_v);

  // The canvas the plot is drawn on when the policy allows one, and null when
  // this window plots on the CPU.  The CPU renderer is kept either way: it is
  // what a composite acquisition is still drawn with, and what the window
  // falls back to if the canvas fails.
  std::unique_ptr<orc::gui::gpu::IScopeSurface> surface;

  // Graticules change with the system, the levels, the acquisition and the
  // graticule choice - none of which move between displayed frames - so they
  // are painted once and handed back to the canvas until one of them does.
  struct GraticuleKey {
    orc::VideoSystem system = orc::VideoSystem::Unknown;
    int32_t cvbs_white = 0;
    int32_t cvbs_blanking = 0;
    orc::VectorscopeAcquisitionMode mode =
        orc::VectorscopeAcquisitionMode::DecodedComponent;
    int graticule_mode = 0;
    bool has_chroma = true;

    bool matches(const GraticuleKey& other) const {
      return system == other.system && cvbs_white == other.cvbs_white &&
             cvbs_blanking == other.cvbs_blanking && mode == other.mode &&
             graticule_mode == other.graticule_mode &&
             has_chroma == other.has_chroma;
    }
  };
  GraticuleKey graticule_key;
  bool graticule_valid = false;
  QImage canvas_underlay;
  QImage canvas_overlay;

  /// Repaint the two graticule images if anything they depend on has moved.
  void refreshCanvasGraticules(VectorscopeDialog* dialog,
                               const orc::VectorscopeData& data,
                               bool has_chroma);
};

#include <QCloseEvent>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <random>

namespace {

constexpr double kMajorMarkerLengthPixels = 18.0;
constexpr double kMinorMarkerLengthPixels = 10.0;
constexpr double kIqLabelOffsetPixels = 22.0;
constexpr double kColorLabelOffsetPixels = 48.0;
// NTSC I and Q axis angles in standard vectorscope degrees (0=right, 90=up,
// counterclockwise positive).
// SMPTE 170M-2004 §7.3: I and Q are rotated 33° from the V (R-Y) and U (B-Y)
// axes respectively.  In (U,V) space the positive-I direction is
// (-sin33°, cos33°) → atan2(cos33°, -sin33°) = 123°, and positive-Q is
// (cos33°, sin33°) → atan2(sin33°, cos33°) = 33°.
constexpr double kNtscIAxisStandardDegrees = 123.0;
constexpr double kNtscNegIAxisStandardDegrees = -57.0;
constexpr double kNtscQAxisStandardDegrees = 33.0;
constexpr double kNtscNegQAxisStandardDegrees = -147.0;
constexpr double kIqLabelAngularOffsetDegrees = 4.0;
constexpr double kZoneHalfAngleDegrees = 13.0;
constexpr double kZoneHalfRadialSpanPercent = 0.14;
constexpr double kTargetBoxSizePixels = 42.0;
constexpr double kTargetCrosshairSizePixels = 22.0;
constexpr double kMeasurementTargetBoxSizePixels = 30.0;
constexpr double kMeasurementCrosshairSizePixels = 16.0;
constexpr double kBurstLabelOffsetPixels = 40.0;

// Upper bound for the line-select spin boxes.  A PAL frame is the largest of
// the supported systems at 625 lines (EBU Tech. 3280-E §1.3.1); NTSC and PAL-M
// frames are shorter, and out-of-range values are clamped by the acquisition.
constexpr int kMaxSelectableLine = 625;

// Lines the composite measurement readout can occupy: burst amplitude,
// subcarrier jitter, contributing line count, PAL V-switch split error and
// chroma-to-burst ratio.  Keep in sync with updateMeasurementReadout().
constexpr int kMeasurementReadoutLines = 5;

QColor vectorscopeTargetColor(int rgb) {
  switch (rgb) {
    case 1:
      return QColor(70, 150, 255, 120);
    case 2:
      return QColor(70, 230, 120, 120);
    case 3:
      return QColor(90, 235, 235, 120);
    case 4:
      return QColor(255, 90, 90, 120);
    case 5:
      return QColor(230, 90, 230, 120);
    case 6:
      return QColor(245, 215, 80, 120);
    default:
      return QColor(255, 255, 255, 120);
  }
}

void drawNtcsIqLabels(QPainter& painter,
                      const orc::gui::VectorscopePlotGeometry& geometry) {
  struct AxisLabel {
    const char* text;
    double angle_degrees;
    double label_angle_offset_degrees;
  };

  const AxisLabel axis_labels[] = {
      {"I", kNtscIAxisStandardDegrees, kIqLabelAngularOffsetDegrees},
      {"Q", kNtscQAxisStandardDegrees, -kIqLabelAngularOffsetDegrees},
      {"-I", kNtscNegIAxisStandardDegrees, -kIqLabelAngularOffsetDegrees},
      {"-Q", kNtscNegQAxisStandardDegrees, kIqLabelAngularOffsetDegrees}};

  QFont font = painter.font();
  font.setPointSize(24);
  font.setBold(true);
  painter.setFont(font);
  painter.setPen(QPen(QColor(200, 200, 200), 1));

  const double label_radius_uv =
      orc::gui::kVectorscopeSignedFullScale -
      geometry.pixelsToMagnitude(kIqLabelOffsetPixels);

  for (const auto& axis_label : axis_labels) {
    const QPointF label_centre = geometry.pointFromStandardDegrees(
        axis_label.angle_degrees + axis_label.label_angle_offset_degrees,
        label_radius_uv);
    const QString text(axis_label.text);
    const QFontMetrics metrics(font);
    const QRect text_rect = metrics.boundingRect(text);

    painter.drawText(
        static_cast<int>(label_centre.x()) - (text_rect.width() / 2),
        static_cast<int>(label_centre.y()) + (text_rect.height() / 3), text);
  }
}

void drawReferenceAxis(QPainter& painter,
                       const orc::gui::VectorscopePlotGeometry& geometry,
                       double standard_angle_degrees) {
  painter.drawLine(
      geometry.pointFromStandardDegrees(
          standard_angle_degrees, 0.2 * orc::gui::kVectorscopeSignedFullScale),
      geometry.pointFromStandardDegrees(standard_angle_degrees,
                                        orc::gui::kVectorscopeSignedFullScale));
}

void drawCircleMarkers(QPainter& painter,
                       const orc::gui::VectorscopePlotGeometry& geometry) {
  const double outer_radius_uv = orc::gui::kVectorscopeSignedFullScale;
  const double major_marker_length_uv =
      geometry.pixelsToMagnitude(kMajorMarkerLengthPixels);
  const double minor_marker_length_uv =
      geometry.pixelsToMagnitude(kMinorMarkerLengthPixels);

  for (int degrees = 0; degrees < 360; degrees += 2) {
    const bool is_major_marker = (degrees % 10) == 0;
    const double marker_length_uv =
        is_major_marker ? major_marker_length_uv : minor_marker_length_uv;
    const double angle_radians = (static_cast<double>(degrees) * M_PI) / 180.0;

    painter.setPen(
        QPen(Qt::white, is_major_marker
                            ? orc::gui::kVectorscopeMajorMarkerStrokeWidth
                            : orc::gui::kVectorscopeMinorMarkerStrokeWidth));
    painter.drawLine(
        geometry.pointFromVectorscopeAngle(angle_radians,
                                           outer_radius_uv - marker_length_uv),
        geometry.pointFromVectorscopeAngle(angle_radians, outer_radius_uv));
  }
}

void drawColorZone(QPainter& painter,
                   const orc::gui::VectorscopePlotGeometry& geometry,
                   double angle_radians, double magnitude_uv,
                   const QColor& color) {
  const double zone_half_angle_radians = (kZoneHalfAngleDegrees * M_PI) / 180.0;
  const double radial_span_uv = magnitude_uv * kZoneHalfRadialSpanPercent;
  const double inner_radius_uv = std::max(0.0, magnitude_uv - radial_span_uv);
  const double outer_radius_uv = std::min(orc::gui::kVectorscopeSignedFullScale,
                                          magnitude_uv + radial_span_uv);

  QPainterPath zone_path;
  zone_path.moveTo(geometry.pointFromVectorscopeAngle(
      angle_radians - zone_half_angle_radians, inner_radius_uv));

  for (int step = 0; step <= 12; ++step) {
    const double t = static_cast<double>(step) / 12.0;
    const double arc_angle = angle_radians - zone_half_angle_radians +
                             (t * zone_half_angle_radians * 2.0);
    zone_path.lineTo(
        geometry.pointFromVectorscopeAngle(arc_angle, outer_radius_uv));
  }

  for (int step = 12; step >= 0; --step) {
    const double t = static_cast<double>(step) / 12.0;
    const double arc_angle = angle_radians - zone_half_angle_radians +
                             (t * zone_half_angle_radians * 2.0);
    zone_path.lineTo(
        geometry.pointFromVectorscopeAngle(arc_angle, inner_radius_uv));
  }

  zone_path.closeSubpath();

  QColor fill_color = color;
  fill_color.setAlpha(52);
  QColor outline_color = color;
  outline_color.setAlpha(180);

  painter.save();
  painter.setPen(QPen(outline_color, 1));
  painter.setBrush(fill_color);
  painter.drawPath(zone_path);
  painter.restore();
}

void drawTargetBox(QPainter& painter,
                   const orc::gui::VectorscopePlotGeometry& geometry,
                   const QPointF& centre, const QColor& color,
                   double box_size_pixels = kTargetBoxSizePixels,
                   double crosshair_size_pixels = kTargetCrosshairSizePixels) {
  const double half_box = box_size_pixels / 2.0;
  const double half_crosshair = crosshair_size_pixels / 2.0;
  const QRectF box_rect(centre.x() - half_box, centre.y() - half_box,
                        box_size_pixels, box_size_pixels);

  QColor outline_color = color;
  outline_color.setAlpha(220);

  painter.save();
  painter.setPen(QPen(outline_color, 2));
  painter.setBrush(Qt::NoBrush);
  painter.drawRect(box_rect);
  painter.drawLine(QPointF(centre.x() - half_crosshair, centre.y()),
                   QPointF(centre.x() + half_crosshair, centre.y()));
  painter.drawLine(QPointF(centre.x(), centre.y() - half_crosshair),
                   QPointF(centre.x(), centre.y() + half_crosshair));
  painter.restore();
}

// Bresenham line rasteriser — increments hit_count for every pixel on the
// segment from (x0,y0) to (x1,y1), clamped to the canvas bounds.
void accumulateLine(std::vector<uint32_t>& buf, int canvas_size, int x0, int y0,
                    int x1, int y1) {
  const int dx = std::abs(x1 - x0);
  const int sx = (x0 < x1) ? 1 : -1;
  const int dy = -std::abs(y1 - y0);
  const int sy = (y0 < y1) ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    if (x0 >= 0 && x0 < canvas_size && y0 >= 0 && y0 < canvas_size) {
      buf[static_cast<size_t>(y0) * static_cast<size_t>(canvas_size) +
          static_cast<size_t>(x0)]++;
    }
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

}  // namespace

// ============================================================================
// AspectRatioLabel Implementation
// ============================================================================

AspectRatioLabel::AspectRatioLabel(QWidget* parent) : QLabel(parent) {
  setAlignment(Qt::AlignCenter);
  setStyleSheet("border: 1px solid #ccc; background-color: black;");
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  setMinimumSize(256, 256);  // Allow shrinking to a reasonable minimum
}

void AspectRatioLabel::setPixmap(const QPixmap& pixmap) {
  original_pixmap_ = pixmap;
  updateScaledPixmap();
}

void AspectRatioLabel::resizeEvent(QResizeEvent* event) {
  QLabel::resizeEvent(event);
  updateScaledPixmap();
}

void AspectRatioLabel::updateScaledPixmap() {
  if (original_pixmap_.isNull()) {
    QLabel::setPixmap(QPixmap());
    return;
  }

  // Calculate size to fit while maintaining aspect ratio
  // For 1:1 aspect ratio, use the smaller dimension
  int size = std::min(width(), height());

  QPixmap scaled = original_pixmap_.scaled(size, size, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);

  QLabel::setPixmap(scaled);
}

// ============================================================================
// VectorscopeDialog Implementation
// ============================================================================

VectorscopeDialog::VectorscopeDialog(QWidget* parent)
    : QDialog(parent), d_(std::make_unique<VectorscopeDialogPrivate>()) {
  updateWindowTitle();
  setWindowFlags(Qt::Window);
  resize(1120, 900);

  setupUI();
  connectSignals();
}

VectorscopeDialog::~VectorscopeDialog() = default;

int VectorscopeDialog::getGraticuleMode() const {
  return graticule_group_->checkedId();
}

void VectorscopeDialog::setScopeLabel(const QString& scope_label) {
  d_->scope_label = scope_label;
  updateWindowTitle();
}

void VectorscopeDialog::updateWindowTitle() {
  if (d_->node_id.is_valid()) {
    setWindowTitle(
        QString("%1 - Node %2").arg(d_->scope_label).arg(d_->node_id.value()));
    return;
  }
  setWindowTitle(d_->scope_label);
}

void VectorscopeDialog::setStage(orc::NodeID node_id) {
  d_->node_id = node_id;
  updateWindowTitle();
}

void VectorscopeDialog::setupUI() {
  QVBoxLayout* main_layout = new QVBoxLayout(this);

  // Info label
  info_label_ = new QLabel();
  info_label_->setStyleSheet("font-weight: bold;");
  main_layout->addWidget(info_label_);

  // Main content: display on left, controls on right
  QHBoxLayout* content_layout = new QHBoxLayout();

  // Left side: Vectorscope display with aspect ratio maintenance.  The label
  // is built whichever path is in force: it is what a canvas that fails at
  // run time hands the plot back to.
  scope_label_ = new AspectRatioLabel();
  content_layout->addWidget(scope_label_, 1);

  d_->surface = orc::gui::gpu::createScopeSurface(this);
  if (d_->surface) {
    QWidget* canvas = d_->surface->widget();
    canvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    canvas->setMinimumSize(200, 200);
    content_layout->addWidget(canvas, 1);
    d_->surface->setBackgroundColor(Qt::black);
    scope_label_->hide();
  }

  // Right side: Controls
  QVBoxLayout* controls_layout = new QVBoxLayout();

  // Display options group
  QGroupBox* display_group = new QGroupBox("Display Options");
  QVBoxLayout* display_layout = new QVBoxLayout(display_group);

  blend_color_checkbox_ = new QCheckBox("Colorize");
  blend_color_checkbox_->setChecked(true);
  defocus_checkbox_ = new QCheckBox("Defocus");
  draw_lines_checkbox_ = new QCheckBox("Draw Trace Lines");
  draw_lines_checkbox_->setChecked(true);

  // Point size spinbox
  QHBoxLayout* point_layout = new QHBoxLayout();
  QLabel* point_label = new QLabel("Gain:");
  point_size_spinbox_ = new QSpinBox();
  point_size_spinbox_->setRange(1, 10);
  point_size_spinbox_->setValue(3);
  point_layout->addWidget(point_label);
  point_layout->addWidget(point_size_spinbox_);
  point_layout->addStretch();

  display_layout->addWidget(blend_color_checkbox_);
  display_layout->addWidget(defocus_checkbox_);
  display_layout->addWidget(draw_lines_checkbox_);
  display_layout->addLayout(point_layout);

  controls_layout->addWidget(display_group);

  // Acquisition readout — which signal the scope is looking at.  This is not
  // a choice: a colour-domain stage output has decoder planes to plot and a
  // signal-domain one has a carrier to demodulate, so the stage the user
  // selected already settles it.
  QGroupBox* acquisition_group_box = new QGroupBox("Acquisition");
  QVBoxLayout* acquisition_layout = new QVBoxLayout(acquisition_group_box);
  acquisition_label_ = new QLabel();
  acquisition_layout->addWidget(acquisition_label_);
  controls_layout->addWidget(acquisition_group_box);

  // Sampling group — what the acquisition takes off the frame.  The line
  // select applies to both acquisitions, in the interlaced frame-line
  // numbering both of them report, so the same range means the same lines
  // whichever scope is in force.  The window radios pick a region of the
  // line and only a composite acquisition has one: the decoded planes carry
  // active picture, with no sync, porch or burst to choose between.
  sampling_group_ = new QGroupBox("Sampling");
  QVBoxLayout* sampling_layout = new QVBoxLayout(sampling_group_);

  window_options_ = new QWidget(sampling_group_);
  QVBoxLayout* window_layout = new QVBoxLayout(window_options_);
  window_layout->setContentsMargins(0, 0, 0, 0);

  window_group_ = new QButtonGroup(this);
  window_burst_radio_ = new QRadioButton("Burst only");
  window_active_radio_ = new QRadioButton("Active line");
  window_whole_radio_ = new QRadioButton("Whole line");
  window_whole_radio_->setChecked(true);

  window_group_->addButton(
      window_burst_radio_,
      static_cast<int>(orc::VectorscopeSampleWindow::BurstOnly));
  window_group_->addButton(
      window_active_radio_,
      static_cast<int>(orc::VectorscopeSampleWindow::ActiveLine));
  window_group_->addButton(
      window_whole_radio_,
      static_cast<int>(orc::VectorscopeSampleWindow::WholeLine));

  window_layout->addWidget(window_burst_radio_);
  window_layout->addWidget(window_active_radio_);
  window_layout->addWidget(window_whole_radio_);
  sampling_layout->addWidget(window_options_);

  active_area_only_checkbox_ = new QCheckBox("Active picture only");
  active_area_only_checkbox_->setChecked(true);
  sampling_layout->addWidget(active_area_only_checkbox_);

  all_lines_checkbox_ = new QCheckBox("All lines");
  all_lines_checkbox_->setChecked(true);
  sampling_layout->addWidget(all_lines_checkbox_);

  // Line numbers are presented 1-based throughout the GUI; the contract's
  // line range is 0-based, so the accessors subtract one.
  QHBoxLayout* first_line_layout = new QHBoxLayout();
  first_line_layout->addWidget(new QLabel("First:"));
  first_line_spinbox_ = new QSpinBox();
  first_line_spinbox_->setRange(1, kMaxSelectableLine);
  first_line_spinbox_->setValue(1);
  first_line_layout->addWidget(first_line_spinbox_);
  sampling_layout->addLayout(first_line_layout);

  QHBoxLayout* last_line_layout = new QHBoxLayout();
  last_line_layout->addWidget(new QLabel("Last:"));
  last_line_spinbox_ = new QSpinBox();
  last_line_spinbox_->setRange(1, kMaxSelectableLine);
  last_line_spinbox_->setValue(kMaxSelectableLine);
  last_line_layout->addWidget(last_line_spinbox_);
  sampling_layout->addLayout(last_line_layout);

  controls_layout->addWidget(sampling_group_);

  // Field selection group
  QGroupBox* field_select_group = new QGroupBox("Field Selection");
  QVBoxLayout* field_select_layout = new QVBoxLayout(field_select_group);

  field_select_group_ = new QButtonGroup(this);

  field_select_all_radio_ = new QRadioButton("All Fields");
  field_select_first_radio_ = new QRadioButton("First Field Only");
  field_select_second_radio_ = new QRadioButton("Second Field Only");

  field_select_all_radio_->setChecked(true);

  field_select_group_->addButton(field_select_all_radio_, 0);
  field_select_group_->addButton(field_select_first_radio_, 1);
  field_select_group_->addButton(field_select_second_radio_, 2);

  field_select_layout->addWidget(field_select_all_radio_);
  field_select_layout->addWidget(field_select_first_radio_);
  field_select_layout->addWidget(field_select_second_radio_);

  controls_layout->addWidget(field_select_group);

  // Graticule group
  QGroupBox* graticule_group = new QGroupBox("Graticule");
  QVBoxLayout* graticule_layout = new QVBoxLayout(graticule_group);

  graticule_group_ = new QButtonGroup(this);

  graticule_none_radio_ = new QRadioButton("None");
  graticule_full_radio_ = new QRadioButton("100%");
  graticule_75_radio_ = new QRadioButton("75%");
  graticule_both_radio_ = new QRadioButton("Both");

  graticule_75_radio_->setChecked(true);

  graticule_group_->addButton(graticule_none_radio_, 0);
  graticule_group_->addButton(graticule_full_radio_, 1);
  graticule_group_->addButton(graticule_75_radio_, 2);
  graticule_group_->addButton(graticule_both_radio_, 3);

  graticule_layout->addWidget(graticule_none_radio_);
  graticule_layout->addWidget(graticule_75_radio_);
  graticule_layout->addWidget(graticule_full_radio_);
  graticule_layout->addWidget(graticule_both_radio_);

  controls_layout->addWidget(graticule_group);

  // Measurement readouts (composite acquisition only).
  measurements_group_ = new QGroupBox("Measurements");
  QVBoxLayout* measurements_layout = new QVBoxLayout(measurements_group_);
  // No word wrap: the readout is short lines separated explicitly.  QLabel
  // reports a one-line minimum whatever it holds, so the tallest readout's
  // height is reserved up front — otherwise the column is laid out for the
  // placeholder and the group clips the readings when they arrive.
  measurements_label_ = new QLabel("—");
  measurements_label_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  measurements_label_->setMinimumHeight(
      kMeasurementReadoutLines *
      QFontMetrics(measurements_label_->font()).lineSpacing());
  measurements_layout->addWidget(measurements_label_);
  controls_layout->addWidget(measurements_group_);

  controls_layout->addStretch();

  // Set maximum width for controls panel to keep them from shrinking too much
  QWidget* controls_widget = new QWidget();
  controls_widget->setLayout(controls_layout);
  controls_widget->setMaximumWidth(240);

  // The control column is taller than the dialog's minimum height once the
  // acquisition and sampling groups are present, so it scrolls rather than
  // forcing the scope display to shrink.
  QScrollArea* controls_scroll = new QScrollArea();
  controls_scroll->setWidget(controls_widget);
  controls_scroll->setWidgetResizable(true);
  controls_scroll->setFrameShape(QFrame::NoFrame);
  controls_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  controls_scroll->setMaximumWidth(260);
  controls_scroll->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  content_layout->addWidget(controls_scroll);

  main_layout->addLayout(content_layout, 1);

  updateAcquisitionControlState();

  // Initial display
  clearDisplay();
}

void VectorscopeDialog::connectSignals() {
  connect(blend_color_checkbox_, &QCheckBox::toggled, this,
          &VectorscopeDialog::onBlendColorToggled);
  connect(defocus_checkbox_, &QCheckBox::toggled, this,
          &VectorscopeDialog::onDefocusToggled);
  connect(draw_lines_checkbox_, &QCheckBox::toggled, this,
          &VectorscopeDialog::onDrawLinesToggled);
  connect(active_area_only_checkbox_, &QCheckBox::toggled, this,
          &VectorscopeDialog::onActiveAreaOnlyToggled);
  connect(point_size_spinbox_, QOverload<int>::of(&QSpinBox::valueChanged),
          this, &VectorscopeDialog::onPointSizeChanged);
  connect(field_select_group_, QOverload<int>::of(&QButtonGroup::idClicked),
          this, [this](int) { onFieldSelectionChanged(); });
  connect(graticule_group_, QOverload<int>::of(&QButtonGroup::idClicked), this,
          [this](int) { onGraticuleChanged(); });
  connect(window_group_, QOverload<int>::of(&QButtonGroup::idClicked), this,
          [this](int) { onSampleWindowChanged(); });
  connect(all_lines_checkbox_, &QCheckBox::toggled, this,
          [this](bool) { onLineRangeChanged(); });
  connect(first_line_spinbox_, QOverload<int>::of(&QSpinBox::valueChanged),
          this, [this](int) { onLineRangeChanged(); });
  connect(last_line_spinbox_, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [this](int) { onLineRangeChanged(); });
}

bool VectorscopeDialog::isActiveAreaOnly() const {
  return active_area_only_checkbox_ && active_area_only_checkbox_->isChecked();
}

orc::VectorscopeAcquisitionMode VectorscopeDialog::acquisitionMode() const {
  return acquisition_mode_;
}

void VectorscopeDialog::setAcquisitionMode(
    orc::VectorscopeAcquisitionMode mode) {
  if (acquisition_mode_ == mode) {
    return;
  }

  acquisition_mode_ = mode;
  updateAcquisitionControlState();

  // The two acquisitions are different data sets, not different renderings of
  // one; the previous stage's samples cannot be re-plotted in the new mode.
  d_->last_data.reset();
  clearDisplay();
  emit dataRefreshRequested();
}

orc::VectorscopeSampleWindow VectorscopeDialog::sampleWindow() const {
  if (window_burst_radio_ && window_burst_radio_->isChecked()) {
    return orc::VectorscopeSampleWindow::BurstOnly;
  }
  if (window_active_radio_ && window_active_radio_->isChecked()) {
    return orc::VectorscopeSampleWindow::ActiveLine;
  }
  return orc::VectorscopeSampleWindow::WholeLine;
}

uint32_t VectorscopeDialog::firstLine() const {
  if (!first_line_spinbox_ || !all_lines_checkbox_ ||
      all_lines_checkbox_->isChecked()) {
    return 0;
  }
  // Spin boxes are 1-based for display; the contract's range is 0-based.
  return static_cast<uint32_t>(std::max(
      0,
      std::min(first_line_spinbox_->value(), last_line_spinbox_->value()) - 1));
}

uint32_t VectorscopeDialog::lastLine() const {
  if (!last_line_spinbox_ || !all_lines_checkbox_ ||
      all_lines_checkbox_->isChecked()) {
    // 0 means "to the last line of the frame".
    return 0;
  }
  return static_cast<uint32_t>(std::max(
      0,
      std::max(first_line_spinbox_->value(), last_line_spinbox_->value()) - 1));
}

void VectorscopeDialog::applyAcquisitionTo(
    orc::PreviewCoordinate& coordinate) const {
  coordinate.vectorscope_active_area_only = isActiveAreaOnly();
  coordinate.vectorscope_window = sampleWindow();
  coordinate.vectorscope_first_line = firstLine();
  coordinate.vectorscope_last_line = lastLine();
}

void VectorscopeDialog::updateAcquisitionControlState() {
  const bool composite =
      acquisitionMode() == orc::VectorscopeAcquisitionMode::CompositeCarrier;

  if (acquisition_label_) {
    acquisition_label_->setText(composite ? "Composite carrier\n(measurement)"
                                          : "Decoded component\n(grading)");
    acquisition_label_->setToolTip(
        composite
            ? "Chroma demodulated straight from the composite carrier: no "
              "delay-line averaging, no PAL V-switch correction, burst "
              "included. Shows what the signal looks like."
            : "The U/V planes the chroma decoder produced. Shows what the "
              "decoder output looks like.");
  }

  // The line select applies to both acquisitions; only the window radios and
  // the burst readouts describe a composite acquisition alone, so only those
  // are hidden on the decoded plot.
  if (window_options_) {
    window_options_->setVisible(composite);
  }
  if (measurements_group_) {
    measurements_group_->setVisible(composite);
  }
  if (first_line_spinbox_ && last_line_spinbox_ && all_lines_checkbox_) {
    const bool explicit_range = !all_lines_checkbox_->isChecked();
    first_line_spinbox_->setEnabled(explicit_range);
    last_line_spinbox_->setEnabled(explicit_range);
  }
  if (active_area_only_checkbox_) {
    // The restriction is vertical on both, and horizontal only where nothing
    // else governs the horizontal: a composite acquisition already picks its
    // part of the line with the window radios.
    active_area_only_checkbox_->setToolTip(
        composite
            ? "Plot only the active picture lines. Use the sampling window "
              "above to restrict the acquisition along the line."
            : "Plot only the active picture area of the decoded frame.");
  }
}

void VectorscopeDialog::updateMeasurementReadout() {
  if (!measurements_label_) {
    return;
  }

  if (!d_->last_data.has_value() ||
      d_->last_data->acquisition_mode !=
          orc::VectorscopeAcquisitionMode::CompositeCarrier) {
    measurements_label_->setText("—");
    return;
  }

  const orc::VectorscopeMeasurements& m = d_->last_data->measurements;
  if (!m.valid) {
    measurements_label_->setText("No burst lock");
    return;
  }

  QString text = QString("Burst: %1 IRE (%2 %)\nJitter: %3° rms\nLines: %4")
                     .arg(m.burst_amplitude_ire, 0, 'f', 2)
                     .arg(m.burst_amplitude_percent, 0, 'f', 1)
                     .arg(m.burst_phase_jitter_degrees, 0, 'f', 2)
                     .arg(m.burst_line_count);

  if (orc::gui::hasSwitchedVAxis(d_->last_data->system)) {
    text += QString("\nV-switch split err: %1°")
                .arg(m.burst_phase_split_error_degrees, 0, 'f', 2);
  }

  if (m.chroma_to_burst_ratio > 0.0) {
    text +=
        QString("\nChroma/burst: %1").arg(m.chroma_to_burst_ratio, 0, 'f', 3);
  }

  measurements_label_->setText(text);
}

void VectorscopeDialog::updateVectorscope(const orc::VectorscopeData& data) {
  if (data.samples.empty()) {
    info_label_->setText(
        QString("Field %1 - No vectorscope data")
            .arg(data.field_number + 1));  // Convert to 1-based
    clearDisplay();
    return;
  }

  d_->last_data = data;
  d_->current_field_number = data.field_number;
  renderVectorscope(data);
  ORC_LOG_DEBUG("Vectorscope updated for field {} ({} samples)",
                data.field_number, data.samples.size());
}

void VectorscopeDialog::renderVectorscope(const orc::VectorscopeData& data) {
  downgradeIfCanvasFailed();

  if (data.samples.empty()) {
    ORC_LOG_DEBUG(
        "VectorscopeDialog: renderVectorscope called with empty samples for "
        "field {}",
        data.field_number);
    clearDisplay();
    return;
  }

  const orc::gui::VectorscopePlotGeometry geometry;
  const int size = geometry.canvas_size;

  // Check if this is mono/no-chroma data (all samples near origin).
  constexpr double CHROMA_THRESHOLD = 1000.0;
  bool has_chroma = false;
  for (const auto& sample : data.samples) {
    if (std::abs(sample.u) > CHROMA_THRESHOLD ||
        std::abs(sample.v) > CHROMA_THRESHOLD) {
      has_chroma = true;
      break;
    }
  }

  const int graticule_mode = graticule_group_->checkedId();
  const bool colorize = blend_color_checkbox_->isChecked();
  const bool defocus = defocus_checkbox_->isChecked();
  const bool draw_trace_lines = draw_lines_checkbox_->isChecked();
  const int field_select = field_select_group_->checkedId();
  // Gain 1–10 from the spinbox maps directly to the brightness knee formula.
  const float gain = static_cast<float>(point_size_spinbox_->value());

  // Calculate IRE range for debug logging (CVBS_U10_4FSC 10-bit domain).
  const double ire_range = data.cvbs_white - data.cvbs_blanking;
  const double black_percent = (data.cvbs_blanking / 1023.0) * 100.0;
  const double white_percent = (data.cvbs_white / 1023.0) * 100.0;

  ORC_LOG_DEBUG(
      "VectorscopeDialog: renderVectorscope field={} samples={} graticule={} "
      "colorize={} defocus={} field_select={} system={} white={} blanking={} "
      "chroma_detected={}",
      data.field_number, data.samples.size(), graticule_mode, colorize, defocus,
      field_select, static_cast<int>(data.system), data.cvbs_white,
      data.cvbs_blanking, has_chroma);
  ORC_LOG_DEBUG(
      "VectorscopeDialog: CVBS levels - blanking={:.2f}% ({}) white={:.2f}% "
      "({}) range={:.0f} ({}=NTSC, {}=PAL)",
      black_percent, data.cvbs_blanking, white_percent, data.cvbs_white,
      ire_range, static_cast<int>(orc::VideoSystem::NTSC),
      static_cast<int>(orc::VideoSystem::PAL));

  // On a bench instrument the graticule is behind the phosphor and the trace
  // is in front of it, and the composite plot is drawn that way: its vectors
  // are points that land on the targets, so a graticule painted over them
  // would hide each one under its own target crosshair — the one place a
  // measurement scope must never lose it.  The decoded plot is a cloud rather
  // than a set of points, and covering the whole graticule with it would help
  // nobody, so there the graticule stays on top.
  const bool dwell_intensity =
      (data.acquisition_mode ==
       orc::VectorscopeAcquisitionMode::CompositeCarrier);

  // A composite acquisition stays with the CPU renderer whichever path this
  // window is on: its brightness anchor is read off a histogram of the spread
  // dwell across the whole plot, and a canvas cannot produce that figure
  // without reading the plot back and stalling the frame.  The decoded
  // acquisition has no such reduction - its brightness is a function of the
  // count under the pixel and nothing else - so that is what the canvas
  // plots.
  if (d_->surface && !dwell_intensity) {
    renderVectorscopeOnCanvas(data, has_chroma);
    updateInfoLabel(data, field_select);
    return;
  }

  QImage image(size, size, QImage::Format_RGB888);
  image.fill(Qt::black);
  {
    QPainter painter(&image);
    if (graticule_mode != 0) {
      d_->drawColorZones(painter, this, data.system, data.cvbs_white,
                         data.cvbs_blanking, data.acquisition_mode);
      if (dwell_intensity) {
        d_->drawGraticule(painter, this, data.system, data.cvbs_white,
                          data.cvbs_blanking, data.acquisition_mode);
      }
    }
  }

  // -------------------------------------------------------------------------
  // Pass 1 — accumulate sample hits into a 2-D count buffer.
  //
  // Consecutive samples within the same field are linked by Bresenham lines
  // so the trace path between samples also accumulates hits, mimicking the
  // continuous beam trace of an analogue vectorscope.  Those transit hits are
  // kept in their own buffer: the beam only passes through them, so they must
  // not set the brightness reference for the points it rests on, and there
  // are far more of them — a frame of colour bars deposits several times as
  // much ink joining the vectors as it does landing on them.
  // -------------------------------------------------------------------------
  const size_t buf_size = static_cast<size_t>(size) * static_cast<size_t>(size);
  std::vector<uint32_t> hit_count(buf_size, 0);
  std::vector<uint32_t> transit_count(draw_trace_lines ? buf_size : 0, 0);

  std::minstd_rand random_engine(12345);
  std::normal_distribution<double> normal_dist(0.0, 100.0);

  std::optional<QPoint> prev_point;
  uint8_t prev_field_id = 255;    // invalid sentinel
  uint16_t prev_line_number = 0;  // meaningful only once prev_point is set

  // Lines that actually reached the plot, which is what turns a hit count
  // into a per-line dwell below.  Samples arrive line by line, so a change of
  // line is a new line.
  uint32_t plotted_lines = 0;
  uint8_t counted_field_id = 255;
  uint16_t counted_line_number = 0;

  // The trace only ever occupies part of the canvas, and everything after this
  // pass — the beam spot, the brightness reference, the compositing — only has
  // to visit the box it landed in.  Tracking that box here costs nothing;
  // rediscovering it later means a full sweep of the plot for each of them.
  // Transits are bounded by it too: both ends of a joined pair are on the
  // canvas, so the segment between them cannot leave their bounding box.
  int min_x = size;
  int max_x = -1;
  int min_y = size;
  int max_y = -1;

  for (const auto& sample : data.samples) {
    if (field_select == 1 && sample.field_id != 0) continue;
    if (field_select == 2 && sample.field_id != 1) continue;

    double u = sample.u;
    double v = sample.v;
    if (defocus) {
      u += normal_dist(random_engine);
      v += normal_dist(random_engine);
    }

    if (sample.field_id != counted_field_id ||
        sample.line_number != counted_line_number || plotted_lines == 0) {
      counted_field_id = sample.field_id;
      counted_line_number = sample.line_number;
      ++plotted_lines;
    }

    const QPointF plot_point = geometry.mapUV(u, v);

    if (orc::gui::isWithinVectorscopeCanvas(plot_point, size)) {
      const int px = static_cast<int>(plot_point.x());
      const int py = static_cast<int>(plot_point.y());

      hit_count[static_cast<size_t>(py) * static_cast<size_t>(size) +
                static_cast<size_t>(px)]++;
      if (px < min_x) min_x = px;
      if (px > max_x) max_x = px;
      if (py < min_y) min_y = py;
      if (py > max_y) max_y = py;

      // Connect consecutive samples of the same line as a trace line.  The
      // beam is continuous along a line and blanked over the retrace to the
      // next one, so joining across the line boundary would strike a chord
      // across the plot that the signal never traced.
      if (draw_trace_lines && prev_point.has_value() &&
          sample.field_id == prev_field_id &&
          sample.line_number == prev_line_number) {
        accumulateLine(transit_count, size, prev_point->x(), prev_point->y(),
                       px, py);
      }
      prev_point = QPoint(px, py);
      prev_field_id = sample.field_id;
      prev_line_number = sample.line_number;
    } else {
      prev_point.reset();
    }
  }

  // -------------------------------------------------------------------------
  // Pass 2 — render each hit pixel with brightness from count and, when
  // colorize is on, hue derived from the pixel's U/V canvas position.
  //
  // Decoded plot: brightness = min(count * 5 * gain + 128, 255) / 255,
  // matching WaveformMonitorWidget (ITU-R BT.601 norm).  A single hit → ~52%
  // brightness; full saturation after ~26 hits at gain=1.
  //
  // Composite plot: intensity is linear in dwell, the way a phosphor's is in
  // the charge the beam leaves on it.  A whole line of signal is orders of
  // magnitude more samples than the decoded active picture, and most of them
  // are the beam in transit between colour-bar vectors or riding out a sync
  // edge — real parts of the trace, but ones the beam crosses once where it
  // rests on a bar for the width of the bar.  The decoded formula lifts a
  // single crossing to half brightness, which buries the vectors under
  // everything the beam passed through on the way there.
  //
  // Position color: for each pixel the U/V coordinates are recovered from the
  // canvas geometry, then the BT.601 inverse matrix at Y=0.5 gives the RGB
  // colour that would produce a signal at that chroma position (ITU-R
  // BT.470-6 §1.1.2).  Max-component normalisation ensures every point has
  // at least one full channel — achromatic samples near the origin render as
  // white.
  // -------------------------------------------------------------------------
  const float k = 5.0f * gain;

  // Dividing the hit count by the number of lines plotted, and by the
  // subsampling stride, turns it into the number of samples of a line the
  // beam spent on this pixel — a figure that does not move when the line
  // range, the field selection or the sample ceiling change.  Gain is then
  // the intensity knob: it sets how few samples a line are enough to reach
  // full brightness, so raising it lifts the faint transitions into view
  // without moving the vectors, which are already saturated.  A colour-bar
  // vector is tens of samples a line; the transit between two of them is one.
  constexpr float kSaturationSamplesPerLine = 8.0f;
  const float per_line_anchor =
      (plotted_lines > 0)
          ? ((kSaturationSamplesPerLine * static_cast<float>(plotted_lines)) /
             static_cast<float>(std::max(data.sample_stride, 1u)))
          : 0.0f;

  // A beam has a finite spot.  Without one a vector that never moves lands on
  // a single pixel: 121 of them on a 1024-pixel canvas for a frame of colour
  // bars, which at any sensible display scale is smaller than the graticule
  // mark it is supposed to be read against, and effectively invisible.  The
  // spot here is a Gaussian a quarter of a per cent of the plot diameter
  // across, the order of a CRT vectorscope's, and it leaves the total charge
  // — and so where the trace sits — untouched.
  const double spot_sigma = static_cast<double>(size) / 410.0;
  const int spot_radius =
      std::max(1, static_cast<int>(std::ceil(3.0 * spot_sigma)));
  std::vector<float> spot(static_cast<size_t>(spot_radius) + 1, 0.0f);
  {
    double sum = 0.0;
    for (int t = 0; t <= spot_radius; ++t) {
      const double weight =
          std::exp(-0.5 * (t * t) / (spot_sigma * spot_sigma));
      spot[static_cast<size_t>(t)] = static_cast<float>(weight);
      sum += (t == 0) ? weight : (2.0 * weight);
    }
    for (float& weight : spot) weight /= static_cast<float>(sum);
  }

  // The spot reaches spot_radius beyond the trace, so that is the box every
  // pass from here on works in.
  const int spread_min_x = std::max(0, min_x - spot_radius);
  const int spread_max_x = std::min(size - 1, max_x + spot_radius);
  const int spread_min_y = std::max(0, min_y - spot_radius);
  const int spread_max_y = std::min(size - 1, max_y + spot_radius);

  std::vector<float> scratch;
  auto spread_counts = [&](const std::vector<uint32_t>& counts,
                           std::vector<float>& out) {
    out.assign(buf_size, 0.0f);
    if (counts.empty()) return;

    scratch.assign(buf_size, 0.0f);
    // Raw pointers through both sweeps: this is a megapixel convolved by
    // seventeen taps twice over, and an indexed container access per tap is
    // the difference between a render that keeps up with playback and one
    // that does not.
    const uint32_t* const source = counts.data();
    float* const middle = scratch.data();
    float* const result = out.data();
    const float* const weights = spot.data();

    // A vectorscope trace is a handful of vectors and the paths between them,
    // so most of the box that contains it is empty canvas.  One cheap sweep
    // for each row's occupied span turns the convolution below from a sweep of
    // the box into a sweep of the trace.
    std::vector<int> row_lo(static_cast<size_t>(size), size);
    std::vector<int> row_hi(static_cast<size_t>(size), -1);
    for (int y = spread_min_y; y <= spread_max_y; ++y) {
      const size_t base = static_cast<size_t>(y) * static_cast<size_t>(size);
      int lo = size;
      int hi = -1;
      for (int x = spread_min_x; x <= spread_max_x; ++x) {
        if (source[base + x] == 0) continue;
        if (lo > x) lo = x;
        hi = x;
      }
      row_lo[static_cast<size_t>(y)] = lo;
      row_hi[static_cast<size_t>(y)] = hi;
    }

    // Whether the spot overhangs the canvas is settled once per pixel (per
    // row, for the vertical sweep) rather than re-tested under every tap: the
    // overhang only happens where the trace runs into the edge of the plot,
    // and the interior — which is nearly all of it — then reduces to a plain
    // symmetric sum.
    for (int y = spread_min_y; y <= spread_max_y; ++y) {
      if (row_hi[static_cast<size_t>(y)] < 0) continue;
      const size_t base = static_cast<size_t>(y) * static_cast<size_t>(size);
      const int row_from =
          std::max(0, row_lo[static_cast<size_t>(y)] - spot_radius);
      const int row_to =
          std::min(size - 1, row_hi[static_cast<size_t>(y)] + spot_radius);
      for (int x = row_from; x <= row_to; ++x) {
        float total = weights[0] * static_cast<float>(source[base + x]);
        if (x >= spot_radius && x + spot_radius < size) {
          for (int t = 1; t <= spot_radius; ++t) {
            total += weights[t] * (static_cast<float>(source[base + x - t]) +
                                   static_cast<float>(source[base + x + t]));
          }
        } else {
          for (int t = 1; t <= spot_radius; ++t) {
            const float weight = weights[t];
            if (x - t >= 0) {
              total += weight * static_cast<float>(source[base + x - t]);
            }
            if (x + t < size) {
              total += weight * static_cast<float>(source[base + x + t]);
            }
          }
        }
        middle[base + x] = total;
      }
    }
    for (int y = spread_min_y; y <= spread_max_y; ++y) {
      // The rows the spot reaches down from decide this row's span: the
      // horizontal sweep has already widened each of them by the spot radius.
      int column_from = size;
      int column_to = -1;
      for (int j = std::max(spread_min_y, y - spot_radius);
           j <= std::min(spread_max_y, y + spot_radius); ++j) {
        if (row_hi[static_cast<size_t>(j)] < 0) continue;
        column_from =
            std::min(column_from, row_lo[static_cast<size_t>(j)] - spot_radius);
        column_to =
            std::max(column_to, row_hi[static_cast<size_t>(j)] + spot_radius);
      }
      if (column_to < 0) continue;
      column_from = std::max(0, column_from);
      column_to = std::min(size - 1, column_to);

      const size_t base = static_cast<size_t>(y) * static_cast<size_t>(size);
      if (y >= spot_radius && y + spot_radius < size) {
        for (int x = column_from; x <= column_to; ++x) {
          float total = weights[0] * middle[base + x];
          for (int t = 1; t <= spot_radius; ++t) {
            const size_t step =
                static_cast<size_t>(t) * static_cast<size_t>(size);
            total += weights[t] *
                     (middle[base - step + x] + middle[base + step + x]);
          }
          result[base + x] = total;
        }
      } else {
        for (int x = column_from; x <= column_to; ++x) {
          float total = weights[0] * middle[base + x];
          for (int t = 1; t <= spot_radius; ++t) {
            const float weight = weights[t];
            const size_t step =
                static_cast<size_t>(t) * static_cast<size_t>(size);
            if (y - t >= 0) total += weight * middle[base - step + x];
            if (y + t < size) total += weight * middle[base + step + x];
          }
          result[base + x] = total;
        }
      }
    }
  };

  std::vector<float> dwell;
  std::vector<float> transit_dwell;
  if (dwell_intensity) {
    spread_counts(hit_count, dwell);
    spread_counts(transit_count, transit_dwell);
    scratch.clear();
    scratch.shrink_to_fit();
  }

  // The spot divides an isolated vector's peak by this much, so the per-line
  // anchor below — which is expressed in samples of a line landing on one
  // pixel — has to be measured in the same units as the spread dwell.
  const float spot_peak_fraction = spot[0] * spot[0];

  // Full brightness is given at whichever anchor makes the trace brighter:
  // the dwell a vector reaches when it lands on the same pixel on every line,
  // or the dwell above which the beam spends half its resting time.  The
  // first is right for a clean trace that never moves.  On a real capture
  // noise spreads each vector over a disc, dividing its per-pixel dwell by
  // the area of that disc — which leaves the trace dim at any gain — and the
  // second anchor follows the spreading instead of fighting it.  Taking the
  // lower of the two also means the origin, where the beam rests through
  // blanking, can never starve the rest of the plot: the per-line anchor
  // caps it.
  //
  // The half-of-the-dwell point is read off a histogram rather than a sorted
  // copy of the plot.  Sorting means building and ordering a list as long as
  // the lit part of the canvas on every frame, which is the single most
  // expensive thing the renderer would do; bucketing by brightness and walking
  // the buckets down from the top answers the same question in one sweep, to
  // within a bucket's width of the same value.
  constexpr float kSaturationDwellFraction = 0.5f;
  float anchor = per_line_anchor * spot_peak_fraction;
  if (dwell_intensity && !dwell.empty()) {
    constexpr int kDwellBuckets = 1024;
    double total = 0.0;
    float peak = 0.0f;
    for (int y = spread_min_y; y <= spread_max_y; ++y) {
      const size_t base = static_cast<size_t>(y) * static_cast<size_t>(size);
      for (int x = spread_min_x; x <= spread_max_x; ++x) {
        const float value = dwell[base + x];
        if (value <= 0.0f) continue;
        total += value;
        peak = std::max(peak, value);
      }
    }
    if (peak > 0.0f) {
      std::vector<double> bucket_sum(kDwellBuckets, 0.0);
      const float to_bucket = static_cast<float>(kDwellBuckets) / peak;
      for (int y = spread_min_y; y <= spread_max_y; ++y) {
        const size_t base = static_cast<size_t>(y) * static_cast<size_t>(size);
        for (int x = spread_min_x; x <= spread_max_x; ++x) {
          const float value = dwell[base + x];
          if (value <= 0.0f) continue;
          const int bucket =
              std::min(kDwellBuckets - 1, static_cast<int>(value * to_bucket));
          bucket_sum[static_cast<size_t>(bucket)] += value;
        }
      }
      double accumulated = 0.0;
      for (int bucket = kDwellBuckets - 1; bucket >= 0; --bucket) {
        accumulated += bucket_sum[static_cast<size_t>(bucket)];
        if (accumulated >= kSaturationDwellFraction * total) {
          // The bucket's lower edge, so rounding can only leave the trace
          // brighter than the exact half-dwell point rather than dimmer.
          anchor = std::min(anchor, static_cast<float>(bucket) / to_bucket);
          break;
        }
      }
    }
  }
  const float dwell_scale = (anchor > 0.0f) ? (gain / anchor) : 0.0f;

  // The transit between two vectors is the beam moving at full speed, so it
  // reads as a faint constant however far it has to travel: a pixel crossed
  // once on every line is held near kTransitBrightnessAtUnitGain whatever the
  // vectors either end of it are doing.  Scaling it with the vectors instead
  // would let a frame of colour bars, which lays down several times as much
  // ink joining its vectors as landing on them, wash the plot out.
  constexpr float kTransitBrightnessAtUnitGain = 0.04f;
  const float transit_scale =
      (plotted_lines > 0)
          ? ((gain * kTransitBrightnessAtUnitGain *
              static_cast<float>(std::max(data.sample_stride, 1u))) /
             (static_cast<float>(plotted_lines) * spot_peak_fraction))
          : 0.0f;

  for (int py = spread_min_y; py <= spread_max_y; ++py) {
    uchar* const row = image.scanLine(py);
    for (int px = spread_min_x; px <= spread_max_x; ++px) {
      const size_t index =
          (static_cast<size_t>(py) * static_cast<size_t>(size)) +
          static_cast<size_t>(px);

      float brightness = 0.0f;
      if (dwell_intensity) {
        // The spot puts charge on pixels the beam never landed on — that is
        // the whole point of it — so what is lit here is the spread dwell,
        // not the raw landings.
        brightness = std::min(1.0f, (dwell[index] * dwell_scale) +
                                        (transit_dwell[index] * transit_scale));
      } else {
        const uint32_t count =
            hit_count[index] +
            (transit_count.empty() ? 0u : transit_count[index]);
        if (count == 0) continue;
        brightness =
            std::min(1.0f, (static_cast<float>(count) * k + 128.0f) / 255.0f);
      }
      if (brightness <= 0.0f) continue;

      int cr, cg, cb;
      if (colorize) {
        // Recover U/V at this canvas pixel.
        const double u_uv =
            (px - geometry.centre_point.x()) / geometry.pixels_per_uv_unit;
        const double v_uv =
            -(py - geometry.centre_point.y()) / geometry.pixels_per_uv_unit;

        // ITU-R BT.470-6 §1.1.2 / EBU Tech. 3280-E §2.1 inverse at Y=0.5:
        //   B - Y = U / ku  (ku = 0.492111)
        //   R - Y = V / kv  (kv = 0.877283)
        //   G derived from BT.601 luminance equation
        const double u_n = u_uv / orc::gui::kVectorscopeSignedFullScale;
        const double v_n = v_uv / orc::gui::kVectorscopeSignedFullScale;
        const double r_raw = 0.5 + v_n / 0.877283;
        const double b_raw = 0.5 + u_n / 0.492111;
        const double g_raw = (0.5 - 0.299 * r_raw - 0.114 * b_raw) / 0.587;

        double r_c = std::clamp(r_raw, 0.0, 1.0);
        double g_c = std::clamp(g_raw, 0.0, 1.0);
        double b_c = std::clamp(b_raw, 0.0, 1.0);

        // Normalise to max component so the hue direction is always vivid.
        // The centre (U=V=0) gives equal components → normalises to white.
        const double max_c = std::max({r_c, g_c, b_c});
        if (max_c > 0.001) {
          r_c /= max_c;
          g_c /= max_c;
          b_c /= max_c;
        } else {
          r_c = g_c = b_c = 1.0;
        }

        cr = static_cast<int>(r_c * brightness * 255.0f);
        cg = static_cast<int>(g_c * brightness * 255.0f);
        cb = static_cast<int>(b_c * brightness * 255.0f);
      } else {
        cr = 0;
        cg = static_cast<int>(brightness * 255.0f);
        cb = 0;
      }

      // Written straight into the scanline rather than through
      // QImage::setPixel(): the plot is a megapixel and setPixel() re-derives
      // the format and the row address on every call.
      uchar* const pixel = row + (static_cast<ptrdiff_t>(px) * 3);
      if (dwell_intensity) {
        // The graticule was painted underneath, so the trace has to add to
        // what is already there.  Overwriting punches a dark hole in it
        // wherever the beam passed dimly, which reads as the trace being
        // darker than the graticule it crosses.  The decoded plot has the
        // graticule painted over it instead and keeps its opaque trace.
        pixel[0] = static_cast<uchar>(std::min(255, pixel[0] + cr));
        pixel[1] = static_cast<uchar>(std::min(255, pixel[1] + cg));
        pixel[2] = static_cast<uchar>(std::min(255, pixel[2] + cb));
      } else {
        pixel[0] = static_cast<uchar>(cr);
        pixel[1] = static_cast<uchar>(cg);
        pixel[2] = static_cast<uchar>(cb);
      }
    }
  }

  // Decoded plot: the graticule goes on top of the cloud (see above).
  if (!dwell_intensity && graticule_mode != 0) {
    QPainter painter(&image);
    d_->drawGraticule(painter, this, data.system, data.cvbs_white,
                      data.cvbs_blanking, data.acquisition_mode);
  }

  // Overlay "no chroma" warning when all samples are near the origin.
  if (!has_chroma) {
    QPainter painter(&image);
    painter.setPen(Qt::yellow);
    QFont font = painter.font();
    font.setPointSize(16);
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(QRect(0, size / 2 - 40, size, 80),
                     Qt::AlignCenter | Qt::TextWordWrap, "No chroma present");
  }

  showStaticImage(image);
  updateInfoLabel(data, field_select);
}

void VectorscopeDialog::showStaticImage(const QImage& image) {
  if (d_->surface) {
    // The canvas doubles as this window's image view: a frame with no
    // vertices shows its underlay, scaled the way the label scaled a pixmap.
    orc::gui::gpu::ScopeFrame frame;
    frame.canvas_size = image.size();
    frame.underlay = image;
    d_->surface->setFrame(std::move(frame));
    d_->surface->refresh();
    return;
  }
  scope_label_->setPixmap(QPixmap::fromImage(image));
}

void VectorscopeDialog::downgradeIfCanvasFailed() {
  if (orc::gui::gpu::dropScopeSurfaceIfFailed(d_->surface)) {
    // The label has been there all along with nothing in it; the render that
    // follows this call is what fills it.
    scope_label_->show();
  }
}

void VectorscopeDialog::renderVectorscopeOnCanvas(
    const orc::VectorscopeData& data, bool has_chroma) {
  const orc::gui::VectorscopePlotGeometry geometry;

  orc::gui::gpu::VectorscopeVertexOptions options;
  options.field_select = field_select_group_->checkedId();
  options.defocus = defocus_checkbox_->isChecked();
  options.draw_trace_lines = draw_lines_checkbox_->isChecked();

  orc::gui::gpu::VectorscopeVertices vertices =
      orc::gui::gpu::buildVectorscopeVertices(data, geometry, options);

  d_->refreshCanvasGraticules(this, data, has_chroma);

  orc::gui::gpu::ScopeFrame frame;
  frame.canvas_size = QSize(geometry.canvas_size, geometry.canvas_size);
  frame.points = std::move(vertices.points);
  frame.lines = std::move(vertices.lines);
  frame.blend = orc::gui::gpu::ScopeBlend::kAdd;
  frame.underlay = d_->canvas_underlay;
  frame.overlay = d_->canvas_overlay;
  frame.map = orc::gui::gpu::vectorscopeDecodedMapUniforms(
      geometry, point_size_spinbox_->value(),
      blend_color_checkbox_->isChecked());

  d_->surface->setFrame(std::move(frame));
  d_->surface->refresh();
}

void VectorscopeDialogPrivate::refreshCanvasGraticules(
    VectorscopeDialog* dialog, const orc::VectorscopeData& data,
    bool has_chroma) {
  GraticuleKey key;
  key.system = data.system;
  key.cvbs_white = data.cvbs_white;
  key.cvbs_blanking = data.cvbs_blanking;
  key.mode = data.acquisition_mode;
  key.graticule_mode = dialog->getGraticuleMode();
  key.has_chroma = has_chroma;
  if (graticule_valid && graticule_key.matches(key)) {
    return;
  }

  const int size = orc::gui::kVectorscopeCanvasSize;

  // Behind the trace: the colour zones, on the black the plot sits on.
  QImage under(size, size, QImage::Format_RGB888);
  under.fill(Qt::black);
  if (key.graticule_mode != 0) {
    QPainter painter(&under);
    drawColorZones(painter, dialog, data.system, data.cvbs_white,
                   data.cvbs_blanking, data.acquisition_mode);
  }

  // In front of it: the graticule, which a decoded plot is read against
  // rather than through, and whatever the acquisition has to be warned about.
  QImage over(size, size, QImage::Format_ARGB32_Premultiplied);
  over.fill(Qt::transparent);
  {
    QPainter painter(&over);
    if (key.graticule_mode != 0) {
      drawGraticule(painter, dialog, data.system, data.cvbs_white,
                    data.cvbs_blanking, data.acquisition_mode);
    }
    if (!has_chroma) {
      painter.setPen(Qt::yellow);
      QFont font = painter.font();
      font.setPointSize(16);
      font.setBold(true);
      painter.setFont(font);
      painter.drawText(QRect(0, size / 2 - 40, size, 80),
                       Qt::AlignCenter | Qt::TextWordWrap, "No chroma present");
    }
  }

  // The canvas composites with straight alpha, as the shader's source-over
  // does, so the premultiplied painting surface is undone here.
  canvas_underlay = under.convertToFormat(QImage::Format_RGBA8888);
  canvas_overlay = over.convertToFormat(QImage::Format_RGBA8888);
  graticule_key = key;
  graticule_valid = true;
}

void VectorscopeDialog::updateInfoLabel(const orc::VectorscopeData& data,
                                        int field_select) {
  // Update info label.
  QString field_info;
  if (field_select == 0) {
    field_info = "Both fields";
  } else if (field_select == 1) {
    field_info = "First field only";
  } else {
    field_info = "Second field only";
  }

  if (data.acquisition_mode ==
      orc::VectorscopeAcquisitionMode::CompositeCarrier) {
    QString window_text;
    switch (data.sample_window) {
      case orc::VectorscopeSampleWindow::BurstOnly:
        window_text = "burst only";
        break;
      case orc::VectorscopeSampleWindow::ActiveLine:
        window_text = "active line";
        break;
      case orc::VectorscopeSampleWindow::WholeLine:
        window_text = "whole line";
        break;
    }

    QString stride_text;
    if (data.sample_stride > 1) {
      stride_text = QString(", 1 sample in %1").arg(data.sample_stride);
    }

    // Line numbers are presented 1-based throughout the GUI.
    info_label_->setText(
        QString("Frame %1 - composite - %2 samples (lines %3-%4, %5%6) - %7")
            .arg(data.field_number + 1)
            .arg(data.samples.size())
            .arg(data.first_line + 1)
            .arg(data.last_line + 1)
            .arg(window_text)
            .arg(stride_text)
            .arg(field_info));
  } else {
    const QString sample_area =
        isActiveAreaOnly() ? "active picture" : "full frame";

    // Line numbers are presented 1-based throughout the GUI.
    info_label_->setText(
        QString("Field %1 - %2 samples (%3x%4 %5, lines %6-%7) - %8")
            .arg(data.field_number + 1)
            .arg(data.samples.size())
            .arg(data.width)
            .arg(data.height)
            .arg(sample_area)
            .arg(data.first_line + 1)
            .arg(data.last_line + 1)
            .arg(field_info));
  }

  updateMeasurementReadout();
}

void VectorscopeDialogPrivate::drawGraticule(
    QPainter& painter, VectorscopeDialog* dialog, orc::VideoSystem system,
    int32_t cvbs_white, int32_t cvbs_blanking,
    orc::VectorscopeAcquisitionMode mode) {
  const orc::gui::VectorscopePlotGeometry geometry;

  // A composite acquisition plots the signal as transmitted: the PAL V-switch
  // has not been undone, so the display is not delay-line compensated and
  // every target has a mirror image about the U axis (ITU-R BT.470-6 Table 2
  // item 2.16).  The burst lives on the back porch, so it is in the data set
  // too and gets targets of its own.
  const bool measurement =
      (mode == orc::VectorscopeAcquisitionMode::CompositeCarrier);
  const bool switched_v = measurement && orc::gui::hasSwitchedVAxis(system);

  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen(Qt::white, orc::gui::kVectorscopeAxisStrokeWidth));

  painter.drawLine(
      QPointF(geometry.centre_point.x(), geometry.plot_area.top()),
      QPointF(geometry.centre_point.x(), geometry.plot_area.bottom()));
  painter.drawLine(
      QPointF(geometry.plot_area.left(), geometry.centre_point.y()),
      QPointF(geometry.plot_area.right(), geometry.centre_point.y()));

  painter.setPen(QPen(Qt::white, orc::gui::kVectorscopeCircleStrokeWidth));
  painter.drawEllipse(geometry.plot_area);
  drawCircleMarkers(painter, geometry);

  painter.setPen(QPen(Qt::white, orc::gui::kVectorscopeAxisStrokeWidth));

  // NTSC keeps the full I/Q reference set and labels.
  if (system == orc::VideoSystem::NTSC) {
    drawReferenceAxis(painter, geometry, kNtscIAxisStandardDegrees);
    drawReferenceAxis(painter, geometry, kNtscNegIAxisStandardDegrees);
    drawReferenceAxis(painter, geometry, kNtscQAxisStandardDegrees);
    drawReferenceAxis(painter, geometry, kNtscNegQAxisStandardDegrees);
    drawNtcsIqLabels(painter, geometry);
  }

  // 75% vs 100% targets scaling (mode: 0=none, 1=100%, 2=75%, 3=both)
  const int graticule_mode = dialog->getGraticuleMode();
  const bool draw_graticule = (graticule_mode != 0);
  if (!draw_graticule) {
    return;
  }

  if (measurement) {
    drawBurstTargets(painter, geometry, system, switched_v);
  }

  const int32_t white = cvbs_white;
  const int32_t black = cvbs_blanking;
  if (white <= black) {
    return;
  }

  // Color labels for the six colour bars
  // rgb values: 1=B, 2=G, 3=Cy, 4=R, 5=Mg, 6=Yl
  // A PAL measurement display carries both line phases; the two sets are
  // conventionally distinguished upper case (+V) and lower case (−V).
  static const char* const kUpperCaseLabels[] = {"",  "B",  "G", "Cy",
                                                 "R", "Mg", "Yl"};
  static const char* const kLowerCaseLabels[] = {"",  "b",  "g", "cy",
                                                 "r", "mg", "yl"};
  const bool draw_both = (graticule_mode == 3);

  // Draw targets for six colour bars (R'G'B' 001..110).
  // Targets are in the ±32767 display scale (kVectorscopeSignedFullScale),
  // matching the scale used for all UVSample data.
  auto draw_targets_at_percent = [&](double percent,
                                     orc::VectorscopeLinePhase phase,
                                     bool with_labels) {
    const bool lower_case = (phase == orc::VectorscopeLinePhase::VNegative);
    const double box_size =
        measurement ? kMeasurementTargetBoxSizePixels : kTargetBoxSizePixels;
    const double crosshair_size = measurement ? kMeasurementCrosshairSizePixels
                                              : kTargetCrosshairSizePixels;

    for (int rgb = 1; rgb < 7; rgb++) {
      const orc::UVSample target =
          measurement ? orc::gui::measurementTargetUv(
                            rgb, percent, orc::gui::kVectorscopeSignedFullScale,
                            system, phase)
                      : orc::gui::vectorscopeDisplayTargetUv(
                            rgb, percent, orc::gui::kVectorscopeSignedFullScale,
                            system);
      const QPointF target_point = geometry.mapUV(target.u, target.v);
      const QColor target_color = vectorscopeTargetColor(rgb);

      drawTargetBox(painter, geometry, target_point, target_color, box_size,
                    crosshair_size);

      if (with_labels) {
        const double barTheta = std::atan2(-target.v, target.u);
        const double barMagnitude = std::hypot(target.u, target.v);
        const double label_distance =
            barMagnitude + geometry.pixelsToMagnitude(kColorLabelOffsetPixels);
        const QPointF label_position =
            geometry.pointFromVectorscopeAngle(barTheta, label_distance);

        QFont font = painter.font();
        font.setPointSize(14);
        font.setBold(true);
        painter.setFont(font);
        QColor label_color = target_color;
        label_color.setAlpha(255);
        painter.setPen(
            QPen(label_color, orc::gui::kVectorscopeAxisStrokeWidth));

        QFontMetrics fm(font);
        QString label_text(lower_case ? kLowerCaseLabels[rgb]
                                      : kUpperCaseLabels[rgb]);
        int text_width = fm.horizontalAdvance(label_text);
        int text_height = fm.height();

        painter.drawText(static_cast<int>(label_position.x()) - text_width / 2,
                         static_cast<int>(label_position.y()) + text_height / 4,
                         label_text);
      }
    }
  };

  auto draw_percent = [&](double percent, bool with_labels) {
    draw_targets_at_percent(percent, orc::VectorscopeLinePhase::VPositive,
                            with_labels);
    if (switched_v) {
      draw_targets_at_percent(percent, orc::VectorscopeLinePhase::VNegative,
                              with_labels);
    }
  };

  // In "both" mode draw 75% targets first (no labels), then 100% (with
  // labels so they sit at the outermost ring and don't overlap).
  if (graticule_mode == 2 || draw_both) {
    draw_percent(0.75, !draw_both);
  }
  if (graticule_mode == 1 || draw_both) {
    draw_percent(1.0, true);
  }
}

void VectorscopeDialogPrivate::drawBurstTargets(
    QPainter& painter, const orc::gui::VectorscopePlotGeometry& geometry,
    orc::VideoSystem system, bool switched_v) {
  const double magnitude = orc::gui::nominalBurstMagnitudeUv(
      system, orc::gui::kVectorscopeSignedFullScale);
  const QColor burst_color(235, 235, 235, 220);

  struct BurstTarget {
    double degrees;
    const char* label;
  };

  const BurstTarget switched_targets[] = {
      {orc::gui::kPalBurstVPositiveDegrees, "Burst"},
      {orc::gui::kPalBurstVNegativeDegrees, "burst"}};
  const BurstTarget single_target[] = {{orc::gui::kNtscBurstDegrees, "Burst"}};

  const BurstTarget* targets = switched_v ? switched_targets : single_target;
  const size_t target_count = switched_v ? 2u : 1u;

  QFont font = painter.font();
  font.setPointSize(12);
  font.setBold(true);

  for (size_t i = 0; i < target_count; ++i) {
    const QPointF centre =
        geometry.pointFromStandardDegrees(targets[i].degrees, magnitude);
    drawTargetBox(painter, geometry, centre, burst_color,
                  kMeasurementTargetBoxSizePixels,
                  kMeasurementCrosshairSizePixels);

    const QPointF label_position = geometry.pointFromStandardDegrees(
        targets[i].degrees,
        magnitude + geometry.pixelsToMagnitude(kBurstLabelOffsetPixels));

    painter.save();
    painter.setFont(font);
    painter.setPen(QPen(QColor(235, 235, 235), 1));
    const QFontMetrics metrics(font);
    const QString text(targets[i].label);
    painter.drawText(
        static_cast<int>(label_position.x()) -
            (metrics.horizontalAdvance(text) / 2),
        static_cast<int>(label_position.y()) + (metrics.height() / 4), text);
    painter.restore();
  }
}

void VectorscopeDialogPrivate::drawColorZones(
    QPainter& painter, VectorscopeDialog* dialog, orc::VideoSystem system,
    int32_t cvbs_white, int32_t cvbs_blanking,
    orc::VectorscopeAcquisitionMode mode) {
  const orc::gui::VectorscopePlotGeometry geometry;
  const int graticule_mode = dialog->getGraticuleMode();
  if (graticule_mode == 0) return;
  if (cvbs_white <= cvbs_blanking) return;

  const bool measurement =
      (mode == orc::VectorscopeAcquisitionMode::CompositeCarrier);
  const bool switched_v = measurement && orc::gui::hasSwitchedVAxis(system);

  painter.setRenderHint(QPainter::Antialiasing, true);

  auto draw_zones_for_phase = [&](double percent,
                                  orc::VectorscopeLinePhase phase) {
    for (int rgb = 1; rgb < 7; rgb++) {
      const orc::UVSample target =
          measurement ? orc::gui::measurementTargetUv(
                            rgb, percent, orc::gui::kVectorscopeSignedFullScale,
                            system, phase)
                      : orc::gui::vectorscopeDisplayTargetUv(
                            rgb, percent, orc::gui::kVectorscopeSignedFullScale,
                            system);
      const double barTheta = std::atan2(-target.v, target.u);
      const double barMagnitude = std::hypot(target.u, target.v);
      drawColorZone(painter, geometry, barTheta, barMagnitude,
                    vectorscopeTargetColor(rgb));
    }
  };

  auto draw_zones_at_percent = [&](double percent) {
    draw_zones_for_phase(percent, orc::VectorscopeLinePhase::VPositive);
    if (switched_v) {
      draw_zones_for_phase(percent, orc::VectorscopeLinePhase::VNegative);
    }
  };

  if (graticule_mode == 2 || graticule_mode == 3) {
    draw_zones_at_percent(0.75);
  }
  if (graticule_mode == 1 || graticule_mode == 3) {
    draw_zones_at_percent(1.0);
  }
}

void VectorscopeDialog::clearDisplay() {
  downgradeIfCanvasFailed();

  QImage blank(orc::gui::kVectorscopeCanvasSize,
               orc::gui::kVectorscopeCanvasSize, QImage::Format_RGB888);
  blank.fill(Qt::black);
  {
    QPainter painter(&blank);
    if (d_->last_data.has_value()) {
      const auto& data = *d_->last_data;
      d_->drawColorZones(painter, this, data.system, data.cvbs_white,
                         data.cvbs_blanking, data.acquisition_mode);
      d_->drawGraticule(painter, this, data.system, data.cvbs_white,
                        data.cvbs_blanking, data.acquisition_mode);
    } else {
      const orc::gui::VectorscopePlotGeometry geometry;
      painter.setRenderHint(QPainter::Antialiasing, true);
      painter.setPen(QPen(Qt::white, orc::gui::kVectorscopeAxisStrokeWidth));
      painter.drawLine(
          QPointF(geometry.centre_point.x(), geometry.plot_area.top()),
          QPointF(geometry.centre_point.x(), geometry.plot_area.bottom()));
      painter.drawLine(
          QPointF(geometry.plot_area.left(), geometry.centre_point.y()),
          QPointF(geometry.plot_area.right(), geometry.centre_point.y()));
      painter.setPen(QPen(Qt::white, orc::gui::kVectorscopeCircleStrokeWidth));
      painter.drawEllipse(geometry.plot_area);
      drawCircleMarkers(painter, geometry);
    }
    painter.end();
  }

  showStaticImage(blank);
  info_label_->setText("No data");
}

void VectorscopeDialog::closeEvent(QCloseEvent* event) {
  emit closed();
  QDialog::closeEvent(event);
}

void VectorscopeDialog::onBlendColorToggled() {
  ORC_LOG_DEBUG("VectorscopeDialog: Blend Color toggled -> {}",
                blend_color_checkbox_->isChecked());
  // Re-render with new blend mode
  if (d_->last_data.has_value()) {
    renderVectorscope(*d_->last_data);
  }
}

void VectorscopeDialog::onDefocusToggled() {
  ORC_LOG_DEBUG("VectorscopeDialog: Defocus toggled -> {}",
                defocus_checkbox_->isChecked());
  // Re-render with new defocus settings
  if (d_->last_data.has_value()) {
    renderVectorscope(*d_->last_data);
  }
}

void VectorscopeDialog::onFieldSelectionChanged() {
  ORC_LOG_DEBUG("VectorscopeDialog: Field selection changed -> {}",
                field_select_group_->checkedId());
  // Re-render with new field selection
  if (d_->last_data.has_value()) {
    renderVectorscope(*d_->last_data);
  }
}

void VectorscopeDialog::onGraticuleChanged() {
  ORC_LOG_DEBUG("VectorscopeDialog: Graticule mode changed -> {}",
                graticule_group_->checkedId());
  // Re-render with new graticule
  if (d_->last_data.has_value()) {
    renderVectorscope(*d_->last_data);
  }
}

void VectorscopeDialog::onDrawLinesToggled() {
  ORC_LOG_DEBUG("VectorscopeDialog: Draw Lines toggled -> {}",
                draw_lines_checkbox_->isChecked());
  // Re-render with or without trace lines
  if (d_->last_data.has_value()) {
    renderVectorscope(*d_->last_data);
  }
}

void VectorscopeDialog::onPointSizeChanged() {
  int size = point_size_spinbox_->value();
  ORC_LOG_DEBUG("VectorscopeDialog: Point size changed -> {}", size);
  // Re-render with new point size
  if (d_->last_data.has_value()) {
    renderVectorscope(*d_->last_data);
  }
}

void VectorscopeDialog::onActiveAreaOnlyToggled() {
  ORC_LOG_DEBUG("VectorscopeDialog: Active area only toggled -> {}",
                isActiveAreaOnly());
  emit dataRefreshRequested();
}

void VectorscopeDialog::onSampleWindowChanged() {
  ORC_LOG_DEBUG("VectorscopeDialog: Sample window changed -> {}",
                static_cast<int>(sampleWindow()));
  emit dataRefreshRequested();
}

void VectorscopeDialog::onLineRangeChanged() {
  updateAcquisitionControlState();
  ORC_LOG_DEBUG("VectorscopeDialog: Line range changed -> {}..{}", firstLine(),
                lastLine());
  if (acquisitionMode() == orc::VectorscopeAcquisitionMode::CompositeCarrier) {
    emit dataRefreshRequested();
  }
}
