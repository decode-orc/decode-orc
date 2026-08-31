/*
 * File:        mainwindow.cpp
 * Module:      orc-gui
 * Purpose:     Main application window
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "mainwindow.h"

#include <orc/stage/common_types.h>
#include <orc/stage/node_type.h>

#include "audio_channel_pair_notice.h"
#include "burstlevelanalysisdialog.h"
#include "cataloguedialog.h"
#include "closedcaptiondialog.h"
#include "cvbs_sequence_continuity_notice.h"
#include "dropout_editor_dialog.h"
#include "dropoutanalysisdialog.h"
#include "ffmpegpresetdialog.h"
#include "field_frame_presentation.h"
#include "fieldpreviewwidget.h"
#include "framescopedialog.h"
#include "frametimingdialog.h"
#include "frametimingwidget.h"
#include "generic_analysis_dialog.h"
#include "line_navigation_mapper.h"
#include "logging.h"
#include "logging_controller.h"
#include "logging_settings_dialog.h"
#include "masklineconfigdialog.h"
#include "ntscobserverdialog.h"
#include "orcgraphicsview.h"
#include "pluginmanagerdialog.h"
#include "presenters/include/analysis_presenter.h"
#include "presenters/include/ntsc_observation_presenter.h"
#include "presenters/include/project_presenter.h"
#include "presenters/include/render_presenter.h"
#include "presenters/include/video_parameter_observation_presenter.h"
#include "preview/preview_data_type_resolution.h"
#include "preview_audio_chase.h"
#include "previewdialog.h"
#include "project_load_status_formatter.h"
#include "projectpropertiesdialog.h"
#include "quick_project_planner.h"
#include "render_coordinator.h"
#include "snranalysisdialog.h"
#include "source_join_notice.h"
#include "stage_help_dialog.h"
#include "stageparameterdialog.h"
#include "theme_controller.h"
#include "theme_manager.h"
#include "vbidialog.h"
#include "version.h"
#include "videoparameterobserverdialog.h"
#include "waveformmonitordialog.h"

// Forward declarations for core types used via opaque pointers
namespace orc {
class DAG;
class Project;
class ObservationContext;
}  // namespace orc

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMoveEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <queue>
#include <unordered_set>

// Helper functions to convert between common types and presenter types
namespace {

constexpr const char* kLineScopeViewId = "preview.linescope";
constexpr const char* kFrameTimingViewId = "preview.frame_timing";
constexpr const char* kWaveformMonitorViewId = "preview.frame_timing";

// How long a project load may run before the modal progress dialog appears.
// Small sources are ready well inside this, so the common case never sees a
// dialog flash on screen.
constexpr int kProjectLoadProgressDelayMs = 400;

// Refresh interval for the dialog's elapsed-seconds counter.
constexpr int kProjectLoadProgressTickMs = 500;

// --- Simple hand-drawn toolbar icons ------------------------------------
// The GUI ships no icon assets, so the toolbar glyphs are painted with
// QPainter. They are rendered in a caller-supplied colour (taken from the
// active palette) so they read correctly in both light and dark themes; the
// toolbar regenerates them whenever the theme changes (see syncThemeUi()).

constexpr int kIconPx = 48;
constexpr double kPi = 3.14159265358979323846;

// 2x2 grid of rounded squares — "arrange DAG to grid".
QIcon makeGridIcon(const QColor& color) {
  QPixmap pm(kIconPx, kIconPx);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(Qt::NoPen);
  p.setBrush(color);
  const qreal cell = 16.0;
  const qreal gap = 6.0;
  const qreal start = (kIconPx - (2 * cell + gap)) / 2.0;
  for (int row = 0; row < 2; ++row) {
    for (int col = 0; col < 2; ++col) {
      const qreal x = start + col * (cell + gap);
      const qreal y = start + row * (cell + gap);
      p.drawRoundedRect(QRectF(x, y, cell, cell), 3, 3);
    }
  }
  return QIcon(pm);
}

// Almond eye with a pupil — "show preview".
QIcon makePreviewIcon(const QColor& color) {
  QPixmap pm(kIconPx, kIconPx);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(color, 3.5);
  pen.setJoinStyle(Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  const qreal cx = kIconPx / 2.0;
  const qreal cy = kIconPx / 2.0;
  const qreal hw = 18.0;  // half width
  const qreal h = 11.0;   // vertical bulge
  QPainterPath path;
  path.moveTo(cx - hw, cy);
  path.quadTo(cx, cy - h, cx + hw, cy);
  path.quadTo(cx, cy + h, cx - hw, cy);
  p.drawPath(path);
  p.setPen(Qt::NoPen);
  p.setBrush(color);
  p.drawEllipse(QPointF(cx, cy), 5.0, 5.0);
  return QIcon(pm);
}

// Circular arrow — "reload all sources".
QIcon makeReloadIcon(const QColor& color) {
  QPixmap pm(kIconPx, kIconPx);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  const qreal cx = kIconPx / 2.0;
  const qreal cy = kIconPx / 2.0;
  const qreal r = 13.0;

  // Qt arc angles are sixteenths of a degree measured anticlockwise from three
  // o'clock; a negative span sweeps clockwise. Starting at 60 degrees and
  // sweeping 300 degrees clockwise leaves a gap across the top of the ring for
  // the arrowhead.
  constexpr qreal kArcStartDeg = 60.0;
  constexpr qreal kArcSpanDeg = 300.0;
  QPen pen(color, 3.5);
  pen.setCapStyle(Qt::FlatCap);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  p.drawArc(QRectF(cx - r, cy - r, 2 * r, 2 * r),
            static_cast<int>(kArcStartDeg * 16),
            static_cast<int>(-kArcSpanDeg * 16));

  // Arrowhead at the clockwise end of the arc, aligned with the tangent there
  // so the ring reads as turning rather than as a broken circle.
  const qreal end_rad = (kArcStartDeg - kArcSpanDeg) * kPi / 180.0;
  p.translate(cx + std::cos(end_rad) * r, cy - std::sin(end_rad) * r);
  p.rotate(std::atan2(std::cos(end_rad), std::sin(end_rad)) * 180.0 / kPi);
  const qreal head = 7.0;
  QPainterPath tri;
  tri.moveTo(head, 0.0);
  tri.lineTo(-head * 0.25, -head * 0.8);
  tri.lineTo(-head * 0.25, head * 0.8);
  tri.closeSubpath();
  p.setPen(Qt::NoPen);
  p.setBrush(color);
  p.drawPath(tri);
  return QIcon(pm);
}

// Sun / moon / half-disc depending on mode — the cycling theme button.
QIcon makeThemeIcon(const QColor& color, ThemeManager::Mode mode) {
  QPixmap pm(kIconPx, kIconPx);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  const qreal cx = kIconPx / 2.0;
  const qreal cy = kIconPx / 2.0;
  const qreal r = 11.0;

  if (mode == ThemeManager::Mode::Dark) {
    // Crescent moon: a disc with an offset disc subtracted from it.
    QPainterPath full;
    full.addEllipse(QPointF(cx - 2, cy), r, r);
    QPainterPath cut;
    cut.addEllipse(QPointF(cx + 5, cy - 3), r, r);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(full.subtracted(cut));
  } else if (mode == ThemeManager::Mode::Light) {
    // Sun: disc + eight rays.
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QPointF(cx, cy), r * 0.6, r * 0.6);
    QPen pen(color, 3.0);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    for (int i = 0; i < 8; ++i) {
      const qreal a = i * kPi / 4.0;
      p.drawLine(
          QPointF(cx + std::cos(a) * (r * 0.9), cy + std::sin(a) * (r * 0.9)),
          QPointF(cx + std::cos(a) * (r * 1.35),
                  cy + std::sin(a) * (r * 1.35)));
    }
  } else {
    // Auto: a ring with its right half filled (adaptive light/dark).
    QPen pen(color, 3.0);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(cx, cy), r, r);
    QPainterPath half;
    half.moveTo(cx, cy - r);
    half.arcTo(QRectF(cx - r, cy - r, 2 * r, 2 * r), 90, -180);
    half.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(half);
  }
  return QIcon(pm);
}

orc::presenters::VideoFormat toPresenterVideoFormat(orc::VideoSystem system) {
  switch (system) {
    case orc::VideoSystem::NTSC:
      return orc::presenters::VideoFormat::NTSC;
    case orc::VideoSystem::PAL:
      return orc::presenters::VideoFormat::PAL;
    case orc::VideoSystem::PAL_M:
      return orc::presenters::VideoFormat::PAL_M;
    case orc::VideoSystem::Unknown:
      return orc::presenters::VideoFormat::Unknown;
  }
  return orc::presenters::VideoFormat::Unknown;
}

orc::presenters::SourceType toPresenterSourceType(orc::SourceType type) {
  switch (type) {
    case orc::SourceType::Composite:
      return orc::presenters::SourceType::Composite;
    case orc::SourceType::YC:
      return orc::presenters::SourceType::YC;
    case orc::SourceType::Unknown:
      return orc::presenters::SourceType::Unknown;
  }
  return orc::presenters::SourceType::Unknown;
}

std::map<std::string, orc::ParameterValue>
sourceParametersToVideoParamsStageValues(const orc::SourceParameters& params) {
  // Colour burst range and IRE levels derived from system constants.
  // EBU Tech. 3280-E §1.1 (PAL) / SMPTE 244M-2003 §4.1 (NTSC) /
  // ITU-R BT.1700-1 Annex 1 Part B (PAL_M).
  // Colour burst sample range: EBU Tech. 3280-E Table 1 (PAL) /
  // SMPTE 244M-2003 Table 1 (NTSC/PAL_M).
  const int32_t cb_start = (params.system == orc::VideoSystem::PAL) ? 98 : 72;
  const int32_t cb_end = (params.system == orc::VideoSystem::PAL) ? 138 : 108;
  return {{"colourBurstStart", cb_start},
          {"colourBurstEnd", cb_end},
          {"activeVideoStart", params.active_video_start},
          {"activeVideoEnd", params.active_video_end},
          {"firstActiveFieldLine", params.first_active_frame_line / 2},
          {"lastActiveFieldLine", params.last_active_frame_line / 2},
          {"whiteLevel", params.white_level},
          {"blackLevel", params.blanking_level}};
}

orc::ParameterValue resolveEffectiveParameterValue(
    const orc::ParameterDescriptor& desc,
    const std::map<std::string, orc::ParameterValue>& values) {
  auto it = values.find(desc.name);
  if (it != values.end()) {
    return it->second;
  }
  if (desc.constraints.default_value.has_value()) {
    return *desc.constraints.default_value;
  }

  switch (desc.type) {
    case orc::ParameterType::INT32:
      return int32_t{0};
    case orc::ParameterType::UINT32:
      return uint32_t{0};
    case orc::ParameterType::DOUBLE:
      return double{0.0};
    case orc::ParameterType::BOOL:
      return false;
    case orc::ParameterType::STRING:
    case orc::ParameterType::FILE_PATH:
      return std::string{};
  }

  return std::string{};
}

orc::ParameterValue normalizeEffectiveParameterValue(
    const orc::ParameterDescriptor& desc, const orc::ParameterValue& value) {
  switch (desc.type) {
    case orc::ParameterType::INT32: {
      int32_t normalized = std::get<int32_t>(value);
      if (desc.constraints.min_value.has_value()) {
        normalized = std::max(normalized,
                              std::get<int32_t>(*desc.constraints.min_value));
      }
      if (desc.constraints.max_value.has_value()) {
        normalized = std::min(normalized,
                              std::get<int32_t>(*desc.constraints.max_value));
      }
      return normalized;
    }
    case orc::ParameterType::UINT32: {
      uint32_t normalized = std::get<uint32_t>(value);
      if (desc.constraints.min_value.has_value()) {
        normalized = std::max(normalized,
                              std::get<uint32_t>(*desc.constraints.min_value));
      }
      if (desc.constraints.max_value.has_value()) {
        normalized = std::min(normalized,
                              std::get<uint32_t>(*desc.constraints.max_value));
      }
      return normalized;
    }
    case orc::ParameterType::DOUBLE: {
      double normalized = std::get<double>(value);
      if (desc.constraints.min_value.has_value()) {
        normalized =
            std::max(normalized, std::get<double>(*desc.constraints.min_value));
      }
      if (desc.constraints.max_value.has_value()) {
        normalized =
            std::min(normalized, std::get<double>(*desc.constraints.max_value));
      }
      normalized = std::round(normalized * 10000.0) / 10000.0;
      return normalized;
    }
    case orc::ParameterType::BOOL:
      return std::get<bool>(value);
    case orc::ParameterType::STRING:
    case orc::ParameterType::FILE_PATH:
      return std::get<std::string>(value);
  }

  return value;
}

std::map<std::string, orc::ParameterValue> mergeParameterValues(
    const std::map<std::string, orc::ParameterValue>& base,
    const std::map<std::string, orc::ParameterValue>& overrides) {
  auto merged = base;
  for (const auto& [key, value] : overrides) {
    merged[key] = value;
  }
  return merged;
}

bool isUnsetVideoParamsStageValue(const orc::ParameterValue& value) {
  if (const auto* int_value = std::get_if<int32_t>(&value)) {
    return *int_value == -1;
  }
  return false;
}

void applyMetadataFallbackValues(
    std::map<std::string, orc::ParameterValue>& current_values,
    const std::map<std::string, orc::ParameterValue>& metadata_values) {
  for (const auto& [param_name, metadata_value] : metadata_values) {
    auto current_it = current_values.find(param_name);
    if (current_it == current_values.end() ||
        isUnsetVideoParamsStageValue(current_it->second)) {
      current_values[param_name] = metadata_value;
    }
  }
}
}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      preview_dialog_(nullptr),
      vbi_dialog_(nullptr),
      ntsc_observer_dialog_(nullptr),
      closed_caption_dialog_(nullptr),
      dag_view_(nullptr),
      dag_model_(nullptr),
      dag_scene_(nullptr),
      save_project_action_(nullptr),
      save_project_as_action_(nullptr),
      edit_project_action_(nullptr),
      show_preview_action_(nullptr),
      auto_show_preview_action_(nullptr),
      render_coordinator_(nullptr),
      current_view_node_id_(),
      last_dropout_node_id_(),
      last_dropout_mode_(orc::DropoutAnalysisMode::FULL_FIELD),
      last_dropout_output_type_(orc::PreviewOutputType::Frame_Field1_First),
      last_snr_node_id_(),
      last_snr_mode_(orc::SNRAnalysisMode::WHITE),
      last_snr_output_type_(orc::PreviewOutputType::Frame_Field1_First),
      current_output_type_(orc::PreviewOutputType::Frame_Field1_First),
      current_option_id_(
          "interlaced_clamped")  // Default to "Interlaced Clamped" option
      ,
      current_aspect_ratio_mode_(
          orc::AspectRatioMode::DAR_4_3)  // Default to 4:3
      ,
      trigger_progress_dialog_(nullptr),
      last_line_scope_field_index_(std::numeric_limits<uint64_t>::max()),
      last_line_scope_line_number_(-1),
      last_line_scope_image_x_(-1),
      last_line_scope_image_y_(-1) {
  // Create and start render coordinator
  render_coordinator_ = std::make_unique<RenderCoordinator>(this);

  // Presenter for dropout editing (uses ProjectPresenter for delegation)
  dropout_presenter_ = std::make_unique<orc::presenters::DropoutPresenter>(
      *project_.presenter());

  // Connect coordinator signals (emitted from worker thread; queue to GUI
  // thread)
  connect(render_coordinator_.get(), &RenderCoordinator::previewReady, this,
          &MainWindow::onPreviewReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::vbiDataReady, this,
          &MainWindow::onVBIDataReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::closedCaptionDataReady,
          this, &MainWindow::onClosedCaptionDataReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::availableOutputsReady,
          this, &MainWindow::onAvailableOutputsReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::audioChannelPairsReady,
          this, &MainWindow::onAudioChannelPairsReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::audioStreamReaderReady,
          this, &MainWindow::onAudioStreamReaderReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::lineSamplesReady, this,
          &MainWindow::onLineSamplesReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::frameTimingDataReady,
          this, &MainWindow::onFrameTimingDataReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(),
          &RenderCoordinator::waveformMonitorDataReady, this,
          &MainWindow::onWaveformMonitorDataReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::dropoutDataReady, this,
          &MainWindow::onDropoutDataReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::snrDataReady, this,
          &MainWindow::onSNRDataReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::burstLevelDataReady,
          this, &MainWindow::onBurstLevelDataReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::catalogueDataReady,
          this, &MainWindow::onCatalogueDataReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::resultsNotAvailable,
          this, &MainWindow::onResultsNotAvailable, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::triggerProgress, this,
          &MainWindow::onTriggerProgress, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::triggerComplete, this,
          &MainWindow::onTriggerComplete, Qt::QueuedConnection);
  connect(render_coordinator_.get(),
          &RenderCoordinator::frameLineNavigationReady, this,
          &MainWindow::onFrameLineNavigationReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::error, this,
          &MainWindow::onCoordinatorError, Qt::QueuedConnection);
  // Phase 5: async observation delivery, background-workload progress, and the
  // Phase 3 invalidation signal (previously emitted but unconnected).
  connect(render_coordinator_.get(), &RenderCoordinator::observationDataReady,
          this, &MainWindow::onObservationDataReady, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::observationProgress,
          this, &MainWindow::onObservationProgress, Qt::QueuedConnection);
  connect(render_coordinator_.get(), &RenderCoordinator::executionProgress,
          this, &MainWindow::onExecutionProgress, Qt::QueuedConnection);
  connect(render_coordinator_.get(),
          &RenderCoordinator::observationsInvalidated, this,
          &MainWindow::onObservationsInvalidated, Qt::QueuedConnection);

  // Set the project for the render coordinator (required before updateDAG)
  render_coordinator_->setProject(project_.presenter()->getCoreProjectHandle());

  // Start the coordinator worker thread
  render_coordinator_->start();

  // Aspect ratio display is handled exclusively in GUI (no core scaling)

  setupUI();
  setupMenus();
  setupToolbar();

  // Slow-render title timer — fires after 2 s if a render is still in-flight so
  // the user knows the application is working (e.g. during 26-second NN
  // inference). beginPreviewRenderInFlight() / endPreviewRenderInFlight()
  // manage start/stop.
  render_slow_timer_ = new QTimer(this);
  render_slow_timer_->setSingleShot(true);
  render_slow_timer_->setInterval(2000);
  connect(render_slow_timer_, &QTimer::timeout, this, [this]() {
    if (preview_dialog_) {
      preview_dialog_->setWindowTitle("Field/Frame Preview - Rendering...");
    }
  });

  // Project-load progress timers. The show timer keeps the modal off screen for
  // loads that finish quickly; the tick timer refreshes the elapsed counter so
  // a stage that holds position for a minute still looks alive.
  project_load_show_timer_ = new QTimer(this);
  project_load_show_timer_->setSingleShot(true);
  project_load_show_timer_->setInterval(kProjectLoadProgressDelayMs);
  connect(project_load_show_timer_, &QTimer::timeout, this, [this]() {
    if (project_load_in_progress_ && project_load_progress_dialog_) {
      ORC_LOG_DEBUG("Project load exceeded {} ms; showing progress dialog",
                    kProjectLoadProgressDelayMs);
      updateProjectLoadProgressLabel();
      project_load_progress_dialog_->show();
      project_load_tick_timer_->start();
    }
  });

  project_load_tick_timer_ = new QTimer(this);
  project_load_tick_timer_->setInterval(kProjectLoadProgressTickMs);
  connect(project_load_tick_timer_, &QTimer::timeout, this,
          [this]() { updateProjectLoadProgressLabel(); });

  updateWindowTitle();

  // Restore window geometry and state from settings
  restoreSettings();

  updateUIState();
  reportPluginRuntimeDiagnostics(true);
}

MainWindow::~MainWindow() {
  // Explicitly disconnect and delete DAG scene/model/view to avoid Qt teardown
  // assertions
  if (dag_scene_) {
    dag_scene_->disconnect();
    delete dag_scene_;
    dag_scene_ = nullptr;
  }
  if (dag_model_) {
    delete dag_model_;
    dag_model_ = nullptr;
  }
  if (dag_view_) {
    delete dag_view_;
    dag_view_ = nullptr;
  }

  if (vbi_dialog_) {
    delete vbi_dialog_;
    vbi_dialog_ = nullptr;
  }
  if (ntsc_observer_dialog_) {
    delete ntsc_observer_dialog_;
    ntsc_observer_dialog_ = nullptr;
  }
  if (closed_caption_dialog_) {
    delete closed_caption_dialog_;
    closed_caption_dialog_ = nullptr;
  }
  if (preview_dialog_) {
    delete preview_dialog_;
    preview_dialog_ = nullptr;
  }
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (!checkUnsavedChanges()) {
    event->ignore();
    return;
  }

  saveSettings();
  QMainWindow::closeEvent(event);
}

void MainWindow::saveSettings() {
  QSettings settings("orc-project", "orc-gui");

  // Save main window geometry and state
  settings.setValue("mainwindow/geometry", saveGeometry());
  settings.setValue("mainwindow/state", saveState());

  // Save preview dialog geometry (ld-analyse pattern)
  settings.setValue("previewdialog/geometry", preview_dialog_->saveGeometry());
}

void MainWindow::restoreSettings() {
  QSettings settings("orc-project", "orc-gui");

  // Restore main window geometry and state
  if (settings.contains("mainwindow/geometry")) {
    restoreGeometry(settings.value("mainwindow/geometry").toByteArray());
  } else {
    resize(1200, 800);
  }

  if (settings.contains("mainwindow/state")) {
    restoreState(settings.value("mainwindow/state").toByteArray());
  }

  // Restore preview dialog geometry (ld-analyse pattern)
  if (settings.contains("previewdialog/geometry")) {
    preview_dialog_->restoreGeometry(
        settings.value("previewdialog/geometry").toByteArray());
  }
}

void MainWindow::setupUI() {
  // Create preview dialog (initially hidden)
  preview_dialog_ = new PreviewDialog(this);

  // Create VBI dialog (initially hidden)
  vbi_dialog_ = new VBIDialog(this);

  // Create video parameter observer dialog (initially hidden)
  video_parameter_observer_dialog_ = new VideoParameterObserverDialog(this);

  // Create NTSC observer dialog (initially hidden)
  ntsc_observer_dialog_ = new NtscObserverDialog(this);

  // Create closed caption preview dialog (initially hidden)
  closed_caption_dialog_ = new ClosedCaptionDialog(this);
  // Changing the caption service throws away the decode run, so the window has
  // to be read again even though the previewer has not moved.
  connect(closed_caption_dialog_, &ClosedCaptionDialog::dataRequestNeeded, this,
          &MainWindow::issueClosedCaptionRequests);

  // Note: Dropout, SNR, and Burst Level analysis dialogs are now created
  // per-stage in runAnalysisForNode() to allow each stage to have its own
  // independent dialog

  // Connect preview dialog signals.
  // PreviewDialog owns all navigation state (position, clamping, debouncing).
  // positionChanged  — fires on every scrub/click so MainWindow can update
  // labels. renderRequested  — fires when a render should happen (already
  // debounced).
  connect(preview_dialog_, &PreviewDialog::positionChanged, this, [this](int) {
    updatePreviewInfo();
    preview_dialog_->setSharedPreviewCoordinate(
        buildCurrentPreviewCoordinate());
  });
  connect(preview_dialog_, &PreviewDialog::renderRequested, this, [this](int) {
    if (!preview_render_in_flight_) {
      updateAllPreviewComponents();
    }
    // else: in-flight — onPreviewReady checks currentIndex() vs
    // pending_render_index_
  });
  connect(preview_dialog_, &PreviewDialog::previewModeChanged, this,
          &MainWindow::onPreviewModeChanged);
  connect(preview_dialog_, &PreviewDialog::signalChanged, this, [this](int) {
    refreshPreviewViewAvailability();
    updatePreview();
  });  // Re-render when signal selection changes
  connect(preview_dialog_, &PreviewDialog::aspectRatioModeChanged, this,
          &MainWindow::onAspectRatioModeChanged);
  connect(preview_dialog_, &PreviewDialog::exportPNGRequested, this,
          &MainWindow::onPreviewDialogExportPNG);
  connect(preview_dialog_, &PreviewDialog::showDropoutsChanged, this,
          [this](bool show) {
            render_coordinator_->setShowDropouts(show);
            updatePreview();
          });
  // Creating a playback reader resolves the node's representation, which
  // executes the DAG — so it goes through the worker like every other query.
  connect(preview_dialog_, &PreviewDialog::audioStreamReaderRequested, this,
          [this](size_t pair) {
            if (!current_view_node_id_.is_valid()) {
              preview_dialog_->setAudioStreamReader(nullptr);
              return;
            }
            pending_audio_reader_request_id_ =
                render_coordinator_->requestAudioStreamReader(
                    current_view_node_id_, pair);
          });
  connect(preview_dialog_, &PreviewDialog::showVBIDialogRequested, this,
          &MainWindow::onShowVBIDialog);
  connect(preview_dialog_,
          &PreviewDialog::showVideoParameterObserverDialogRequested, this,
          &MainWindow::onShowVideoParameterObserverDialog);
  connect(preview_dialog_, &PreviewDialog::showNtscObserverDialogRequested,
          this, &MainWindow::onShowNtscObserverDialog);
  connect(preview_dialog_, &PreviewDialog::showClosedCaptionDialogRequested,
          this, &MainWindow::onShowClosedCaptionDialog);
  connect(preview_dialog_, &PreviewDialog::lineScopeRequested, this,
          &MainWindow::onLineScopeRequested);
  connect(preview_dialog_, &PreviewDialog::lineNavigationRequested, this,
          &MainWindow::onLineNavigation);
  connect(preview_dialog_, &PreviewDialog::sampleMarkerMovedInLineScope, this,
          &MainWindow::onSampleMarkerMoved);
  connect(preview_dialog_, &PreviewDialog::frameTimingRequested, this,
          &MainWindow::onFrameTimingRequested);
  connect(preview_dialog_, &PreviewDialog::waveformMonitorRequested, this,
          &MainWindow::onWaveformMonitorRequested);
  connect(preview_dialog_, &PreviewDialog::vectorscopeRequested, this,
          &MainWindow::onPreviewVectorscopeRequested);
  connect(preview_dialog_, &PreviewDialog::histogramRequested, this,
          &MainWindow::onPreviewHistogramRequested);
  // Connect preview frame changed signal to line scope
  // When frame changes, line scope should refresh samples at its current
  // field/line
  auto frame_scope = preview_dialog_->frameScopeDialog();
  if (frame_scope) {
    connect(preview_dialog_, &PreviewDialog::previewFrameChanged, this,
            &MainWindow::onLineScopeRefreshAtFieldLine);
    connect(frame_scope, &FrameScopeDialog::dialogClosed, this,
            &MainWindow::onFrameScopeDialogClosed);
  }

  // Connect preview frame changed signal to frame timing
  auto frame_timing = preview_dialog_->frameTimingDialog();
  if (frame_timing) {
    connect(preview_dialog_, &PreviewDialog::previewFrameChanged, this,
            [this]() {
              auto* dialog = preview_dialog_->frameTimingDialog();
              if (dialog && dialog->isVisible()) {
                onFrameTimingRequested();
              }
            });
    connect(frame_timing, &FrameTimingDialog::refreshRequested, this,
            &MainWindow::onFrameTimingRequested);
    connect(frame_timing, &FrameTimingDialog::setCrosshairsRequested, this,
            &MainWindow::onSetCrosshairsFromFrameTiming);
  }

  // Connect preview frame changed signal to waveform monitor
  auto waveform_monitor = preview_dialog_->waveformMonitorDialog();
  if (waveform_monitor) {
    connect(preview_dialog_, &PreviewDialog::previewFrameChanged, this,
            [this]() {
              auto* dialog = preview_dialog_->waveformMonitorDialog();
              if (dialog && dialog->isVisible()) {
                onWaveformMonitorRequested();
              }
            });
  }

  // Create QtNodes DAG editor
  dag_view_ = new OrcGraphicsView(this);
  dag_model_ = new OrcGraphModel(*project_.presenter(), dag_view_);
  dag_scene_ = new OrcGraphicsScene(*dag_model_, dag_view_);

  dag_view_->setScene(dag_scene_);
  // Set alignment so (0,0) appears at top-left of view
  dag_view_->setAlignment(Qt::AlignLeft | Qt::AlignTop);

  // Connect scene/model signals for DAG modifications
  connectDAGSignals();

  // DAG editor takes up full main window
  setCentralWidget(dag_view_);

  // Status bar
  statusBar()->showMessage("Ready");
}

void MainWindow::reportPluginRuntimeDiagnostics(bool show_error_dialog) {
  if (!project_.presenter()) {
    return;
  }

  const auto loaded_plugins = project_.presenter()->listLoadedPlugins();
  const auto diagnostics = project_.presenter()->listPluginDiagnostics();
  const auto search_paths = project_.presenter()->listPluginSearchPaths();
  const auto registry = project_.presenter()->getPluginRegistry();

  if (!registry.registry_path.empty()) {
    ORC_LOG_DEBUG("Plugin registry path: {}", registry.registry_path);
  }

  if (!registry.entries.empty()) {
    ORC_LOG_DEBUG("Configured {} plugin registry entr{}",
                  registry.entries.size(),
                  registry.entries.size() == 1 ? "y" : "ies");
    for (const auto& entry : registry.entries) {
      ORC_LOG_DEBUG(
          "  registry entry '{}' enabled={} loaded={} exists={} path='{}'",
          entry.selector, entry.enabled ? "true" : "false",
          entry.is_loaded ? "true" : "false",
          entry.path_exists ? "true" : "false", entry.path);
    }
  }

  if (!search_paths.empty()) {
    ORC_LOG_DEBUG("Configured runtime stage plugin search paths: {}",
                  search_paths.size());
    for (const auto& path : search_paths) {
      ORC_LOG_DEBUG("  plugin search path: {}", path);
    }
  }

  if (!loaded_plugins.empty()) {
    ORC_LOG_DEBUG("Loaded {} runtime stage plugin(s)", loaded_plugins.size());
    for (const auto& plugin : loaded_plugins) {
      ORC_LOG_DEBUG("  plugin '{}' version '{}' registered {} stage(s)",
                    plugin.plugin_id, plugin.plugin_version,
                    plugin.registered_stage_names.size());
    }
  }

  int warning_count = 0;
  int error_count = 0;
  QStringList error_lines;

  for (const auto& diagnostic : diagnostics) {
    const QString path_suffix =
        diagnostic.path.empty()
            ? QString()
            : QString(" [%1]").arg(QString::fromStdString(diagnostic.path));
    const QString message =
        QString::fromStdString(diagnostic.message) + path_suffix;

    switch (diagnostic.severity) {
      case orc::presenters::PluginDiagnosticSeverity::Info:
        ORC_LOG_DEBUG("Plugin runtime: {}", message.toStdString());
        break;
      case orc::presenters::PluginDiagnosticSeverity::Warning:
        ++warning_count;
        ORC_LOG_WARN("Plugin runtime: {}", message.toStdString());
        break;
      case orc::presenters::PluginDiagnosticSeverity::Error:
        ++error_count;
        error_lines.push_back(message);
        ORC_LOG_ERROR("Plugin runtime: {}", message.toStdString());
        break;
    }
  }

  if (error_count > 0) {
    statusBar()->showMessage(
        QString("Stage plugin initialization completed with %1 error(s)")
            .arg(error_count),
        7000);

    if (show_error_dialog) {
      QString details;
      const qsizetype max_lines = std::min<qsizetype>(error_lines.size(), 8);
      for (qsizetype index = 0; index < max_lines; ++index) {
        details += QString("- %1\n").arg(error_lines[index]);
      }
      if (error_lines.size() > max_lines) {
        details += QString("- (%1 additional errors omitted)")
                       .arg(error_lines.size() - max_lines);
      }

      QMessageBox::warning(this, "Stage Plugin Errors",
                           QString("Some stage plugins failed to load.\n\n%1")
                               .arg(details.trimmed()));
    }
    return;
  }

  if (warning_count > 0) {
    statusBar()->showMessage(
        QString("Stage plugin initialization completed with %1 warning(s)")
            .arg(warning_count),
        5000);
    return;
  }

  if (!loaded_plugins.empty()) {
    statusBar()->showMessage(
        QString("Loaded %1 runtime stage plugin(s) from %2 registry entr%3")
            .arg(loaded_plugins.size())
            .arg(registry.entries.size())
            .arg(registry.entries.size() == 1 ? "y" : "ies"),
        3000);
  }
}

void MainWindow::setupMenus() {
  auto* file_menu = menuBar()->addMenu("&File");

  // New Project action - opens dialog for all four project types
  auto* new_project_action = file_menu->addAction("&New Project...");
  new_project_action->setShortcut(QKeySequence::New);
  connect(new_project_action, &QAction::triggered, this,
          &MainWindow::onNewProject);

  auto* quick_project_action = file_menu->addAction("&Quick Project...");
  // Avoid conflict with Quit on macOS (Command+Q maps to Ctrl in Qt shortcuts).
  quick_project_action->setShortcut(
      QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Q));
  connect(quick_project_action, &QAction::triggered, this,
          &MainWindow::onQuickProject);

  auto* open_project_action = file_menu->addAction("&Open Project...");
  open_project_action->setShortcut(QKeySequence::Open);
  connect(open_project_action, &QAction::triggered, this,
          &MainWindow::onOpenProject);

  file_menu->addSeparator();

  save_project_action_ = file_menu->addAction("&Save Project");
  save_project_action_->setShortcut(QKeySequence::Save);
  save_project_action_->setEnabled(false);
  connect(save_project_action_, &QAction::triggered, this,
          &MainWindow::onSaveProject);

  save_project_as_action_ = file_menu->addAction("Save Project &As...");
  save_project_as_action_->setShortcut(QKeySequence::SaveAs);
  save_project_as_action_->setEnabled(false);
  connect(save_project_as_action_, &QAction::triggered, this,
          &MainWindow::onSaveProjectAs);

  file_menu->addSeparator();

  edit_project_action_ = file_menu->addAction("&Edit Project...");
  edit_project_action_->setEnabled(false);
  connect(edit_project_action_, &QAction::triggered, this,
          &MainWindow::onEditProject);

  reload_sources_action_ = file_menu->addAction("&Reload All Sources");
  reload_sources_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
  // Application scope so the shortcut works while the Preview window (or any
  // other non-modal window) holds focus; Qt still suppresses it while a modal
  // dialog blocks the main window.
  reload_sources_action_->setShortcutContext(Qt::ApplicationShortcut);
  reload_sources_action_->setEnabled(false);
  connect(reload_sources_action_, &QAction::triggered, this,
          &MainWindow::onReloadAllSources);

  file_menu->addSeparator();

  auto* quit_action = file_menu->addAction("&Quit");
  quit_action->setShortcut(QKeySequence::Quit);
  connect(quit_action, &QAction::triggered, this, &QWidget::close);

  // View menu for DAG operations
  auto* view_menu = menuBar()->addMenu("&View");
  view_menu_ = view_menu;  // shared with setupToolbar() for the toolbar toggle

  show_preview_action_ = view_menu->addAction("Show &Preview");
  show_preview_action_->setShortcut(
      QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
  show_preview_action_->setEnabled(false);
  connect(show_preview_action_, &QAction::triggered, this, [this]() {
    // Show, or raise to the front if already visible.
    preview_dialog_->show();
    preview_dialog_->raise();
    preview_dialog_->activateWindow();
  });

  view_menu->addSeparator();

  auto_show_preview_action_ =
      view_menu->addAction("Show Preview on &Selection");
  auto_show_preview_action_->setCheckable(true);

  // Load setting from QSettings (default: true)
  QSettings settings;
  bool auto_show =
      settings.value("preview/auto_show_on_selection", true).toBool();
  auto_show_preview_action_->setChecked(auto_show);

  // Save setting when changed
  connect(auto_show_preview_action_, &QAction::toggled, this, [](bool checked) {
    QSettings settings;
    settings.setValue("preview/auto_show_on_selection", checked);
  });

  view_menu->addSeparator();

  arrange_dag_action_ = view_menu->addAction("&Arrange DAG to Grid");
  arrange_dag_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
  connect(arrange_dag_action_, &QAction::triggered, this,
          &MainWindow::onArrangeDAGToGrid);

  // Tools menu
  auto* tools_menu = menuBar()->addMenu("&Tools");

  plugin_manager_action_ = tools_menu->addAction("&Plugin Manager...");
  connect(plugin_manager_action_, &QAction::triggered, this, [this]() {
    orc::PluginManagerDialog dlg(this);
    dlg.exec();
  });

  // Diagnostic logging: turns a log file on, chooses how much detail it
  // records, and says where it goes, so a bug report can carry one without the
  // reporter having to relaunch from a command line.
  logging_action_ = tools_menu->addAction("&Logging...");
  connect(logging_action_, &QAction::triggered, this,
          &MainWindow::onConfigureLogging);

  // Themes submenu: overrides the --theme command-line option at runtime.
  tools_menu->addSeparator();
  auto* theme_menu = tools_menu->addMenu("&Themes");
  auto* theme_group = new QActionGroup(this);
  theme_group->setExclusive(true);

  const struct {
    const char* label;
    ThemeManager::Mode mode;
  } theme_items[] = {
      {"&Auto", ThemeManager::Mode::Auto},
      {"&Dark", ThemeManager::Mode::Dark},
      {"&Light", ThemeManager::Mode::Light},
  };

  const ThemeManager::Mode current_mode =
      ThemeController::instance() ? ThemeController::instance()->mode()
                                  : ThemeManager::Mode::Auto;

  for (const auto& item : theme_items) {
    QAction* theme_action = theme_menu->addAction(item.label);
    theme_action->setCheckable(true);
    theme_action->setChecked(item.mode == current_mode);
    theme_group->addAction(theme_action);

    // Keep references so the toolbar's cycling theme button (and OS-driven
    // Auto changes) can keep these checkmarks in sync via syncThemeUi().
    switch (item.mode) {
      case ThemeManager::Mode::Auto:
        theme_auto_action_ = theme_action;
        break;
      case ThemeManager::Mode::Dark:
        theme_dark_action_ = theme_action;
        break;
      case ThemeManager::Mode::Light:
        theme_light_action_ = theme_action;
        break;
    }

    const ThemeManager::Mode mode = item.mode;
    connect(theme_action, &QAction::triggered, this, [mode]() {
      if (auto* controller = ThemeController::instance()) {
        controller->setMode(mode);
      }
    });
  }

  // Help menu
  auto* help_menu = menuBar()->addMenu("&Help");

  auto* user_guide_action = help_menu->addAction("&User Guide...");
  connect(user_guide_action, &QAction::triggered, this, [this]() {
    QFile f(":/orc-gui/docs/main_window.md");
    const QString md = f.open(QIODevice::ReadOnly)
                           ? QString::fromUtf8(f.readAll())
                           : QString{};
    auto* dlg = new StageHelpDialog("Main Window", md, this);
    dlg->show();
  });

  help_menu->addSeparator();

  auto* about_action = help_menu->addAction("&About Orc GUI...");
  connect(about_action, &QAction::triggered, this, &MainWindow::onAbout);
}

void MainWindow::setupToolbar() {
  main_toolbar_ = addToolBar("Main Toolbar");
  main_toolbar_->setObjectName("MainToolBar");
  main_toolbar_->setMovable(false);
  main_toolbar_->setFloatable(false);
  main_toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);

  // Reuse the existing menu actions so their enabled/checked state stays in
  // sync; the menu items themselves are retained. Icons are assigned in
  // syncThemeUi() so they track the active theme.
  main_toolbar_->addAction(arrange_dag_action_);
  main_toolbar_->addAction(show_preview_action_);
  main_toolbar_->addAction(reload_sources_action_);
  main_toolbar_->addSeparator();

  // Single button that cycles Auto -> Light -> Dark. The Tools > Themes
  // submenu remains as the explicit-choice alternative.
  theme_cycle_action_ = new QAction(this);
  main_toolbar_->addAction(theme_cycle_action_);
  connect(theme_cycle_action_, &QAction::triggered, this, []() {
    auto* controller = ThemeController::instance();
    if (!controller) {
      return;
    }
    ThemeManager::Mode next = ThemeManager::Mode::Auto;
    switch (controller->mode()) {
      case ThemeManager::Mode::Auto:
        next = ThemeManager::Mode::Light;
        break;
      case ThemeManager::Mode::Light:
        next = ThemeManager::Mode::Dark;
        break;
      case ThemeManager::Mode::Dark:
        next = ThemeManager::Mode::Auto;
        break;
    }
    controller->setMode(next);
  });

  // View > Show Toolbar — QToolBar supplies a checkable show/hide action.
  QAction* toggle_toolbar = main_toolbar_->toggleViewAction();
  toggle_toolbar->setText("Show &Toolbar");
  if (view_menu_) {
    view_menu_->addSeparator();
    view_menu_->addAction(toggle_toolbar);
  }

  // Refresh icons + theme checkmarks now and whenever the theme changes
  // (from the toolbar button, the Themes submenu, or an OS colour-scheme
  // change while in Auto mode).
  if (auto* controller = ThemeController::instance()) {
    connect(controller, &ThemeController::modeChanged, this,
            &MainWindow::syncThemeUi);
  }
  syncThemeUi();
}

void MainWindow::syncThemeUi() {
  const ThemeManager::Mode mode = ThemeController::instance()
                                      ? ThemeController::instance()->mode()
                                      : ThemeManager::Mode::Auto;

  // Keep the Tools > Themes checkmarks correct even when the mode was changed
  // via the toolbar or the OS.
  if (theme_auto_action_) {
    theme_auto_action_->setChecked(mode == ThemeManager::Mode::Auto);
  }
  if (theme_dark_action_) {
    theme_dark_action_->setChecked(mode == ThemeManager::Mode::Dark);
  }
  if (theme_light_action_) {
    theme_light_action_->setChecked(mode == ThemeManager::Mode::Light);
  }

  if (!main_toolbar_) {
    return;
  }

  // Draw the glyphs in the current text colour so they read in either theme.
  const QColor fg = palette().color(QPalette::WindowText);
  if (arrange_dag_action_) {
    arrange_dag_action_->setIcon(makeGridIcon(fg));
    arrange_dag_action_->setToolTip("Arrange DAG to grid");
  }
  if (show_preview_action_) {
    show_preview_action_->setIcon(makePreviewIcon(fg));
    show_preview_action_->setToolTip("Show preview");
  }
  if (reload_sources_action_) {
    reload_sources_action_->setIcon(makeReloadIcon(fg));
    reload_sources_action_->setToolTip("Reload all sources (Ctrl+R)");
  }
  if (theme_cycle_action_) {
    theme_cycle_action_->setIcon(makeThemeIcon(fg, mode));
    const char* name = mode == ThemeManager::Mode::Dark    ? "Dark"
                       : mode == ThemeManager::Mode::Light ? "Light"
                                                           : "Auto";
    theme_cycle_action_->setToolTip(
        QString("Theme: %1 (click to cycle)").arg(name));
  }
}

void MainWindow::connectDAGSignals() {
  connect(dag_model_, &QtNodes::AbstractGraphModel::connectionCreated, this,
          &MainWindow::onDAGModified);
  connect(dag_model_, &QtNodes::AbstractGraphModel::connectionDeleted, this,
          &MainWindow::onDAGModified);
  connect(dag_model_, &QtNodes::AbstractGraphModel::nodeCreated, this,
          &MainWindow::onDAGModified);
  connect(dag_model_, &QtNodes::AbstractGraphModel::nodeDeleted, this,
          &MainWindow::onDAGModified);
  connect(dag_scene_, &OrcGraphicsScene::nodeSelected, this,
          &MainWindow::onQtNodeSelected);
  connect(dag_scene_, &OrcGraphicsScene::editParametersRequested, this,
          &MainWindow::onEditParameters);
  connect(dag_scene_, &OrcGraphicsScene::triggerStageRequested, this,
          &MainWindow::onTriggerStage);
  connect(dag_scene_, &OrcGraphicsScene::runAnalysisRequested, this,
          &MainWindow::runAnalysisForNode);
  connect(dag_scene_, &QtNodes::BasicGraphicsScene::nodeDoubleClicked, this,
          [this](QtNodes::NodeId) {
            if (!preview_dialog_->isVisible()) {
              preview_dialog_->show();
            }
          });
}

void MainWindow::recreateDAGModelScene() {
  delete dag_scene_;
  delete dag_model_;
  dag_model_ = new OrcGraphModel(*project_.presenter(), dag_view_);
  dag_scene_ = new OrcGraphicsScene(*dag_model_, dag_view_);
  dag_view_->setScene(dag_scene_);
  connectDAGSignals();
}

void MainWindow::onNewProject() {
  // Show selection dialog for all four project types
  newProject();
}

void MainWindow::onOpenProject() {
  // Check for unsaved changes before showing file dialog
  if (!checkUnsavedChanges()) {
    return;
  }

  QString filename = QFileDialog::getOpenFileName(
      this, "Open Project", getLastProjectDirectory(),
      "ORC Project Files (*.orcprj);;All Files (*)");

  if (filename.isEmpty()) {
    ORC_LOG_DEBUG("Project open cancelled");
    return;
  }

  // Remember this directory
  setLastProjectDirectory(QFileInfo(filename).absolutePath());

  ORC_LOG_INFO("Opening project: {}", filename.toStdString());
  openProject(filename);
}

void MainWindow::onSaveProject() {
  if (project_.projectPath().isEmpty()) {
    onSaveProjectAs();
    return;
  }

  saveProject();
}

void MainWindow::onSaveProjectAs() { saveProjectAs(); }

void MainWindow::onEditProject() {
  // Open dialog with current project properties
  ProjectPropertiesDialog dialog(this);
  dialog.setProjectName(
      QString::fromStdString(project_.presenter()->getProjectName()));
  dialog.setProjectDescription(
      QString::fromStdString(project_.presenter()->getProjectDescription()));
  dialog.setAmplitudeUnit(project_.presenter()->getAmplitudeUnit());

  if (dialog.exec() == QDialog::Accepted) {
    // Update project with new values
    QString new_name = dialog.projectName();
    QString new_description = dialog.projectDescription();

    if (new_name.isEmpty()) {
      QMessageBox::warning(this, "Invalid Input",
                           "Project name cannot be empty.");
      return;
    }

    // Update project using presenter
    project_.presenter()->setProjectName(new_name.toStdString());
    project_.presenter()->setProjectDescription(new_description.toStdString());
    project_.presenter()->setAmplitudeUnit(dialog.amplitudeUnit());
    propagateAmplitudeUnit();

    ORC_LOG_INFO("Project properties updated: name='{}', description='{}'",
                 new_name.toStdString(), new_description.toStdString());

    // Update UI to reflect changes
    updateUIState();

    statusBar()->showMessage("Project properties updated", 3000);
  }
}

void MainWindow::onReloadAllSources() {
  const auto result = project_.reloadSources();

  if (result.source_count == 0) {
    statusBar()->showMessage("No sources to reload", 3000);
    return;
  }

  ORC_LOG_INFO("Reloaded {} source(s) from disk", result.source_count);

  // The rebuild replaced every stage object, so the renderer, the node view
  // and the preview are all holding output from stages that no longer exist.
  // This is the same refresh the stage parameter dialogue's Update button
  // performs, minus the parameter write.
  updatePreviewRenderer();
  dag_model_->refresh();
  updatePreview();

  if (!result.rebuilt) {
    // rebuildDAG() swallows the reason and logs it; the common cause is a
    // source file that has been moved, deleted or truncated since the project
    // was opened, so say so rather than leaving an empty preview unexplained.
    QMessageBox::warning(
        this, "Reload All Sources",
        "The sources could not be reloaded.\n\nCheck that every source file "
        "is still present and readable.");
    return;
  }

  statusBar()->showMessage(
      result.source_count == 1
          ? QString("Reloaded 1 source")
          : QString("Reloaded %1 sources").arg(result.source_count),
      3000);
}

void MainWindow::onQuickProject() {
  // Open file dialog to select TBC, TBCC, TBCY, or CVBS file
  QString filename = QFileDialog::getOpenFileName(
      this, "Quick Project - Select Video File", getLastSourceDirectory(),
      "Video Files (*.tbc *.tbcc *.tbcy *.cvbs *.cvbsy *.cvbsc);;"
      "TBC Files (*.tbc);;TBCC Files (*.tbcc);;TBCY Files (*.tbcy);;"
      "CVBS Composite Files (*.cvbs);;"
      "CVBS YC Files (*.cvbsy *.cvbsc);;All Files (*)");

  if (filename.isEmpty()) {
    ORC_LOG_DEBUG("Quick project creation cancelled");
    return;
  }

  // Delegate to quickProject() to do the actual work
  quickProject(filename);
}

void MainWindow::closeAllDialogs() {
  // Close main dialogs (these are persistent, not deleted on close)
  if (preview_dialog_) {
    preview_dialog_
        ->closeChildDialogs();  // Close child dialogs like line scope
    if (preview_dialog_->isVisible()) {
      preview_dialog_->hide();
    }
  }
  if (vbi_dialog_ && vbi_dialog_->isVisible()) {
    vbi_dialog_->hide();
  }
  if (video_parameter_observer_dialog_ &&
      video_parameter_observer_dialog_->isVisible()) {
    video_parameter_observer_dialog_->hide();
  }
  if (ntsc_observer_dialog_ && ntsc_observer_dialog_->isVisible()) {
    ntsc_observer_dialog_->hide();
  }
  if (closed_caption_dialog_ && closed_caption_dialog_->isVisible()) {
    closed_caption_dialog_->hide();
  }

  // Close and delete all per-node analysis dialogs
  // These are set to WA_DeleteOnClose, so closing them will trigger deletion
  // IMPORTANT: Don't call close() while iterating - the dialogs have destroyed
  // signals that erase from the map, which would invalidate the iterator

  // Collect pointers first, then close (avoid iterator invalidation)
  std::vector<QWidget*> dialogs_to_close;

  for (auto& pair : dropout_analysis_dialogs_) {
    if (pair.second) {
      dialogs_to_close.push_back(pair.second);
    }
  }
  for (auto& pair : snr_analysis_dialogs_) {
    if (pair.second) {
      dialogs_to_close.push_back(pair.second);
    }
  }
  for (auto& pair : burst_level_analysis_dialogs_) {
    if (pair.second) {
      dialogs_to_close.push_back(pair.second);
    }
  }
  for (auto& pair : catalogue_dialogs_) {
    if (pair.second) {
      dialogs_to_close.push_back(pair.second);
    }
  }
  // Clear maps before closing to prevent destroyed signal handlers from
  // modifying them
  dropout_analysis_dialogs_.clear();
  snr_analysis_dialogs_.clear();
  burst_level_analysis_dialogs_.clear();
  catalogue_dialogs_.clear();

  // Now safe to close all dialogs
  for (auto* dialog : dialogs_to_close) {
    dialog->close();
  }

  // Close the trigger progress dialog. Results reads have none of their own:
  // they only ever read what a trigger already produced, so there is no work
  // to report progress on.
  if (trigger_progress_dialog_) {
    trigger_progress_dialog_->close();
    delete trigger_progress_dialog_;
    trigger_progress_dialog_ = nullptr;
  }
}

void MainWindow::closeResultViewers() {
  // Collect first, then close: closing deletes the dialog, whose destroyed
  // handler erases its own map entry, which would invalidate a live iterator.
  std::vector<QWidget*> viewers;
  const auto collect = [&viewers](auto& dialogs) {
    for (auto& [node_id, dialog] : dialogs) {
      if (dialog) {
        viewers.push_back(dialog);
      }
    }
    dialogs.clear();
  };
  collect(dropout_analysis_dialogs_);
  collect(snr_analysis_dialogs_);
  collect(burst_level_analysis_dialogs_);
  collect(catalogue_dialogs_);

  // Drop the reads still in flight with them: their answers describe stage
  // objects that no longer exist, and without this a late response would
  // report "nothing to show" about a window the reader never asked to close.
  pending_dropout_requests_.clear();
  pending_snr_requests_.clear();
  pending_burst_level_requests_.clear();
  pending_catalogue_requests_.clear();

  for (auto* viewer : viewers) {
    viewer->close();
  }
}

void MainWindow::createAndShowAnalysisDialog(const orc::NodeID& node_id,
                                             const std::string& stage_name) {
  orc::presenters::AnalysisPresenter analysis_presenter(
      project_.presenter()->getCoreProjectHandle());
  const auto tools = analysis_presenter.getToolsForStage(stage_name);

  // Descriptor-driven routing: pick the first advertised viewer over the
  // results this trigger has just produced — a batch analysis or a catalogue
  // browser. Stages offering neither simply have nothing to show.
  const auto tool_it =
      std::find_if(tools.begin(), tools.end(), orc::isTriggerResultViewer);

  if (tool_it == tools.end()) {
    ORC_LOG_DEBUG(
        "No descriptor-advertised result viewer for stage '{}' (node '{}')",
        stage_name, node_id.to_string());
    return;
  }

  runAnalysisForNode(*tool_it, node_id, stage_name);
}

void MainWindow::newProject(orc::VideoSystem video_format,
                            orc::SourceType source_format) {
  // Check for unsaved changes before creating new project
  if (!checkUnsavedChanges()) {
    return;
  }

  // Show dialog to choose project name and type if not specified
  QString project_name = "Untitled";
  std::optional<orc::AmplitudeDisplayUnit> unit_override;
  if (video_format == orc::VideoSystem::Unknown ||
      source_format == orc::SourceType::Unknown) {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Project"));

    QLineEdit* name_edit = new QLineEdit("Untitled", &dialog);
    name_edit->setMinimumWidth(200);
    name_edit->selectAll();

    QComboBox* type_combo = new QComboBox(&dialog);
    type_combo->addItems({"NTSC Composite", "NTSC YC", "PAL Composite",
                          "PAL YC", "PAL-M Composite", "PAL-M YC"});

    QComboBox* unit_combo = new QComboBox(&dialog);
    unit_combo->addItem("IRE",
                        static_cast<int>(orc::AmplitudeDisplayUnit::IRE));
    unit_combo->addItem(
        "mV (millivolts)",
        static_cast<int>(orc::AmplitudeDisplayUnit::Millivolts));
    unit_combo->addItem(
        "10-bit samples",
        static_cast<int>(orc::AmplitudeDisplayUnit::Samples10Bit));
    unit_combo->setCurrentIndex(2);  // default: 10-bit samples

    QFormLayout* form = new QFormLayout;
    form->addRow(tr("Project name:"), name_edit);
    form->addRow(tr("Project type:"), type_combo);
    form->addRow(tr("Amplitude units:"), unit_combo);

    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    QVBoxLayout* main_layout = new QVBoxLayout(&dialog);
    main_layout->addLayout(form);
    main_layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
      return;  // User cancelled
    }

    const QString entered_name = name_edit->text().trimmed();
    project_name = entered_name.isEmpty() ? "Untitled" : entered_name;

    // Parse type selection
    const QString item = type_combo->currentText();
    if (item == "NTSC Composite") {
      video_format = orc::VideoSystem::NTSC;
      source_format = orc::SourceType::Composite;
    } else if (item == "NTSC YC") {
      video_format = orc::VideoSystem::NTSC;
      source_format = orc::SourceType::YC;
    } else if (item == "PAL Composite") {
      video_format = orc::VideoSystem::PAL;
      source_format = orc::SourceType::Composite;
    } else if (item == "PAL YC") {
      video_format = orc::VideoSystem::PAL;
      source_format = orc::SourceType::YC;
    } else if (item == "PAL-M Composite") {
      video_format = orc::VideoSystem::PAL_M;
      source_format = orc::SourceType::Composite;
    } else if (item == "PAL-M YC") {
      video_format = orc::VideoSystem::PAL_M;
      source_format = orc::SourceType::YC;
    }

    unit_override = static_cast<orc::AmplitudeDisplayUnit>(
        unit_combo->currentData().toInt());
  }

  ORC_LOG_INFO("Creating new project");

  // Close all dialogs before clearing project
  closeAllDialogs();

  // Clear existing project state
  project_.clear();
  preview_dialog_->stopPlayback();
  preview_dialog_->previewWidget()->clearImage();
  preview_dialog_->previewSlider()->setEnabled(false);
  preview_dialog_->frameJumpSpinBox()->setEnabled(false);
  preview_dialog_->previewSlider()->setValue(0);

  QString error;
  if (!project_.newEmptyProject(project_name,
                                toPresenterVideoFormat(video_format),
                                toPresenterSourceType(source_format), &error)) {
    ORC_LOG_ERROR("Failed to create project: {}", error.toStdString());
    QMessageBox::critical(this, "Error", error);
    return;
  }

  // Update render coordinator with new project pointer (presenter was
  // recreated)
  render_coordinator_->setProject(project_.presenter()->getCoreProjectHandle());

  // Recreate DAG model/scene since the presenter has changed
  if (dag_model_) {
    recreateDAGModelScene();
  }

  // Apply user-selected amplitude unit (dialog path); newEmptyProject already
  // set a convention default, this overrides it if the user chose explicitly.
  if (unit_override.has_value()) {
    project_.presenter()->setAmplitudeUnit(*unit_override);
  }
  propagateAmplitudeUnit();

  // Don't set a project path - leave it empty so user must use "Save As"
  // Project is marked as modified by create_empty_project()

  ORC_LOG_INFO("Project created successfully: {}", project_name.toStdString());
  updateUIState();

  // Initialize preview renderer for new project
  updatePreviewRenderer();
  reportPluginRuntimeDiagnostics(false);

  // Load DAG into embedded viewer
  loadProjectDAG();

  statusBar()->showMessage(
      QString("Created new project: %1").arg(project_name));
}

void MainWindow::openProject(const QString& filename) {
  ORC_LOG_INFO("Loading project: {}", filename.toStdString());

  // Close all dialogs before clearing project
  closeAllDialogs();

  // Clear existing project state
  project_.clear();
  preview_dialog_->stopPlayback();
  preview_dialog_->previewWidget()->clearImage();
  preview_dialog_->previewSlider()->setEnabled(false);
  preview_dialog_->frameJumpSpinBox()->setEnabled(false);
  preview_dialog_->previewSlider()->setValue(0);

  QString error;
  if (!project_.loadFromFile(filename, &error)) {
    ORC_LOG_ERROR("Failed to load project: {}", error.toStdString());
    QMessageBox::critical(this, "Error", error);
    return;
  }

  // Update render coordinator with new project pointer (presenter was
  // recreated)
  render_coordinator_->setProject(project_.presenter()->getCoreProjectHandle());

  // Recreate DAG model/scene since the presenter has changed
  if (dag_model_) {
    recreateDAGModelScene();
  }

  ORC_LOG_DEBUG("Project loaded: {}", project_.projectName().toStdString());

  // Project loaded - user can select a stage in the DAG editor for viewing
  if (project_.hasSource()) {
    ORC_LOG_DEBUG("Source loaded - select a stage in DAG editor for viewing");

    // Show helpful message
    statusBar()->showMessage(
        "Project loaded - select a stage in DAG editor to view", 5000);
  } else {
    ORC_LOG_DEBUG("Project has no source");
  }

  updateUIState();

  // Everything from here hands work to the coordinator's worker thread, and the
  // preview window only appears once it answers. Cover that wait.
  const uint64_t outputs_request_before_load = pending_outputs_request_id_;
  beginProjectLoadProgress();

  // Initialize preview renderer with project DAG
  updatePreviewRenderer();
  reportPluginRuntimeDiagnostics(false);

  // Load DAG into embedded viewer
  loadProjectDAG();

  // Push the loaded project's amplitude unit to all open dialogs
  propagateAmplitudeUnit();

  // Automatically select the source stage with the lowest node ID
  selectLowestSourceStage();

  // No available-outputs request was issued (nothing selectable in the DAG), so
  // nothing will ever arrive to retire the modal.
  if (pending_outputs_request_id_ == outputs_request_before_load) {
    endProjectLoadProgress();
  }

  statusBar()->showMessage(
      QString("Opened project: %1").arg(project_.projectName()));
}

void MainWindow::quickProject(const QString& filename) {
  // Check for unsaved changes before creating new project
  if (!checkUnsavedChanges()) {
    return;
  }

  // Validate file exists
  if (!QFileInfo::exists(filename)) {
    QMessageBox::critical(this, "Error",
                          QString("File not found: %1").arg(filename));
    return;
  }

  QFileInfo file_info(filename);
  QString base_path =
      file_info.absolutePath() + "/" + file_info.completeBaseName();
  QString ext = file_info.suffix().toLower();

  // Determine source type from file extension
  orc::SourceType source_type = orc::SourceType::Unknown;
  std::string primary_file = filename.toStdString();
  std::string secondary_file;
  bool is_cvbs = false;

  if (ext == "tbc") {
    source_type = orc::SourceType::Composite;
    primary_file = filename.toStdString();

    const QString legacy_warning =
        "You have selected a .tbc file which will be treated as a composite "
        "source.  If you meant to load a (dual-TBC) YC source, please rename "
        "your TBC files to use the .tbcc (chroma) extension and .tbcy (luma) "
        "extension before using the quick project function";

    const QString selected_base_name = file_info.completeBaseName();
    const bool selected_chroma_file =
        selected_base_name.endsWith("_chroma", Qt::CaseInsensitive);
    const QString paired_legacy_chroma_path =
        file_info.absolutePath() + "/" + selected_base_name + "_chroma.tbc";
    const bool paired_legacy_chroma_exists =
        QFileInfo::exists(paired_legacy_chroma_path);

    if (selected_chroma_file || paired_legacy_chroma_exists) {
      QMessageBox::warning(this, "Legacy YC TBC Naming Detected",
                           legacy_warning);
    }
  } else if (ext == "tbcc") {
    source_type = orc::SourceType::YC;
    primary_file = filename.toStdString();
    // Look for corresponding .tbcy file
    QString tbcy_path = base_path + ".tbcy";
    if (!QFileInfo::exists(tbcy_path)) {
      QMessageBox::warning(
          this, "Missing File",
          QString("Could not find corresponding Y (luma) file: %1")
              .arg(tbcy_path));
      return;
    }
    secondary_file = tbcy_path.toStdString();
  } else if (ext == "tbcy") {
    source_type = orc::SourceType::YC;
    primary_file = filename.toStdString();
    // Look for corresponding .tbcc file
    QString tbcc_path = base_path + ".tbcc";
    if (!QFileInfo::exists(tbcc_path)) {
      QMessageBox::warning(
          this, "Missing File",
          QString("Could not find corresponding C (chroma) file: %1")
              .arg(tbcc_path));
      return;
    }
    secondary_file = tbcc_path.toStdString();
  } else if (ext == "cvbs") {
    is_cvbs = true;
    source_type = orc::SourceType::Composite;
  } else if (ext == "cvbsy" || ext == "cvbsc") {
    // Dual-file YC CVBS pair (CVBS file format spec, File Naming Convention).
    is_cvbs = true;
    source_type = orc::SourceType::YC;
    const bool selected_luma = (ext == "cvbsy");
    const QString companion_path =
        base_path + (selected_luma ? ".cvbsc" : ".cvbsy");
    if (!QFileInfo::exists(companion_path)) {
      QMessageBox::warning(
          this, "Missing File",
          QString("Could not find corresponding %1 file: %2")
              .arg(selected_luma ? "C (chroma)" : "Y (luma)", companion_path));
      return;
    }
    secondary_file = companion_path.toStdString();
  } else {
    QMessageBox::warning(
        this, "Invalid File",
        QString("Please provide a .tbc, .tbcc, .tbcy, .cvbs, .cvbsy, or .cvbsc "
                "file. Got: %1")
            .arg(ext));
    return;
  }

  // Determine metadata file and read video format
  QString db_path;  // TBC only: set when a .tbc.db SQLite sidecar is present
  orc::VideoSystem video_format = orc::VideoSystem::Unknown;
  std::string source_stage_name;
  // When the source was produced by ld-decode the quick project inserts a
  // dropout correction stage (and, if an EFM sidecar is present, an EFM audio
  // sink) rather than wiring the source straight to the video sink.
  bool is_ld_decode = false;

  if (!is_cvbs) {
    db_path = base_path + ".tbc.db";
    if (!QFileInfo::exists(db_path)) {
      // Check for legacy JSON metadata produced by older ld-decode/vhs-decode
      QString json_path = base_path + ".tbc.json";
      if (QFileInfo::exists(json_path)) {
        ORC_LOG_INFO(
            "TBC source '{}' has legacy JSON metadata; consider re-decoding "
            "with a current version of ld-decode/vhs-decode",
            QFileInfo(json_path).fileName().toStdString());

        // Read video system from the legacy JSON file.
        QFile jf(json_path);
        if (jf.open(QIODevice::ReadOnly)) {
          const QJsonDocument doc = QJsonDocument::fromJson(jf.readAll());
          jf.close();
          if (!doc.isNull() && doc.isObject()) {
            const QString sys = doc["videoParameters"]["system"].toString();
            video_format = orc::video_system_from_string(sys.toStdString());
          }
        }
        if (video_format == orc::VideoSystem::Unknown) {
          QMessageBox::critical(
              this, "Error",
              QString("Failed to read video system from legacy metadata: %1")
                  .arg(json_path));
          return;
        }
        ORC_LOG_INFO("Read video system from legacy JSON: {}",
                     orc::video_system_to_string(video_format));
        // Legacy JSON metadata predates reliable decoder identification and is
        // not necessarily a laserdisc source, so never treat it as ld-decode;
        // the source is wired straight to the video sink.
        is_ld_decode = false;
        ORC_LOG_INFO(
            "Source metadata is legacy JSON; decoder cannot be determined: "
            "ld-decode = false");
        db_path.clear();  // no .tbc.db available; stage will use legacy JSON
      } else {
        QMessageBox::warning(
            this, "Missing Metadata File",
            QString("Metadata file not found:\n%1\n\nRe-run the decoder to "
                    "generate a .tbc.db metadata file.")
                .arg(db_path));
        return;
      }
    } else {
      ORC_LOG_INFO("Reading TBC metadata from: {}", db_path.toStdString());
      auto video_params_opt =
          orc::presenters::ProjectPresenter::readVideoParameters(
              db_path.toStdString());
      if (!video_params_opt) {
        QMessageBox::critical(
            this, "Error",
            QString("Failed to read video parameters from metadata file: %1")
                .arg(db_path));
        return;
      }
      video_format = video_params_opt->system;
      is_ld_decode = (video_params_opt->decoder == "ld-decode");
      const std::string& decoder = video_params_opt->decoder;
      ORC_LOG_INFO("Source metadata reports decoder '{}': ld-decode = {}",
                   decoder.empty() ? "unknown" : decoder, is_ld_decode);
    }

    ORC_LOG_INFO(
        "Detected format: {}, Source type: {}",
        (video_format == orc::VideoSystem::NTSC ? "NTSC" : "PAL"),
        (source_type == orc::SourceType::Composite ? "Composite" : "YC"));

    source_stage_name = "tbc_source";
  } else {
    // CVBS: metadata is in <basename>.meta
    QString meta_path = base_path + ".meta";
    if (!QFileInfo::exists(meta_path)) {
      QMessageBox::warning(
          this, "Missing Metadata File",
          QString("CVBS metadata file not found:\n%1\n\nRe-run the decoder to "
                  "generate a .meta metadata file.")
              .arg(meta_path));
      return;
    }

    ORC_LOG_INFO("Reading CVBS metadata from: {}", meta_path.toStdString());
    auto cvbs_params_opt =
        orc::presenters::ProjectPresenter::readCVBSVideoParameters(
            meta_path.toStdString());
    if (!cvbs_params_opt) {
      QMessageBox::critical(
          this, "Error",
          QString("Failed to read video parameters from CVBS metadata file: %1")
              .arg(meta_path));
      return;
    }
    video_format = cvbs_params_opt->system;
    is_ld_decode = (cvbs_params_opt->decoder == "ld-decode");

    // A source whose content is not continuous loads and decodes normally;
    // say what the marker means and point at the Disc Mapper, then carry on.
    const auto signal_state =
        orc::presenters::ProjectPresenter::readCVBSSignalState(
            meta_path.toStdString());
    if (signal_state) {
      const std::string notice = orc::gui::cvbsSequenceContinuityNotice(
          signal_state->sequence_continuous);
      if (!notice.empty()) {
        ORC_LOG_INFO(
            "CVBS source signal state is '{}' with sequence_continuous = "
            "FALSE",
            signal_state->signal_state_preset);
        QMessageBox::information(this, "Source Sequence Is Not Continuous",
                                 QString::fromStdString(notice));
      }
    }
    {
      const std::string& decoder = cvbs_params_opt->decoder;
      ORC_LOG_INFO("CVBS source metadata reports decoder '{}': ld-decode = {}",
                   decoder.empty() ? "unknown" : decoder, is_ld_decode);
    }

    const std::string fmt_name =
        (video_format == orc::VideoSystem::NTSC    ? "NTSC"
         : video_format == orc::VideoSystem::PAL_M ? "PAL-M"
                                                   : "PAL");
    ORC_LOG_INFO(
        "Detected format: {}, Source type: {}", fmt_name,
        (source_type == orc::SourceType::Composite ? "Composite" : "YC"));

    if (video_format == orc::VideoSystem::NTSC) {
      source_stage_name = "NTSC_CVBS_Source";
    } else if (video_format == orc::VideoSystem::PAL_M) {
      source_stage_name = "PAL_M_CVBS_Source";
    } else {
      source_stage_name = "PAL_CVBS_Source";
    }
  }

  // Close all dialogs before clearing project
  closeAllDialogs();

  // Clear existing project state
  project_.clear();
  preview_dialog_->stopPlayback();
  preview_dialog_->previewWidget()->clearImage();
  preview_dialog_->previewSlider()->setEnabled(false);
  preview_dialog_->frameJumpSpinBox()->setEnabled(false);
  preview_dialog_->previewSlider()->setValue(0);

  // Create empty project
  QString project_name = file_info.completeBaseName();
  QString error;

  if (!project_.newEmptyProject(project_name,
                                toPresenterVideoFormat(video_format),
                                toPresenterSourceType(source_type), &error)) {
    ORC_LOG_ERROR("Failed to create project: {}", error.toStdString());
    QMessageBox::critical(this, "Error", error);
    return;
  }

  // Update render coordinator with new project pointer (presenter was
  // recreated)
  render_coordinator_->setProject(project_.presenter()->getCoreProjectHandle());

  // Add source stage(s), sink, parameters, and connections
  const double grid_offset_x = 50.0;
  const double grid_offset_y = 50.0;
  const double grid_spacing_x = 225.0;

  // Build the downstream chain fed by an already-created and configured source
  // node.  For ld-decode sources a dropout correction stage is inserted before
  // the video sink.  When the source carries an EFM sidecar, an EFM audio
  // decode transform is spliced into the video chain so the video sink embeds
  // the disc's digital audio.  For any other decoder the source is wired
  // straight to the video sink.  Returns true on success; on failure it shows
  // an error dialog and returns false.
  auto build_downstream = [&](orc::NodeID source_node_id, double base_x,
                              double base_y, bool has_efm_sidecar) -> bool {
    const orc::gui::QuickProjectDownstreamPlan plan =
        orc::gui::plan_quick_project_downstream(is_ld_decode, has_efm_sidecar);

    double x = base_x;
    orc::NodeID video_upstream = source_node_id;  // node feeding the video sink

    for (const std::string& stage_name : plan.video_transforms) {
      x += grid_spacing_x;
      ORC_LOG_INFO("Adding {} stage", stage_name);
      orc::NodeID node_id;
      try {
        node_id = project_.presenter()->addNode(stage_name, x, base_y);
      } catch (const std::exception& e) {
        QMessageBox::critical(
            this, "Error",
            QString("Failed to add %1 stage: %2")
                .arg(QString::fromStdString(stage_name), e.what()));
        return false;
      }
      try {
        project_.presenter()->addEdge(video_upstream, node_id);
      } catch (const std::exception& e) {
        QMessageBox::critical(
            this, "Error",
            QString("Failed to connect to %1 stage: %2")
                .arg(QString::fromStdString(stage_name), e.what()));
        return false;
      }
      video_upstream = node_id;
    }

    x += grid_spacing_x;
    ORC_LOG_INFO("Adding video sink stage");
    orc::NodeID sink_node_id;
    try {
      sink_node_id = project_.presenter()->addNode("video_sink", x, base_y);
    } catch (const std::exception& e) {
      QMessageBox::critical(
          this, "Error", QString("Failed to add sink stage: %1").arg(e.what()));
      return false;
    }
    try {
      project_.presenter()->addEdge(video_upstream, sink_node_id);
    } catch (const std::exception& e) {
      QMessageBox::critical(
          this, "Error", QString("Failed to connect stages: %1").arg(e.what()));
      return false;
    }

    return true;
  };

  if (!is_cvbs) {
    // TBC path: source stage → (dropout correction for ld-decode) → sink
    ORC_LOG_INFO("Adding TBC source stage: {}", source_stage_name);
    orc::NodeID source_node_id;
    try {
      source_node_id = project_.presenter()->addNode(
          source_stage_name, grid_offset_x, grid_offset_y);
    } catch (const std::exception& e) {
      QMessageBox::critical(
          this, "Error",
          QString("Failed to add source stage: %1").arg(e.what()));
      return;
    }

    // TBC EFM sidecar is a raw .efm T-value file alongside the source.
    const QString efm_path = base_path + ".efm";
    const bool has_efm_sidecar = QFileInfo::exists(efm_path);

    std::map<std::string, orc::ParameterValue> source_params;
    if (source_type == orc::SourceType::Composite) {
      source_params["input_path"] = primary_file;
      if (!db_path.isEmpty()) {
        source_params["db_path"] = db_path.toStdString();
      }

      QString pcm_path = base_path + ".pcm";
      if (QFileInfo::exists(pcm_path)) {
        source_params["pcm_path"] = pcm_path.toStdString();
      }
      if (has_efm_sidecar) {
        source_params["efm_path"] = efm_path.toStdString();
      }
    } else {
      if (ext == "tbcy") {
        source_params["y_path"] = primary_file;
        source_params["c_path"] = secondary_file;
      } else {
        source_params["y_path"] = secondary_file;
        source_params["c_path"] = primary_file;
      }
      if (!db_path.isEmpty()) {
        source_params["db_path"] = db_path.toStdString();
      }

      QString pcm_path = base_path + ".pcm";
      if (QFileInfo::exists(pcm_path)) {
        source_params["pcm_path"] = pcm_path.toStdString();
      }
      if (has_efm_sidecar) {
        source_params["efm_path"] = efm_path.toStdString();
      }
    }

    try {
      project_.presenter()->setNodeParameters(source_node_id, source_params);
    } catch (const std::exception& e) {
      QMessageBox::critical(
          this, "Error",
          QString("Failed to set parameters on source stage: %1")
              .arg(e.what()));
      return;
    }
    ORC_LOG_INFO("Source stage parameters set successfully");

    if (!build_downstream(source_node_id, grid_offset_x, grid_offset_y,
                          has_efm_sidecar)) {
      return;
    }
  } else {
    // CVBS path: CVBS source stage(s) → sink
    // For composite: one source stage.
    // For YC: two source stages (one for Y, one for C), both wired to sink.
    if (source_type == orc::SourceType::Composite) {
      ORC_LOG_INFO("Adding CVBS composite source stage: {}", source_stage_name);
      orc::NodeID source_node_id;
      try {
        source_node_id = project_.presenter()->addNode(
            source_stage_name, grid_offset_x, grid_offset_y);
      } catch (const std::exception& e) {
        QMessageBox::critical(
            this, "Error",
            QString("Failed to add source stage: %1").arg(e.what()));
        return;
      }

      std::map<std::string, orc::ParameterValue> source_params;
      source_params["input_path"] = primary_file;

      try {
        project_.presenter()->setNodeParameters(source_node_id, source_params);
      } catch (const std::exception& e) {
        QMessageBox::critical(
            this, "Error",
            QString("Failed to set parameters on CVBS source stage: %1")
                .arg(e.what()));
        return;
      }
      ORC_LOG_INFO("CVBS source stage parameters set successfully");

      // The CVBS source auto-loads its EFM sidecar from the .efm.meta index
      // (plus .efm data) alongside the source file.
      const bool has_efm_sidecar = QFileInfo::exists(base_path + ".efm.meta") &&
                                   QFileInfo::exists(base_path + ".efm");
      if (!build_downstream(source_node_id, grid_offset_x, grid_offset_y,
                            has_efm_sidecar)) {
        return;
      }
    } else {
      // CVBS YC: single source stage with both y_path and c_path, wired to
      // sink. A .cvbsy selection makes the chosen file the luma one; a .cvbsc
      // selection makes it the chroma one and the companion the luma.
      const bool selected_luma = (ext == "cvbsy");
      const std::string y_file = selected_luma ? primary_file : secondary_file;
      const std::string c_file = selected_luma ? secondary_file : primary_file;

      ORC_LOG_INFO("Adding CVBS YC source stage: {}", source_stage_name);
      orc::NodeID source_node_id;
      try {
        source_node_id = project_.presenter()->addNode(
            source_stage_name, grid_offset_x, grid_offset_y);
      } catch (const std::exception& e) {
        QMessageBox::critical(
            this, "Error",
            QString("Failed to add YC source stage: %1").arg(e.what()));
        return;
      }

      std::map<std::string, orc::ParameterValue> yc_params;
      yc_params["y_path"] = y_file;
      yc_params["c_path"] = c_file;
      try {
        project_.presenter()->setNodeParameters(source_node_id, yc_params);
      } catch (const std::exception& e) {
        QMessageBox::critical(
            this, "Error",
            QString("Failed to set parameters on YC source stage: %1")
                .arg(e.what()));
        return;
      }
      ORC_LOG_INFO("CVBS YC source stage parameters set successfully");

      // The CVBS source derives its EFM sidecar from the Y file's base name;
      // that resolves to <base>.efm.meta / <base>.efm alongside the source.
      const bool has_efm_sidecar = QFileInfo::exists(base_path + ".efm.meta") &&
                                   QFileInfo::exists(base_path + ".efm");
      if (!build_downstream(source_node_id, grid_offset_x, grid_offset_y,
                            has_efm_sidecar)) {
        return;
      }
    }
  }

  // Rebuild DAG from the newly created project structure
  project_.rebuildDAG();

  // Recreate DAG model/scene since the presenter has changed
  if (dag_model_) {
    recreateDAGModelScene();
  }

  // Don't set a project path - leave it empty so user must use "Save As"
  // Project is marked as modified by create_empty_project() and subsequent
  // operations

  ORC_LOG_INFO("Quick project created successfully from: {}",
               filename.toStdString());

  // Remember this source directory
  setLastSourceDirectory(QFileInfo(filename).absolutePath());

  // Update UI - preview renderer and DAG display. As with openProject(), the
  // wait that follows belongs to the coordinator's worker thread, so cover it
  // with the project-load modal.
  updateUIState();
  const uint64_t outputs_request_before_load = pending_outputs_request_id_;
  beginProjectLoadProgress();
  updatePreviewRenderer();
  reportPluginRuntimeDiagnostics(false);
  loadProjectDAG();

  // Push the project's amplitude unit to any open dialogs
  propagateAmplitudeUnit();

  // Automatically select the source stage with the lowest node ID
  selectLowestSourceStage();

  if (pending_outputs_request_id_ == outputs_request_before_load) {
    endProjectLoadProgress();
  }

  statusBar()->showMessage("Quick project created successfully", 5000);
}

bool MainWindow::saveProject() {
  if (project_.projectPath().isEmpty()) {
    return saveProjectAs();
  }

  ORC_LOG_INFO("Saving project: {}", project_.projectPath().toStdString());

  QString error;
  if (!project_.saveToFile(project_.projectPath(), &error)) {
    ORC_LOG_ERROR("Failed to save project: {}", error.toStdString());
    QMessageBox::critical(this, "Error", error);
    return false;
  }

  ORC_LOG_DEBUG("Project saved successfully");
  updateUIState();  // Update UI state after save (disable save button)
  statusBar()->showMessage("Project saved");
  return true;
}

bool MainWindow::saveProjectAs() {
  // Determine the full default path (directory + filename)
  QString defaultPath;

  ORC_LOG_DEBUG("saveProjectAs: project path = '{}'",
                project_.projectPath().toStdString());
  ORC_LOG_DEBUG("saveProjectAs: project name = '{}'",
                project_.projectName().toStdString());

  if (!project_.projectPath().isEmpty()) {
    // Use existing project path if available
    defaultPath = project_.projectPath();
    ORC_LOG_DEBUG("saveProjectAs: using existing path: '{}'",
                  defaultPath.toStdString());
  } else {
    // Construct filename from project name
    QString projectName = project_.projectName();
    if (projectName.isEmpty()) {
      projectName = "Untitled";
    }
    // Remove any characters that might be problematic in filenames
    projectName =
        projectName.replace(QRegularExpression("[<>:\"/\\\\|?*]"), "_");
    // Use QDir to properly construct the full path
    QDir dir(getLastProjectDirectory());
    defaultPath = dir.filePath(projectName + ".orcprj");
    ORC_LOG_DEBUG("saveProjectAs: constructed path: '{}'",
                  defaultPath.toStdString());
  }

  ORC_LOG_DEBUG("saveProjectAs: final default path = '{}'",
                defaultPath.toStdString());

  QString filename = QFileDialog::getSaveFileName(
      this, "Save Project As",
      defaultPath,  // Full path including filename
      "ORC Project Files (*.orcprj);;All Files (*)");

  if (filename.isEmpty()) {
    return false;
  }

  // Ensure .orcprj extension
  if (!filename.endsWith(".orcprj", Qt::CaseInsensitive)) {
    filename += ".orcprj";
  }

  // Remember this directory
  setLastProjectDirectory(QFileInfo(filename).absolutePath());

  QString error;
  if (!project_.saveToFile(filename, &error)) {
    QMessageBox::critical(this, "Error", error);
    return false;
  }

  updateUIState();
  statusBar()->showMessage("Project saved as " + filename);
  return true;
}

void MainWindow::updateUIState() {
  bool has_project = !project_.projectName().isEmpty();
  bool has_preview = current_view_node_id_.is_valid();
  bool has_saved_path = !project_.projectPath().isEmpty();

  // Enable/disable actions based on project state
  if (save_project_action_) {
    // Save is only enabled if project is modified AND has a saved path
    // (i.e., the first Save As must be done before using Save)
    save_project_action_->setEnabled(has_project && project_.isModified() &&
                                     has_saved_path);
  }
  if (save_project_as_action_) {
    save_project_as_action_->setEnabled(has_project);
  }
  if (edit_project_action_) {
    edit_project_action_->setEnabled(has_project);
  }
  if (reload_sources_action_) {
    // Nothing to re-read until the project holds at least one source stage.
    reload_sources_action_->setEnabled(has_project && project_.hasSource());
  }
  if (plugin_manager_action_) {
    plugin_manager_action_->setEnabled(!has_project);
  }

  // Enable/disable DAG view based on project state
  if (dag_view_) {
    dag_view_->setEnabled(has_project);
    dag_view_->setShowWelcomeMessage(!has_project);
  }

  // Enable aspect ratio selector when preview is available (in preview dialog)
  if (preview_dialog_) {
    preview_dialog_->aspectRatioCombo()->setEnabled(has_preview);
  }

  // Update window title to reflect modified state
  updateWindowTitle();
}

void MainWindow::onPreviewIndexChanged(int /*index*/) {
  // Navigation is now handled entirely by PreviewDialog.
}

