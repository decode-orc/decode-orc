/*
 * File:        previewdialog.cpp
 * Module:      orc-gui
 * Purpose:     Separate preview window for field/frame viewing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "previewdialog.h"

#include <QCloseEvent>
#include <QFile>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMoveEvent>
#include <QProgressDialog>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QThread>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <utility>

#include "audio_channel_pair_notice.h"
#include "field_frame_presentation.h"
#include "fieldpreviewwidget.h"
#include "framescopedialog.h"
#include "frametimingdialog.h"
#include "logging.h"
#include "preview/histogram_dialog.h"
#include "preview/vectorscope_dialog.h"
#include "preview_audio_chase.h"
#include "stage_help_dialog.h"
#include "waveformmonitordialog.h"

namespace {

constexpr const char* kLineScopeViewId = "preview.linescope";
constexpr const char* kFrameTimingViewId = "preview.frame_timing";
constexpr const char* kWaveformMonitorViewId = "preview.frame_timing";
constexpr const char* kComponentVectorscopeViewId = "preview.vectorscope";
constexpr const char* kHistogramViewId = "preview.histogram";

// Combo entry that plays nothing — it is also how the user silences playback,
// so it is named for both. Always present, so the selector reads the same
// whether or not the node happens to carry audio.
constexpr const char* kNoAudioEntry = "Mute/None";

// Preparing audio is usually instantaneous (a CVBS source reads its WAV per
// frame) but can take minutes (an EFM disc decode). Only put a dialog on
// screen once the wait has become long enough to need explaining.
constexpr int kAudioPrimeDialogDelayMs = 400;

// How often the prime dialog picks up the worker's published progress.
constexpr int kAudioPrimeTickMs = 200;

// Convert the presenter-layer VideoSystem to the core orc::VideoSystem used by
// the field/frame presentation helpers. The two enums are mirrors; this
// translation keeps the GUI layer decoupled from core types.
orc::VideoSystem toOrcVideoSystem(orc::presenters::VideoSystem sys) {
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

}  // namespace

PreviewDialog::PreviewDialog(QWidget* parent)
    : QDialog(parent),
      frame_scope_dialog_(nullptr),
      frame_timing_dialog_(nullptr),
      waveform_monitor_dialog_(nullptr),
      vectorscope_dialog_(nullptr),
      nav_debounce_timer_(new QTimer(this)),
      playback_timer_(new QTimer(this)),
      audio_controller_(new orc::gui::PreviewAudioController(this)),
      audio_prime_show_timer_(new QTimer(this)),
      audio_prime_tick_timer_(new QTimer(this)) {
  nav_debounce_timer_->setSingleShot(true);
  nav_debounce_timer_->setInterval(100);  // ms: coalesces rapid scrub moves
  connect(nav_debounce_timer_, &QTimer::timeout, this, [this]() {
    // The scrub has settled: audio suspended by the drag resumes here, at the
    // position the render about to be requested will show.
    resumeAudioAtCurrentIndex();
    emit renderRequested(currentIndex());
  });

  // ITU-R BT.470-6 §5.1 (PAL 25 fps) / SMPTE 170M-2004 §2 (NTSC ~30 fps).
  // Default to PAL rate; MainWindow calls setPlaybackFrameRateMs() once it
  // knows the video standard. With audio playing the interval only sets how
  // often the chase target is re-read; the audio device sets the pace.
  playback_timer_->setInterval(40);
  connect(playback_timer_, &QTimer::timeout, this,
          &PreviewDialog::onPlaybackTick);

  // The last frame's audio finishing is what ends an audio-clocked session:
  // the video may still be several frames behind, but there is nothing left
  // to chase.
  connect(audio_controller_, &orc::gui::PreviewAudioController::finished, this,
          &PreviewDialog::stopPlayback);

  audio_prime_show_timer_->setSingleShot(true);
  audio_prime_show_timer_->setInterval(kAudioPrimeDialogDelayMs);
  connect(audio_prime_show_timer_, &QTimer::timeout, this,
          &PreviewDialog::showAudioPrimeDialog);

  audio_prime_tick_timer_->setInterval(kAudioPrimeTickMs);
  connect(audio_prime_tick_timer_, &QTimer::timeout, this,
          &PreviewDialog::updateAudioPrimeDialog);

  setupUI();
  setWindowTitle("Field/Frame Preview");

  // Use Qt::Window flag to allow independent positioning without forcing
  // z-order
  setWindowFlags(Qt::Window);

  // Don't destroy on close, just hide
  setAttribute(Qt::WA_DeleteOnClose, false);

  // Set default size - geometry will be restored by MainWindow
  resize(800, 700);
}

void PreviewDialog::setSignalControlsVisible(bool visible) {
  signal_label_->setVisible(visible);
  signal_combo_->setVisible(visible);
}

void PreviewDialog::setObserverAvailabilityForFormat(
    orc::presenters::VideoFormat format) {
  // SMPTE 170M-2004 §8.4 / CEA-608-E §4: the NTSC-specific observers (FM code,
  // white flag) and line 21 closed captions exist only on 525-line NTSC.
  const bool is_ntsc = (format == orc::presenters::VideoFormat::NTSC);
  show_ntsc_observer_action_->setEnabled(is_ntsc);
  show_closed_caption_action_->setEnabled(is_ntsc);
}

PreviewDialog::~PreviewDialog() = default;

const std::string& PreviewDialog::kComponentVectorscopeViewIdRef() {
  static const std::string view_id{kComponentVectorscopeViewId};
  return view_id;
}

const std::string& PreviewDialog::kHistogramViewIdRef() {
  static const std::string view_id{kHistogramViewId};
  return view_id;
}

void PreviewDialog::navigateToIndex(int zero_based) {
  const int clamped = std::clamp(zero_based, preview_slider_->minimum(),
                                 preview_slider_->maximum());
  nav_debounce_timer_->stop();  // cancel any pending debounced render
  setIndex(clamped);
  resumeAudioAtCurrentIndex();  // rebase the audio clock on the new position
  emit positionChanged(clamped);
  emit renderRequested(clamped);
}

void PreviewDialog::navigateToIndexDebounced(int zero_based) {
  const int clamped = std::clamp(zero_based, preview_slider_->minimum(),
                                 preview_slider_->maximum());
  setIndex(clamped);  // update UI immediately for visual feedback
  suspendAudioForScrub();
  emit positionChanged(clamped);
  nav_debounce_timer_
      ->start();  // (re)starts; fires renderRequested when settled
}

void PreviewDialog::setIndex(int zero_based) {
  // Silently sync both slider and spinbox without emitting any signals.
  preview_slider_->blockSignals(true);
  frame_jump_spinbox_->blockSignals(true);
  preview_slider_->setValue(zero_based);
  frame_jump_spinbox_->setValue(zero_based + 1);  // 1-indexed display
  preview_slider_->blockSignals(false);
  frame_jump_spinbox_->blockSignals(false);
}

void PreviewDialog::setupUI() {
  auto* mainLayout = new QVBoxLayout(this);

  // Menu bar
  menu_bar_ = new QMenuBar(this);
  auto* fileMenu = menu_bar_->addMenu("&File");
  export_png_action_ = fileMenu->addAction("&Export PNG...");
  export_png_action_->setShortcut(
      QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
  connect(export_png_action_, &QAction::triggered, this,
          &PreviewDialog::exportPNGRequested);

  auto* observersMenu = menu_bar_->addMenu("&Observers");
  show_vbi_action_ = observersMenu->addAction("&VBI Decoder");
  show_vbi_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
  connect(show_vbi_action_, &QAction::triggered, this,
          &PreviewDialog::showVBIDialogRequested);

  show_ntsc_observer_action_ = observersMenu->addAction("&NTSC Observer");
  show_ntsc_observer_action_->setShortcut(
      QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
  connect(show_ntsc_observer_action_, &QAction::triggered, this,
          &PreviewDialog::showNtscObserverDialogRequested);

  show_video_parameter_observer_action_ =
      observersMenu->addAction("&Video Parameters");
  show_video_parameter_observer_action_->setShortcut(
      QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
  connect(show_video_parameter_observer_action_, &QAction::triggered, this,
          &PreviewDialog::showVideoParameterObserverDialogRequested);

  show_closed_caption_action_ = observersMenu->addAction("&Closed Captions");
  show_closed_caption_action_->setShortcut(
      QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
  connect(show_closed_caption_action_, &QAction::triggered, this,
          &PreviewDialog::showClosedCaptionDialogRequested);

  auto* viewMenu = menu_bar_->addMenu("&View");
  show_frame_timing_action_ = viewMenu->addAction("&Frame Timing");
  show_frame_timing_action_->setShortcut(
      QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
  connect(show_frame_timing_action_, &QAction::triggered, this, [this]() {
    if (!hasAvailablePreviewView(kFrameTimingViewId)) {
      status_bar_->showMessage("Frame timing is not available for this stage",
                               2000);
      return;
    }
    frame_timing_open_requested_ = true;
    emit frameTimingRequested();
  });

  show_waveform_monitor_action_ = viewMenu->addAction("&Waveform Monitor");
  show_waveform_monitor_action_->setShortcut(
      QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W));
  show_waveform_monitor_action_->setVisible(false);
  show_waveform_monitor_action_->setEnabled(false);
  connect(show_waveform_monitor_action_, &QAction::triggered, this, [this]() {
    if (!hasAvailablePreviewView(kWaveformMonitorViewId)) {
      status_bar_->showMessage(
          "Waveform monitor is not available for this stage", 2000);
      return;
    }
    waveform_monitor_open_requested_ = true;
    emit waveformMonitorRequested();
  });

  show_component_vectorscope_action_ = viewMenu->addAction("&Vectorscope");
  show_component_vectorscope_action_->setShortcut(
      QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
  show_component_vectorscope_action_->setVisible(false);
  show_component_vectorscope_action_->setEnabled(false);
  connect(show_component_vectorscope_action_, &QAction::triggered, this,
          &PreviewDialog::onComponentVectorscopeActionTriggered);

  show_histogram_action_ = viewMenu->addAction("&Video Histogram");
  show_histogram_action_->setShortcut(
      QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
  show_histogram_action_->setVisible(false);
  show_histogram_action_->setEnabled(false);
  connect(show_histogram_action_, &QAction::triggered, this,
          &PreviewDialog::onHistogramActionTriggered);

  auto* helpMenu = menu_bar_->addMenu("&Help");
  auto* user_guide_action = helpMenu->addAction("&User Guide...");
  connect(user_guide_action, &QAction::triggered, this, [this]() {
    QFile f(":/orc-gui/docs/preview_window.md");
    const QString md = f.open(QIODevice::ReadOnly)
                           ? QString::fromUtf8(f.readAll())
                           : QString{};
    auto* dlg = new StageHelpDialog("Preview Window", md, this);
    dlg->show();
  });

  mainLayout->setMenuBar(menu_bar_);

  // Preview widget
  preview_widget_ = new FieldPreviewWidget(this);
  preview_widget_->setMinimumSize(640, 480);
  mainLayout->addWidget(preview_widget_, 1);

  // Preview info label
  preview_info_label_ = new QLabel("No preview available");
  mainLayout->addWidget(preview_info_label_);

  // Slider controls with navigation buttons
  auto* sliderLayout = new QHBoxLayout();

  // Navigation buttons
  first_button_ = new QPushButton("<<");
  prev_button_ = new QPushButton("<");
  play_pause_button_ = new QPushButton("▶");  // ▶ play symbol
  next_button_ = new QPushButton(">");
  last_button_ = new QPushButton(">>");

  // Set auto-repeat on prev/next buttons for navigation
  // Increased delay to reduce sensitivity for single-frame stepping
  prev_button_->setAutoRepeat(true);
  prev_button_->setAutoRepeatDelay(200);
  prev_button_->setAutoRepeatInterval(30);

  next_button_->setAutoRepeat(true);
  next_button_->setAutoRepeatDelay(200);
  next_button_->setAutoRepeatInterval(30);

  // Prevent navigation buttons from being treated as dialog default buttons
  // (without this, the << button appears focused when the spinbox has focus)
  first_button_->setAutoDefault(false);
  prev_button_->setAutoDefault(false);
  play_pause_button_->setAutoDefault(false);
  next_button_->setAutoDefault(false);
  last_button_->setAutoDefault(false);

  // Set fixed width for navigation buttons
  first_button_->setFixedWidth(40);
  prev_button_->setFixedWidth(40);
  play_pause_button_->setFixedWidth(40);
  next_button_->setFixedWidth(40);
  last_button_->setFixedWidth(40);

  play_pause_button_->setToolTip("Play / Pause");

  slider_min_label_ = new QLabel("0");
  slider_max_label_ = new QLabel("0");
  preview_slider_ = new QSlider(Qt::Horizontal);
  preview_slider_->setEnabled(false);
  // Set tracking to false for better performance during scrubbing
  // This makes the slider only emit valueChanged when released, not during drag
  // For real-time preview during drag, we can use sliderMoved signal separately
  preview_slider_->setTracking(
      true);  // Keep true for now, but we'll throttle updates in MainWindow

  sliderLayout->addWidget(first_button_);
  sliderLayout->addWidget(prev_button_);
  sliderLayout->addWidget(play_pause_button_);
  sliderLayout->addWidget(next_button_);
  sliderLayout->addWidget(last_button_);

  // Jump-to spin box (1-indexed, to the right of navigation buttons)
  frame_jump_spinbox_ = new QSpinBox();
  frame_jump_spinbox_->setMinimum(1);
  frame_jump_spinbox_->setMaximum(1);
  frame_jump_spinbox_->setEnabled(false);
  {
    // Size the spinbox to comfortably hold up to 5-digit numbers
    QFontMetrics fm(frame_jump_spinbox_->font());
    frame_jump_spinbox_->setMinimumWidth(fm.horizontalAdvance("99999") +
                                         36);  // +36 for arrows/margins
  }
  frame_jump_spinbox_->setToolTip("Jump directly to a frame/field number");
  sliderLayout->addWidget(frame_jump_spinbox_);

  sliderLayout->addWidget(slider_min_label_);
  sliderLayout->addWidget(preview_slider_, 1);
  sliderLayout->addWidget(slider_max_label_);
  mainLayout->addLayout(sliderLayout);

  // Control row: Preview mode and aspect ratio
  auto* controlLayout = new QHBoxLayout();

  controlLayout->addWidget(new QLabel("Preview Mode:"));
  preview_mode_combo_ = new QComboBox();
  controlLayout->addWidget(preview_mode_combo_);

  signal_label_ = new QLabel("Channel:");
  signal_label_->setVisible(false);  // Hidden by default, shown for YC sources
  controlLayout->addWidget(signal_label_);
  signal_combo_ = new QComboBox();
  signal_combo_->addItem("Y+C");
  signal_combo_->addItem("Luma (Y)");
  signal_combo_->addItem("Chroma (C)");
  signal_combo_->setVisible(false);  // Hidden by default, shown for YC sources
  controlLayout->addWidget(signal_combo_);

  controlLayout->addWidget(new QLabel("Aspect Ratio:"));
  aspect_ratio_combo_ = new QComboBox();
  controlLayout->addWidget(aspect_ratio_combo_);

  // Add Zoom 1:1 button
  zoom1to1_button_ = new QPushButton("Zoom 1:1");
  zoom1to1_button_->setAutoDefault(false);
  zoom1to1_button_->setToolTip("Resize preview to original image size");
  controlLayout->addWidget(zoom1to1_button_);

  // Add Dropouts toggle button
  dropouts_button_ = new QPushButton("Dropouts: Off");
  dropouts_button_->setAutoDefault(false);
  dropouts_button_->setCheckable(true);
  dropouts_button_->setChecked(false);
  dropouts_button_->setToolTip("Show/hide dropout regions");
  controlLayout->addWidget(dropouts_button_);

  setupAudioControls(controlLayout);

  controlLayout->addStretch();
  mainLayout->addLayout(controlLayout);

  // Status bar
  status_bar_ = new QStatusBar(this);
  status_bar_->showMessage("No stage selected");
  mainLayout->addWidget(status_bar_);

  // -----------------------------------------------------------------------
  // Internal sync: keep spinbox display in step with slider (handles range-
  // clamping done by Qt when setRange is called, and also setIndex calls).
  // These connections do NOT emit navigation signals.
  // -----------------------------------------------------------------------
  connect(preview_slider_, &QSlider::valueChanged, [this](int value) {
    frame_jump_spinbox_->blockSignals(true);
    frame_jump_spinbox_->setValue(value + 1);
    frame_jump_spinbox_->blockSignals(false);
  });
  connect(preview_slider_, &QSlider::rangeChanged, [this](int min, int max) {
    frame_jump_spinbox_->setRange(min + 1, max + 1);
  });

  // -----------------------------------------------------------------------
  // Navigation: all sources call navigateToIndex / navigateToIndexDebounced.
  // Those two methods are the single authority for position changes and
  // decide which signals to emit.
  // -----------------------------------------------------------------------

  // First / Prev / Next / Last buttons — always immediate
  connect(first_button_, &QPushButton::clicked,
          [this]() { navigateToIndex(preview_slider_->minimum()); });
  connect(prev_button_, &QPushButton::clicked,
          [this]() { navigateToIndex(preview_slider_->value() - 1); });
  connect(next_button_, &QPushButton::clicked,
          [this]() { navigateToIndex(preview_slider_->value() + 1); });
  connect(last_button_, &QPushButton::clicked,
          [this]() { navigateToIndex(preview_slider_->maximum()); });

  // Play / Pause button — toggle playback state
  connect(play_pause_button_, &QPushButton::clicked, [this]() {
    if (is_playing_) {
      stopPlayback();
    } else {
      // Restart from the first frame when playback is started at the end.
      if (currentIndex() >= preview_slider_->maximum()) {
        navigateToIndex(preview_slider_->minimum());
      }
      startPlayback();
    }
  });

  // Slider drag — debounced for smooth scrubbing
  connect(preview_slider_, &QSlider::sliderMoved,
          [this](int value) { navigateToIndexDebounced(value); });
  // Slider release — commit immediately (cancels any pending debounce)
  connect(preview_slider_, &QSlider::sliderReleased,
          [this]() { navigateToIndex(preview_slider_->value()); });

  // Spinbox changes (arrow buttons AND keyboard typing) — debounced so that
  // intermediate digits while typing are coalesced into a final render.
  connect(frame_jump_spinbox_, QOverload<int>::of(&QSpinBox::valueChanged),
          [this](int one_based) { navigateToIndexDebounced(one_based - 1); });
  // Non-navigation signals
  connect(preview_mode_combo_,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &PreviewDialog::previewModeChanged);
  connect(signal_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &PreviewDialog::signalChanged);
  connect(aspect_ratio_combo_,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &PreviewDialog::aspectRatioModeChanged);

  // Connect dropouts button
  connect(dropouts_button_, &QPushButton::toggled, [this](bool checked) {
    dropouts_button_->setText(checked ? "Dropouts: On" : "Dropouts: Off");
    emit showDropoutsChanged(checked);
  });

  // Connect Zoom 1:1 button
  connect(zoom1to1_button_, &QPushButton::clicked, [this]() {
    QSize img_size = preview_widget_->originalImageSize();
    if (img_size.isEmpty()) {
      return;  // No image to zoom to
    }

    // The image from core already has aspect ratio scaling applied,
    // so we can use the image size directly for 1:1 zoom
    QSize display_size = img_size;

    // Calculate total window size based on widget size
    // We need to account for all other UI elements
    int extra_height = height() - preview_widget_->height();
    int extra_width = width() - preview_widget_->width();

    // Set the preview widget to the original size
    preview_widget_->setMinimumSize(display_size);
    preview_widget_->setMaximumSize(display_size);

    // Adjust the dialog size
    adjustSize();

    // Reset size constraints after resize
    preview_widget_->setMinimumSize(320, 240);
    preview_widget_->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
  });

  // Create frame scope dialog (replaces LineScopeDialog)
  frame_scope_dialog_ = new FrameScopeDialog(this);

  // Create frame timing dialog
  frame_timing_dialog_ = new FrameTimingDialog(this);

  // Create waveform monitor dialog
  waveform_monitor_dialog_ = new WaveformMonitorDialog(this);

  // Connect to dialog hide/close events to disable cross-hairs and drop the
  // "user wants this open" intent, so an in-flight data response cannot
  // re-open a dialog the user has just closed.
  connect(frame_scope_dialog_, &QDialog::finished, this, [this]() {
    line_scope_open_requested_ = false;
    preview_widget_->setCrosshairsEnabled(false);
  });
  connect(frame_scope_dialog_, &QDialog::rejected, this, [this]() {
    line_scope_open_requested_ = false;
    preview_widget_->setCrosshairsEnabled(false);
  });

  connect(frame_timing_dialog_, &QDialog::finished, this,
          [this]() { frame_timing_open_requested_ = false; });

  connect(waveform_monitor_dialog_, &QDialog::finished, this,
          [this]() { waveform_monitor_open_requested_ = false; });

  // Adapt FrameScopeDialog::lineNavigationRequested (size_t frame_line) to
  // PreviewDialog::lineNavigationRequested (int current_line).
  // Connected once here to avoid stacking lambda connections in showLineScope.
  connect(
      frame_scope_dialog_, &FrameScopeDialog::lineNavigationRequested, this,
      [this](int dir, uint64_t frame_id, size_t frame_line, int sx, int pw) {
        emit lineNavigationRequested(dir, frame_id,
                                     static_cast<int>(frame_line), sx, pw);
      });

  // Connect line clicked signal
  connect(preview_widget_, &FieldPreviewWidget::lineClicked,
          [this](int image_x, int image_y) {
            if (!hasAvailablePreviewView(kLineScopeViewId)) {
              status_bar_->showMessage(
                  "Line scope is not available for this stage", 2000);
              return;
            }
            line_scope_open_requested_ = true;
            emit lineScopeRequested(image_x, image_y);
          });
}

void PreviewDialog::setPlaybackFrameRateMs(int ms) {
  playback_timer_->setInterval(ms);
}

void PreviewDialog::stopPlayback() {
  // Audio teardown runs unconditionally: a session can be waiting for a reader
  // or priming before is_playing_ has ever driven the timer.
  endAudioPreparation();
  audio_controller_->stop();
  audio_state_ = AudioState::kIdle;
  playback_timer_->stop();

  if (!is_playing_) {
    return;
  }
  is_playing_ = false;
  play_pause_button_->setText("▶");  // ▶ play symbol
}

void PreviewDialog::setCurrentNode(const QString& node_label,
                                   const QString& node_id) {
  Q_UNUSED(node_label);
  status_bar_->showMessage(
      QString("Viewing output from stage: %1").arg(node_id));
}

void PreviewDialog::setCurrentNodeId(orc::NodeID node_id) {
  // Pair indices and the representation behind a reader are both node-
  // specific, so a different node invalidates everything audio holds. The
  // owner re-enumerates and calls setAudioChannelPairs() with the new list.
  if (node_id != current_node_id_) {
    invalidateAudioSource();
    setAudioChannelPairs({});
  }

  current_node_id_ = node_id;

  if (vectorscope_dialog_ && vectorscope_dialog_->isVisible() &&
      node_id.is_valid()) {
    vectorscope_node_id_ = node_id;
    vectorscope_dialog_->setStage(node_id);
  }
}

// ===========================================================================
// Audio playback
// ===========================================================================

void PreviewDialog::setupAudioControls(QHBoxLayout* layout) {
  audio_label_ = new QLabel("Audio:");
  layout->addWidget(audio_label_);

  audio_pair_combo_ = new QComboBox();
  layout->addWidget(audio_pair_combo_);

  audio_volume_slider_ = new QSlider(Qt::Horizontal);
  audio_volume_slider_->setRange(0, 100);  // per cent of full scale
  audio_volume_slider_->setValue(100);
  audio_volume_slider_->setFixedWidth(80);
  audio_volume_slider_->setToolTip("Playback volume");
  layout->addWidget(audio_volume_slider_);

  connect(audio_pair_combo_,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int) {
            // Only user-driven changes reach here: repopulating blocks the
            // signal, so this is the choice worth carrying to the next node.
            rememberSelectedAudioPair();
            // A different pair needs a different reader, and the running
            // session belongs to the old one. Playback is torn down and then
            // taken up again on the new pair at the position reached, so
            // comparing pairs does not cost a trip back to the Play button.
            const bool was_playing = is_playing_;
            invalidateAudioSource();
            updateAudioControlStates();
            if (was_playing) {
              startPlayback();
            }
          });

  connect(audio_volume_slider_, &QSlider::valueChanged, this,
          [this](int value) {
            audio_controller_->setVolume(static_cast<double>(value) / 100.0);
          });

#ifndef ORC_GUI_AUDIO_PLAYBACK
  // Built without an audio backend: these controls could never do anything, so
  // they are not offered at all and playback keeps its video-only path.
  audio_label_->setVisible(false);
  audio_pair_combo_->setVisible(false);
  audio_volume_slider_->setVisible(false);
#endif

  setAudioChannelPairs({});
}

void PreviewDialog::setAudioChannelPairs(
    const std::vector<orc::AudioPairView>& pairs) {
  audio_pairs_ = pairs;

  // Repopulating resets the selection, which must not be read as the user
  // picking a different pair.
  const QSignalBlocker block(audio_pair_combo_);
  audio_pair_combo_->clear();
  audio_pair_combo_->addItem(kNoAudioEntry);
  for (const auto& pair : audio_pairs_) {
    const QString name =
        QString::fromStdString(pair.name.empty() ? pair.origin : pair.name);
    const QString label =
        QString("%1: %2").arg(static_cast<qulonglong>(pair.index)).arg(name);
    audio_pair_combo_->addItem(label, QVariant::fromValue<qulonglong>(
                                          static_cast<qulonglong>(pair.index)));
  }
  audio_pair_combo_->setCurrentIndex(comboIndexForRememberedPair());

  updateAudioControlStates();
}

void PreviewDialog::rememberSelectedAudioPair() {
  const int index = audio_pair_combo_->currentIndex();
  const size_t pair_index = static_cast<size_t>(index - 1);  // 0 is "Mute/None"
  if (index <= 0 || pair_index >= audio_pairs_.size()) {
    remembered_audio_pair_.reset();  // "Mute/None" is a choice worth keeping
    return;
  }
  remembered_audio_pair_ = audio_pairs_[pair_index];
}

int PreviewDialog::comboIndexForRememberedPair() const {
  if (!remembered_audio_pair_.has_value()) {
    return 0;  // "Mute/None"
  }

  // Pair indices are node-specific, so the remembered pair is matched by what
  // describes it — name and origin — and the index only breaks ties between
  // otherwise identical pairs. No match means this node does not carry the
  // pair and the selector falls back to "Mute/None", leaving the choice
  // remembered for a node that does.
  const orc::AudioPairView& want = *remembered_audio_pair_;
  int fallback = 0;
  for (size_t i = 0; i < audio_pairs_.size(); ++i) {
    const orc::AudioPairView& pair = audio_pairs_[i];
    if (pair.name != want.name || pair.origin != want.origin) {
      continue;
    }
    const int combo_index = static_cast<int>(i) + 1;  // 0 is "Mute/None"
    if (pair.index == want.index) {
      return combo_index;
    }
    if (fallback == 0) {
      fallback = combo_index;
    }
  }
  return fallback;
}

void PreviewDialog::updateAudioControlStates() {
  const bool has_pairs = !audio_pairs_.empty();
  const bool pair_selected = selectedAudioPair().has_value();

  audio_label_->setEnabled(has_pairs);
  audio_pair_combo_->setEnabled(has_pairs);
  // Volume acts on a selected pair whether or not it is playing, so the level
  // can be set before pressing Play.
  audio_volume_slider_->setEnabled(pair_selected);

  audio_pair_combo_->setToolTip(
      has_pairs
          ? QString("Audio channel pair to play with the preview")
          : QString::fromStdString(orc::gui::audioChannelPairPreviewNotice()));
}

std::optional<size_t> PreviewDialog::selectedAudioPair() const {
  const QVariant data = audio_pair_combo_->currentData();
  if (!data.isValid()) {
    return std::nullopt;  // "Mute/None"
  }
  return static_cast<size_t>(data.toULongLong());
}

bool PreviewDialog::isAudioPlaybackActive() const {
  return audio_state_ == AudioState::kPlaying;
}

void PreviewDialog::setAudioOutput(
    std::unique_ptr<orc::gui::IAudioOutput> output) {
  audio_controller_->setAudioOutput(std::move(output));
}

void PreviewDialog::setAudioItemsPerFrame(uint32_t items_per_frame) {
  audio_controller_->setItemsPerFrame(items_per_frame);
}

void PreviewDialog::invalidateAudioSource() {
  stopPlayback();
  audio_reader_.reset();
  audio_reader_pair_.reset();
  audio_controller_->setReader(nullptr);
}

void PreviewDialog::startPlayback() {
  const std::optional<size_t> pair = selectedAudioPair();
  if (!pair.has_value() || !ensureAudioOutput()) {
    beginVideoOnlyPlayback();
    return;
  }

  // Playback intent is taken now, before the reader exists: preparing audio
  // can take a while and pressing Play again must cancel it.
  is_playing_ = true;
  play_pause_button_->setText("⏸");  // ⏸ pause symbol

  if (audio_reader_ && audio_reader_pair_ == pair) {
    beginAudioPlayback();  // Already primed — resume costs nothing
    return;
  }

  audio_state_ = AudioState::kAwaitingReader;
  status_bar_->showMessage("Preparing audio…");
  emit audioStreamReaderRequested(*pair);
}

void PreviewDialog::beginVideoOnlyPlayback() {
  audio_state_ = AudioState::kIdle;
  is_playing_ = true;
  play_pause_button_->setText("⏸");  // ⏸ pause symbol
  playback_timer_->start();
}

bool PreviewDialog::ensureAudioOutput() {
  if (audio_controller_->hasAudioOutput()) {
    return true;
  }
  if (audio_output_unavailable_) {
    return false;
  }

  // First playback with a pair selected is what touches the audio subsystem;
  // a project with no audio never gets here.
  auto output = orc::gui::createSystemAudioOutput();
  if (!output) {
    ORC_LOG_WARN(
        "PreviewDialog: No audio output backend; playing the preview silently");
    audio_output_unavailable_ = true;
    status_bar_->showMessage("No audio output is available; playing video only",
                             4000);
    return false;
  }
  audio_controller_->setAudioOutput(std::move(output));
  return true;
}

void PreviewDialog::setAudioStreamReader(
    std::shared_ptr<orc::presenters::IAudioStreamReader> reader) {
  if (audio_state_ != AudioState::kAwaitingReader) {
    return;  // Superseded: playback was stopped or the pair changed
  }

  const std::optional<size_t> pair = selectedAudioPair();
  if (!reader || !pair.has_value()) {
    ORC_LOG_DEBUG("PreviewDialog: No playable audio for the selected pair");
    status_bar_->showMessage(
        "The selected channel pair has no playable audio; playing video only",
        4000);
    beginVideoOnlyPlayback();
    return;
  }

  audio_reader_ = std::move(reader);
  audio_reader_pair_ = pair;
  beginAudioPrime();
}

void PreviewDialog::beginAudioPrime() {
  audio_state_ = AudioState::kPriming;
  ++audio_prime_generation_;
  const uint64_t generation = audio_prime_generation_;

  audio_prime_state_ = std::make_shared<AudioPrimeProgressState>();
  auto reader = audio_reader_;
  auto state = audio_prime_state_;

  // The decode runs off the GUI thread (and off the render worker, which it
  // would otherwise block for its whole duration). Progress is published into
  // the shared state and polled here, so the worker never touches a Qt object.
  // The thread owns a share of the reader, so it stays valid even if the
  // dialogue abandons the wait.
  auto* thread = QThread::create([reader, state]() {
    try {
      reader->prime(
          [state](uint64_t done, uint64_t total, const std::string& message) {
            const std::lock_guard<std::mutex> lock(state->mutex);
            state->done = done;
            state->total = total;
            state->message = message;
          });
    } catch (const std::exception& e) {
      ORC_LOG_ERROR("PreviewDialog: Audio priming failed: {}", e.what());
    } catch (...) {
      ORC_LOG_ERROR("PreviewDialog: Audio priming failed");
    }
  });
  connect(thread, &QThread::finished, this,
          [this, generation]() { finishAudioPrime(generation); });
  connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  thread->start();

  audio_prime_show_timer_->start();
}

void PreviewDialog::finishAudioPrime(uint64_t generation) {
  if (generation != audio_prime_generation_ ||
      audio_state_ != AudioState::kPriming) {
    return;  // The wait was abandoned; this decode's result is nobody's
  }

  endAudioPreparation();
  audio_controller_->setReader(audio_reader_);
  beginAudioPlayback();
}

void PreviewDialog::beginAudioPlayback() {
  const uint64_t frame = orc::gui::preview_audio::frameForPreviewIndex(
      static_cast<uint64_t>(std::max(0, currentIndex())),
      audio_controller_->itemsPerFrame());

  if (!audio_controller_->start(frame)) {
    ORC_LOG_WARN("PreviewDialog: Audio playback could not start at frame {}",
                 frame);
    status_bar_->showMessage("Audio could not be started; playing video only",
                             4000);
    beginVideoOnlyPlayback();
    return;
  }

  audio_state_ = AudioState::kPlaying;
  is_playing_ = true;
  play_pause_button_->setText("⏸");  // ⏸ pause symbol
  status_bar_->clearMessage();
  playback_timer_->start();
}

void PreviewDialog::onPlaybackTick() {
  if (audio_state_ == AudioState::kPlaying) {
    if (!audio_controller_->isPlaying()) {
      stopPlayback();  // The device stopped under us
      return;
    }

    // Frames the renderer could not deliver in time are skipped simply by the
    // target having moved on by more than one; the in-flight gate above this
    // dialogue coalesces the rest.
    const uint64_t target = std::min<uint64_t>(
        audio_controller_->targetPreviewIndex(),
        static_cast<uint64_t>(std::max(0, preview_slider_->maximum())));
    const int target_index = static_cast<int>(target);
    if (target_index != currentIndex()) {
      chase_navigation_ = true;
      navigateToIndex(target_index);
      chase_navigation_ = false;
    }
    return;
  }

  const int next = currentIndex() + 1;
  if (next > preview_slider_->maximum()) {
    stopPlayback();
    return;
  }
  navigateToIndex(next);
}

void PreviewDialog::suspendAudioForScrub() {
  if (chase_navigation_ || audio_state_ != AudioState::kPlaying) {
    return;
  }
  // A drag emits a new position per mouse move, and rebasing the device on
  // each one would thrash it. The clock instead goes quiet for the duration of
  // the scrub and is restarted once the position settles; the chase stops with
  // it so it cannot pull the preview off the position being dragged to.
  playback_timer_->stop();
  audio_controller_->stop();
}

void PreviewDialog::resumeAudioAtCurrentIndex() {
  if (chase_navigation_ || audio_state_ != AudioState::kPlaying) {
    return;
  }
  // Restarting the device at the settled position rebases the clock by
  // construction, so no correction term is ever carried across a seek.
  beginAudioPlayback();
}

void PreviewDialog::showAudioPrimeDialog() {
  if (audio_state_ != AudioState::kPriming || audio_prime_dialog_) {
    return;
  }

  // No cancel button: the decode runs inside a producer's std::call_once and
  // nothing in that path is interruptible, so offering Cancel would be a lie.
  auto* dialog = new QProgressDialog("Preparing audio…", QString(), 0, 0, this);
  dialog->setWindowTitle("Preview Audio");
  dialog->setWindowModality(Qt::WindowModal);
  dialog->setCancelButton(nullptr);
  dialog->setAutoClose(false);
  dialog->setAutoReset(false);
  dialog->setAttribute(Qt::WA_DeleteOnClose, false);
  // QProgressDialog shows itself from an internal timer once minimumDuration
  // elapses, which would race this one. Push it out of reach and reset() (which
  // stops that timer) so the show decision stays here.
  dialog->setMinimumDuration(std::numeric_limits<int>::max());
  dialog->reset();
  audio_prime_dialog_ = dialog;

  updateAudioPrimeDialog();
  dialog->show();
  audio_prime_tick_timer_->start();
}

void PreviewDialog::updateAudioPrimeDialog() {
  if (!audio_prime_dialog_ || !audio_prime_state_) {
    return;
  }

  uint64_t done = 0;
  uint64_t total = 0;
  std::string message;
  {
    const std::lock_guard<std::mutex> lock(audio_prime_state_->mutex);
    done = audio_prime_state_->done;
    total = audio_prime_state_->total;
    message = audio_prime_state_->message;
  }

  audio_prime_dialog_->setLabelText(
      message.empty() ? QString("Preparing audio…")
                      : QString("Preparing audio… %1")
                            .arg(QString::fromStdString(message)));

  // A producer that cannot size the work reports total 0; the dialog then
  // stays a busy indicator (range 0..0) rather than claiming a percentage.
  if (total > 0) {
    audio_prime_dialog_->setRange(0, 100);
    audio_prime_dialog_->setValue(
        static_cast<int>(std::min<uint64_t>(99, done * 100 / total)));
  }
}

void PreviewDialog::endAudioPreparation() {
  // Bumping the generation orphans any prime still running: it finishes,
  // releases its share of the reader and self-deletes with nobody listening.
  ++audio_prime_generation_;
  audio_prime_state_.reset();
  audio_prime_show_timer_->stop();
  audio_prime_tick_timer_->stop();

  if (audio_prime_dialog_) {
    audio_prime_dialog_->hide();
    audio_prime_dialog_->deleteLater();
    audio_prime_dialog_ = nullptr;
  }

  if (audio_state_ == AudioState::kAwaitingReader ||
      audio_state_ == AudioState::kPriming) {
    audio_state_ = AudioState::kIdle;
    status_bar_->clearMessage();
  }
}

void PreviewDialog::setAvailablePreviewViews(
    const std::vector<orc::PreviewViewDescriptor>& views) {
  available_preview_view_ids_.clear();
  for (const auto& view : views) {
    available_preview_view_ids_.insert(view.id);
  }

  const bool line_scope_available = hasAvailablePreviewView(kLineScopeViewId);
  const bool frame_timing_available =
      hasAvailablePreviewView(kFrameTimingViewId);
  const bool waveform_monitor_available =
      hasAvailablePreviewView(kWaveformMonitorViewId);
  const bool component_vectorscope_available =
      hasAvailablePreviewView(kComponentVectorscopeViewId);

  if (show_frame_timing_action_) {
    show_frame_timing_action_->setVisible(frame_timing_available);
    show_frame_timing_action_->setEnabled(frame_timing_available);
  }

  if (show_waveform_monitor_action_) {
    show_waveform_monitor_action_->setVisible(waveform_monitor_available);
    show_waveform_monitor_action_->setEnabled(waveform_monitor_available);
  }

  if (!line_scope_available) {
    line_scope_open_requested_ = false;
    if (frame_scope_dialog_ && frame_scope_dialog_->isVisible()) {
      frame_scope_dialog_->close();
    }
  }

  if (!frame_timing_available) {
    frame_timing_open_requested_ = false;
    if (frame_timing_dialog_ && frame_timing_dialog_->isVisible()) {
      frame_timing_dialog_->close();
    }
  }

  if (!waveform_monitor_available) {
    waveform_monitor_open_requested_ = false;
    if (waveform_monitor_dialog_ && waveform_monitor_dialog_->isVisible()) {
      waveform_monitor_dialog_->close();
    }
  }

  if (show_component_vectorscope_action_) {
    show_component_vectorscope_action_->setVisible(
        component_vectorscope_available);
    show_component_vectorscope_action_->setEnabled(
        component_vectorscope_available);
  }

  if (!component_vectorscope_available) {
    closeVectorscopeDialogs();
  }

  const bool histogram_available = hasAvailablePreviewView(kHistogramViewId);
  if (show_histogram_action_) {
    show_histogram_action_->setVisible(histogram_available);
    show_histogram_action_->setEnabled(histogram_available);
  }
  if (!histogram_available) {
    closeHistogramDialog();
  }
}

bool PreviewDialog::hasAvailablePreviewView(const std::string& view_id) const {
  return available_preview_view_ids_.find(view_id) !=
         available_preview_view_ids_.end();
}

void PreviewDialog::setSharedPreviewCoordinate(
    const orc::PreviewCoordinate& coordinate) {
  if (!coordinate.is_valid()) {
    return;
  }

  shared_preview_coordinate_ = coordinate;
  if (vectorscope_dialog_) {
    vectorscope_dialog_->applyAcquisitionTo(*shared_preview_coordinate_);
  }
  emit previewCoordinateChanged(*shared_preview_coordinate_);
}

void PreviewDialog::showVectorscopeForNode(orc::NodeID node_id) {
  if (!node_id.is_valid()) {
    return;
  }

  if (!vectorscope_dialog_) {
    vectorscope_dialog_ = new VectorscopeDialog(this);
    vectorscope_dialog_->setAttribute(Qt::WA_DeleteOnClose, false);

    connect(vectorscope_dialog_, &VectorscopeDialog::dataRefreshRequested, this,
            [this]() {
              if (!current_node_id_.is_valid()) {
                return;
              }

              orc::PreviewCoordinate coordinate;
              if (shared_preview_coordinate_.has_value() &&
                  shared_preview_coordinate_->is_valid()) {
                coordinate = *shared_preview_coordinate_;
              } else {
                coordinate.field_index = static_cast<uint64_t>(currentIndex());
              }

              vectorscope_dialog_->applyAcquisitionTo(coordinate);
              emit vectorscopeRequested(coordinate);
            });

    connect(vectorscope_dialog_, &QObject::destroyed, this, [this]() {
      vectorscope_dialog_ = nullptr;
      vectorscope_node_id_ = orc::NodeID{};
    });
  }

  vectorscope_dialog_->setScopeLabel(QStringLiteral("Vectorscope"));
  vectorscope_dialog_->setAcquisitionMode(vectorscope_acquisition_mode_);

  vectorscope_node_id_ = node_id;
  vectorscope_dialog_->setStage(node_id);
  vectorscope_dialog_->show();
  vectorscope_dialog_->raise();
  vectorscope_dialog_->activateWindow();
}

void PreviewDialog::updateVectorscope(
    orc::NodeID node_id, const std::optional<orc::VectorscopeData>& data) {
  Q_UNUSED(node_id);

  if (!vectorscope_dialog_) {
    return;
  }

  if (!vectorscope_dialog_->isVisible()) {
    return;
  }

  if (data.has_value()) {
    vectorscope_dialog_->updateVectorscope(*data);
  } else {
    vectorscope_dialog_->clearDisplay();
  }
}

bool PreviewDialog::isVectorscopeVisibleForNode(orc::NodeID node_id) const {
  Q_UNUSED(node_id);
  return vectorscope_dialog_ && vectorscope_dialog_->isVisible();
}

void PreviewDialog::applyVectorscopeAcquisition(
    orc::PreviewCoordinate& coordinate) const {
  if (!vectorscope_dialog_) {
    return;
  }
  vectorscope_dialog_->applyAcquisitionTo(coordinate);
}

void PreviewDialog::setVectorscopeAcquisitionMode(
    orc::VectorscopeAcquisitionMode mode) {
  vectorscope_acquisition_mode_ = mode;
  if (vectorscope_dialog_) {
    vectorscope_dialog_->setAcquisitionMode(mode);
  }
}

orc::VectorscopeAcquisitionMode PreviewDialog::vectorscopeAcquisitionMode()
    const {
  if (!vectorscope_dialog_) {
    return orc::VectorscopeAcquisitionMode::DecodedComponent;
  }
  return vectorscope_dialog_->acquisitionMode();
}

void PreviewDialog::onSampleMarkerMoved(int sample_x) {
  // Emit signal for MainWindow to update cross-hairs
  // MainWindow has the context to map sample_x properly
  emit sampleMarkerMovedInLineScope(sample_x);
}

void PreviewDialog::closeEvent(QCloseEvent* event) {
  stopPlayback();
  closeChildDialogs();
  QDialog::closeEvent(event);
}

void PreviewDialog::closeChildDialogs() {
  // Drop every open-intent first: a request may still be in flight for a
  // dialog that is not yet visible, and its response must not open a window
  // after the preview has gone.
  line_scope_open_requested_ = false;
  frame_timing_open_requested_ = false;
  waveform_monitor_open_requested_ = false;

  if (frame_scope_dialog_ && frame_scope_dialog_->isVisible()) {
    frame_scope_dialog_->close();
  }

  if (frame_timing_dialog_ && frame_timing_dialog_->isVisible()) {
    frame_timing_dialog_->close();
  }

  if (waveform_monitor_dialog_ && waveform_monitor_dialog_->isVisible()) {
    waveform_monitor_dialog_->close();
  }

  closeVectorscopeDialogs();
  closeHistogramDialog();

  // Disable cross-hairs when closing
  if (preview_widget_) {
    preview_widget_->setCrosshairsEnabled(false);
  }
}

void PreviewDialog::forwardAmplitudeUnit(orc::AmplitudeDisplayUnit unit) {
  if (frame_scope_dialog_) {
    frame_scope_dialog_->setAmplitudeUnit(unit);
  }
  if (frame_timing_dialog_) {
    frame_timing_dialog_->setAmplitudeUnit(unit);
  }
  if (waveform_monitor_dialog_) {
    waveform_monitor_dialog_->setAmplitudeUnit(unit);
  }
}

void PreviewDialog::closeVectorscopeDialogs() {
  if (vectorscope_dialog_) {
    vectorscope_dialog_->close();
  }
  vectorscope_node_id_ = orc::NodeID{};
}

void PreviewDialog::showHistogramForNode(orc::NodeID node_id) {
  if (!node_id.is_valid()) {
    return;
  }

  if (!histogram_dialog_) {
    histogram_dialog_ = new HistogramDialog(this);
    histogram_dialog_->setAttribute(Qt::WA_DeleteOnClose, false);

    connect(histogram_dialog_, &QObject::destroyed, this, [this]() {
      histogram_dialog_ = nullptr;
      histogram_node_id_ = orc::NodeID{};
    });
  }

  histogram_node_id_ = node_id;
  histogram_dialog_->show();
  histogram_dialog_->raise();
  histogram_dialog_->activateWindow();
}

void PreviewDialog::updateHistogram(
    orc::NodeID node_id, const std::optional<orc::VideoHistogramData>& data) {
  Q_UNUSED(node_id);

  if (!histogram_dialog_ || !histogram_dialog_->isVisible()) {
    return;
  }

  if (data.has_value()) {
    histogram_dialog_->updateHistogram(*data);
  } else {
    histogram_dialog_->clearDisplay();
  }
}

bool PreviewDialog::isHistogramVisibleForNode(orc::NodeID node_id) const {
  Q_UNUSED(node_id);
  return histogram_dialog_ && histogram_dialog_->isVisible();
}

void PreviewDialog::closeHistogramDialog() {
  if (histogram_dialog_) {
    histogram_dialog_->close();
  }
  histogram_node_id_ = orc::NodeID{};
}

bool PreviewDialog::isLineScopeVisible() const {
  return frame_scope_dialog_ && frame_scope_dialog_->isVisible();
}
void PreviewDialog::showLineScope(
    const QString& node_id, int stage_index, uint64_t field_index,
    int line_number, int sample_x, const std::vector<int16_t>& samples,
    const std::optional<orc::presenters::VideoParametersView>& video_params,
    int preview_image_width, int original_sample_x, int original_image_y,
    orc::PreviewOutputType /*preview_mode*/,
    const std::vector<int16_t>& y_samples,
    const std::vector<int16_t>& c_samples) {
  if (frame_scope_dialog_) {
    // Line samples arrive asynchronously. If the user closed the scope while
    // a request was in flight (easily done during playback, which re-requests
    // on every frame), the response must not resurrect the window.
    if (!frame_scope_dialog_->isVisible() && !line_scope_open_requested_) {
      return;
    }

    current_line_scope_preview_width_ = preview_image_width;
    current_line_scope_samples_count_ = static_cast<int>(samples.size());
    if (current_line_scope_samples_count_ == 0 && !y_samples.empty()) {
      current_line_scope_samples_count_ = static_cast<int>(y_samples.size());
    }

    connect(frame_scope_dialog_, &FrameScopeDialog::refreshRequested, this,
            &PreviewDialog::lineScopeRequested, Qt::UniqueConnection);

    connect(frame_scope_dialog_, &FrameScopeDialog::sampleMarkerMoved, this,
            &PreviewDialog::onSampleMarkerMoved, Qt::UniqueConnection);

    if (samples.empty() && y_samples.empty() && c_samples.empty()) {
      preview_widget_->setCrosshairsEnabled(false);
    } else {
      preview_widget_->setCrosshairsEnabled(true);
    }

    // FrameScopeDialog identifies the line by frame and frame-flat line
    // (docs/gui-user-guide/dialogues/line-scope.md), but the response
    // addresses it as a sequential field plus a line within that field.
    //
    // The frame holding a field is field_index / 2. The line has to be rebased
    // onto the frame-flat numbering orc::make_line_label() expects - field 2's
    // block follows field 1's - or both fields of a frame report the same
    // label and adjacent preview rows look identical.
    const uint64_t frame_id = field_index / 2;
    const int field_line = std::max(0, line_number - 1);
    const size_t frame_line = video_params.has_value()
                                  ? orc::gui::frameFlatLineIndex(
                                        field_index, field_line,
                                        toOrcVideoSystem(video_params->system))
                                  : static_cast<size_t>(field_line);

    frame_scope_dialog_->setFrameLineSamples(
        node_id, stage_index, frame_id, frame_line, sample_x, samples,
        video_params, preview_image_width, original_sample_x, original_image_y,
        y_samples, c_samples);

    const bool was_visible = frame_scope_dialog_->isVisible();
    if (!was_visible) {
      const int margin = 12;
      const QRect preview_geom = frameGeometry();
      QSize scope_size = frame_scope_dialog_->size();
      const QSize hint_size = frame_scope_dialog_->sizeHint();
      if (hint_size.isValid()) {
        scope_size = hint_size;
      }

      QScreen* screen = this->screen();
      if (!screen) {
        screen = QGuiApplication::primaryScreen();
      }
      QRect screen_geom = screen ? screen->availableGeometry() : QRect();

      int target_x = preview_geom.right() + margin;
      int target_y = preview_geom.top();

      auto clamp = [](int value, int min_v, int max_v) {
        return std::max(min_v, std::min(value, max_v));
      };

      if (screen) {
        const int screen_left = screen_geom.left();
        const int screen_top = screen_geom.top();
        const int screen_right =
            screen_geom.left() + screen_geom.width() - scope_size.width();
        const int screen_bottom =
            screen_geom.top() + screen_geom.height() - scope_size.height();

        const int right_x = preview_geom.right() + margin;
        const int left_x = preview_geom.left() - margin - scope_size.width();

        if (right_x <= screen_right) {
          target_x = right_x;
        } else if (left_x >= screen_left) {
          target_x = left_x;
        } else {
          target_x = clamp(preview_geom.right() - scope_size.width(),
                           screen_left, screen_right);
        }

        target_y = clamp(target_y, screen_top, screen_bottom);
      }

      frame_scope_dialog_->move(target_x, target_y);
      frame_scope_dialog_->show();
    }

    if (!was_visible) {
      frame_scope_dialog_->raise();
    }
  }
}
void PreviewDialog::notifyFrameChanged() { emit previewFrameChanged(); }