void MainWindow::onNavigatePreview(int delta) {
  if (!preview_dialog_->previewSlider()->isEnabled()) {
    return;
  }
  // In frame view modes, keyboard arrows skip 2 slider positions at a time
  // so one keypress = one frame; in field mode one keypress = one field.
  const int step =
      (current_output_type_ == orc::PreviewOutputType::Frame_Field1_First ||
       current_output_type_ == orc::PreviewOutputType::Frame_Reversed)
          ? 2
          : 1;
  const int cur = preview_dialog_->previewSlider()->value();
  preview_dialog_->navigateToIndex(cur + delta * step);
}

void MainWindow::onPreviewModeChanged(int index) {
  // Get output type from stored available outputs
  if (index < 0 || index >= static_cast<int>(available_outputs_.size())) {
    return;
  }

  // Remember previous type and current position
  auto previous_type = current_output_type_;
  int current_position = preview_dialog_->previewSlider()->value();

  // Update to new type and option ID
  current_output_type_ = available_outputs_[index].type;
  current_option_id_ = available_outputs_[index].option_id;

  // Update dropouts button state based on the new output's availability
  bool dropouts_available = available_outputs_[index].dropouts_available;
  if (preview_dialog_ && preview_dialog_->dropoutsButton()) {
    if (!dropouts_available) {
      // Disable and turn off dropouts for outputs where they're not available
      preview_dialog_->dropoutsButton()->setEnabled(false);
      preview_dialog_->dropoutsButton()->setChecked(false);
      render_coordinator_->setShowDropouts(false);
    } else {
      // Re-enable dropouts button for outputs that support it
      preview_dialog_->dropoutsButton()->setEnabled(true);
    }
  }

  // Convert position between field and frame indices
  uint64_t new_position = current_position;

  // Determine if previous and new types are field-based or frame-based
  bool previous_is_field =
      (previous_type == orc::PreviewOutputType::Frame_Field1 ||
       previous_type == orc::PreviewOutputType::Frame_Field2 ||
       previous_type == orc::PreviewOutputType::Luma);
  bool new_is_field =
      (current_output_type_ == orc::PreviewOutputType::Frame_Field1 ||
       current_output_type_ == orc::PreviewOutputType::Frame_Field2 ||
       current_output_type_ == orc::PreviewOutputType::Luma);

  // Get first_field_offset - this is the same for all frame-based outputs
  // (determined by field 0 parity) We can get it from any frame-based output
  uint64_t first_field_offset = 0;
  for (const auto& output : available_outputs_) {
    if (output.type == orc::PreviewOutputType::Frame_Field1_First ||
        output.type == orc::PreviewOutputType::Frame_Reversed ||
        output.type == orc::PreviewOutputType::Split) {
      first_field_offset = output.first_field_offset;
      ORC_LOG_DEBUG("Found first_field_offset: {}", first_field_offset);
      break;
    }
  }

  if (previous_is_field && !new_is_field) {
    // Converting from field to frame: select frame containing current field
    // Frame F contains fields: F*2 + offset and F*2 + offset + 1
    // So field N is in frame: (N - offset) / 2
    if (current_position >= first_field_offset) {
      new_position = (current_position - first_field_offset) / 2;
    } else {
      new_position = 0;
    }
    ORC_LOG_DEBUG("Field->Frame conversion: field {} -> frame {} (offset: {})",
                  current_position, new_position, first_field_offset);
  } else if (!previous_is_field && new_is_field) {
    // Converting from frame to field: select first field of current frame
    // Frame F maps to first field at: F*2 + offset
    new_position =
        (static_cast<uint64_t>(current_position * 2)) + first_field_offset;
    ORC_LOG_DEBUG("Frame->Field conversion: frame {} -> field {} (offset: {})",
                  current_position, new_position, first_field_offset);
  }
  // Otherwise, both are same type (field->field or frame->frame), keep position
  // as-is

  // Update viewer controls (slider range, step, labels) without triggering a
  // render yet
  refreshViewerControls(true /* skip_preview */);
  refreshPreviewViewAvailability();

  // Set the calculated position (after refreshViewerControls updates the range)
  if (new_position >= 0 &&
      new_position <=
          static_cast<uint64_t>(preview_dialog_->previewSlider()->maximum())) {
    preview_dialog_->setIndex(static_cast<int>(new_position));
  }

  updateAllPreviewComponents();
}

void MainWindow::onAspectRatioModeChanged(int index) {
  // Handle aspect ratio entirely in GUI: set preview widget correction
  static std::vector<orc::AspectRatioModeInfo> available_modes = {
      {orc::AspectRatioMode::SAR_1_1, "1:1 (Square)", 1.0},
      {orc::AspectRatioMode::DAR_4_3, "4:3 (Display)", 0.7}};
  if (index < 0 || index >= static_cast<int>(available_modes.size())) {
    return;
  }

  current_aspect_ratio_mode_ = available_modes[index].mode;

  // Determine correction factor from the selected output metadata.
  // This keeps all GUI paths consistent with core-provided DAR correction.
  auto computeAspectCorrection = [this]() -> double {
    double correction = 1.0;
    for (const auto& output : available_outputs_) {
      if (output.type == current_output_type_ &&
          output.option_id == current_option_id_) {
        correction = output.dar_aspect_correction;
        break;
      }
    }
    return correction;
  };

  double correction =
      (current_aspect_ratio_mode_ == orc::AspectRatioMode::DAR_4_3)
          ? computeAspectCorrection()
          : 1.0;

  if (preview_dialog_ && preview_dialog_->previewWidget()) {
    preview_dialog_->previewWidget()->setAspectCorrection(correction);
  }

  // No core-side scaling; re-render image to ensure redraw uses new scaling
  updatePreview();
}