void PreviewDialog::onComponentVectorscopeActionTriggered() {
  if (!hasAvailablePreviewView(kComponentVectorscopeViewId) ||
      !current_node_id_.is_valid()) {
    return;
  }

  showVectorscopeForNode(current_node_id_);

  orc::PreviewCoordinate coordinate;
  if (shared_preview_coordinate_.has_value() &&
      shared_preview_coordinate_->is_valid()) {
    coordinate = *shared_preview_coordinate_;
  } else {
    coordinate.field_index = static_cast<uint64_t>(currentIndex());
    coordinate.line_index = 0;
    coordinate.sample_offset = 0;
  }

  if (vectorscope_dialog_) {
    vectorscope_dialog_->applyAcquisitionTo(coordinate);
  }

  emit vectorscopeRequested(coordinate);
}

void PreviewDialog::onHistogramActionTriggered() {
  if (!hasAvailablePreviewView(kHistogramViewId) ||
      !current_node_id_.is_valid()) {
    return;
  }

  showHistogramForNode(current_node_id_);

  orc::PreviewCoordinate coordinate;
  if (shared_preview_coordinate_.has_value() &&
      shared_preview_coordinate_->is_valid()) {
    coordinate = *shared_preview_coordinate_;
  } else {
    coordinate.field_index = static_cast<uint64_t>(currentIndex());
    coordinate.line_index = 0;
    coordinate.sample_offset = 0;
  }

  emit histogramRequested(coordinate);
}