void MainWindow::updateWindowTitle() {
  QString title = "No Project Loaded";

  QString project_name = project_.projectName();
  if (!project_name.isEmpty()) {
    title = project_name;

    // Append video format label when known.
    const auto fmt = project_.presenter()->getVideoFormat();
    QString fmt_label;
    switch (fmt) {
      case orc::presenters::VideoFormat::PAL:
        fmt_label = "PAL";
        break;
      case orc::presenters::VideoFormat::NTSC:
        fmt_label = "NTSC";
        break;
      case orc::presenters::VideoFormat::PAL_M:
        fmt_label = "PAL-M";
        break;
      default:
        break;
    }

    // Append source type (Composite / Y/C) when known.
    const auto src = project_.presenter()->getSourceType();
    QString src_label;
    switch (src) {
      case orc::presenters::SourceType::Composite:
        src_label = "Composite";
        break;
      case orc::presenters::SourceType::YC:
        src_label = "Y/C";
        break;
      default:
        break;
    }

    if (!fmt_label.isEmpty() || !src_label.isEmpty()) {
      QString bracket;
      if (!fmt_label.isEmpty()) bracket += fmt_label;
      if (!fmt_label.isEmpty() && !src_label.isEmpty()) bracket += " - ";
      if (!src_label.isEmpty()) bracket += src_label;
      title += " [" + bracket + "]";
    }

    // Add source name if available
    if (project_.hasSource()) {
      QString source_name = project_.getSourceName();
      if (!source_name.isEmpty()) {
        title += " - " + source_name;
      }
    }

    // Add modified indicator
    if (project_.isModified()) {
      title += " *";
    }
  }

  setWindowTitle(title);

  // Force window manager to update the title bar immediately
  // This ensures the title updates even when the DAG editor window is active
  QApplication::processEvents();
}

bool MainWindow::checkUnsavedChanges() {
  // Check if project has unsaved changes
  if (!project_.isModified()) {
    return true;  // No unsaved changes, safe to proceed
  }

  // Show confirmation dialog
  QMessageBox msgBox(this);
  msgBox.setWindowTitle("Unsaved Changes");
  msgBox.setText("The project has unsaved changes.");
  msgBox.setInformativeText("Do you want to save your changes?");
  msgBox.setStandardButtons(QMessageBox::Save | QMessageBox::Discard |
                            QMessageBox::Cancel);
  msgBox.setDefaultButton(QMessageBox::Save);
  msgBox.setIcon(QMessageBox::Question);

  int ret = msgBox.exec();

  switch (ret) {
    case QMessageBox::Save:
      // Always show SaveAs dialog to make it clear what's being saved.
      // Proceed with the pending operation (close/open/new) only if the
      // save actually happened; a cancelled or failed save keeps the
      // user in the editor.
      return saveProjectAs();

    case QMessageBox::Discard:
      // Discard changes and proceed
      return true;

    case QMessageBox::Cancel:
    default:
      // Cancel the operation
      return false;
  }
}

void MainWindow::updatePreviewInfo() {
  // Update standard-specific observer menu availability: the NTSC observers
  // (FM code, white flag) and closed captions are NTSC-only (see
  // PreviewDialog::setObserverAvailabilityForFormat). Done before the early
  // returns so the menu never keeps the previous project's availability while
  // no stage is selected.
  auto video_format_presenter = project_.presenter()->getVideoFormat();
  preview_dialog_->setObserverAvailabilityForFormat(video_format_presenter);

  if (current_view_node_id_.is_valid() == false) {
    preview_dialog_->previewInfoLabel()->setText("No stage selected");
    preview_dialog_->sliderMinLabel()->setText("");
    preview_dialog_->sliderMaxLabel()->setText("");
    return;
  }

  // Special handling for placeholder node
  if (current_view_node_id_ == NodeID(-999)) {
    preview_dialog_->previewInfoLabel()->setText("No source available");
    preview_dialog_->sliderMinLabel()->setText("");
    preview_dialog_->sliderMaxLabel()->setText("");
    return;
  }

  bool is_ntsc = (video_format_presenter == orc::presenters::VideoFormat::NTSC);

  // ITU-R BT.470-6 §5.1 (PAL 25 fps) / SMPTE 170M-2004 §2 (NTSC ~29.97 fps).
  // ITU-R BT.1700-1 Annex 1 Part B (PAL-M 29.97 fps, same rate as NTSC).
  // Keep playback timer interval in sync with the detected video standard.
  const bool is_palm =
      (video_format_presenter == orc::presenters::VideoFormat::PAL_M);
  preview_dialog_->setPlaybackFrameRateMs((is_ntsc || is_palm) ? 33 : 40);

  // Get detailed display info from core
  int current_index = preview_dialog_->previewSlider()->value();
  int total = preview_dialog_->previewSlider()->maximum() + 1;

  ORC_LOG_DEBUG("updatePreviewInfo: current_output_type={}, index={}, total={}",
                static_cast<int>(current_output_type_), current_index, total);

  // Determine if PAL or NTSC for presentation numbering
  bool is_pal = (video_format_presenter == orc::presenters::VideoFormat::PAL);

  // Get display name from available outputs
  QString type_name = "Item";  // Default fallback
  for (const auto& output : available_outputs_) {
    if (output.type == current_output_type_) {
      type_name = QString::fromStdString(output.display_name);
      break;
    }
  }

  // Build presentation-based display info using GUI helpers
  QString info_text;
  QString slider_min_text;
  QString slider_max_text;

  if (current_output_type_ == orc::PreviewOutputType::Frame_Field1 ||
      current_output_type_ == orc::PreviewOutputType::Frame_Field2) {
    // Field mode: show 1-indexed field numbers
    uint64_t field_id = static_cast<uint64_t>(current_index);
    uint64_t min_field_id = 0;
    uint64_t max_field_id = static_cast<uint64_t>(total - 1);

    info_text = QString("%1 %2 / %3")
                    .arg(type_name)
                    .arg(field_id + 1)  // 1-indexed presentation
                    .arg(total);

    slider_min_text = QString::number(min_field_id + 1);  // 1-indexed
    slider_max_text = QString::number(max_field_id + 1);  // 1-indexed
  } else if (current_output_type_ ==
                 orc::PreviewOutputType::Frame_Field1_First ||
             current_output_type_ == orc::PreviewOutputType::Frame_Reversed) {
    // Frame mode: show 1-indexed frame numbers with constituent field numbers
    uint64_t frame_index = static_cast<uint64_t>(current_index);
    uint64_t frame_number = frame_index + 1;  // 1-indexed

    // Calculate constituent field IDs (0-indexed)
    uint64_t first_field_id = frame_index * 2;
    uint64_t second_field_id = first_field_id + 1;

    // Convert to 1-indexed for display
    uint64_t first_field_number = first_field_id + 1;
    uint64_t second_field_number = second_field_id + 1;

    if (current_output_type_ == orc::PreviewOutputType::Frame_Reversed) {
      // Reversed: show second field first
      info_text = QString("%1 %2 (Fields %3-%4) / %5")
                      .arg(type_name)
                      .arg(frame_number)
                      .arg(second_field_number)
                      .arg(first_field_number)
                      .arg(total);
    } else {
      // Normal: show first field first
      info_text = QString("%1 %2 (Fields %3-%4) / %5")
                      .arg(type_name)
                      .arg(frame_number)
                      .arg(first_field_number)
                      .arg(second_field_number)
                      .arg(total);
    }

    // Slider labels show frame numbers (1-indexed)
    slider_min_text = "1";
    slider_max_text = QString::number(total);
  } else {
    // Other modes (Split, Luma, Chroma, Composite): use simple indexing
    info_text = QString("%1 %2 / %3")
                    .arg(type_name)
                    .arg(current_index + 1)  // 1-indexed
                    .arg(total);

    slider_min_text = "1";
    slider_max_text = QString::number(total);
  }

  ORC_LOG_DEBUG("updatePreviewInfo: display=\"{}\"", info_text.toStdString());

  // Update UI
  preview_dialog_->sliderMinLabel()->setText(slider_min_text);
  preview_dialog_->sliderMaxLabel()->setText(slider_max_text);
  preview_dialog_->previewInfoLabel()->setText(info_text);
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
  if (!preview_dialog_->previewSlider()->isEnabled()) {
    QMainWindow::keyPressEvent(event);
    return;
  }

  switch (event->key()) {
    case Qt::Key_Left:
      onNavigatePreview(-1);
      event->accept();
      break;
    case Qt::Key_Right:
      onNavigatePreview(1);
      event->accept();
      break;
    case Qt::Key_Home:
      preview_dialog_->navigateToIndex(0);
      event->accept();
      break;
    case Qt::Key_End:
      preview_dialog_->navigateToIndex(
          preview_dialog_->previewSlider()->maximum());
      event->accept();
      break;
    case Qt::Key_PageUp:
      onNavigatePreview(-10);
      event->accept();
      break;
    case Qt::Key_PageDown:
      onNavigatePreview(10);
      event->accept();
      break;
    default:
      QMainWindow::keyPressEvent(event);
  }
}

void MainWindow::loadProjectDAG() {
  if (!dag_model_) {
    return;
  }

  ORC_LOG_DEBUG("MainWindow: loading project DAG for visualization");

  // QtNodes model will automatically sync with Project via OrcGraphModel
  // Just need to refresh the model to update the view
  dag_model_->refresh();

  // Position view to show top-left node
  positionViewToTopLeft();

  statusBar()->showMessage("Loaded DAG from project", 2000);
}

void MainWindow::positionViewToTopLeft() {
  if (!dag_view_ || !dag_scene_) {
    return;
  }

  const auto nodes = project_.presenter()->getNodes();
  if (nodes.empty()) {
    return;
  }

  // Find the minimum x and y coordinates to determine top-left position
  double min_x = std::numeric_limits<double>::max();
  double min_y = std::numeric_limits<double>::max();

  for (const auto& node : nodes) {
    if (node.x_position < min_x) min_x = node.x_position;
    if (node.y_position < min_y) min_y = node.y_position;
  }

  // Add padding from viewport edges
  const double viewport_padding = 20.0;

  // Calculate the center point needed to show top-left node at viewport's
  // top-left We want min_x, min_y to appear at the top-left with padding, so we
  // center on a point that's offset by half the viewport size minus the padding
  QRectF viewportRect = dag_view_->viewport()->rect();
  QPointF centerPoint(min_x + viewportRect.width() / 2 - viewport_padding,
                      min_y + viewportRect.height() / 2 - viewport_padding);
  dag_view_->centerOn(centerPoint);
}

void MainWindow::onEditParameters(const orc::NodeID& node_id) {
  ORC_LOG_DEBUG("Edit parameters requested for node: {}", node_id);

  // Find the node in the project
  const auto nodes = project_.presenter()->getNodes();
  auto node_it = std::find_if(nodes.begin(), nodes.end(),
                              [&node_id](const orc::presenters::NodeInfo& n) {
                                return n.node_id == node_id;
                              });

  if (node_it == nodes.end()) {
    QMessageBox::warning(this, "Edit Parameters",
                         QString("Stage '%1' not found")
                             .arg(QString::fromStdString(node_id.to_string())));
    return;
  }

  std::string stage_name = node_it->stage_name;

  // Check if stage exists in registry
  if (!orc::presenters::ProjectPresenter::hasStage(stage_name)) {
    QMessageBox::warning(this, "Edit Parameters",
                         QString("Unknown stage type '%1'")
                             .arg(QString::fromStdString(stage_name)));
    return;
  }

  // Get parameter descriptors using presenter (handles video format/source type
  // context internally)
  auto param_descriptors = project_.presenter()->getStageParameters(stage_name);

  if (param_descriptors.empty()) {
    QMessageBox::information(
        this, "Edit Parameters",
        QString("Stage '%1' does not have configurable parameters")
            .arg(QString::fromStdString(stage_name)));
    return;
  }

  // Get current parameter values from the node
  auto current_values = project_.presenter()->getNodeParameters(node_id);

  // audio_channel_map / audio_align / AudioSink / tbc_sink: restrict the
  // channel-pair dropdown to the pairs the node's input actually carries. The
  // stage descriptor lists all container slots; here we narrow it using the
  // upstream node's audio pair count.
  std::optional<size_t> input_audio_pair_count;
  if (stage_name == "audio_channel_map" || stage_name == "audio_align" ||
      stage_name == "AudioSink" || stage_name == "tbc_sink") {
    auto* core_project = project_.presenter()->getCoreProjectHandle();
    if (core_project) {
      orc::presenters::RenderPresenter render_presenter(core_project);
      // Throwaway helper presenter: rendering/parameter reads only — no
      // sidecar, scheduler, or sweeps (construction must stay cheap on the
      // GUI thread).
      render_presenter.setBackgroundObservationEnabled(false);
      render_presenter.setDAG(project_.getDAG());

      orc::NodeID input_source_node_id = node_id;
      const auto edges = project_.presenter()->getEdges();
      auto input_edge =
          std::find_if(edges.begin(), edges.end(),
                       [&node_id](const orc::presenters::EdgeInfo& edge) {
                         return edge.target_node == node_id;
                       });
      if (input_edge != edges.end()) {
        input_source_node_id = input_edge->source_node;
      }

      const auto pair_names =
          render_presenter.getAudioChannelPairNames(input_source_node_id);
      input_audio_pair_count = pair_names.size();
      if (!pair_names.empty()) {
        // Combo entry "value␟label": stored value is the bare index (or "new"),
        // display adds the pair description when present, e.g. "0: Analogue".
        const char sep = StageParameterDialog::kComboValueLabelSeparator;
        auto pair_entry = [&](size_t p) {
          return orc::gui::audioChannelPairComboEntry(p, pair_names[p], sep);
        };

        for (auto& desc : param_descriptors) {
          if (desc.name == "channel_pair" ||
              desc.name == "audio_channel_pair") {
            desc.constraints.allowed_strings.clear();
            for (size_t p = 0; p < pair_names.size(); ++p) {
              desc.constraints.allowed_strings.push_back(pair_entry(p));
            }
          } else if (desc.name == "target_pair") {
            desc.constraints.allowed_strings.clear();
            desc.constraints.allowed_strings.push_back(
                std::string("new") + sep + "New channel pair");
            for (size_t p = 0; p < pair_names.size(); ++p) {
              desc.constraints.allowed_strings.push_back(pair_entry(p));
            }
          }
        }
      }
    }
  }

  std::optional<std::map<std::string, orc::ParameterValue>> reset_values;
  if (stage_name == "video_params") {
    auto* core_project = project_.presenter()->getCoreProjectHandle();
    if (core_project) {
      orc::presenters::RenderPresenter render_presenter(core_project);
      // Throwaway helper presenter: rendering/parameter reads only — no
      // sidecar, scheduler, or sweeps (construction must stay cheap on the
      // GUI thread).
      render_presenter.setBackgroundObservationEnabled(false);
      render_presenter.setDAG(project_.getDAG());

      // Reset values should come from the stage input path (pre-override),
      // not from the video_params node output (which already includes
      // overrides).
      orc::NodeID metadata_source_node_id = node_id;
      const auto edges = project_.presenter()->getEdges();
      auto input_edge =
          std::find_if(edges.begin(), edges.end(),
                       [&node_id](const orc::presenters::EdgeInfo& edge) {
                         return edge.target_node == node_id;
                       });

      if (input_edge != edges.end()) {
        metadata_source_node_id = input_edge->source_node;
      }

      ORC_LOG_DEBUG(
          "Video params reset source resolved: target_node='{}', "
          "source_node='{}', used_upstream_input={}",
          node_id.to_string(), metadata_source_node_id.to_string(),
          (input_edge != edges.end()));

      if (auto source_params =
              render_presenter.getVideoParameters(metadata_source_node_id)) {
        reset_values = sourceParametersToVideoParamsStageValues(*source_params);
        applyMetadataFallbackValues(current_values, *reset_values);
      }
    }
  }

  // Get display name for the dialog title
  std::string display_name = stage_name;  // Fallback to stage_name
  const orc::NodeTypeInfo* type_info = orc::get_node_type_info(stage_name);
  if (type_info && !type_info->display_name.empty()) {
    display_name = type_info->display_name;
  }

  // Retrieve stage description for the dialog header
  std::string stage_description;
  if (type_info && !type_info->description.empty()) {
    stage_description = type_info->description;
  }

  // An audio stage whose input carries no channel pairs cannot be configured
  // usefully — say so in the dialog header rather than letting the user pick a
  // pair the input does not have. The TBC sink is the exception: its export
  // succeeds either way and only the optional .pcm sidecar is affected, so it
  // gets the milder note.
  if (input_audio_pair_count) {
    stage_description = (stage_name == "tbc_sink")
                            ? orc::gui::withAudioChannelPairSidecarNotice(
                                  stage_description, *input_audio_pair_count)
                            : orc::gui::withAudioChannelPairNotice(
                                  stage_description, *input_audio_pair_count);
  }

  // Source Join orders its inputs by node ID, and nothing in the form says
  // which numbers those are — a multi-input stage has one input port, so the
  // connections do not name their sources. Put the connected nodes, with the
  // IDs the graph draws on them, at the top of the dialog.
  if (stage_name == "source_join") {
    std::vector<orc::gui::ConnectedInputNode> connected_inputs;
    for (const auto& edge : project_.presenter()->getEdges()) {
      if (edge.target_node != node_id) continue;
      auto source_it =
          std::find_if(nodes.begin(), nodes.end(),
                       [&edge](const orc::presenters::NodeInfo& n) {
                         return n.node_id == edge.source_node;
                       });
      std::string name;
      if (source_it != nodes.end()) {
        name = source_it->label;
        if (name.empty()) {
          const orc::NodeTypeInfo* source_type =
              orc::get_node_type_info(source_it->stage_name);
          name =
              source_type ? source_type->display_name : source_it->stage_name;
        }
      }
      connected_inputs.push_back(
          orc::gui::ConnectedInputNode{edge.source_node.value(), name});
    }
    stage_description = orc::gui::withSourceJoinInputNodesNotice(
        stage_description, connected_inputs);
  }

  // Show parameter dialog
  StageParameterDialog dialog(stage_name, display_name, stage_description,
                              param_descriptors, current_values,
                              project_.projectPath(), reset_values, this);

  auto apply_dialog_values = [&]() {
    auto new_values = dialog.get_values();

    try {
      if (!project_.presenter()->setNodeParameters(node_id, new_values)) {
        throw std::runtime_error("Presenter rejected parameter update");
      }

      // Rebuild DAG to pick up the new parameter values
      project_.rebuildDAG();

      // Update the preview renderer with the new DAG
      updatePreviewRenderer();

      // Refresh QtNodes view
      dag_model_->refresh();

      // Update the preview to show the changes
      updatePreview();

      statusBar()->showMessage(
          QString("Updated parameters for stage '%1'")
              .arg(QString::fromStdString(node_id.to_string())),
          3000);
    } catch (const std::exception& e) {
      // Parameter validation failed - show error and reset parameters to empty
      QMessageBox::critical(
          this, "Parameter Validation Error",
          QString("Failed to set parameters: %1\n\nParameters have been reset.")
              .arg(QString::fromStdString(e.what())));

      // Reset parameters to empty map
      std::map<std::string, orc::ParameterValue> empty_params;
      try {
        project_.presenter()->setNodeParameters(node_id, empty_params);

        // Rebuild DAG with empty parameters
        project_.rebuildDAG();
        updatePreviewRenderer();
        dag_model_->refresh();
        updatePreview();
      } catch (const std::exception& reset_error) {
        // If reset also fails, log it but don't crash
        ORC_LOG_ERROR("Failed to reset parameters after validation error: {}",
                      reset_error.what());
      }
    }
  };

  connect(&dialog, &StageParameterDialog::update_requested, this,
          apply_dialog_values);

  if (dialog.exec() == QDialog::Accepted) {
    apply_dialog_values();
  }
}

void MainWindow::onTriggerStage(const orc::NodeID& node_id) {
  ORC_LOG_DEBUG("Trigger stage requested for node: {}", node_id.to_string());

  try {
    // Store the node being triggered so we can request data after completion
    pending_trigger_node_id_ = node_id;

    // Create progress dialog
    trigger_progress_dialog_ =
        new QProgressDialog("Starting trigger...", "Cancel", 0, 100, this);
    trigger_progress_dialog_->setWindowTitle("Processing");
    trigger_progress_dialog_->setWindowModality(Qt::ApplicationModal);
    trigger_progress_dialog_->setMinimumDuration(0);
    trigger_progress_dialog_->setValue(0);
    // No activateWindow(): requesting activation on a short-lived modal leaves
    // the GNOME/Wayland startup "busy" cursor spinner stuck after it closes.
    trigger_progress_dialog_->setAttribute(Qt::WA_ShowWithoutActivating);
    trigger_progress_dialog_->show();
    trigger_progress_dialog_->raise();

    // Connect cancel button
    connect(trigger_progress_dialog_, &QProgressDialog::canceled, this,
            [this]() {
              ORC_LOG_DEBUG("User canceled trigger");
              render_coordinator_->cancelTrigger();
              // Clean up the dialog and request state
              pending_trigger_request_id_ = 0;
              pending_trigger_node_id_ = orc::NodeID();  // Reset to invalid ID
              if (trigger_progress_dialog_) {
                trigger_progress_dialog_->blockSignals(true);
                trigger_progress_dialog_->deleteLater();
                // QPointer will be nulled when dialog is deleted
              }
            });

    // Request trigger from coordinator (async, thread-safe)
    pending_trigger_request_id_ = render_coordinator_->requestTrigger(node_id);

  } catch (const std::exception& e) {
    QString msg = QString("Error starting trigger: %1").arg(e.what());
    ORC_LOG_ERROR("{}", msg.toStdString());
    QMessageBox::critical(this, "Trigger Error", msg);

    if (trigger_progress_dialog_) {
      delete trigger_progress_dialog_;
      trigger_progress_dialog_ = nullptr;
    }
  }
}

void MainWindow::onNodeSelectedForView(const orc::NodeID& node_id) {
  ORC_LOG_DEBUG("Main window: switching view to node '{}'",
                node_id.to_string());
  applyStageSelection(node_id);
}

void MainWindow::applyStageSelection(const orc::NodeID& node_id) {
  // Update which node is being viewed
  current_view_node_id_ = node_id;

  if (preview_dialog_) {
    preview_dialog_->setCurrentNodeId(node_id);
  }

  // Start the slow-render timer immediately so stages that run inference during
  // requestAvailableOutputs (e.g. nnNtsc) still show "Rendering..." while the
  // worker is busy — even before updatePreview() is eventually called.
  beginPreviewRenderInFlight();

  // Request available outputs from coordinator
  pending_outputs_request_id_ =
      render_coordinator_->requestAvailableOutputs(node_id);

  // Audio channel pairs are node-specific; the dialogue has already cleared
  // its selector (setCurrentNodeId) and repopulates when this answers.
  pending_audio_pairs_request_id_ =
      render_coordinator_->requestAudioChannelPairs(node_id);

  // Note: Analysis dialogs (dropout/SNR) are triggered from stage context menu,
  // not automatically updated when switching nodes

  // The rest will happen in onAvailableOutputsReady callback
}

void MainWindow::onDAGModified() {
  // QtNodes model automatically updates the Project via OrcGraphModel
  // The model change triggers this slot, so Project is already updated

  // Rebuild DAG to reflect the changes (new nodes, edges, etc.)
  project_.rebuildDAG();

  // Update the preview renderer with new DAG structure
  updatePreviewRenderer();

  // Refresh the displayed preview to show the changes
  updatePreview();

  // Update UI state to reflect modified project (window title, save button,
  // etc.)
  updateUIState();
}

void MainWindow::onArrangeDAGToGrid() {
  if (!dag_model_) {
    return;
  }

  // Hierarchical layout - arrange nodes left to right based on topological
  // order
  const auto nodes = project_.presenter()->getNodes();
  const auto edges = project_.presenter()->getEdges();

  if (nodes.empty()) {
    return;
  }

  const double grid_spacing_x = 225.0;
  const double grid_spacing_y = 125.0;
  const double grid_offset_x = 50.0;  // Small offset from edges
  const double grid_offset_y = 50.0;  // Small offset from edges

  // Build adjacency list (forward edges: source -> targets)
  std::map<orc::NodeID, std::vector<orc::NodeID>> forward_edges;
  std::map<orc::NodeID, int> in_degree;

  // Initialize in-degree for all nodes
  for (const auto& node : nodes) {
    in_degree[node.node_id] = 0;
    forward_edges[node.node_id] = {};
  }

  // Count in-degrees and build forward edge list
  for (const auto& edge : edges) {
    in_degree[edge.target_node]++;
    forward_edges[edge.source_node].push_back(edge.target_node);
  }

  // Calculate depth level for each node (BFS from sources)
  std::map<orc::NodeID, int> node_depth;
  std::queue<orc::NodeID> queue;

  // Start with source nodes (in-degree 0)
  for (const auto& [node_id, degree] : in_degree) {
    if (degree == 0) {
      node_depth[node_id] = 0;
      queue.push(node_id);
    }
  }

  // BFS to assign depths
  while (!queue.empty()) {
    orc::NodeID current_id = queue.front();
    queue.pop();

    int current_depth = node_depth[current_id];

    // Process all targets of this node
    for (const auto& target_id : forward_edges[current_id]) {
      // Update depth if not set or if we found a longer path
      if (node_depth.find(target_id) == node_depth.end() ||
          node_depth[target_id] < current_depth + 1) {
        node_depth[target_id] = current_depth + 1;
        queue.push(target_id);
      }
    }
  }

  // Group nodes by depth level
  std::map<int, std::vector<orc::NodeID>> levels;
  for (const auto& [node_id, depth] : node_depth) {
    levels[depth].push_back(node_id);
  }

  // Build reverse edges (target -> sources) for ordering
  std::map<orc::NodeID, std::vector<orc::NodeID>> reverse_edges;
  for (const auto& edge : edges) {
    reverse_edges[edge.target_node].push_back(edge.source_node);
  }

  // Order nodes within each level to minimize edge crossings
  // We'll use a simple heuristic: order by median position of connected nodes
  // in previous/next layer
  std::map<orc::NodeID, double> node_y_position;

  for (const auto& [depth, level_nodes] : levels) {
    std::vector<std::pair<double, orc::NodeID>> nodes_with_order;

    for (const auto& node_id : level_nodes) {
      double order_key = 0.0;
      int count = 0;

      // Consider inputs from previous layer
      if (reverse_edges.count(node_id) > 0) {
        for (const auto& input_id : reverse_edges[node_id]) {
          if (node_y_position.count(input_id) > 0) {
            order_key += node_y_position[input_id];
            count++;
          }
        }
      }

      // Consider outputs to next layer (also helps with ordering)
      if (forward_edges.count(node_id) > 0) {
        for (const auto& output_id : forward_edges[node_id]) {
          if (node_y_position.count(output_id) > 0) {
            order_key += node_y_position[output_id];
            count++;
          }
        }
      }

      // Use median position of connected nodes as ordering key
      if (count > 0) {
        order_key /= count;
      } else {
        // No connections yet, use arbitrary order
        order_key = static_cast<double>(nodes_with_order.size());
      }

      nodes_with_order.push_back({order_key, node_id});
    }

    // Sort nodes by their order key
    std::sort(nodes_with_order.begin(), nodes_with_order.end());

    // Position nodes: each depth level gets a column
    double x = grid_offset_x + depth * grid_spacing_x;

    for (size_t i = 0; i < nodes_with_order.size(); ++i) {
      const auto& node_id = nodes_with_order[i].second;
      double y = grid_offset_y + static_cast<double>(i) * grid_spacing_y;
      node_y_position[node_id] = y;  // Remember position for next layer
      project_.presenter()->setNodePosition(node_id, x, y);
    }
  }

  // Refresh the view
  dag_model_->refresh();

  // Position view to show top-left corner (where grid starts)
  positionViewToTopLeft();

  // Mark project as modified since we changed node positions
  project_.setModified(true);

  // Update UI to reflect modified state
  updateUIState();

  statusBar()->showMessage("Arranged DAG to grid", 2000);
}

void MainWindow::onAbout() {
  QMessageBox about_box(this);
  about_box.setWindowTitle("About Orc GUI");
  about_box.setIconPixmap(
      QPixmap(":/orc-gui/icon.png")
          .scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));

  QString about_text =
      QString(
          "<h2>Orc GUI</h2>"
          "<p><b>Version:</b> %1</p>"
          "<p>Decode Orchestration GUI</p>"
          "<p><b>Copyright:</b> © 2026 Simon Inns</p>"
          "<p><b>License:</b> GNU General Public License v3.0 or later</p>"
          "<p>This program is free software: you can redistribute it and/or "
          "modify "
          "it under the terms of the GNU General Public License as published "
          "by "
          "the Free Software Foundation, either version 3 of the License, or "
          "(at your option) any later version.</p>"
          "<p>This program is distributed in the hope that it will be useful, "
          "but WITHOUT ANY WARRANTY; without even the implied warranty of "
          "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the "
          "GNU General Public License for more details.</p>"
          "<p>You should have received a copy of the GNU General Public "
          "License "
          "along with this program. If not, see "
          "<a "
          "href='https://www.gnu.org/licenses/'>https://www.gnu.org/licenses/</"
          "a>.</p>")
          .arg(ORC_VERSION);

  about_box.setText(about_text);
  about_box.setTextFormat(Qt::RichText);
  about_box.setTextInteractionFlags(Qt::TextBrowserInteraction);
  about_box.exec();
}

void MainWindow::onConfigureLogging() {
  auto* controller = orc::LoggingController::instance();
  if (controller == nullptr) {
    // No controller exists when the window is hosted outside the application
    // (a test harness); there is nothing to reconfigure.
    QMessageBox::warning(this, "Logging",
                         "Logging cannot be reconfigured in this session.");
    return;
  }

  orc::LoggingSettingsDialog dialog(
      controller->settings(), orc::LoggingController::defaultLogFilePath(),
      this);

  // A log file that cannot be opened keeps the dialogue up so the path can be
  // corrected; everything else closes it.
  while (dialog.exec() == QDialog::Accepted) {
    const orc::LoggingSettings requested = dialog.settings();
    const auto result = controller->apply(requested);
    if (result.ok) {
      if (requested.file_logging_enabled) {
        ORC_LOG_INFO("Logging to '{}' at level '{}'",
                     result.log_file.toStdString(),
                     requested.level.toStdString());
      } else {
        ORC_LOG_INFO("Logging to a file turned off");
      }
      return;
    }

    QMessageBox::warning(
        this, "Logging",
        QString("The log file '%1' could not be opened:\n%2\n\nMessages are "
                "still being written to the console.")
            .arg(result.log_file, result.error));
  }
}

void MainWindow::updatePreview() {
  // If no node selected, clear display
  if (current_view_node_id_.is_valid() == false) {
    ORC_LOG_DEBUG("updatePreview: no node selected, returning");
    preview_dialog_->previewWidget()->clearImage();
    return;
  }

  int current_index = preview_dialog_->previewSlider()->value();

  ORC_LOG_DEBUG("updatePreview: rendering output type {} index {} at node '{}'",
                static_cast<int>(current_output_type_), current_index,
                current_view_node_id_.to_string());

  // For YC sources, combine option_id with signal selection (Y/C/Y+C)
  std::string effective_option_id = current_option_id_;
  if (preview_dialog_->signalCombo()->isVisible()) {
    int signal_index = preview_dialog_->signalCombo()->currentIndex();
    std::string suffix;
    switch (signal_index) {
      case 0:
        suffix = "_yc";
        break;  // Y+C composite
      case 1:
        suffix = "_y";
        break;  // Luma only
      case 2:
        suffix = "_c";
        break;  // Chroma only
      default:
        suffix = "_yc";
        break;
    }
    effective_option_id = current_option_id_ + suffix;
    ORC_LOG_DEBUG("  YC source: base option '{}' + signal suffix '{}' = '{}'",
                  current_option_id_, suffix, effective_option_id);
  }

  // During playback the next request is almost always the following frame, so
  // stages are told to expect a sequential run and may pre-fetch. Scrubbing and
  // one-off navigations stay Random — a pre-fetch there is wasted work.
  const orc::PreviewNavigationHint navigation_hint =
      preview_dialog_->isPlaying() ? orc::PreviewNavigationHint::Sequential
                                   : orc::PreviewNavigationHint::Random;

  // Request preview from coordinator (async, thread-safe)
  pending_preview_request_id_ = render_coordinator_->requestPreview(
      current_view_node_id_, current_output_type_, current_index,
      effective_option_id, navigation_hint);

  // Mark that a render is now in-flight (will be cleared when onPreviewReady is
  // called)
  beginPreviewRenderInFlight();

  // Record exactly which index we just dispatched so onPreviewReady can
  // compare against latest_requested_preview_index_ correctly.
  pending_render_index_ = current_index;
}

orc::VideoDataType MainWindow::inferCurrentVideoDataType() const {
  // PAL-M uses PAL colour encoding, so it maps to the PAL data types.
  // ITU-R BT.1700-1 Annex 1 Part B.
  const auto infer_fmt = project_.presenter()
                             ? project_.presenter()->getVideoFormat()
                             : orc::presenters::VideoFormat::Unknown;
  const bool is_pal = (infer_fmt == orc::presenters::VideoFormat::PAL ||
                       infer_fmt == orc::presenters::VideoFormat::PAL_M);

  // The selected preview output type says which domain the user is looking at.
  orc::VideoDataType candidate = is_pal ? orc::VideoDataType::CompositePAL
                                        : orc::VideoDataType::CompositeNTSC;

  if (current_output_type_ == orc::PreviewOutputType::Frame_Field1_First ||
      current_output_type_ == orc::PreviewOutputType::Frame_Reversed ||
      current_output_type_ == orc::PreviewOutputType::Split) {
    candidate =
        is_pal ? orc::VideoDataType::ColourPAL : orc::VideoDataType::ColourNTSC;
  } else if (preview_dialog_ && preview_dialog_->signalCombo() &&
             preview_dialog_->signalCombo()->isVisible()) {
    candidate =
        is_pal ? orc::VideoDataType::YC_PAL : orc::VideoDataType::YC_NTSC;
  }

  // The output type alone is ambiguous, so reconcile it against what the stage
  // actually declares (see resolvePreviewDataType).
  if (!render_coordinator_ || !current_view_node_id_.is_valid()) {
    return candidate;
  }

  return orc::gui::resolvePreviewDataType(
      candidate, render_coordinator_->getStageDataTypes(current_view_node_id_));
}

orc::PreviewCoordinate MainWindow::buildCurrentPreviewCoordinate() const {
  orc::PreviewCoordinate coordinate;
  coordinate.field_index =
      preview_dialog_ ? static_cast<uint64_t>(preview_dialog_->currentIndex())
                      : 0;
  coordinate.line_index =
      (last_line_scope_line_number_ >= 0)
          ? static_cast<uint32_t>(last_line_scope_line_number_)
          : 0U;
  coordinate.sample_offset =
      (last_line_scope_image_x_ >= 0)
          ? static_cast<uint32_t>(last_line_scope_image_x_)
          : 0U;
  coordinate.data_type_context = inferCurrentVideoDataType();
  return coordinate;
}

void MainWindow::refreshPreviewViewAvailability() {
  if (!preview_dialog_ || !render_coordinator_ ||
      !current_view_node_id_.is_valid()) {
    return;
  }

  const orc::VideoDataType data_type = inferCurrentVideoDataType();
  const auto views = render_coordinator_->getAvailablePreviewViews(
      current_view_node_id_, data_type);
  preview_dialog_->setAvailablePreviewViews(views);

  // The vectorscope acquisition follows the stage's output rather than a user
  // choice: a colour-domain output has decoder planes to plot, a signal-domain
  // one has a carrier to demodulate.
  preview_dialog_->setVectorscopeAcquisitionMode(
      orc::gui::isColourDomainDataType(data_type)
          ? orc::VectorscopeAcquisitionMode::DecodedComponent
          : orc::VectorscopeAcquisitionMode::CompositeCarrier);
}

void MainWindow::refreshVectorscopeForCurrentCoordinate() {
  if (!preview_dialog_ || !render_coordinator_ ||
      !current_view_node_id_.is_valid()) {
    return;
  }

  const std::string active_vectorscope_view_id =
      preview_dialog_->activeVectorscopeViewId();
  if (!preview_dialog_->hasAvailablePreviewView(active_vectorscope_view_id)) {
    preview_dialog_->updateVectorscope(current_view_node_id_, std::nullopt);
    return;
  }

  // Keep vectorscope requests off the presenter while preview rendering is
  // active. This avoids UI-thread synchronous vectorscope calls racing the
  // worker render path.
  if (preview_render_in_flight_) {
    return;
  }

  if (!preview_dialog_->isVectorscopeVisibleForNode(current_view_node_id_)) {
    return;
  }

  orc::PreviewCoordinate coordinate =
      preview_dialog_->sharedPreviewCoordinate().has_value()
          ? *preview_dialog_->sharedPreviewCoordinate()
          : buildCurrentPreviewCoordinate();

  // Vectorscope must follow the currently displayed preview item.
  // Shared coordinates may come from line-scope clicks (absolute field
  // indices), which would otherwise desynchronize frame-based vectorscope
  // requests.
  coordinate.field_index =
      static_cast<uint64_t>(preview_dialog_->currentIndex());
  coordinate.data_type_context = inferCurrentVideoDataType();
  preview_dialog_->applyVectorscopeAcquisition(coordinate);

  if (!orc::gui::isColourDomainDataType(coordinate.data_type_context)) {
    // A composite acquisition addresses a frame of the carrier, so the
    // preview item index has to be resolved against the preview mode: the
    // flat field modes index sequential fields, every other mode indexes
    // frames.
    if (current_output_type_ == orc::PreviewOutputType::Frame_Field1 ||
        current_output_type_ == orc::PreviewOutputType::Frame_Field2) {
      coordinate.field_index /= 2;
    }
  }

  if (!coordinate.is_valid()) {
    return;
  }

  const auto result = render_coordinator_->requestPreviewViewData(
      current_view_node_id_, active_vectorscope_view_id,
      coordinate.data_type_context, coordinate);

  if (!result.success ||
      result.payload_kind != orc::PreviewViewPayloadKind::Vectorscope) {
    preview_dialog_->updateVectorscope(current_view_node_id_, std::nullopt);
    ORC_LOG_DEBUG("refreshVectorscopeForCurrentCoordinate: {}",
                  result.error_message.empty() ? "no vectorscope payload"
                                               : result.error_message);
    return;
  }

  preview_dialog_->updateVectorscope(current_view_node_id_, result.vectorscope);
}

void MainWindow::onPreviewVectorscopeRequested(
    const orc::PreviewCoordinate& coordinate) {
  if (!current_view_node_id_.is_valid()) {
    return;
  }

  orc::PreviewCoordinate updated = coordinate;
  updated.data_type_context = inferCurrentVideoDataType();
  preview_dialog_->setSharedPreviewCoordinate(updated);
  refreshVectorscopeForCurrentCoordinate();
}

void MainWindow::refreshHistogramForCurrentCoordinate() {
  if (!preview_dialog_ || !render_coordinator_ ||
      !current_view_node_id_.is_valid()) {
    return;
  }

  const std::string histogram_view_id = PreviewDialog::kHistogramViewIdRef();
  if (!preview_dialog_->hasAvailablePreviewView(histogram_view_id)) {
    preview_dialog_->updateHistogram(current_view_node_id_, std::nullopt);
    return;
  }

  if (preview_render_in_flight_) {
    return;
  }

  if (!preview_dialog_->isHistogramVisibleForNode(current_view_node_id_)) {
    return;
  }

  orc::PreviewCoordinate coordinate =
      preview_dialog_->sharedPreviewCoordinate().has_value()
          ? *preview_dialog_->sharedPreviewCoordinate()
          : buildCurrentPreviewCoordinate();

  coordinate.field_index =
      static_cast<uint64_t>(preview_dialog_->currentIndex());
  coordinate.data_type_context = inferCurrentVideoDataType();
  if (!coordinate.is_valid()) {
    return;
  }

  const auto result = render_coordinator_->requestPreviewViewData(
      current_view_node_id_, histogram_view_id, coordinate.data_type_context,
      coordinate);

  if (!result.success ||
      result.payload_kind != orc::PreviewViewPayloadKind::Histogram) {
    preview_dialog_->updateHistogram(current_view_node_id_, std::nullopt);
    ORC_LOG_DEBUG("refreshHistogramForCurrentCoordinate: {}",
                  result.error_message.empty() ? "no histogram payload"
                                               : result.error_message);
    return;
  }

  preview_dialog_->updateHistogram(current_view_node_id_, result.histogram);
}

void MainWindow::onPreviewHistogramRequested(
    const orc::PreviewCoordinate& coordinate) {
  if (!current_view_node_id_.is_valid()) {
    return;
  }

  orc::PreviewCoordinate updated = coordinate;
  updated.data_type_context = inferCurrentVideoDataType();
  preview_dialog_->setSharedPreviewCoordinate(updated);
  refreshHistogramForCurrentCoordinate();
}

void MainWindow::updatePreviewModeCombo() {
  ORC_LOG_DEBUG(
      "updatePreviewModeCombo: current_output_type={}, current_option_id='{}'",
      static_cast<int>(current_output_type_), current_option_id_);

  // Block signals while updating combo box
  preview_dialog_->previewModeCombo()->blockSignals(true);

  // Clear existing items
  preview_dialog_->previewModeCombo()->clear();

  // Populate from available outputs
  int current_type_index = 0;
  for (size_t i = 0; i < available_outputs_.size(); ++i) {
    const auto& output = available_outputs_[i];
    preview_dialog_->previewModeCombo()->addItem(
        QString::fromStdString(output.display_name));

    ORC_LOG_DEBUG("  output[{}]: type={}, option_id='{}', display_name='{}'", i,
                  static_cast<int>(output.type), output.option_id,
                  output.display_name);

    // Track which index matches current output type AND option_id
    // (multiple outputs can have same type, e.g., Split (Y) and Split (Raw))
    if (output.type == current_output_type_ &&
        output.option_id == current_option_id_) {
      current_type_index = static_cast<int>(i);
      ORC_LOG_DEBUG("  -> MATCH at index {}", i);
    }
  }

  ORC_LOG_DEBUG("updatePreviewModeCombo: setting combo to index {}",
                current_type_index);

  // Set current selection to match current output type
  if (!available_outputs_.empty()) {
    preview_dialog_->previewModeCombo()->setCurrentIndex(current_type_index);
    preview_dialog_->previewModeCombo()->setEnabled(true);
  } else {
    preview_dialog_->previewModeCombo()->setEnabled(false);
  }

  // Restore signals
  preview_dialog_->previewModeCombo()->blockSignals(false);
}

void MainWindow::updateAspectRatioCombo() {
  // Populate aspect ratio combo (client-side, no coordinator needed)
  static std::vector<orc::AspectRatioModeInfo> available_modes = {
      {orc::AspectRatioMode::SAR_1_1, "1:1 (Square)", 1.0},
      {orc::AspectRatioMode::DAR_4_3, "4:3 (Display)", 0.7}};

  // Block signals while updating combo box
  preview_dialog_->aspectRatioCombo()->blockSignals(true);

  // Clear existing items
  preview_dialog_->aspectRatioCombo()->clear();

  // Populate combo from available modes
  int current_index = 0;
  for (size_t i = 0; i < available_modes.size(); ++i) {
    const auto& mode_info = available_modes[i];
    preview_dialog_->aspectRatioCombo()->addItem(
        QString::fromStdString(mode_info.display_name));

    // Track which index matches current mode
    if (mode_info.mode == current_aspect_ratio_mode_) {
      current_index = static_cast<int>(i);
    }
  }

  // Set current selection to match the current mode
  if (!available_modes.empty()) {
    preview_dialog_->aspectRatioCombo()->setCurrentIndex(current_index);
  }

  // Restore signals
  preview_dialog_->aspectRatioCombo()->blockSignals(false);

  // Apply correction immediately for current mode using active area
  auto computeAspectCorrection = [this]() -> double {
    double correction = 1.0;
    // Always use the fallback method to avoid concurrent DAG access issues
    for (const auto& output : available_outputs_) {
      if (output.type == current_output_type_ &&
          output.option_id == current_option_id_) {
        correction = output.dar_aspect_correction;
        break;
      }
    }
    return correction;
  };

  double correction =
      (current_aspect_ratio_mode_ == orc::AspectRatioMode::DAR_4_3)
          ? computeAspectCorrection()
          : 1.0;
  if (preview_dialog_ && preview_dialog_->previewWidget()) {
    preview_dialog_->previewWidget()->setAspectCorrection(correction);
  }
}

void MainWindow::refreshViewerControls(bool skip_preview) {
  // This helper updates all viewer controls based on current node's available
  // outputs Should be called after available_outputs_ is populated

  // Audio maps to whole frames, so the chase target has to know how many
  // preview indices one frame spans at the current output.
  if (preview_dialog_) {
    preview_dialog_->setAudioItemsPerFrame(
        orc::gui::preview_audio::itemsPerFrameForOutputType(
            current_output_type_));
  }

  if (current_view_node_id_.is_valid() == false || available_outputs_.empty()) {
    ORC_LOG_DEBUG("refreshViewerControls: no node or outputs");
    return;
  }

  // Update the preview mode combo box
  updatePreviewModeCombo();

  // Update the aspect ratio combo box
  updateAspectRatioCombo();

  // Apply aspect correction based on selected mode
  // Use the fallback method to avoid concurrent DAG access from GUI and render
  // threads
  double correction = 1.0;
  if (current_aspect_ratio_mode_ == orc::AspectRatioMode::DAR_4_3) {
    for (const auto& output : available_outputs_) {
      if (output.type == current_output_type_ &&
          output.option_id == current_option_id_) {
        correction = output.dar_aspect_correction;
        break;
      }
    }
  }
  if (preview_dialog_ && preview_dialog_->previewWidget()) {
    preview_dialog_->previewWidget()->setAspectCorrection(correction);
  }

  // Check if current output has separate channels (YC source) and show/hide
  // signal selector
  bool has_separate_channels = false;
  for (const auto& output : available_outputs_) {
    if (output.has_separate_channels) {
      has_separate_channels = true;
      break;
    }
  }
  preview_dialog_->setSignalControlsVisible(has_separate_channels);

  // Get count for current output type and option_id
  int new_total = 0;
  for (const auto& output : available_outputs_) {
    if (output.type == current_output_type_ &&
        output.option_id == current_option_id_) {
      new_total = static_cast<int>(output.count);
      break;
    }
  }

  // Update slider range and labels
  if (new_total > 0) {
    preview_dialog_->previewSlider()->setRange(0, new_total - 1);

    // Clamp current slider position to new range
    if (preview_dialog_->previewSlider()->value() >= new_total) {
      preview_dialog_->previewSlider()->setValue(0);
    }

    preview_dialog_->previewSlider()->setEnabled(true);
    preview_dialog_->frameJumpSpinBox()->setEnabled(true);
  }

  // Update all preview-related components (image, info, VBI dialog)
  if (!skip_preview) {
    updateAllPreviewComponents();
  }
}

void MainWindow::updatePreviewRenderer() {
  ORC_LOG_DEBUG("Updating preview renderer");

  // A playback reader holds the representation it was created from alive, so
  // any DAG or parameter edit makes it stale. Stop playback and drop it before
  // the worker rebuilds; the selector repopulates with the refreshed outputs.
  if (preview_dialog_) {
    preview_dialog_->invalidateAudioSource();
  }

  // Same reasoning for the result viewers. Every caller of this function has
  // just rebuilt the DAG, which replaces every stage object and with it every
  // cached analysis and catalogue — so what these windows are showing was
  // produced by parameters that are no longer set. They close rather than sit
  // there looking current; triggering the node again fills fresh ones.
  closeResultViewers();

  // Get the DAG - could be null for empty projects, that's fine
  auto dag = project_.hasSource() ? project_.getDAG() : nullptr;

  // Debug: show what we're working with
  if (dag) {
    auto nodes = project_.presenter()->getNodes();
    ORC_LOG_DEBUG("DAG contains {} nodes:", nodes.size());
    for (const auto& node : nodes) {
      ORC_LOG_DEBUG("  - {}", node.node_id);
    }
  } else {
    ORC_LOG_DEBUG("No DAG (new/empty project)");
  }

  // Send DAG update to coordinator (thread-safe)
  render_coordinator_->updateDAG(dag);

  // Check if current node is still valid, or if we need to switch
  bool need_to_switch = false;
  if (current_view_node_id_.is_valid() == false) {
    // No node selected yet - use suggestion
    need_to_switch = true;
  } else {
    // Check if current node still exists using presenter
    bool current_exists = project_.presenter()->hasNode(current_view_node_id_);

    // If current node was deleted or is placeholder when real nodes exist,
    // switch
    if ((!current_exists && current_view_node_id_ != NodeID(-999)) ||
        (current_view_node_id_ == NodeID(-999) &&
         project_.presenter()->getFirstNode().is_valid())) {
      need_to_switch = true;
    }
  }

  // Node selection logic: The coordinator now handles renderer updates
  // internally. When we need to switch nodes (e.g., current was deleted), we'll
  // request outputs for the new node, and onAvailableOutputsReady will handle
  // the rest.
  if (need_to_switch) {
    // Note: Could implement coordinator->requestSuggestedViewNode() to get a
    // smart suggestion (e.g., prefer sink nodes, or nodes with most
    // connections). Current approach: Simple fallback to first node is adequate
    // for typical workflows.
    ORC_LOG_DEBUG("Node switching needed - using first node fallback");
    if (current_view_node_id_.is_valid() == false) {
      // Pick first node as temporary fallback
      NodeID first_node = project_.presenter()->getFirstNode();
      if (first_node.is_valid()) {
        selectStageInDAG(first_node);
      }
    }
  } else {
    // Keep current node - request fresh outputs in case parameters changed
    if (current_view_node_id_.is_valid()) {
      ORC_LOG_DEBUG("Keeping current node '{}', refreshing outputs",
                    current_view_node_id_.to_string());
      pending_outputs_request_id_ =
          render_coordinator_->requestAvailableOutputs(current_view_node_id_);
    }
  }
}

void MainWindow::onPreviewDialogExportPNG() {
  if (current_view_node_id_.is_valid() == false) {
    QMessageBox::information(this, "Export PNG",
                             "No preview available to export.");
    return;
  }

  // Get filename from user
  QString filename = QFileDialog::getSaveFileName(
      this, "Export Preview as PNG", getLastExportDirectory(),
      "PNG Images (*.png);;All Files (*)");

  if (filename.isEmpty()) {
    return;  // User cancelled
  }

  // Ensure .png extension
  if (!filename.endsWith(".png", Qt::CaseInsensitive)) {
    filename += ".png";
  }

  // Remember directory
  setLastExportDirectory(QFileInfo(filename).absolutePath());

  int current_index = preview_dialog_->previewSlider()->value();

  // Match export output selection to the currently displayed preview signal
  // for YC sources (Y+C / Y / C).
  std::string effective_option_id = current_option_id_;
  if (preview_dialog_->signalCombo()->isVisible()) {
    int signal_index = preview_dialog_->signalCombo()->currentIndex();
    std::string suffix;
    switch (signal_index) {
      case 0:
        suffix = "_yc";
        break;  // Y+C composite
      case 1:
        suffix = "_y";
        break;  // Luma only
      case 2:
        suffix = "_c";
        break;  // Chroma only
      default:
        suffix = "_yc";
        break;
    }
    effective_option_id = current_option_id_ + suffix;
  }

  // Export should honor the currently selected aspect ratio mode.
  double aspect_correction = 1.0;
  if (current_aspect_ratio_mode_ == orc::AspectRatioMode::DAR_4_3) {
    for (const auto& output : available_outputs_) {
      if (output.type == current_output_type_ &&
          output.option_id == current_option_id_) {
        aspect_correction = output.dar_aspect_correction;
        break;
      }
    }
  }

  // Request PNG save via coordinator (delegates to core)
  ORC_LOG_INFO("Requesting PNG export to: {}", filename.toStdString());

  render_coordinator_->requestSavePNG(
      current_view_node_id_, current_output_type_, current_index,
      filename.toStdString(), effective_option_id, aspect_correction);

  statusBar()->showMessage(QString("Exporting preview to %1...").arg(filename),
                           2000);
}

// Settings helpers

void MainWindow::selectLowestSourceStage() {
  // Find source stage with the lowest node ID
  const auto nodes = project_.presenter()->getNodes();
  const auto all_stages = orc::presenters::ProjectPresenter::getAllStages();
  orc::NodeID lowest_source_id;
  bool found = false;

  for (const auto& node : nodes) {
    // Check if this stage is a source by looking up its info
    auto stage_it = std::find_if(all_stages.begin(), all_stages.end(),
                                 [&node](const orc::presenters::StageInfo& s) {
                                   return s.name == node.stage_name;
                                 });

    if (stage_it != all_stages.end() && stage_it->is_source) {
      if (!found || node.node_id < lowest_source_id) {
        lowest_source_id = node.node_id;
        found = true;
      }
    }
  }

  if (found) {
    ORC_LOG_DEBUG("Auto-selecting source stage: {}",
                  lowest_source_id.to_string());
    selectStageInDAG(lowest_source_id);
  } else {
    ORC_LOG_DEBUG("No source stages found to auto-select");
  }
}

void MainWindow::selectStageInDAG(const orc::NodeID& node_id) {
  if (!dag_model_ || !dag_scene_) {
    return;
  }
  QtNodes::NodeId qt_node_id = dag_model_->getQtNodeId(node_id);
  if (qt_node_id == QtNodes::InvalidNodeId) {
    return;
  }
  dag_scene_->selectNode(qt_node_id);
}

QString MainWindow::getLastProjectDirectory() const {
  QSettings settings("orc-project", "orc-gui");
  QString dir = settings.value("lastProjectDirectory", QString()).toString();
  if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
    return QDir::homePath();
  }
  return dir;
}

void MainWindow::setLastProjectDirectory(const QString& path) {
  QSettings settings("orc-project", "orc-gui");
  settings.setValue("lastProjectDirectory", path);
}

QString MainWindow::getLastSourceDirectory() const {
  QSettings settings("orc-project", "orc-gui");
  QString dir = settings.value("lastSourceDirectory", QString()).toString();
  if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
    return QDir::homePath();
  }
  return dir;
}

void MainWindow::setLastSourceDirectory(const QString& path) {
  QSettings settings("orc-project", "orc-gui");
  settings.setValue("lastSourceDirectory", path);
}

QString MainWindow::getLastExportDirectory() const {
  QSettings settings("orc-project", "orc-gui");
  QString dir = settings.value("lastExportDirectory", QString()).toString();
  if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
    return QDir::homePath();
  }
  return dir;
}

void MainWindow::setLastExportDirectory(const QString& path) {
  QSettings settings("orc-project", "orc-gui");
  settings.setValue("lastExportDirectory", path);
}

void MainWindow::onNodeContextMenu(QtNodes::NodeId nodeId, const QPointF& pos) {
  ORC_LOG_DEBUG("Context menu requested for node: {}", nodeId);

  // The OrcGraphicsScene already handles context menus
  // This slot is here for future extension if needed
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  // Future: Add event filtering if needed
  return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onQtNodeSelected(QtNodes::NodeId nodeId) {
  if (!dag_model_) {
    return;
  }

  // Convert QtNodes ID to ORC node ID
  NodeID orc_node_id = dag_model_->getOrcNodeId(nodeId);
  if (orc_node_id.is_valid()) {
    ORC_LOG_DEBUG("QtNode {} selected -> ORC node '{}'", nodeId, orc_node_id);
    onNodeSelectedForView(orc_node_id);
  }
}

void MainWindow::runAnalysisForNode(const orc::AnalysisToolInfo& tool_info,
                                    const orc::NodeID& node_id,
                                    const std::string& stage_name) {
  const bool is_dropout_analysis_tool =
      tool_info.stage_tool_contract ==
          "decode-orc.stage-tools.dropout-analysis.v1" ||
      tool_info.id == "dropout_analysis";
  const bool is_snr_analysis_tool =
      tool_info.stage_tool_contract ==
          "decode-orc.stage-tools.snr-analysis.v1" ||
      tool_info.id == "snr_analysis";
  const bool is_burst_level_analysis_tool =
      tool_info.stage_tool_contract ==
          "decode-orc.stage-tools.burst-level-analysis.v1" ||
      tool_info.id == "burst_level_analysis";
  // Any stage whose tool is a catalogue browser routes to the generic viewer;
  // the host needs to know nothing about the service it catalogues.
  const bool is_catalogue_tool =
      tool_info.stage_tool_contract == orc::kCatalogueBrowserContractId ||
      tool_info.stage_tool_kind == "catalogue_browser";

  ORC_LOG_DEBUG("Running analysis '{}' for node '{}'", tool_info.name,
                node_id.to_string());

  // Descriptor contract-based dispatch: tools that advertise their behavior via
  // SDK StageToolDescriptor are routed via stage_tool_contract. Legacy built-in
  // tools propagate the same contract strings through applyLegacyToolMetadata()
  // in the presenter, so this check covers both paths.
  const bool is_mask_line_config_tool =
      tool_info.stage_tool_contract ==
      "decode-orc.stage-tools.mask-line-config.v1";
  const bool is_ffmpeg_preset_tool = tool_info.stage_tool_contract ==
                                     "decode-orc.stage-tools.ffmpeg-preset.v1";
  const bool is_dropout_editor_tool =
      tool_info.stage_tool_contract ==
      "decode-orc.stage-tools.dropout-editor.v1";

  // Special-case: Mask Line Configuration uses a custom rules-based config
  // dialog
  if (is_mask_line_config_tool) {
    ORC_LOG_DEBUG("Opening mask line configuration dialog for node '{}'",
                  node_id.to_string());

    // Get current parameters from the node
    auto nodes = project_.presenter()->getNodes();
    auto node_it = std::find_if(nodes.begin(), nodes.end(),
                                [&node_id](const orc::presenters::NodeInfo& n) {
                                  return n.node_id == node_id;
                                });

    if (node_it == nodes.end()) {
      QMessageBox::warning(this, "Error", "Could not find node in project.");
      return;
    }

    // Create and show the config dialog
    MaskLineConfigDialog dialog(this);

    // Provide amplitude unit and video parameters for unit-aware display.
    dialog.setAmplitudeUnit(project_.presenter()->getAmplitudeUnit());
    {
      auto* core_project = project_.presenter()->getCoreProjectHandle();
      if (core_project) {
        orc::presenters::RenderPresenter rp(core_project);
        // Throwaway helper presenter: parameter reads only — keep cheap.
        rp.setBackgroundObservationEnabled(false);
        rp.setDAG(project_.getDAG());
        auto vp = rp.getVideoParameters(node_id);
        if (vp.has_value()) {
          dialog.setVideoParameters(
              orc::presenters::toVideoParametersView(*vp));
        }
      }
    }

    // Load current parameters from presenter
    auto current_params = project_.presenter()->getNodeParameters(node_id);
    dialog.set_parameters(current_params);

    // Show dialog and apply if accepted
    if (dialog.exec() == QDialog::Accepted) {
      auto new_params = dialog.get_parameters();

      ORC_LOG_DEBUG("Mask line config accepted, applying parameters");

      try {
        // Update node parameters using presenter
        project_.presenter()->setNodeParameters(node_id, new_params);

        // Mark project as modified
        project_.setModified(true);

        // Update UI to reflect modified state
        updateUIState();

        // Rebuild DAG to pick up the new parameter values
        project_.rebuildDAG();

        // Update the preview renderer with the new DAG
        updatePreviewRenderer();

        // Refresh QtNodes view
        dag_model_->refresh();

        // Update the preview to show the changes
        updatePreview();

        statusBar()->showMessage(
            QString("Applied mask line configuration to '%1'")
                .arg(QString::fromStdString(node_id.to_string())),
            3000);
      } catch (const std::exception& e) {
        QMessageBox::critical(this, "Configuration Error",
                              QString("Failed to apply configuration: %1")
                                  .arg(QString::fromStdString(e.what())));
      }
    }

    return;
  }

  // Special-case: FFmpeg Preset Configuration uses a custom preset dialog
  if (is_ffmpeg_preset_tool) {
    ORC_LOG_DEBUG("Opening FFmpeg preset configuration dialog for node '{}'",
                  node_id.to_string());

    // Get current parameters from the node
    auto nodes = project_.presenter()->getNodes();
    auto node_it = std::find_if(nodes.begin(), nodes.end(),
                                [&node_id](const orc::presenters::NodeInfo& n) {
                                  return n.node_id == node_id;
                                });

    if (node_it == nodes.end()) {
      QMessageBox::warning(this, "Error", "Could not find node in project.");
      return;
    }

    // Create and show the preset dialog
    FFmpegPresetDialog dialog(project_.projectPath(), this);

    // Load current parameters from presenter
    auto current_params = project_.presenter()->getNodeParameters(node_id);
    dialog.set_parameters(current_params);

    // Show dialog and apply if accepted
    if (dialog.exec() == QDialog::Accepted) {
      auto new_params = dialog.get_parameters();

      ORC_LOG_DEBUG("FFmpeg preset config accepted, applying parameters");

      try {
        // Update node parameters using presenter
        project_.presenter()->setNodeParameters(node_id, new_params);

        // Mark project as modified
        project_.setModified(true);

        // Update UI to reflect modified state
        updateUIState();

        // Rebuild DAG to pick up the new parameter values
        project_.rebuildDAG();

        // Update the preview renderer with the new DAG
        updatePreviewRenderer();

        // Refresh QtNodes view
        dag_model_->refresh();

        // Update the preview to show the changes
        updatePreview();

        statusBar()->showMessage(
            QString("Applied FFmpeg export preset to '%1'")
                .arg(QString::fromStdString(node_id.to_string())),
            3000);
      } catch (const std::exception& e) {
        QMessageBox::critical(this, "Configuration Error",
                              QString("Failed to apply configuration: %1")
                                  .arg(QString::fromStdString(e.what())));
      }
    }

    return;
  }

  // Special-case: Vectorscope is a live visualization dialog, not a batch
  // analysis
  if (tool_info.id == "vectorscope") {
    // Ensure preview selection matches the requested node.
    if (current_view_node_id_ != node_id) {
      onNodeSelectedForView(node_id);
    }

    preview_dialog_->setCurrentNodeId(node_id);
    preview_dialog_->showVectorscopeForNode(node_id);
    refreshVectorscopeForCurrentCoordinate();
    return;
  }

  // Special-case: Dropout Editor opens interactive editor dialog
  if (is_dropout_editor_tool) {
    // Get the project
    if (!dag_model_) {
      ORC_LOG_ERROR("No DAG model available for dropout editor");
      return;
    }

    // Get node and edge information from presenter
    const auto nodes = project_.presenter()->getNodes();
    auto node_it = std::find_if(
        nodes.begin(), nodes.end(),
        [&node_id](const auto& n) { return n.node_id == node_id; });

    if (node_it == nodes.end()) {
      ORC_LOG_ERROR("Node '{}' not found in project", node_id.to_string());
      return;
    }

    // Execute DAG to get the VideoFrameRepresentation for the input node
    // Find the input edge to this node
    const auto edges = project_.presenter()->getEdges();
    orc::NodeID input_node_id;
    bool found_input = false;
    for (const auto& edge : edges) {
      if (edge.target_node == node_id) {
        input_node_id = edge.source_node;
        found_input = true;
        break;
      }
    }

    if (!found_input) {
      QMessageBox::warning(
          this, "Error",
          "Could not find input node for dropout editor. "
          "Ensure the dropout_map stage has a valid video source connected.");
      return;
    }

    // Create a RenderPresenter for the editor so it renders frames through
    // the same pipeline as the preview dialog. The dialog holds a shared_ptr
    // as it is non-modal; dropout overlay baking is disabled because the
    // editor draws its own overlay graphics.
    auto* core_project = project_.presenter()->getCoreProjectHandle();
    if (!core_project) {
      QMessageBox::warning(this, "Error", "Invalid project state.");
      return;
    }

    auto render_presenter = makeRenderPresenterAdapter(core_project);
    // Auxiliary presenter: rendering only. Must be set before setDAG() so the
    // build stays cheap on the GUI thread and no second background observation
    // pipeline (scheduler/sweeps) is spawned — the coordinator's presenter
    // already runs one for this process.
    render_presenter->setBackgroundObservationEnabled(false);
    render_presenter->setDAG(project_.getDAG());
    render_presenter->setShowDropouts(false);

    // Open the editor dialog as a non-modal independent window
    DropoutEditorDialog* dialog = nullptr;
    try {
      dialog = new DropoutEditorDialog(node_id, dropout_presenter_.get(),
                                       render_presenter, input_node_id, this);
    } catch (const std::exception& e) {
      QMessageBox::warning(
          this, "Error",
          QString("Failed to open dropout editor: %1").arg(e.what()));
      return;
    }
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    auto apply_dropout_map = [this, node_id, dialog]() {
      auto edited_map = dialog->getDropoutMap();

      ORC_LOG_DEBUG("Dropout editor returned map with {} frame entries",
                    edited_map.size());

      if (dropout_presenter_->setDropoutMap(node_id, edited_map)) {
        project_.setModified(true);
        updateUIState();
        project_.rebuildDAG();
        // Refresh the graph so the node's status dot reflects the new map
        // (statuses are cached in the model until it is refreshed).
        dag_model_->refresh();
        updatePreviewRenderer();
        ORC_LOG_DEBUG("Dropout map updated for node '{}'", node_id.to_string());
        updatePreview();
      } else {
        QMessageBox::warning(this, "Error", "Failed to save dropout map");
      }
    };

    connect(dialog, &QDialog::accepted, this, apply_dropout_map);
    connect(dialog, &DropoutEditorDialog::applied, this, apply_dropout_map);

    // Show as non-modal window
    dialog->show();
    return;
  }

  // Special-case: Dropout Analysis triggers batch processing and shows dialog
  if (is_dropout_analysis_tool) {
    // Get node info from project
    auto nodes = project_.presenter()->getNodes();
    auto node_it = std::find_if(nodes.begin(), nodes.end(),
                                [&node_id](const orc::presenters::NodeInfo& n) {
                                  return n.node_id == node_id;
                                });

    QString node_label = QString::fromStdString(stage_name);
    if (node_it != nodes.end()) {
      node_label = QString::fromStdString(
          node_it->label.empty() ? node_it->stage_name : node_it->label);
    }
    QString title = QString("Dropout Analysis - %1 (%2)")
                        .arg(node_label)
                        .arg(QString::fromStdString(node_id.to_string()));

    // Get or create dialog for this stage
    DropoutAnalysisDialog* dialog = nullptr;
    auto it = dropout_analysis_dialogs_.find(node_id);
    if (it == dropout_analysis_dialogs_.end()) {
      // Create new dialog for this stage
      dialog = new DropoutAnalysisDialog(this);
      dialog->setWindowTitle(title);
      dialog->setAttribute(Qt::WA_DeleteOnClose, true);

      // Connect destroyed signal to clean up map entry
      connect(dialog, &QObject::destroyed,
              [this, node_id]() { dropout_analysis_dialogs_.erase(node_id); });

      dropout_analysis_dialogs_[node_id] = dialog;
    } else {
      dialog = it->second;
    }

    // Show the dialog (but it will be empty until data arrives)
    dialog->show();
    dialog->raise();
    dialog->activateWindow();

    // Request dropout data from coordinator (triggers batch processing)
    // Mode is controlled by the sink stage parameter, not the dialog
    uint64_t request_id = render_coordinator_->requestDropoutData(
        node_id, orc::DropoutAnalysisMode::FULL_FIELD);
    pending_dropout_requests_[request_id] = node_id;

    ORC_LOG_DEBUG(
        "Requested dropout analysis data for node '{}', request_id={}",
        node_id.to_string(), request_id);
    return;
  }

  // Special-case: SNR Analysis triggers batch processing and shows dialog
  if (is_snr_analysis_tool) {
    // Get node info from project
    auto nodes = project_.presenter()->getNodes();
    auto node_it = std::find_if(nodes.begin(), nodes.end(),
                                [&node_id](const orc::presenters::NodeInfo& n) {
                                  return n.node_id == node_id;
                                });

    QString node_label = QString::fromStdString(stage_name);
    if (node_it != nodes.end()) {
      node_label = QString::fromStdString(
          node_it->label.empty() ? node_it->stage_name : node_it->label);
    }
    QString title = QString("SNR Analysis - %1 (%2)")
                        .arg(node_label)
                        .arg(QString::fromStdString(node_id.to_string()));

    // Get or create dialog for this stage
    SNRAnalysisDialog* dialog = nullptr;
    auto it = snr_analysis_dialogs_.find(node_id);
    if (it == snr_analysis_dialogs_.end()) {
      // Create new dialog for this stage
      dialog = new SNRAnalysisDialog(this);
      dialog->setWindowTitle(title);
      dialog->setAttribute(Qt::WA_DeleteOnClose, true);

      // Connect mode changed to re-request data
      connect(dialog, &SNRAnalysisDialog::modeChanged,
              [this, node_id, dialog]() {
                if (dialog->isVisible()) {
                  // Show progress dialog for this stage
                  auto mode = dialog->getCurrentMode();
                  uint64_t request_id =
                      render_coordinator_->requestSNRData(node_id, mode);
                  pending_snr_requests_[request_id] = node_id;
                }
              });

      // Connect destroyed signal to clean up map entry
      connect(dialog, &QObject::destroyed,
              [this, node_id]() { snr_analysis_dialogs_.erase(node_id); });

      snr_analysis_dialogs_[node_id] = dialog;
    } else {
      dialog = it->second;
    }

    // Show the dialog (but it will be empty until data arrives)
    dialog->show();
    dialog->raise();
    dialog->activateWindow();

    // Request SNR data from coordinator (triggers batch processing)
    auto mode = dialog->getCurrentMode();
    uint64_t request_id = render_coordinator_->requestSNRData(node_id, mode);
    pending_snr_requests_[request_id] = node_id;

    ORC_LOG_DEBUG(
        "Requested SNR analysis data for node '{}', mode {}, request_id={}",
        node_id.to_string(), static_cast<int>(mode), request_id);
    return;
  }

  // Special-case: Burst Level Analysis triggers batch processing and shows
  // dialog
  if (is_burst_level_analysis_tool) {
    // Get node info from project
    auto nodes = project_.presenter()->getNodes();
    auto node_it = std::find_if(nodes.begin(), nodes.end(),
                                [&node_id](const orc::presenters::NodeInfo& n) {
                                  return n.node_id == node_id;
                                });

    QString node_label = QString::fromStdString(stage_name);
    if (node_it != nodes.end()) {
      node_label = QString::fromStdString(
          node_it->label.empty() ? node_it->stage_name : node_it->label);
    }
    QString title = QString("Burst Level Analysis - %1 (%2)")
                        .arg(node_label)
                        .arg(QString::fromStdString(node_id.to_string()));

    // Get or create dialog for this stage
    BurstLevelAnalysisDialog* dialog = nullptr;
    auto it = burst_level_analysis_dialogs_.find(node_id);
    if (it == burst_level_analysis_dialogs_.end()) {
      // Create new dialog for this stage
      dialog = new BurstLevelAnalysisDialog(this);
      dialog->setWindowTitle(title);
      dialog->setAttribute(Qt::WA_DeleteOnClose, true);

      // Connect destroyed signal to clean up map entry
      connect(dialog, &QObject::destroyed, [this, node_id]() {
        burst_level_analysis_dialogs_.erase(node_id);
      });

      burst_level_analysis_dialogs_[node_id] = dialog;
    } else {
      dialog = it->second;
    }

    // Show the dialog (but it will be empty until data arrives)
    dialog->show();
    dialog->raise();
    dialog->activateWindow();

    // Request burst level data from coordinator (triggers batch processing)
    uint64_t request_id = render_coordinator_->requestBurstLevelData(node_id);
    pending_burst_level_requests_[request_id] = node_id;

    ORC_LOG_DEBUG(
        "Requested burst level analysis data for node '{}', request_id={}",
        node_id.to_string(), request_id);
    return;
  }

  // Catalogue browser: decode the range and show the generic browser.
  if (is_catalogue_tool) {
    auto nodes = project_.presenter()->getNodes();
    auto node_it = std::find_if(nodes.begin(), nodes.end(),
                                [&node_id](const orc::presenters::NodeInfo& n) {
                                  return n.node_id == node_id;
                                });

    QString node_label = QString::fromStdString(stage_name);
    if (node_it != nodes.end()) {
      node_label = QString::fromStdString(
          node_it->label.empty() ? node_it->stage_name : node_it->label);
    }
    // The tool names itself — "Teletext Pages", "NABTS Records" — so the title
    // says what the reader asked for without the host knowing what that is.
    const QString title = QString("%1 - %2 (%3)")
                              .arg(QString::fromStdString(tool_info.name))
                              .arg(node_label)
                              .arg(QString::fromStdString(node_id.to_string()));

    // Get or create the viewer for this node
    CatalogueDialog* dialog = nullptr;
    auto it = catalogue_dialogs_.find(node_id);
    if (it == catalogue_dialogs_.end()) {
      dialog = new CatalogueDialog(this);
      dialog->setWindowTitle(title);
      dialog->setAttribute(Qt::WA_DeleteOnClose, true);

      // Connect destroyed signal to clean up map entry
      connect(dialog, &QObject::destroyed,
              [this, node_id]() { catalogue_dialogs_.erase(node_id); });

      // Changing how the catalogue is presented asks the stage to build the
      // same catalogue another way. It is a read like the first one — nothing
      // upstream runs — so the page on screen stays put until the new one
      // arrives rather than being replaced by a pending state.
      connect(dialog, &CatalogueDialog::viewSelectionChanged, this,
              [this, node_id](const QString& option_id,
                              const QStringList& active_toggles) {
                if (!render_coordinator_) {
                  return;
                }
                std::vector<std::string> toggles;
                toggles.reserve(static_cast<size_t>(active_toggles.size()));
                for (const QString& toggle_id : active_toggles) {
                  toggles.push_back(toggle_id.toStdString());
                }
                const uint64_t view_request_id =
                    render_coordinator_->requestCatalogueData(
                        node_id, option_id.toStdString(), toggles);
                pending_catalogue_requests_[view_request_id] = node_id;
                ORC_LOG_DEBUG(
                    "Requested catalogue for node '{}' under view option "
                    "'{}' with {} toggle(s) on, request_id={}",
                    node_id.to_string(), option_id.toStdString(),
                    toggles.size(), view_request_id);
              });

      catalogue_dialogs_[node_id] = dialog;
    } else {
      dialog = it->second;
    }

    // Show the viewer (empty until the catalogue arrives)
    dialog->showPending();
    dialog->show();
    dialog->raise();
    dialog->activateWindow();

    // Request the catalogue from the coordinator, which triggers the stage
    // first when it has not been triggered yet.
    const uint64_t request_id =
        render_coordinator_->requestCatalogueData(node_id);
    pending_catalogue_requests_[request_id] = node_id;

    ORC_LOG_DEBUG("Requested catalogue for node '{}', request_id={}",
                  node_id.to_string(), request_id);
    return;
  }

  // Default path: Generic analysis dialog for all other tools
  // This handles tools like Frame Corruption Generator using auto-generated UI

  // Create an AnalysisPresenter to get tool parameters and run the analysis
  auto* analysis_presenter = new orc::presenters::AnalysisPresenter(
      project_.presenter()->getCoreProjectHandle());

  // Get tool info to verify it exists
  const auto tool_info_full = analysis_presenter->getToolInfo(tool_info.id);
  if (tool_info_full.name.empty()) {
    ORC_LOG_WARN("Analysis tool '{}' (id='{}') not found", tool_info.name,
                 tool_info.id);
    QMessageBox::warning(this, "Analysis Tool Not Found",
                         QString("The analysis tool '%1' was not found.")
                             .arg(QString::fromStdString(tool_info.name)));
    delete analysis_presenter;
    return;
  }

  // Create and show generic analysis dialog
  auto* dialog = new orc::gui::GenericAnalysisDialog(
      tool_info.id, tool_info_full, analysis_presenter, node_id,
      project_.presenter()->getCoreProjectHandle(), this);
  dialog->setAttribute(Qt::WA_DeleteOnClose, true);

  // Connect apply signal to actually apply results to the stage
  connect(
      dialog, &orc::gui::GenericAnalysisDialog::applyResultsRequested,
      [this, tool_info_id = tool_info.id, node_id,
       analysis_presenter](const orc::AnalysisResult& result) {
        ORC_LOG_DEBUG("Applying analysis results from tool '{}' to node '{}'",
                      tool_info_id, node_id.to_string());

        try {
          // Specialized presenters have already applied results to the graph
          // via applyResultToGraph() MainWindow just needs to update the UI

          // Rebuild DAG and update preview to reflect changes
          project_.rebuildDAG();
          updatePreviewRenderer();
          dag_model_->refresh();
          updatePreview();

          statusBar()->showMessage(
              QString("Applied analysis results from '%1' to node '%2'")
                  .arg(QString::fromStdString(tool_info_id))
                  .arg(QString::fromStdString(node_id.to_string())),
              5000);
          QMessageBox::information(
              this, "Results Applied",
              "Analysis results have been successfully applied to the stage.");
        } catch (const std::exception& e) {
          ORC_LOG_ERROR("Failed to apply analysis results: {}", e.what());
          QMessageBox::warning(
              this, "Apply Failed",
              QString("Failed to apply results: %1").arg(e.what()));
        }
      });

  dialog->show();
  dialog->raise();
  dialog->activateWindow();
}

QProgressDialog* MainWindow::createAnalysisProgressDialog(
    const QString& title, const QString& message,
    QPointer<QProgressDialog>& existingDialog) {
  if (existingDialog) {
    delete existingDialog;
  }

  auto* dialog = new QProgressDialog(message, QString(), 0, 100, this);
  dialog->setWindowTitle(title);
  dialog->setWindowModality(Qt::ApplicationModal);
  dialog->setMinimumDuration(0);
  dialog->setCancelButton(nullptr);  // No cancel for now
  dialog->setValue(0);
  // No activateWindow(): requesting activation on a short-lived modal leaves
  // the GNOME/Wayland startup "busy" cursor spinner stuck after it closes.
  dialog->setAttribute(Qt::WA_ShowWithoutActivating);
  dialog->show();
  dialog->raise();

  existingDialog = dialog;
  return dialog;
}

void MainWindow::onShowVBIDialog() {
  if (!vbi_dialog_) {
    return;
  }

  // Show the dialog first
  vbi_dialog_->show();
  vbi_dialog_->raise();
  vbi_dialog_->activateWindow();

  // Update VBI information after showing
  updateVBIDialog();
}

void MainWindow::onShowVideoParameterObserverDialog() {
  if (!video_parameter_observer_dialog_) {
    return;
  }

  video_parameter_observer_dialog_->show();
  video_parameter_observer_dialog_->raise();
  video_parameter_observer_dialog_->activateWindow();

  updateVideoParameterObserverDialog();
}

void MainWindow::onShowNtscObserverDialog() {
  if (!ntsc_observer_dialog_) {
    return;
  }

  // Show the dialog first
  ntsc_observer_dialog_->show();
  ntsc_observer_dialog_->raise();
  ntsc_observer_dialog_->activateWindow();

  // Update NTSC observer information after showing
  updateNtscObserverDialog();
}

void MainWindow::onShowClosedCaptionDialog() {
  if (!closed_caption_dialog_) {
    return;
  }

  // Show the dialog first
  closed_caption_dialog_->show();
  closed_caption_dialog_->raise();
  closed_caption_dialog_->activateWindow();

  // Update closed caption information after showing
  updateClosedCaptionDialog();
}

void MainWindow::onFrameTimingRequested() {
  if (!current_view_node_id_.is_valid()) {
    ORC_LOG_WARN("No node selected for field timing view");
    return;
  }

  if (!preview_dialog_ ||
      !preview_dialog_->hasAvailablePreviewView(kFrameTimingViewId)) {
    ORC_LOG_DEBUG("Field timing view is not available for the selected stage");
    return;
  }

  // Request field timing data for current preview frame/field
  int current_index = preview_dialog_->previewSlider()->value();
  pending_frame_timing_request_id_ =
      render_coordinator_->requestFrameTimingData(
          current_view_node_id_, current_output_type_, current_index);

  ORC_LOG_DEBUG("Requested field timing data (request_id={})",
                pending_frame_timing_request_id_);
}

void MainWindow::onFrameScopeDialogClosed() {
  ORC_LOG_DEBUG(
      "onFrameScopeDialogClosed: resetting line scope state (was line={}, "
      "field={})",
      last_line_scope_line_number_, last_line_scope_field_index_);
  // Clear the marker state when line scope is closed
  last_line_scope_field_index_ = std::numeric_limits<uint64_t>::max();
  last_line_scope_line_number_ = -1;
  last_line_scope_image_x_ = -1;
  last_line_scope_image_y_ = -1;

  // Update field timing dialog if it's visible to remove the marker
  auto* frame_timing_dialog = preview_dialog_->frameTimingDialog();
  if (frame_timing_dialog && frame_timing_dialog->isVisible()) {
    onFrameTimingRequested();
  }
}

void MainWindow::onSetCrosshairsFromFrameTiming() {
  auto* frame_timing_dialog = preview_dialog_->frameTimingDialog();
  if (!frame_timing_dialog) {
    return;
  }

  // Get video parameters to convert sample to field/line/x
  std::optional<orc::presenters::VideoParametersView> video_params;
  if (current_view_node_id_.is_valid()) {
    auto* core_project = project_.presenter()->getCoreProjectHandle();
    if (core_project) {
      orc::presenters::RenderPresenter render_presenter(core_project);
      // Throwaway helper presenter: rendering/parameter reads only — no
      // sidecar, scheduler, or sweeps (construction must stay cheap on the
      // GUI thread).
      render_presenter.setBackgroundObservationEnabled(false);
      render_presenter.setDAG(project_.getDAG());
      auto vp = render_presenter.getVideoParameters(current_view_node_id_);
      if (vp.has_value()) {
        video_params = orc::presenters::toVideoParametersView(*vp);
      }
    }
  }

  if (!video_params.has_value() || video_params->frame_width_nominal <= 0) {
    ORC_LOG_WARN("No video parameters available for set crosshairs");
    return;
  }

  // Get center sample from timing widget
  int center_sample = frame_timing_dialog->timingWidget()->getCenterSample();
  if (center_sample < 0) {
    ORC_LOG_WARN("No valid center sample position");
    return;
  }

  const int fw = video_params->frame_width_nominal;

  // Get the actual field heights from the dialog (these come from VFR
  // descriptors) This is important for PAL where first field = 312 lines,
  // second field = 313 lines
  const int first_fh = frame_timing_dialog->firstFieldHeight();
  const int second_fh = frame_timing_dialog->secondFieldHeight();

  if (first_fh == 0 || fw == 0) {
    ORC_LOG_WARN("Invalid field dimensions");
    return;
  }

  // Determine which field the sample is in
  int samples_per_first_field = fw * first_fh;
  uint64_t field_offset = 0;
  int sample_in_field = center_sample;
  int field_height_for_sample = first_fh;

  // Check if we're showing two fields (frame mode)
  if (current_output_type_ == orc::PreviewOutputType::Frame_Field1_First ||
      current_output_type_ == orc::PreviewOutputType::Frame_Reversed ||
      current_output_type_ == orc::PreviewOutputType::Split) {
    // Determine which field based on sample position
    int frame_index = preview_dialog_->previewSlider()->value();
    uint64_t field1 = static_cast<uint64_t>(frame_index) * 2;
    uint64_t field2 = frame_index * 2 + 1;

    if (center_sample >= samples_per_first_field) {
      // In second field
      field_offset = field2;
      sample_in_field = center_sample - samples_per_first_field;
      field_height_for_sample = second_fh > 0 ? second_fh : first_fh;
    } else {
      // In first field
      field_offset = field1;
      sample_in_field = center_sample;
      field_height_for_sample = first_fh;
    }
  } else {
    // Single field mode
    field_offset = preview_dialog_->previewSlider()->value();
    field_height_for_sample = first_fh;
  }

  // Convert sample position to line and x
  int line_number = sample_in_field / fw;
  int sample_x = sample_in_field % fw;

  // Clamp to valid range using the actual field height for this specific field
  if (line_number >= field_height_for_sample) {
    line_number = field_height_for_sample - 1;
  }
  if (sample_x >= fw) sample_x = fw - 1;

  ORC_LOG_DEBUG(
      "Setting crosshairs from field timing: center_sample={}, field={}, "
      "line={}, x={}",
      center_sample, field_offset, line_number, sample_x);

  // Map field/line to image coordinates
  int image_height =
      preview_dialog_->previewWidget()->originalImageSize().height();
  auto mapping = render_coordinator_->mapFieldToImage(
      current_view_node_id_, current_output_type_,
      preview_dialog_->previewSlider()->value(), field_offset, line_number,
      image_height, current_option_id_);

  if (!mapping.is_valid) {
    ORC_LOG_WARN("Failed to map field/line to image coordinates");
    return;
  }

  // Request line samples at this position (which will set crosshairs)
  onLineScopeRequested(sample_x, mapping.image_y);
}

void MainWindow::onFrameTimingDataReady(
    uint64_t request_id, uint64_t field_index,
    std::optional<uint64_t> field_index_2, std::vector<int16_t> samples,
    std::vector<int16_t> samples_2, std::vector<int16_t> y_samples,
    std::vector<int16_t> c_samples, std::vector<int16_t> y_samples_2,
    std::vector<int16_t> c_samples_2, int first_field_height,
    int second_field_height) {
  Q_UNUSED(request_id);

  ORC_LOG_DEBUG(
      "Field timing data ready: field {}{}, {} composite samples, {} Y "
      "samples, {} C samples",
      field_index,
      field_index_2.has_value() ? " + " + std::to_string(field_index_2.value())
                                : "",
      samples.size(), y_samples.size(), c_samples.size());

  auto* frame_timing_dialog = preview_dialog_->frameTimingDialog();
  if (!frame_timing_dialog) {
    ORC_LOG_WARN("No field timing dialog available!");
    return;
  }

  // Get video parameters for mV conversion
  std::optional<orc::presenters::VideoParametersView> video_params;
  if (current_view_node_id_.is_valid()) {
    // Create temporary render presenter to get video parameters
    auto* core_project = project_.presenter()->getCoreProjectHandle();
    if (core_project) {
      orc::presenters::RenderPresenter render_presenter(core_project);
      // Throwaway helper presenter: rendering/parameter reads only — no
      // sidecar, scheduler, or sweeps (construction must stay cheap on the
      // GUI thread).
      render_presenter.setBackgroundObservationEnabled(false);
      render_presenter.setDAG(project_.getDAG());

      auto vp = render_presenter.getVideoParameters(current_view_node_id_);
      if (vp.has_value()) {
        // Convert to view model
        video_params = orc::presenters::toVideoParametersView(*vp);
      }
    }
  }

  // Set the field data and show the dialog
  std::optional<int> marker_sample;
  if (video_params.has_value() && video_params->frame_width_nominal > 0 &&
      last_line_scope_field_index_ != std::numeric_limits<uint64_t>::max() &&
      last_line_scope_image_x_ >= 0 && last_line_scope_line_number_ >= 0) {
    const int fw = video_params->frame_width_nominal;
    // Derive the fallback field height from the video system.
    const auto vp_sys = [&]() -> orc::VideoSystem {
      switch (video_params->system) {
        case orc::presenters::VideoSystem::PAL:
          return orc::VideoSystem::PAL;
        case orc::presenters::VideoSystem::NTSC:
          return orc::VideoSystem::NTSC;
        case orc::presenters::VideoSystem::PAL_M:
          return orc::VideoSystem::PAL_M;
        default:
          return orc::VideoSystem::Unknown;
      }
    }();
    const int fallback_fh =
        static_cast<int>(orc::calculate_padded_field_height(vp_sys));
    const int first_fh =
        (first_field_height > 0) ? first_field_height : fallback_fh;
    const int second_fh =
        (second_field_height > 0) ? second_field_height : fallback_fh;

    ORC_LOG_DEBUG(
        "Field timing marker calculation: last_field={}, last_line={}, "
        "last_x={}, field_index={}, field_index_2={}",
        last_line_scope_field_index_, last_line_scope_line_number_,
        last_line_scope_image_x_, field_index,
        field_index_2.has_value() ? field_index_2.value() : 0);

    int clamped_x = std::max(0, std::min(last_line_scope_image_x_, fw - 1));

    // Determine which field in the timing data this maps to
    uint64_t f1 = field_index;
    std::optional<uint64_t> f2 = field_index_2;
    if (last_line_scope_field_index_ == f1) {
      if (last_line_scope_line_number_ < first_fh) {
        int local_sample = last_line_scope_line_number_ * fw + clamped_x;
        marker_sample = local_sample;
        ORC_LOG_DEBUG("Marker in field 1 at sample {}", local_sample);
      }
    } else if (f2.has_value() && last_line_scope_field_index_ == f2.value()) {
      if (last_line_scope_line_number_ < second_fh) {
        int local_sample = last_line_scope_line_number_ * fw + clamped_x;
        marker_sample =
            local_sample + fw * first_fh;  // offset by actual first field size
        ORC_LOG_DEBUG("Marker in field 2 at sample {} (offset {})",
                      marker_sample.value(), local_sample);
      }
    } else {
      ORC_LOG_DEBUG("Marker field {} doesn't match f1={} or f2={}",
                    last_line_scope_field_index_, f1,
                    f2.has_value() ? f2.value() : 0);
    }
  }

  frame_timing_dialog->setFieldData(
      QString::fromStdString(current_view_node_id_.to_string()), field_index,
      samples, field_index_2, samples_2, y_samples, c_samples, y_samples_2,
      c_samples_2, video_params, marker_sample, first_field_height,
      second_field_height);

  // Only show/raise/activate if not already visible, the parent preview
  // dialog is still open, and the user still wants this dialog open. Guards
  // against pending async callbacks re-opening the dialog after the user has
  // closed either it or the preview window.
  if (!frame_timing_dialog->isVisible() && preview_dialog_->isVisible() &&
      preview_dialog_->isFrameTimingOpenRequested()) {
    frame_timing_dialog->show();
    frame_timing_dialog->raise();
    frame_timing_dialog->activateWindow();
  }
}

void MainWindow::onWaveformMonitorRequested() {
  if (!current_view_node_id_.is_valid()) {
    ORC_LOG_WARN("No node selected for waveform monitor view");
    return;
  }

  if (!preview_dialog_ ||
      !preview_dialog_->hasAvailablePreviewView(kWaveformMonitorViewId)) {
    ORC_LOG_DEBUG("Waveform monitor is not available for the selected stage");
    return;
  }

  const int current_index = preview_dialog_->previewSlider()->value();
  pending_waveform_monitor_request_id_ =
      render_coordinator_->requestWaveformMonitorData(
          current_view_node_id_, current_output_type_, current_index);

  ORC_LOG_DEBUG("Requested waveform monitor data (request_id={})",
                pending_waveform_monitor_request_id_);
}

void MainWindow::onWaveformMonitorDataReady(
    uint64_t request_id, std::vector<int16_t> composite_samples,
    std::vector<int16_t> y_samples, std::vector<int16_t> c_samples,
    int first_field_height, int second_field_height) {
  Q_UNUSED(request_id);

  ORC_LOG_DEBUG(
      "Waveform monitor data ready: {} composite samples, {} Y samples, {} C "
      "samples",
      composite_samples.size(), y_samples.size(), c_samples.size());

  auto* wm_dialog = preview_dialog_->waveformMonitorDialog();
  if (!wm_dialog) {
    ORC_LOG_WARN("No waveform monitor dialog available!");
    return;
  }

  // Get video parameters for mV conversion and level markers
  std::optional<orc::presenters::VideoParametersView> video_params;
  if (current_view_node_id_.is_valid()) {
    auto* core_project = project_.presenter()->getCoreProjectHandle();
    if (core_project) {
      orc::presenters::RenderPresenter render_presenter(core_project);
      // Throwaway helper presenter: rendering/parameter reads only — no
      // sidecar, scheduler, or sweeps (construction must stay cheap on the
      // GUI thread).
      render_presenter.setBackgroundObservationEnabled(false);
      render_presenter.setDAG(project_.getDAG());
      auto vp = render_presenter.getVideoParameters(current_view_node_id_);
      if (vp.has_value()) {
        video_params = orc::presenters::toVideoParametersView(*vp);
      }
    }
  }

  wm_dialog->setData(std::move(composite_samples), std::move(y_samples),
                     std::move(c_samples), first_field_height,
                     second_field_height, video_params);

  // Only show/raise/activate if not already visible, the parent preview
  // dialog is still open, and the user still wants this dialog open. Guards
  // against pending async callbacks re-opening the dialog after the user has
  // closed either it or the preview window.
  if (!wm_dialog->isVisible() && preview_dialog_->isVisible() &&
      preview_dialog_->isWaveformMonitorOpenRequested()) {
    wm_dialog->show();
    wm_dialog->raise();
    wm_dialog->activateWindow();
  }
}

// Vectorscope is preview-owned and refreshed through the registry request
// contract.

void MainWindow::beginPreviewRenderInFlight() {
  preview_render_in_flight_ = true;
  render_slow_timer_->start();
}

void MainWindow::endPreviewRenderInFlight() {
  render_slow_timer_->stop();
  if (preview_dialog_) {
    preview_dialog_->setWindowTitle("Field/Frame Preview");
  }
  preview_render_in_flight_ = false;
}

void MainWindow::beginProjectLoadProgress() {
  // A load already in progress keeps its dialog and its elapsed time.
  if (project_load_in_progress_) {
    return;
  }

  project_load_in_progress_ = true;
  project_load_stage_label_.clear();
  project_load_current_ = 0;
  project_load_total_ = 0;
  project_load_elapsed_.start();

  // No cancel button: the wait is a stage executing inside the worker thread
  // (typically a source opening its metadata) and nothing in that path is
  // interruptible, so offering Cancel would be a lie. Created hidden — the show
  // timer decides whether the load lasts long enough to warrant a dialog.
  auto* dialog = new QProgressDialog("Preparing preview\xE2\x80\xA6", QString(),
                                     0, 0, this);
  dialog->setWindowTitle("Opening Project");
  dialog->setWindowModality(Qt::ApplicationModal);
  dialog->setCancelButton(nullptr);
  dialog->setAutoClose(false);
  dialog->setAutoReset(false);
  dialog->setAttribute(Qt::WA_DeleteOnClose, false);
  // QProgressDialog puts itself on screen from an internal timer once
  // minimumDuration elapses, which would race project_load_show_timer_ and can
  // flash the dialog on loads that finish immediately. Push that duration out
  // of reach and reset() (which stops the internal timer) so our own timer is
  // the only thing that can show it.
  dialog->setMinimumDuration(std::numeric_limits<int>::max());
  dialog->reset();
  dialog->hide();
  project_load_progress_dialog_ = dialog;

  project_load_show_timer_->start();
}

void MainWindow::updateProjectLoadProgressLabel() {
  if (!project_load_in_progress_ || !project_load_progress_dialog_) {
    return;
  }
  const int elapsed_seconds =
      static_cast<int>(project_load_elapsed_.elapsed() / 1000);
  const std::string text = orc::gui::formatProjectLoadStatus(
      project_load_stage_label_.toStdString(), project_load_current_,
      project_load_total_, elapsed_seconds);
  // setLabelText() on a modal QProgressDialog can re-enter the event loop, so
  // re-check the pointer before touching the dialog again elsewhere.
  project_load_progress_dialog_->setLabelText(QString::fromStdString(text));
}

void MainWindow::endProjectLoadProgress() {
  if (!project_load_in_progress_) {
    return;
  }
  project_load_in_progress_ = false;
  project_load_show_timer_->stop();
  project_load_tick_timer_->stop();

  ORC_LOG_DEBUG("Project load preparation finished after {} ms",
                project_load_elapsed_.elapsed());

  // Clear the member before closing: a modal dialog's close can re-enter the
  // event loop and deliver another coordinator response, which would otherwise
  // find a half-torn-down dialog (the crash pattern documented on the analysis
  // progress dialogs).
  QProgressDialog* dialog = project_load_progress_dialog_.data();
  project_load_progress_dialog_.clear();
  if (dialog) {
    dialog->blockSignals(true);
    dialog->hide();
    dialog->deleteLater();
  }
}

void MainWindow::updateAllPreviewComponents() {
  updatePreview();
  updatePreviewInfo();
  // NOTE: Do NOT call updateLineScope() here. The line scope should maintain
  // its current field/line across frame changes. Automatic refresh would cause
  // the displayed line to jump because the mapping from image_y to field
  // coordinates changes between frames. The line scope is only updated when:
  // 1. User clicks on the preview to select a new line
  // 2. User navigates up/down with line scope buttons
  // 3. Line scope is initially opened
  // 4. Frame changes - line scope updates via its own connection to
  // previewFrameChanged signal
  updateVBIDialog();
  updateVideoParameterObserverDialog();
  updateNtscObserverDialog();
  updateClosedCaptionDialog();

  // Notify line scope dialog that preview frame has changed
  // Line scope will refresh samples at its current field/line position via
  // orc-core
  if (preview_dialog_) {
    ORC_LOG_DEBUG("updateAllPreviewComponents: calling notifyFrameChanged");
    preview_dialog_->notifyFrameChanged();
  }

  // For analysis dialogs, only update the frame marker position (not the full
  // data) Full data is only loaded when the dialog is first opened
  int32_t current_frame = 1;
  if (preview_dialog_ && preview_dialog_->previewSlider()) {
    current_frame =
        static_cast<int32_t>(preview_dialog_->previewSlider()->value()) + 1;
  }

  // Update all visible dropout analysis dialogs
  for (auto& pair : dropout_analysis_dialogs_) {
    if (pair.second && pair.second->isVisible()) {
      pair.second->updateFrameMarker(current_frame);
    }
  }

  // Update all visible SNR analysis dialogs
  for (auto& pair : snr_analysis_dialogs_) {
    if (pair.second && pair.second->isVisible()) {
      pair.second->updateFrameMarker(current_frame);
    }
  }

  // Update all visible burst level analysis dialogs
  for (auto& pair : burst_level_analysis_dialogs_) {
    if (pair.second && pair.second->isVisible()) {
      pair.second->updateFrameMarker(current_frame);
    }
  }
}

void MainWindow::updateVBIDialog() {
  // Only update if VBI dialog is visible
  if (!vbi_dialog_ || !vbi_dialog_->isVisible()) {
    return;
  }

  // Get current field being displayed
  if (!current_view_node_id_.is_valid()) {
    vbi_dialog_->clearVBIInfo();
    return;
  }

  // Get the current index from the preview slider
  int current_index = preview_dialog_->previewSlider()->value();

  // Check if we're in frame mode (any mode that shows two fields)
  bool is_frame_mode =
      (current_output_type_ == orc::PreviewOutputType::Frame_Field1_First ||
       current_output_type_ == orc::PreviewOutputType::Frame_Reversed ||
       current_output_type_ == orc::PreviewOutputType::Split);

  // Request VBI data from coordinator
  if (is_frame_mode) {
    // Frame mode - get field IDs from core library (handles field ordering)
    auto frame_fields = render_coordinator_->getFrameFields(
        current_view_node_id_, current_index);
    if (!frame_fields.is_valid) {
      vbi_dialog_->clearVBIInfo();
      return;
    }
    orc::FieldID field1_id(frame_fields.first_field);
    orc::FieldID field2_id(frame_fields.second_field);
    // Request both fields - VBI interpretation requires data from both fields
    // (e.g., CLV timecode may be split across fields). Newly issued ids
    // supersede any in-flight ones, whose responses are dropped as stale in
    // onVBIDataReady().
    pending_vbi_is_frame_mode_ = true;
    pending_vbi_field1_ready_ = false;
    pending_vbi_field2_ready_ = false;
    pending_vbi_request_id_field1_ =
        render_coordinator_->requestVBIData(current_view_node_id_, field1_id);
    pending_vbi_request_id_field2_ =
        render_coordinator_->requestVBIData(current_view_node_id_, field2_id);
  } else {
    // Field mode - request single field
    pending_vbi_is_frame_mode_ = false;
    pending_vbi_field1_ready_ = false;
    pending_vbi_field2_ready_ = false;
    orc::FieldID field_id(current_index);
    pending_vbi_request_id_field1_ =
        render_coordinator_->requestVBIData(current_view_node_id_, field_id);
    pending_vbi_request_id_field2_ = 0;
  }
}

void MainWindow::updateVideoParameterObserverDialog() {
  // Phase 5: observation data is fetched asynchronously through the coordinator
  // (no synchronous render on the UI thread). Both observer dialogs share one
  // request flow; the response updates whichever dialog is visible.
  refreshObserverDialogs();
}

void MainWindow::updateNtscObserverDialog() {
  // Phase 5: see updateVideoParameterObserverDialog(). Both observer dialogs
  // share the same async request flow.
  refreshObserverDialogs();
}

void MainWindow::refreshObserverDialogs() {
  const bool vp_visible = video_parameter_observer_dialog_ &&
                          video_parameter_observer_dialog_->isVisible();
  const bool ntsc_visible =
      ntsc_observer_dialog_ && ntsc_observer_dialog_->isVisible();
  if (!vp_visible && !ntsc_visible) {
    return;
  }

  if (!current_view_node_id_.is_valid()) {
    if (vp_visible) {
      video_parameter_observer_dialog_->clearObservations();
    }
    if (ntsc_visible) {
      ntsc_observer_dialog_->clearObservations();
    }
    return;
  }

  const int current_index = preview_dialog_->previewSlider()->value();
  const bool is_frame_mode =
      (current_output_type_ == orc::PreviewOutputType::Frame_Field1_First ||
       current_output_type_ == orc::PreviewOutputType::Frame_Reversed ||
       current_output_type_ == orc::PreviewOutputType::Split);

  orc::FieldID field1_id;
  orc::FieldID field2_id;
  if (is_frame_mode) {
    auto frame_fields = render_coordinator_->getFrameFields(
        current_view_node_id_, current_index);
    if (!frame_fields.is_valid) {
      if (vp_visible) {
        video_parameter_observer_dialog_->clearObservations();
      }
      if (ntsc_visible) {
        ntsc_observer_dialog_->clearObservations();
      }
      return;
    }
    field1_id = orc::FieldID(frame_fields.first_field);
    field2_id = orc::FieldID(frame_fields.second_field);
  } else {
    field1_id = orc::FieldID(current_index);
  }

  // Reset the per-frame combine state and issue the async request(s). Newly
  // issued ids supersede any in-flight ones, whose responses are dropped as
  // stale in onObservationDataReady().
  pending_obs_frame_mode_ = is_frame_mode;
  pending_obs_field1_id_ = field1_id;
  pending_obs_field2_id_ = field2_id;
  pending_obs_field1_ready_ = false;
  pending_obs_field2_ready_ = false;

  if (vp_visible) {
    video_parameter_observer_dialog_->showPending();
  }
  if (ntsc_visible) {
    ntsc_observer_dialog_->showPending();
  }

  pending_obs_request_id_field1_ = render_coordinator_->requestObservations(
      current_view_node_id_, field1_id);
  if (is_frame_mode) {
    pending_obs_request_id_field2_ = render_coordinator_->requestObservations(
        current_view_node_id_, field2_id);
  } else {
    pending_obs_request_id_field2_ = 0;
  }
}

std::optional<uint64_t> MainWindow::previewFrameForObservers() const {
  const int current_index = preview_dialog_->previewSlider()->value();
  const bool is_frame_mode =
      (current_output_type_ == orc::PreviewOutputType::Frame_Field1_First ||
       current_output_type_ == orc::PreviewOutputType::Frame_Reversed ||
       current_output_type_ == orc::PreviewOutputType::Split);

  if (!is_frame_mode) {
    // Field previews index fields; one frame-keyed observation request covers
    // both fields of the parent frame.
    return static_cast<uint64_t>(current_index) / 2;
  }

  // Frame previews index frames directly; getFrameFields() validates the
  // index and accounts for field ordering.
  auto frame_fields = render_coordinator_->getFrameFields(
      current_view_node_id_, static_cast<uint64_t>(current_index));
  if (!frame_fields.is_valid) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(current_index);
}

void MainWindow::updateClosedCaptionDialog() {
  // Only update while the dialog is visible (observer-dialog convention).
  if (!closed_caption_dialog_ || !closed_caption_dialog_->isVisible()) {
    return;
  }

  if (!current_view_node_id_.is_valid()) {
    pending_closed_caption_requests_.clear();
    closed_caption_cache_node_id_ = orc::NodeID();
    closed_caption_dialog_->clearContent();
    return;
  }

  // The trailing-window caption cache only carries over while the view node is
  // unchanged; a node switch means different observations.
  if (closed_caption_cache_node_id_ != current_view_node_id_) {
    closed_caption_cache_node_id_ = current_view_node_id_;
    pending_closed_caption_requests_.clear();
    closed_caption_dialog_->clearCache();
  }

  const auto current_frame = previewFrameForObservers();
  if (!current_frame) {
    closed_caption_dialog_->clearContent();
    return;
  }

  closed_caption_dialog_->setCurrentFrame(*current_frame);
  issueClosedCaptionRequests();
}

void MainWindow::issueClosedCaptionRequests() {
  if (!closed_caption_dialog_ || !current_view_node_id_.is_valid()) {
    return;
  }

  // Drop in-flight requests whose frame has left the window; keep the rest.
  // Stepping forward slides the window by one frame, so cancelling the whole
  // set each time would keep re-issuing reads that never get the chance to
  // complete. The upper bound is what the dialog will still accept rather than
  // where the previewer is: a backward step does not make a read already in
  // flight useless.
  const uint64_t window_start = closed_caption_dialog_->windowStartFrame();
  const uint64_t retained_limit = closed_caption_dialog_->retainedFrameLimit();
  std::unordered_set<uint64_t> frames_in_flight;
  for (auto it = pending_closed_caption_requests_.begin();
       it != pending_closed_caption_requests_.end();) {
    if (it->second < window_start || it->second > retained_limit) {
      it = pending_closed_caption_requests_.erase(it);
    } else {
      frames_in_flight.insert(it->second);
      ++it;
    }
  }

  // Request only the window frames the dialog holds no caption data for and
  // has no outstanding request for. The dialog hands these out a batch at a
  // time; each delivery calls back here for the next batch.
  const auto needed_frames = closed_caption_dialog_->framesNeedingData();
  closed_caption_dialog_->showPending();
  for (const uint64_t frame : needed_frames) {
    if (frames_in_flight.count(frame) != 0) {
      continue;
    }
    const uint64_t request_id = render_coordinator_->requestClosedCaptionData(
        current_view_node_id_, orc::FieldID(frame * 2));
    pending_closed_caption_requests_.emplace(request_id, frame);
  }
}

void MainWindow::onLineScopeRequested(int image_x, int image_y) {
  ORC_LOG_DEBUG("Line scope requested at image position ({}, {})", image_x,
                image_y);

  if (!current_view_node_id_.is_valid()) {
    ORC_LOG_WARN("No node selected for line scope");
    return;
  }

  if (!preview_dialog_ ||
      !preview_dialog_->hasAvailablePreviewView(kLineScopeViewId)) {
    ORC_LOG_DEBUG("Line scope view is not available for the selected stage");
    return;
  }

  // Store original image x coordinate for later use (to avoid rounding errors)
  last_line_scope_image_x_ = image_x;
  last_line_scope_image_y_ = image_y;

  // Get the preview image height for split mode
  int image_height =
      preview_dialog_->previewWidget()->originalImageSize().height();

  // Use orc-core to map image coordinates to field coordinates
  // This ensures the GUI doesn't duplicate field ordering logic
  auto mapping = render_coordinator_->mapImageToField(
      current_view_node_id_, current_output_type_,
      preview_dialog_->previewSlider()->value(), image_y, image_height,
      current_option_id_);

  if (!mapping.is_valid) {
    ORC_LOG_WARN("Failed to map image coordinates to field");
    return;
  }

  uint64_t field_index = mapping.field_index;
  // Core API returns 0-based field line (0 to field_height-1)
  // Convert to 1-based for display and helper functions (1 to field_height)
  int field_line_0based = mapping.field_line;
  int field_line_1based = field_line_0based + 1;

  // Store the clicked field/line immediately so that
  // onLineScopeRefreshAtFieldLine can use the correct position before the async
  // response arrives. Responses must not overwrite these — only user actions
  // (click or up/down) should.
  last_line_scope_field_index_ = field_index;
  last_line_scope_line_number_ = field_line_0based;

  orc::PreviewCoordinate coordinate;
  coordinate.field_index = field_index;
  coordinate.line_index = static_cast<uint32_t>(std::max(field_line_0based, 0));
  coordinate.sample_offset = static_cast<uint32_t>(std::max(image_x, 0));
  coordinate.data_type_context = inferCurrentVideoDataType();
  preview_dialog_->setSharedPreviewCoordinate(coordinate);

  // Map image_x from preview image coordinates to field sample coordinates
  // The preview widget gives us coordinates in the rendered RGB image space,
  // but the field data may have a different width (no aspect ratio correction
  // in samples) We need to get the original field width to do proper mapping
  // For now, we'll pass image_x and let the backend handle clamping
  // TODO(sdi): Get actual field descriptor to properly map coordinates
  int sample_x = image_x;

  ORC_LOG_DEBUG(
      "Requesting line samples for field {}, line {} (0-based: {}), sample_x "
      "{}",
      field_index, field_line_1based, field_line_0based, sample_x);

  // Get the preview image width for coordinate mapping
  int preview_image_width =
      preview_dialog_->previewWidget()->originalImageSize().width();

  // Request line samples from the coordinator using Field output type
  // Note: Core API expects 0-based line numbers
  pending_line_sample_request_id_ = render_coordinator_->requestLineSamples(
      current_view_node_id_, orc::PreviewOutputType::Frame_Field1, field_index,
      field_line_0based,  // Core API uses 0-based
      sample_x, preview_image_width);
}

void MainWindow::onLineSamplesReady(
    uint64_t request_id, uint64_t field_index, int line_number, int sample_x,
    std::vector<int16_t> samples,
    std::optional<orc::SourceParameters> video_params,
    std::vector<int16_t> y_samples, std::vector<int16_t> c_samples) {
  Q_UNUSED(request_id);

  // Core API returns 0-based line numbers, convert to 1-based for display
  int line_number_0based = line_number;
  int line_number_1based = line_number_0based + 1;

  ORC_LOG_DEBUG(
      "Line samples ready: {} samples for field {}, line {} (0-based: {}), "
      "sample_x={} (YC: Y={}, C={}) mode={}",
      samples.size(), field_index, line_number_1based, line_number_0based,
      sample_x, y_samples.size(), c_samples.size(),
      static_cast<int>(current_output_type_));

  // Convert public API SourceParameters to presenter VideoParametersView for
  // dialogs
  std::optional<orc::presenters::VideoParametersView> view_params;
  if (video_params.has_value()) {
    view_params = orc::presenters::toVideoParametersView(video_params.value());
  }

  if (!preview_dialog_) {
    ORC_LOG_WARN("No preview dialog available!");
    return;
  }

  // Get preview image width for coordinate mapping
  int preview_image_width =
      preview_dialog_->previewWidget()->originalImageSize().width();
  int preview_image_height =
      preview_dialog_->previewWidget()->originalImageSize().height();

  // NOTE: last_line_scope_field_index_ and last_line_scope_line_number_ are
  // intentionally NOT updated here. They represent user intent (set by click
  // or up/down navigation) and must not be overwritten by async responses,
  // which would cause in-flight refresh responses to cancel navigation during
  // playback.

  // Store context for sample marker updates
  last_line_scope_preview_width_ = preview_image_width;
  last_line_scope_samples_count_ = static_cast<int>(samples.size());
  if (!samples.empty()) {
    // Use composite samples if available
  } else if (!y_samples.empty()) {
    // Use Y samples for size if no composite
    last_line_scope_samples_count_ = static_cast<int>(y_samples.size());
  }

  uint64_t current_index = preview_dialog_->previewSlider()->value();

  // The response's field_index may be from a past frame when the decoder is
  // slow. Resolve to the current frame's matching-parity field so that
  // mapFieldToImage always receives a field belonging to current_index.
  uint64_t effective_field_for_mapping = field_index;
  if (current_output_type_ == orc::PreviewOutputType::Frame_Field1_First ||
      current_output_type_ == orc::PreviewOutputType::Frame_Reversed) {
    auto frame_fields = render_coordinator_->getFrameFields(
        current_view_node_id_, current_index);
    if (frame_fields.is_valid) {
      bool is_odd = (field_index % 2) == 1;
      bool first_is_odd = (frame_fields.first_field % 2) == 1;
      effective_field_for_mapping = (is_odd == first_is_odd)
                                        ? frame_fields.first_field
                                        : frame_fields.second_field;
    }
  } else if (current_output_type_ == orc::PreviewOutputType::Frame_Field1 ||
             current_output_type_ == orc::PreviewOutputType::Frame_Field2) {
    effective_field_for_mapping = current_index;
  } else if (current_output_type_ == orc::PreviewOutputType::Split) {
    bool is_odd = (field_index % 2) == 1;
    effective_field_for_mapping =
        is_odd ? (current_index * 2 + 1) : (current_index * 2);
  }

  auto mapping = render_coordinator_->mapFieldToImage(
      current_view_node_id_, current_output_type_, current_index,
      effective_field_for_mapping, line_number_0based, preview_image_height,
      current_option_id_);

  int image_y = 0;
  if (mapping.is_valid) {
    image_y = mapping.image_y;
    preview_dialog_->previewWidget()->updateCrosshairsPosition(
        last_line_scope_image_x_, image_y);
    last_line_scope_image_y_ = image_y;
  } else {
    ORC_LOG_WARN(
        "Failed to map field coordinates to image - not updating cross-hairs "
        "to avoid jumping");
    image_y = line_number_0based;
  }

  int original_sample_x = last_line_scope_image_x_;
  int sample_index = sample_x;
  int calculated_image_y = image_y;

  // Store field/line for later navigation (already stored above)
  // last_line_scope_field_index_ = field_index;
  // last_line_scope_line_number_ = line_number_0based;

  orc::PreviewCoordinate coordinate;
  coordinate.field_index = field_index;
  coordinate.line_index =
      static_cast<uint32_t>(std::max(line_number_0based, 0));
  coordinate.sample_offset = static_cast<uint32_t>(std::max(sample_index, 0));
  coordinate.data_type_context = inferCurrentVideoDataType();
  preview_dialog_->setSharedPreviewCoordinate(coordinate);

  // Show the line scope dialog with the samples, including the current node_id
  // Pass 1-based line number for display
  QString node_id_str =
      QString::fromStdString(current_view_node_id_.to_string());

  // Calculate stage index (1-based) from the current node
  int stage_index = 1;
  const auto nodes = project_.presenter()->getNodes();
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].node_id == current_view_node_id_) {
      stage_index = static_cast<int>(i) + 1;  // Convert to 1-based
      break;
    }
  }

  const bool scope_was_visible = preview_dialog_->isLineScopeVisible();
  preview_dialog_->showLineScope(
      node_id_str, stage_index, field_index, line_number_1based, sample_index,
      samples, view_params, preview_image_width, original_sample_x,
      calculated_image_y, current_output_type_, y_samples, c_samples);

  // If the dialog just became visible, navigation may have occurred while the
  // initial line samples were in-flight (onLineScopeRefreshAtFieldLine skips
  // when the dialog is not yet visible). Trigger a refresh now so the scope
  // shows the current frame rather than the frame that was clicked on.
  if (!scope_was_visible && preview_dialog_->isLineScopeVisible()) {
    onLineScopeRefreshAtFieldLine();
  }

  // Update field timing dialog if it's visible (to update the marker position)
  if (preview_dialog_->frameTimingDialog() &&
      preview_dialog_->frameTimingDialog()->isVisible()) {
    ORC_LOG_DEBUG("Refreshing field timing dialog to update marker position");
    onFrameTimingRequested();
  }

  refreshVectorscopeForCurrentCoordinate();
  refreshHistogramForCurrentCoordinate();
}

void MainWindow::onFrameLineNavigationReady(
    uint64_t request_id, orc::FrameLineNavigationResult result) {
  Q_UNUSED(request_id);

  ORC_LOG_DEBUG(
      "Frame line navigation ready: valid={}, new_field={}, new_line={}",
      result.is_valid, result.new_field_index, result.new_line_number);

  if (!result.is_valid) {
    ORC_LOG_DEBUG(
        "Frame line navigation out of bounds, staying at current position");
    return;
  }

  // Request line samples at the new position using the unified helper
  ORC_LOG_DEBUG("Requesting samples at field {}, line {}",
                result.new_field_index, result.new_line_number);

  requestLineSamplesForNavigation(
      result.new_field_index, result.new_line_number, last_line_scope_image_x_,
      preview_dialog_->previewWidget()->originalImageSize().width());
}

void MainWindow::requestLineSamplesForNavigation(uint64_t field_index,
                                                 int line_number, int sample_x,
                                                 int preview_image_width) {
  // Unified helper to request line samples for navigation
  // This ensures all modes (Field, Frame, Split) handle updates consistently
  // Update stored field/line BEFORE requesting so they're available when
  // samples arrive
  ORC_LOG_DEBUG(
      "requestLineSamplesForNavigation: storing field={}, line={} (was "
      "field={}, line={})",
      field_index, line_number, last_line_scope_field_index_,
      last_line_scope_line_number_);
  last_line_scope_field_index_ = field_index;
  last_line_scope_line_number_ = line_number;

  // CRITICAL: Update last_line_scope_image_x_ so it's preserved for the
  // response This ensures the marker position is maintained across navigation
  last_line_scope_image_x_ = sample_x;

  ORC_LOG_DEBUG(
      "requestLineSamplesForNavigation: mode={}, field={}, line={}, "
      "sample_x={}, width={}",
      static_cast<int>(current_output_type_), field_index, line_number,
      sample_x, preview_image_width);

  pending_line_sample_request_id_ = render_coordinator_->requestLineSamples(
      current_view_node_id_, orc::PreviewOutputType::Frame_Field1, field_index,
      line_number, sample_x, preview_image_width);

  ORC_LOG_DEBUG("requestLineSamplesForNavigation: request_id={}",
                pending_line_sample_request_id_);
}

void MainWindow::onSampleMarkerMoved(int sample_x) {
  if (!preview_dialog_) {
    return;
  }

  // Map sample_x from field-space back to preview-space
  int preview_x = (sample_x * last_line_scope_preview_width_) /
                  last_line_scope_samples_count_;

  // Calculate image_y from stored field/line using orc-core
  int image_height =
      preview_dialog_->previewWidget()->originalImageSize().height();
  auto image_coords = render_coordinator_->mapFieldToImage(
      current_view_node_id_, current_output_type_,
      preview_dialog_->previewSlider()->value(), last_line_scope_field_index_,
      last_line_scope_line_number_, image_height, current_option_id_);

  // Use the remapped Y if available, otherwise fall back to the stored Y.
  // For stages like the FFmpeg/raw video sink the field-to-image mapping may
  // fail, but the Y position is already known from when the line was selected.
  // Moving the position marker only changes X, so we can always update the
  // cross-hairs as long as we have a valid Y coordinate.
  int image_y = -1;
  if (image_coords.is_valid) {
    image_y = image_coords.image_y;
  } else if (last_line_scope_image_y_ >= 0) {
    image_y = last_line_scope_image_y_;
  }

  if (image_y >= 0) {
    preview_dialog_->previewWidget()->updateCrosshairsPosition(preview_x,
                                                               image_y);
    last_line_scope_image_x_ = preview_x;
    last_line_scope_image_y_ = image_y;

    if (preview_dialog_->frameTimingDialog() &&
        preview_dialog_->frameTimingDialog()->isVisible()) {
      ORC_LOG_DEBUG(
          "Refreshing field timing dialog to update marker after sample marker "
          "move");
      onFrameTimingRequested();
    }
  } else {
    ORC_LOG_WARN(
        "Failed to map field coordinates in onSampleMarkerMoved - not updating "
        "cross-hairs");
  }
}

void MainWindow::refreshLineScopeForCurrentStage() {
  if (!preview_dialog_ || !preview_dialog_->isLineScopeVisible()) {
    return;  // Line scope not visible, nothing to do
  }

  if (!current_view_node_id_.is_valid()) {
    // No valid stage selected - clear line scope and cross-hairs
    ORC_LOG_DEBUG("No valid stage for line scope, clearing");
    preview_dialog_->previewWidget()->setCrosshairsEnabled(false);

    // Show empty line scope
    QString node_id_str = "(none)";
    preview_dialog_->showLineScope(node_id_str, 0, 0, 0, 0,
                                   std::vector<int16_t>(),  // Empty samples
                                   std::nullopt, 0, 0, 0, current_output_type_);
    return;
  }

  // If we have a valid stored position, try to keep the same image location on
  // stage change
  if (last_line_scope_line_number_ >= 0 && last_line_scope_image_x_ >= 0 &&
      last_line_scope_image_y_ >= 0) {
    int image_height =
        preview_dialog_->previewWidget()->originalImageSize().height();
    auto mapping = render_coordinator_->mapImageToField(
        current_view_node_id_, current_output_type_,
        preview_dialog_->previewSlider()->value(), last_line_scope_image_y_,
        image_height, current_option_id_);

    if (mapping.is_valid) {
      int preview_image_width =
          preview_dialog_->previewWidget()->originalImageSize().width();
      ORC_LOG_DEBUG(
          "Stage changed - refreshing line scope at image position ({}, {}) -> "
          "field={}, line={}",
          last_line_scope_image_x_, last_line_scope_image_y_,
          mapping.field_index, mapping.field_line);
      requestLineSamplesForNavigation(mapping.field_index, mapping.field_line,
                                      last_line_scope_image_x_,
                                      preview_image_width);
      return;
    }
    ORC_LOG_WARN(
        "Stage changed - failed to map stored image position to field; "
        "clearing line scope");
  }

  // Fallback: clear line scope for stage switch
  ORC_LOG_DEBUG("Stage changed - clearing line scope state");
  preview_dialog_->previewWidget()->setCrosshairsEnabled(false);

  // Clear the stored line scope state to sentinel values
  last_line_scope_field_index_ = std::numeric_limits<uint64_t>::max();
  last_line_scope_line_number_ = -1;
  last_line_scope_image_x_ = -1;
  last_line_scope_image_y_ = -1;

  // Show empty line scope to indicate no data for this stage until user clicks
  QString node_id_str =
      QString::fromStdString(current_view_node_id_.to_string());
  preview_dialog_->showLineScope(node_id_str, 0, 0, 0, 0,
                                 std::vector<int16_t>(),  // Empty samples
                                 std::nullopt, 0, 0, 0, current_output_type_);
}

void MainWindow::onLineScopeRefreshAtFieldLine() {
  ORC_LOG_DEBUG("onLineScopeRefreshAtFieldLine called");

  if (!preview_dialog_ || !preview_dialog_->isLineScopeVisible()) {
    ORC_LOG_DEBUG("Line scope not visible, skipping refresh");
    return;
  }

  if (!current_view_node_id_.is_valid()) {
    ORC_LOG_DEBUG("No valid node, skipping refresh");
    return;
  }

  // If no line has been selected yet (line number is sentinel -1), skip refresh
  // This can happen when field timing dialog triggers frame changes before line
  // scope is initialized
  if (last_line_scope_line_number_ < 0) {
    ORC_LOG_DEBUG(
        "No valid line number stored, skipping refresh (line scope not yet "
        "initialized)");
    return;
  }

  // If we don't have a valid stored field, try to initialize from current
  // slider position
  if (last_line_scope_field_index_ == std::numeric_limits<uint64_t>::max()) {
    ORC_LOG_DEBUG("No stored field index, initializing from current mode");

    if (current_output_type_ == orc::PreviewOutputType::Frame_Field1 ||
        current_output_type_ == orc::PreviewOutputType::Frame_Field2) {
      last_line_scope_field_index_ = preview_dialog_->previewSlider()->value();
    } else if (current_output_type_ == orc::PreviewOutputType::Split) {
      // For split mode, use first field of the pair
      uint64_t pair_index = preview_dialog_->previewSlider()->value();
      last_line_scope_field_index_ = pair_index * 2;
    } else if (current_output_type_ ==
                   orc::PreviewOutputType::Frame_Field1_First ||
               current_output_type_ == orc::PreviewOutputType::Frame_Reversed) {
      // For frame mode, get fields from frame and use first one
      auto frame_fields = render_coordinator_->getFrameFields(
          current_view_node_id_, preview_dialog_->previewSlider()->value());
      if (frame_fields.is_valid) {
        last_line_scope_field_index_ = frame_fields.first_field;
      } else {
        ORC_LOG_WARN("Failed to get frame fields, cannot initialize");
        return;
      }
    }
    ORC_LOG_DEBUG("Initialized field_index to {}",
                  last_line_scope_field_index_);
  }

  ORC_LOG_DEBUG("Refreshing line scope: was at field={}, line={}",
                last_line_scope_field_index_, last_line_scope_line_number_);

  uint64_t new_field_index = last_line_scope_field_index_;

  // If in Frame mode, get which fields are in the current frame and pick the
  // one with same parity
  if (current_output_type_ == orc::PreviewOutputType::Frame_Field1_First ||
      current_output_type_ == orc::PreviewOutputType::Frame_Reversed) {
    uint64_t frame_index = preview_dialog_->previewSlider()->value();
    auto frame_fields =
        render_coordinator_->getFrameFields(current_view_node_id_, frame_index);

    if (frame_fields.is_valid) {
      // Pick field with same parity (odd/even) as the one we were viewing
      bool was_odd = (last_line_scope_field_index_ % 2) == 1;
      bool first_is_odd = (frame_fields.first_field % 2) == 1;

      if (was_odd == first_is_odd) {
        new_field_index = frame_fields.first_field;
      } else {
        new_field_index = frame_fields.second_field;
      }

      ORC_LOG_DEBUG(
          "Frame {} contains fields [{}, {}], using field {} (parity match)",
          frame_index, frame_fields.first_field, frame_fields.second_field,
          new_field_index);
    } else {
      ORC_LOG_WARN("Failed to get frame fields, keeping old field index");
    }
  }
  // In Field or Split mode, just use the current field index from the slider
  else if (current_output_type_ == orc::PreviewOutputType::Frame_Field1 ||
           current_output_type_ == orc::PreviewOutputType::Frame_Field2) {
    new_field_index = preview_dialog_->previewSlider()->value();
    ORC_LOG_DEBUG("Field mode: using field {} from slider", new_field_index);
  } else if (current_output_type_ == orc::PreviewOutputType::Split) {
    // In Split mode, preserve the parity (odd/even) of the field we were
    // viewing Split pairs are always: (0,1), (2,3), (4,5), etc.
    uint64_t pair_index = preview_dialog_->previewSlider()->value();
    uint64_t first_field_in_pair = pair_index * 2;
    uint64_t second_field_in_pair = pair_index * 2 + 1;

    // Preserve the parity of the field we were viewing
    bool was_odd = (last_line_scope_field_index_ % 2) == 1;

    if (was_odd) {
      new_field_index = second_field_in_pair;
    } else {
      new_field_index = first_field_in_pair;
    }
    ORC_LOG_DEBUG(
        "Split mode: pair_index={}, preserving parity, using field={}",
        pair_index, new_field_index);
  }

  // Request samples at the SAME line number in the new field
  int sample_x = last_line_scope_image_x_;
  int preview_width =
      preview_dialog_->previewWidget()->originalImageSize().width();

  ORC_LOG_DEBUG("Requesting samples at field={}, line={}, sample_x={}",
                new_field_index, last_line_scope_line_number_, sample_x);

  pending_line_sample_request_id_ = render_coordinator_->requestLineSamples(
      current_view_node_id_, orc::PreviewOutputType::Frame_Field1,
      new_field_index, last_line_scope_line_number_, sample_x, preview_width);
}

void MainWindow::onLineNavigation(int direction, uint64_t /*current_field*/,
                                  int /*current_line*/, int sample_x,
                                  int preview_image_width) {
  ORC_LOG_DEBUG("Line navigation requested: direction={}, sample_x={}",
                direction, sample_x);

  if (!current_view_node_id_.is_valid() || !preview_dialog_) {
    return;
  }

  // sample_x is in preview-space/image coordinates; keep this stable across
  // navigation.
  last_line_scope_image_x_ = sample_x;

  if (last_line_scope_line_number_ < 0 ||
      last_line_scope_field_index_ == std::numeric_limits<uint64_t>::max()) {
    ORC_LOG_DEBUG("Line navigation skipped: no valid stored field/line state");
    return;
  }

  const uint64_t output_index = preview_dialog_->previewSlider()->value();

  // The dialog's current_field may be from a past frame when the decoder is
  // slow (e.g. chroma/FFmpeg). Recompute the effective field from our stored
  // parity intent and the *current* slider position, matching the same logic
  // as onLineScopeRefreshAtFieldLine, so that mapFieldToImage always receives
  // a field that belongs to the current output frame.
  uint64_t effective_field = last_line_scope_field_index_;

  if (current_output_type_ == orc::PreviewOutputType::Frame_Field1_First ||
      current_output_type_ == orc::PreviewOutputType::Frame_Reversed) {
    auto frame_fields = render_coordinator_->getFrameFields(
        current_view_node_id_, output_index);
    if (frame_fields.is_valid) {
      bool was_odd = (last_line_scope_field_index_ % 2) == 1;
      bool first_is_odd = (frame_fields.first_field % 2) == 1;
      effective_field = (was_odd == first_is_odd) ? frame_fields.first_field
                                                  : frame_fields.second_field;
      ORC_LOG_DEBUG("Frame {}: fields [{},{}], using field {} for navigation",
                    output_index, frame_fields.first_field,
                    frame_fields.second_field, effective_field);
    } else {
      ORC_LOG_WARN("Failed to get frame fields for navigation");
      return;
    }
  } else if (current_output_type_ == orc::PreviewOutputType::Frame_Field1 ||
             current_output_type_ == orc::PreviewOutputType::Frame_Field2) {
    effective_field = output_index;
  } else if (current_output_type_ == orc::PreviewOutputType::Split) {
    bool was_odd = (last_line_scope_field_index_ % 2) == 1;
    effective_field = was_odd ? (output_index * 2 + 1) : (output_index * 2);
  }

  const int effective_line = last_line_scope_line_number_;

  const int image_height =
      preview_dialog_->previewWidget()->originalImageSize().height();
  const auto navigation_target = orc::gui::computeLineNavigationTarget(
      {
          direction,
          effective_field,
          effective_line,
          image_height,
      },
      [this, output_index](uint64_t field_index, int line_number,
                           int local_image_height) {
        return render_coordinator_->mapFieldToImage(
            current_view_node_id_, current_output_type_, output_index,
            field_index, line_number, local_image_height, current_option_id_);
      },
      [this, output_index](int image_y, int local_image_height) {
        return render_coordinator_->mapImageToField(
            current_view_node_id_, current_output_type_, output_index, image_y,
            local_image_height, current_option_id_);
      });

  if (!navigation_target.is_valid) {
    ORC_LOG_DEBUG(
        "Line navigation rejected: no valid next field/line mapping available");
    return;
  }

  ORC_LOG_DEBUG("Line navigation mapped to field {}, line {}",
                navigation_target.field_index, navigation_target.line_number);
  requestLineSamplesForNavigation(navigation_target.field_index,
                                  navigation_target.line_number, sample_x,
                                  preview_image_width);
}

void MainWindow::propagateAmplitudeUnit() {
  if (!project_.presenter()) {
    return;
  }
  const orc::AmplitudeDisplayUnit unit =
      project_.presenter()->getAmplitudeUnit();
  if (video_parameter_observer_dialog_) {
    video_parameter_observer_dialog_->setAmplitudeUnit(unit);
    // Re-render with the new unit if the dialog is currently visible.
    updateVideoParameterObserverDialog();
  }
  for (auto& [id, dlg] : burst_level_analysis_dialogs_) {
    if (dlg) dlg->setAmplitudeUnit(unit);
  }
  if (preview_dialog_) {
    preview_dialog_->forwardAmplitudeUnit(unit);
  }
}
