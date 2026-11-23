/*
 * File:        preview_compare_window.cpp
 * Module:      orc-gui
 * Purpose:     A/B Compare window: flicker two nodes' previews in place so
 *              small differences show, and export the pair as a WebP
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_compare_window.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "stage_help_dialog.h"

namespace orc::gui {

namespace {

// The flicker intervals offered, in milliseconds; 0 is "Off".
struct IntervalChoice {
  const char* label;
  uint32_t interval_ms;
};
constexpr IntervalChoice kIntervalChoices[] = {
    {"0.5 s", 500}, {"0.8 s", 800}, {"1 s", 1000}, {"Off", 0}};

// The zoom levels offered: powers of two, so every sample is a whole block
// of pixels and the zoom is a scale the export can repeat.
constexpr int kZoomLevels[] = {1, 2, 4, 8};

// The interval an export uses when the flicker is off: the CLI's default.
constexpr uint32_t kExportIntervalWhenOff = 800;

// Gain range: the CLI accepts 0 – 64, where 0 means "no stretched half";
// the window's Normal view is how that is asked for here.
constexpr double kMinGain = 0.5;
constexpr double kMaxGain = 64.0;
constexpr int kMinWindow = 1;
constexpr int kMaxWindow = 255;

// Pan step of the scroll bars, in widget pixels.
constexpr int kPanStep = 16;

// Where the A/B label sits over the picture.
constexpr int kSideLabelMargin = 8;

const char* const kHelpResource = ":/orc-gui/docs/preview_window.md";

// Quote one shell word, POSIX style, when it needs it.
QString shellWord(const QString& word) {
  static const QString kSafe = QStringLiteral(
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
      "-_./,:=+@%");
  const bool safe =
      !word.isEmpty() && std::all_of(word.begin(), word.end(),
                                     [](QChar c) { return kSafe.contains(c); });
  if (safe) {
    return word;
  }
  QString quoted = word;
  quoted.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
  return QStringLiteral("'") + quoted + QStringLiteral("'");
}

}  // namespace

// ===========================================================================
// CompareViewport
// ===========================================================================

CompareViewport::CompareViewport(QWidget* parent)
    : orc::view::FrameViewportWidget(parent) {
  setZoomRange(kMinZoom, kMaxZoom);
  setZoomLevel(kMinZoom);
  setSmoothScaling(false);
  setFocusPolicy(Qt::StrongFocus);
}

void CompareViewport::wheelEvent(QWheelEvent* event) {
  if (!hasImage() || !event->modifiers().testFlag(Qt::ControlModifier)) {
    orc::view::FrameViewportWidget::wheelEvent(event);
    return;
  }
  const int steps = event->angleDelta().y();
  if (steps == 0) {
    event->accept();
    return;
  }
  const double old_zoom = zoomLevel();
  const double new_zoom = std::clamp(
      steps > 0 ? old_zoom * 2.0 : old_zoom / 2.0, kMinZoom, kMaxZoom);
  if (new_zoom != old_zoom) {
    // The sample under the cursor is the one that must not move.
    const QPoint cursor = event->position().toPoint();
    const QPoint old_origin = visibleOrigin();
    setZoomLevel(new_zoom);
    setVisibleOrigin(orc::view::FrameViewGeometry::scrollAfterZoom(
        old_origin, cursor, new_zoom / old_zoom));
  }
  event->accept();
}

// ===========================================================================
// PreviewCompareWindow
// ===========================================================================

const QString& PreviewCompareWindow::helpHeading() {
  static const QString kHeading = QStringLiteral("A/B Compare");
  return kHeading;
}

PreviewCompareWindow::PreviewCompareWindow(RenderCoordinator* coordinator,
                                           QWidget* parent)
    : QWidget(parent, Qt::Window), coordinator_(coordinator) {
  setWindowTitle(QStringLiteral("A/B Compare"));
  setupUi();

  flicker_timer_ = new QTimer(this);
  connect(flicker_timer_, &QTimer::timeout, this,
          &PreviewCompareWindow::onTimerTick);

  if (coordinator_ != nullptr) {
    connect(coordinator_, &RenderCoordinator::availableOutputsReady, this,
            &PreviewCompareWindow::onAvailableOutputs, Qt::QueuedConnection);
    connect(coordinator_, &RenderCoordinator::comparisonReady, this,
            &PreviewCompareWindow::onComparisonReady, Qt::QueuedConnection);
    connect(coordinator_, &RenderCoordinator::comparisonExported, this,
            &PreviewCompareWindow::onComparisonExported, Qt::QueuedConnection);
  }

  updateFlickerControls();
  updateSideLabel();
  updateCommandLine();
}

PreviewCompareWindow::~PreviewCompareWindow() = default;

void PreviewCompareWindow::setupUi() {
  auto* layout = new QVBoxLayout(this);

  // --- Nodes ---------------------------------------------------------------
  auto* nodes_row = new QHBoxLayout();
  node_a_combo_ = new QComboBox();
  node_b_combo_ = new QComboBox();
  node_a_combo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  node_b_combo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  node_a_combo_->setToolTip(
      QStringLiteral("Node A: the stage shown as A. Only stages that offer "
                     "the preview's output and option are listed."));
  node_b_combo_->setToolTip(
      QStringLiteral("Node B: the stage compared with A. Only stages that "
                     "offer the preview's output and option are listed."));
  swap_button_ = new QPushButton(QStringLiteral("Swap"));
  swap_button_->setToolTip(QStringLiteral("Swap nodes A and B"));
  nodes_row->addWidget(new QLabel(QStringLiteral("A:")));
  nodes_row->addWidget(node_a_combo_, 1);
  nodes_row->addWidget(swap_button_);
  nodes_row->addWidget(new QLabel(QStringLiteral("B:")));
  nodes_row->addWidget(node_b_combo_, 1);
  layout->addLayout(nodes_row);

  // --- Flicker and view ----------------------------------------------------
  auto* view_row = new QHBoxLayout();
  show_a_button_ = new QPushButton(QStringLiteral("A"));
  show_b_button_ = new QPushButton(QStringLiteral("B"));
  show_a_button_->setToolTip(QStringLiteral("Show A and hold (A key)"));
  show_b_button_->setToolTip(QStringLiteral("Show B and hold (B key)"));
  // The label says what the picture is doing, Animate or Hold, and is
  // set with the tooltip in updateFlickerControls().
  hold_button_ = new QPushButton(QStringLiteral("Animate"));
  hold_button_->setCheckable(true);
  interval_combo_ = new QComboBox();
  for (const auto& choice : kIntervalChoices) {
    interval_combo_->addItem(QString::fromLatin1(choice.label),
                             QVariant::fromValue<uint>(choice.interval_ms));
  }
  interval_combo_->setCurrentIndex(interval_combo_->findData(
      QVariant::fromValue<uint>(ABFlickerState::kDefaultIntervalMs)));
  interval_combo_->setToolTip(
      QStringLiteral("How long each side is shown; Off stops the flicker"));

  zoom_combo_ = new QComboBox();
  for (int level : kZoomLevels) {
    zoom_combo_->addItem(QStringLiteral("%1×").arg(level), level);
  }
  zoom_combo_->setToolTip(
      QStringLiteral("Zoom, nearest-neighbour (Ctrl+wheel also zooms)"));

  view_mode_combo_ = new QComboBox();
  view_mode_combo_->addItem(
      QStringLiteral("Normal"),
      static_cast<int>(orc::PreviewCompareViewMode::Normal));
  view_mode_combo_->addItem(
      QStringLiteral("Stretched"),
      static_cast<int>(orc::PreviewCompareViewMode::Stretched));
  view_mode_combo_->addItem(
      QStringLiteral("Normal + Stretched"),
      static_cast<int>(orc::PreviewCompareViewMode::NormalAndStretched));
  view_mode_combo_->addItem(
      QStringLiteral("Difference"),
      static_cast<int>(orc::PreviewCompareViewMode::Difference));
  view_mode_combo_->setToolTip(
      QStringLiteral("Stretched amplifies each sample's distance from A's "
                     "local median; Difference shows B − A about mid "
                     "grey"));

  gain_spin_ = new QDoubleSpinBox();
  gain_spin_->setRange(kMinGain, kMaxGain);
  gain_spin_->setSingleStep(0.5);
  gain_spin_->setDecimals(1);
  gain_spin_->setValue(orc::kPreviewCompareStretchGain);
  gain_spin_->setToolTip(
      QStringLiteral("Gain of the stretch, and of the difference"));
  window_spin_ = new QSpinBox();
  window_spin_->setRange(kMinWindow, kMaxWindow);
  window_spin_->setSingleStep(2);
  window_spin_->setValue(static_cast<int>(orc::kPreviewCompareStretchWindow));
  window_spin_->setToolTip(
      QStringLiteral("Size of the stretch's median window, in samples (odd)"));

  view_row->addWidget(show_a_button_);
  view_row->addWidget(show_b_button_);
  view_row->addWidget(hold_button_);
  view_row->addWidget(new QLabel(QStringLiteral("Interval:")));
  view_row->addWidget(interval_combo_);
  view_row->addWidget(new QLabel(QStringLiteral("Zoom:")));
  view_row->addWidget(zoom_combo_);
  view_row->addWidget(new QLabel(QStringLiteral("View:")));
  view_row->addWidget(view_mode_combo_);
  view_row->addWidget(new QLabel(QStringLiteral("Gain:")));
  view_row->addWidget(gain_spin_);
  view_row->addWidget(new QLabel(QStringLiteral("Window:")));
  view_row->addWidget(window_spin_);
  view_row->addStretch(1);
  layout->addLayout(view_row);

  // --- Picture with its pan bars -------------------------------------------
  auto* view_container = new QWidget();
  auto* view_grid = new QGridLayout(view_container);
  view_grid->setContentsMargins(0, 0, 0, 0);
  view_grid->setSpacing(0);
  viewport_ = new CompareViewport();
  viewport_->setMinimumSize(480, 320);
  horizontal_pan_bar_ = new QScrollBar(Qt::Horizontal);
  vertical_pan_bar_ = new QScrollBar(Qt::Vertical);
  horizontal_pan_bar_->setVisible(false);
  vertical_pan_bar_->setVisible(false);
  view_grid->addWidget(viewport_, 0, 0);
  view_grid->addWidget(vertical_pan_bar_, 0, 1);
  view_grid->addWidget(horizontal_pan_bar_, 1, 0);
  view_grid->setRowStretch(0, 1);
  view_grid->setColumnStretch(0, 1);
  layout->addWidget(view_container, 1);

  // Drawn over the picture so the side shown is never in doubt, whatever
  // the zoom.
  side_label_ = new QLabel(viewport_);
  side_label_->setObjectName(QStringLiteral("compareSideLabel"));
  side_label_->setStyleSheet(QStringLiteral(
      "QLabel { background-color: rgba(0, 0, 0, 170); color: white; "
      "padding: 3px 6px; font-weight: bold; }"));
  side_label_->setAttribute(Qt::WA_TransparentForMouseEvents);
  side_label_->move(kSideLabelMargin, kSideLabelMargin);
  side_label_->raise();

  // --- Status, messages and export -----------------------------------------
  status_label_ = new QLabel();
  layout->addWidget(status_label_);
  message_label_ = new QLabel();
  message_label_->setWordWrap(true);
  message_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(message_label_);

  auto* export_row = new QHBoxLayout();
  command_line_edit_ = new QLineEdit();
  command_line_edit_->setReadOnly(true);
  command_line_edit_->setToolTip(
      QStringLiteral("The orc-cli command that writes the same WebP as "
                     "Export WebP..."));
  export_button_ = new QPushButton(QStringLiteral("Export WebP..."));
  help_button_ = new QPushButton(QStringLiteral("Help"));
  export_row->addWidget(new QLabel(QStringLiteral("CLI:")));
  export_row->addWidget(command_line_edit_, 1);
  export_row->addWidget(export_button_);
  export_row->addWidget(help_button_);
  layout->addLayout(export_row);

  resize(960, 720);

  // --- Behaviour -----------------------------------------------------------
  connect(node_a_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int) {
            if (const auto request = currentRequest()) {
              preferred_a_ = request->node_a;
            }
            updateSideLabel();
            requestRender();
          });
  connect(node_b_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int) {
            updateSideLabel();
            requestRender();
          });
  connect(swap_button_, &QPushButton::clicked, this,
          &PreviewCompareWindow::swapNodes);
  connect(show_a_button_, &QPushButton::clicked, this,
          [this]() { showSide(ABSide::A); });
  connect(show_b_button_, &QPushButton::clicked, this,
          [this]() { showSide(ABSide::B); });
  connect(hold_button_, &QPushButton::clicked, this,
          [this](bool) { toggleHold(); });
  connect(interval_combo_, qOverload<int>(&QComboBox::currentIndexChanged),
          this, [this](int) {
            flicker_.setIntervalMs(interval_combo_->currentData().toUInt());
            updateFlickerControls();
            syncTimer();
            updateCommandLine();
          });
  connect(zoom_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int) {
            viewport_->setZoomLevel(zoom_combo_->currentData().toInt());
          });
  connect(viewport_, &orc::view::FrameViewportWidget::zoomChanged, this,
          [this](double zoom) {
            const int index =
                zoom_combo_->findData(static_cast<int>(std::lround(zoom)));
            if (index >= 0) {
              const QSignalBlocker block(zoom_combo_);
              zoom_combo_->setCurrentIndex(index);
            }
            updateCommandLine();
          });
  connect(view_mode_combo_, qOverload<int>(&QComboBox::currentIndexChanged),
          this, [this](int) {
            updateFlickerControls();
            updateSideLabel();
            requestRender();
          });
  connect(gain_spin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double) { requestRender(); });
  connect(window_spin_, qOverload<int>(&QSpinBox::valueChanged), this,
          [this](int value) {
            // The median window has a centre sample, so it is odd.
            if (value % 2 == 0) {
              window_spin_->setValue(std::min(value + 1, kMaxWindow));
              return;
            }
            requestRender();
          });
  connect(export_button_, &QPushButton::clicked, this,
          &PreviewCompareWindow::onExportClicked);
  connect(help_button_, &QPushButton::clicked, this,
          &PreviewCompareWindow::onHelpClicked);

  // The bars drive the viewport's origin; the viewport sets their ranges.
  connect(viewport_, &orc::view::FrameViewportWidget::panRangeChanged, this,
          [this](QSize max_origin) {
            const QSignalBlocker block_h(horizontal_pan_bar_);
            const QSignalBlocker block_v(vertical_pan_bar_);
            horizontal_pan_bar_->setRange(0, max_origin.width());
            horizontal_pan_bar_->setPageStep(viewport_->width());
            horizontal_pan_bar_->setSingleStep(kPanStep);
            vertical_pan_bar_->setRange(0, max_origin.height());
            vertical_pan_bar_->setPageStep(viewport_->height());
            vertical_pan_bar_->setSingleStep(kPanStep);
            horizontal_pan_bar_->setVisible(max_origin.width() > 0);
            vertical_pan_bar_->setVisible(max_origin.height() > 0);
            updateCommandLine();
          });
  connect(viewport_, &orc::view::FrameViewportWidget::visibleOriginChanged,
          this, [this](QPoint origin) {
            const QSignalBlocker block_h(horizontal_pan_bar_);
            const QSignalBlocker block_v(vertical_pan_bar_);
            horizontal_pan_bar_->setValue(origin.x());
            vertical_pan_bar_->setValue(origin.y());
            updateCommandLine();
          });
  connect(horizontal_pan_bar_, &QScrollBar::valueChanged, this,
          [this](int value) {
            viewport_->setVisibleOrigin(
                QPoint(value, viewport_->visibleOrigin().y()));
          });
  connect(vertical_pan_bar_, &QScrollBar::valueChanged, this,
          [this](int value) {
            viewport_->setVisibleOrigin(
                QPoint(viewport_->visibleOrigin().x(), value));
          });

  // Window-wide keys. A spin box that has focus still takes what is typed
  // into it: it claims printable keys before shortcuts see them.
  auto* hold_key = new QShortcut(QKeySequence(Qt::Key_Space), this);
  // They do what the buttons do, so they stop where the buttons are disabled.
  connect(hold_key, &QShortcut::activated, this, [this]() {
    if (hold_button_->isEnabled()) {
      toggleHold();
    }
  });
  auto* a_key = new QShortcut(QKeySequence(Qt::Key_A), this);
  connect(a_key, &QShortcut::activated, this, [this]() {
    if (show_a_button_->isEnabled()) {
      showSide(ABSide::A);
    }
  });
  auto* b_key = new QShortcut(QKeySequence(Qt::Key_B), this);
  connect(b_key, &QShortcut::activated, this, [this]() {
    if (show_b_button_->isEnabled()) {
      showSide(ABSide::B);
    }
  });
}

// --- Context from the owner -------------------------------------------------

void PreviewCompareWindow::setNodes(std::vector<CompareNodeChoice> nodes) {
  nodes_ = std::move(nodes);
  queries_stale_ = true;
  if (isVisible()) {
    startNodeQueries();
  }
}

void PreviewCompareWindow::setOutput(orc::PreviewOutputType type,
                                     const std::string& option_id,
                                     const QString& output_name) {
  const bool changed = type != output_type_ || option_id != option_id_;
  output_type_ = type;
  option_id_ = option_id;
  output_name_ = output_name;
  if (!changed) {
    return;
  }
  // The answers already in hand say which nodes offer the new output too.
  // The request changes with the output, so the rebuild asks for the render.
  rebuildNodeCombos();
}

void PreviewCompareWindow::setIndex(uint64_t index) {
  if (index == index_) {
    return;
  }
  index_ = index;
  requestRender();
}

void PreviewCompareWindow::setPreferredNodeA(orc::NodeID node) {
  preferred_a_ = node;
  const int index = node_a_combo_->findData(node.value());
  if (index >= 0) {
    node_a_combo_->setCurrentIndex(index);
  }
}

void PreviewCompareWindow::setProjectPath(const QString& path) {
  project_path_ = path;
  updateCommandLine();
}

void PreviewCompareWindow::refresh() {
  queries_stale_ = true;
  delivery_.reset();
  if (isVisible()) {
    startNodeQueries();
    requestRender();
  }
}

// --- Asking which nodes offer the output ------------------------------------

void PreviewCompareWindow::startNodeQueries() {
  queries_stale_ = false;
  node_outputs_.clear();
  nodes_to_query_.clear();
  for (const auto& node : nodes_) {
    nodes_to_query_.push_back(node.node_id);
  }
  // The preview's node first: it is A, and the window can render as soon as
  // one other node has answered.
  if (preferred_a_) {
    auto it = std::find(nodes_to_query_.begin(), nodes_to_query_.end(),
                        *preferred_a_);
    if (it != nodes_to_query_.end()) {
      nodes_to_query_.erase(it);
      nodes_to_query_.push_front(*preferred_a_);
    }
  }
  pending_outputs_request_ = 0;
  rebuildNodeCombos();
  queryNextNode();
}

void PreviewCompareWindow::queryNextNode() {
  if (coordinator_ == nullptr || nodes_to_query_.empty()) {
    pending_outputs_request_ = 0;
    status_label_->clear();
    return;
  }
  const orc::NodeID node = nodes_to_query_.front();
  nodes_to_query_.pop_front();
  status_label_->setText(
      QStringLiteral("Checking which stages offer this output (%1 of %2)...")
          .arg(nodes_.size() - nodes_to_query_.size())
          .arg(nodes_.size()));
  pending_outputs_request_ = coordinator_->requestAvailableOutputs(node);
  // The answer does not say which node it is for.
  node_in_flight_ = node;
}

void PreviewCompareWindow::onAvailableOutputs(
    uint64_t request_id, std::vector<orc::PreviewOutputInfo> outputs) {
  // Shared with the main window, which asks the same question for its node.
  if (request_id == 0 || request_id != pending_outputs_request_) {
    return;
  }
  node_outputs_[node_in_flight_.value()] = std::move(outputs);
  rebuildNodeCombos();
  queryNextNode();
}

bool PreviewCompareWindow::nodeOffersOutput(orc::NodeID node) const {
  const auto it = node_outputs_.find(node.value());
  if (it == node_outputs_.end()) {
    return false;
  }
  return std::any_of(it->second.begin(), it->second.end(),
                     [this](const orc::PreviewOutputInfo& output) {
                       return output.is_available && output.count > 0 &&
                              output.type == output_type_ &&
                              output.option_id == option_id_;
                     });
}

void PreviewCompareWindow::rebuildNodeCombos() {
  const auto before = currentRequest();
  const QVariant old_a = node_a_combo_->currentData();
  const QVariant old_b = node_b_combo_->currentData();

  {
    const QSignalBlocker block_a(node_a_combo_);
    const QSignalBlocker block_b(node_b_combo_);
    node_a_combo_->clear();
    node_b_combo_->clear();
    for (const auto& node : nodes_) {
      if (!nodeOffersOutput(node.node_id)) {
        continue;
      }
      const QString text = QStringLiteral("%1 (%2)").arg(
          node.label, QString::number(node.node_id.value()));
      node_a_combo_->addItem(text, node.node_id.value());
      node_b_combo_->addItem(text, node.node_id.value());
    }

    int a_index = -1;
    if (preferred_a_) {
      a_index = node_a_combo_->findData(preferred_a_->value());
    }
    if (a_index < 0 && old_a.isValid()) {
      a_index = node_a_combo_->findData(old_a);
    }
    if (a_index < 0 && node_a_combo_->count() > 0) {
      a_index = 0;
    }
    node_a_combo_->setCurrentIndex(a_index);

    int b_index = old_b.isValid() ? node_b_combo_->findData(old_b) : -1;
    if (b_index < 0) {
      // The first node that is not A, so a new window compares two nodes.
      for (int i = 0; i < node_b_combo_->count(); ++i) {
        if (i != a_index) {
          b_index = i;
          break;
        }
      }
    }
    node_b_combo_->setCurrentIndex(b_index);
  }

  const bool can_pick = node_a_combo_->count() > 0;
  node_a_combo_->setEnabled(can_pick);
  node_b_combo_->setEnabled(can_pick);
  swap_button_->setEnabled(node_a_combo_->count() > 1);

  if (node_a_combo_->count() == 0 && nodes_to_query_.empty() &&
      pending_outputs_request_ == 0 && !node_outputs_.empty()) {
    setMessage(QStringLiteral("No stage offers the preview's output (%1).")
                   .arg(output_name_),
               true);
  }

  updateSideLabel();
  if (currentRequest() != before) {
    requestRender();
  }
}

// --- Rendering --------------------------------------------------------------

std::optional<orc::PreviewCompareRequest> PreviewCompareWindow::currentRequest()
    const {
  if (node_a_combo_->currentIndex() < 0 || node_b_combo_->currentIndex() < 0) {
    return std::nullopt;
  }
  orc::PreviewCompareRequest request;
  request.node_a = orc::NodeID(node_a_combo_->currentData().toInt());
  request.node_b = orc::NodeID(node_b_combo_->currentData().toInt());
  request.output_type = output_type_;
  request.option_id = option_id_;
  request.index = index_;
  return request;
}

orc::PreviewCompareViewMode PreviewCompareWindow::viewMode() const {
  return static_cast<orc::PreviewCompareViewMode>(
      view_mode_combo_->currentData().toInt());
}

orc::PreviewCompareViewSettings PreviewCompareWindow::viewSettings() const {
  orc::PreviewCompareViewSettings settings;
  settings.mode = viewMode();
  settings.stretch_window = static_cast<uint32_t>(window_spin_->value());
  settings.stretch_gain = gain_spin_->value();
  settings.difference_gain = gain_spin_->value();
  return settings;
}

void PreviewCompareWindow::requestRender() {
  if (coordinator_ == nullptr || !isVisible()) {
    return;
  }
  const auto request = currentRequest();
  if (!request) {
    pending_compare_request_ = 0;
    return;
  }
  pending_compare_request_ =
      coordinator_->requestComparison(*request, viewSettings());
  status_label_->setText(QStringLiteral("Rendering..."));
}

void PreviewCompareWindow::onComparisonReady(uint64_t request_id,
                                             ComparisonDeliveryPtr delivery) {
  if (request_id != pending_compare_request_ || !delivery) {
    return;
  }
  pending_compare_request_ = 0;
  if (pending_outputs_request_ == 0) {
    status_label_->clear();
  }

  if (!delivery->ok()) {
    const std::string& error = delivery->result.success
                                   ? delivery->view.error
                                   : delivery->result.error;
    setMessage(QString::fromStdString(error), true);
    delivery_.reset();
    viewport_->clearImage();
    syncTimer();
    updateCommandLine();
    return;
  }

  // A new size (another mode, another stage) is a new picture: start it at
  // the top left rather than wherever the old one was panned to.
  const bool reset_origin = !delivery_ || delivery_->view_settings.mode !=
                                              delivery->view_settings.mode;
  delivery_ = std::move(delivery);
  if (reset_origin) {
    viewport_->setVisibleOrigin(QPoint(0, 0));
  }

  QStringList warnings;
  for (const auto& warning : delivery_->result.warnings) {
    warnings << QString::fromStdString(warning);
  }
  setMessage(warnings.join(QStringLiteral("\n")), false);

  showCurrentImage();
  syncTimer();
  updateCommandLine();
}

void PreviewCompareWindow::showCurrentImage() {
  if (!delivery_) {
    return;
  }
  if (delivery_->view_settings.mode ==
      orc::PreviewCompareViewMode::Difference) {
    viewport_->setImage(delivery_->difference);
  } else {
    viewport_->setImage(flicker_.shownSide() == ABSide::A ? delivery_->image_a
                                                          : delivery_->image_b);
  }
  updateSideLabel();
}

// --- Flicker ----------------------------------------------------------------

void PreviewCompareWindow::syncTimer() {
  const bool flickering =
      delivery_ && flicker_.isRunning() &&
      delivery_->view_settings.mode != orc::PreviewCompareViewMode::Difference;
  if (!flickering) {
    flicker_timer_->stop();
    return;
  }
  const int interval = static_cast<int>(flicker_.intervalMs());
  if (!flicker_timer_->isActive() || flicker_timer_->interval() != interval) {
    flicker_timer_->start(interval);
  }
}

void PreviewCompareWindow::onTimerTick() {
  if (flicker_.tick()) {
    showCurrentImage();
  }
}

void PreviewCompareWindow::toggleHold() {
  flicker_.toggleHold();
  updateFlickerControls();
  syncTimer();
}

void PreviewCompareWindow::showSide(ABSide side) {
  flicker_.show(side);
  updateFlickerControls();
  syncTimer();
  showCurrentImage();
  updateSideLabel();
}

void PreviewCompareWindow::swapNodes() {
  const int a_index = node_a_combo_->currentIndex();
  const int b_index = node_b_combo_->currentIndex();
  if (a_index < 0 || b_index < 0) {
    return;
  }
  // The pair already shown is the same pair swapped, so it is swapped here
  // and the flicker follows the picture; the request that follows is for
  // the swapped pair, which the presenter renders afresh.
  {
    const QSignalBlocker block_a(node_a_combo_);
    const QSignalBlocker block_b(node_b_combo_);
    node_a_combo_->setCurrentIndex(b_index);
    node_b_combo_->setCurrentIndex(a_index);
  }
  if (const auto request = currentRequest()) {
    preferred_a_ = request->node_a;
  }
  if (delivery_) {
    auto swapped = std::make_shared<ComparisonDelivery>(*delivery_);
    std::swap(swapped->image_a, swapped->image_b);
    std::swap(swapped->result.request.node_a, swapped->result.request.node_b);
    delivery_ = std::move(swapped);
  }
  flicker_.swap();
  showCurrentImage();
  requestRender();
}

bool PreviewCompareWindow::viewShowsSides() const {
  return viewMode() != orc::PreviewCompareViewMode::Difference;
}

void PreviewCompareWindow::updateFlickerControls() {
  const bool sides = viewShowsSides();
  show_a_button_->setEnabled(sides);
  show_b_button_->setEnabled(sides);
  interval_combo_->setEnabled(sides);

  const QSignalBlocker block(hold_button_);
  const bool animating = !flicker_.isHeld() && flicker_.intervalMs() != 0;
  hold_button_->setChecked(!animating);
  hold_button_->setEnabled(sides && flicker_.intervalMs() != 0);
  hold_button_->setText(animating ? QStringLiteral("Animate")
                                  : QStringLiteral("Hold"));
  hold_button_->setToolTip(
      animating ? QStringLiteral("Switching between A and B. Click or press "
                                 "Space to hold on the side shown.")
                : QStringLiteral("Held on the side shown. Click or press "
                                 "Space to switch between A and B again."));
}

void PreviewCompareWindow::updateSideLabel() {
  const auto request = currentRequest();
  if (!request) {
    side_label_->hide();
    return;
  }
  QString text;
  if (viewMode() == orc::PreviewCompareViewMode::Difference) {
    text = QStringLiteral("B − A  ×%1").arg(gain_spin_->value(), 0, 'g', 3);
  } else if (flicker_.shownSide() == ABSide::A) {
    text = QStringLiteral("A: ") + labelFor(request->node_a);
  } else {
    text = QStringLiteral("B: ") + labelFor(request->node_b);
  }
  side_label_->setText(text);
  side_label_->adjustSize();
  side_label_->show();
  side_label_->raise();
}

QString PreviewCompareWindow::labelFor(orc::NodeID node) const {
  for (const auto& choice : nodes_) {
    if (choice.node_id == node) {
      return choice.label;
    }
  }
  return QString::number(node.value());
}

// --- Export -----------------------------------------------------------------

std::optional<QRect> PreviewCompareWindow::visibleNativeRect() const {
  if (!delivery_ || !viewport_->hasImage() || delivery_->native_width == 0 ||
      delivery_->native_height == 0) {
    return std::nullopt;
  }
  const int native_w = static_cast<int>(delivery_->native_width);
  const int native_h = static_cast<int>(delivery_->native_height);
  const QSize shown = viewport_->imageSize();

  const QPointF top_left = viewport_->imageFromWidget(QPointF(0, 0));
  const QPointF bottom_right = viewport_->imageFromWidget(
      QPointF(viewport_->width(), viewport_->height()));
  const int x0 = std::max(0, static_cast<int>(std::floor(top_left.x())));
  const int y0 = std::max(0, static_cast<int>(std::floor(top_left.y())));
  const int x1 =
      std::min(shown.width(), static_cast<int>(std::ceil(bottom_right.x())));
  const int y1 =
      std::min(shown.height(), static_cast<int>(std::ceil(bottom_right.y())));
  if (x1 <= x0 || y1 <= y0) {
    return std::nullopt;
  }

  int left = x0;
  int right = x1;
  if (shown.width() > native_w) {
    // Normal + Stretched: [normal | gap | stretched]. A column of either half
    // is the same sample of the render; the crop covers every sample seen in
    // either.
    const int gap = shown.width() - 2 * native_w;
    const int stretched_start = native_w + gap;
    left = native_w;
    right = 0;
    if (x0 < native_w) {
      left = std::min(left, x0);
      right = std::max(right, std::min(x1, native_w));
    }
    if (x1 > stretched_start) {
      left = std::min(left, std::max(x0, stretched_start) - stretched_start);
      right = std::max(right, x1 - stretched_start);
    }
    if (right <= left) {
      return std::nullopt;
    }
  }
  const QRect rect(left, y0, right - left, y1 - y0);
  return rect.intersected(QRect(0, 0, native_w, native_h));
}

orc::PreviewCompareExportSettings PreviewCompareWindow::exportSettings() const {
  orc::PreviewCompareExportSettings settings;
  if (const auto crop = visibleNativeRect()) {
    settings.crop_mode = orc::PreviewCompareCropMode::Explicit;
    settings.crop_x = static_cast<uint32_t>(crop->x());
    settings.crop_y = static_cast<uint32_t>(crop->y());
    settings.crop_width = static_cast<uint32_t>(crop->width());
    settings.crop_height = static_cast<uint32_t>(crop->height());
  }
  // The export always has the normal half; every view but Normal adds the
  // stretched one.
  settings.stretch = viewMode() != orc::PreviewCompareViewMode::Normal;
  settings.stretch_window = static_cast<uint32_t>(window_spin_->value());
  settings.stretch_gain = gain_spin_->value();
  settings.scale = static_cast<uint32_t>(
      std::clamp(static_cast<int>(std::lround(viewport_->zoomLevel())),
                 static_cast<int>(CompareViewport::kMinZoom),
                 static_cast<int>(CompareViewport::kMaxZoom)));
  settings.interval_ms = flicker_.intervalMs() != 0 ? flicker_.intervalMs()
                                                    : kExportIntervalWhenOff;
  return settings;
}

QString PreviewCompareWindow::commandLine(const QString& output_path) const {
  const auto request = currentRequest();
  if (!request) {
    return QString();
  }
  const auto settings = exportSettings();
  const char* type_name =
      orc::preview_output_type_command_name(request->output_type);

  QStringList words;
  words << QStringLiteral("orc-cli") << QStringLiteral("preview")
        << QStringLiteral("compare")
        << shellWord(project_path_.isEmpty() ? QStringLiteral("project.orcprj")
                                             : project_path_)
        << QStringLiteral("--a") << QString::number(request->node_a.value())
        << QStringLiteral("--b") << QString::number(request->node_b.value())
        << QStringLiteral("--frame") << QString::number(request->index + 1);
  if (type_name != nullptr) {
    words << QStringLiteral("--output-type") << QString::fromLatin1(type_name);
  }
  if (!request->option_id.empty()) {
    words << QStringLiteral("--option")
          << shellWord(QString::fromStdString(request->option_id));
  }
  if (settings.crop_mode == orc::PreviewCompareCropMode::Explicit) {
    words << QStringLiteral("--crop")
          << QStringLiteral("%1,%2,%3,%4")
                 .arg(settings.crop_x)
                 .arg(settings.crop_y)
                 .arg(settings.crop_width)
                 .arg(settings.crop_height);
  }
  if (settings.stretch) {
    words << QStringLiteral("--stretch")
          << QString::number(settings.stretch_gain, 'g', 6)
          << QStringLiteral("--stretch-window")
          << QString::number(settings.stretch_window);
  } else {
    words << QStringLiteral("--stretch") << QStringLiteral("0");
  }
  words << QStringLiteral("--scale") << QString::number(settings.scale)
        << QStringLiteral("--interval") << QString::number(settings.interval_ms)
        << QStringLiteral("-o") << shellWord(output_path);
  return words.join(QLatin1Char(' '));
}

void PreviewCompareWindow::updateCommandLine() {
  const QString path = pending_export_path_.isEmpty()
                           ? QStringLiteral("ab.webp")
                           : pending_export_path_;
  command_line_edit_->setText(commandLine(path));
  export_button_->setEnabled(delivery_ != nullptr);
}

void PreviewCompareWindow::onExportClicked() {
  QString path = QFileDialog::getSaveFileName(
      this, QStringLiteral("Export A/B Comparison"),
      pending_export_path_.isEmpty() ? QStringLiteral("ab.webp")
                                     : pending_export_path_,
      QStringLiteral("WebP images (*.webp)"));
  if (path.isEmpty()) {
    return;
  }
  if (QFileInfo(path).suffix().isEmpty()) {
    path += QStringLiteral(".webp");
  }
  exportTo(path);
}

void PreviewCompareWindow::exportTo(const QString& path) {
  const auto request = currentRequest();
  if (coordinator_ == nullptr || !request || !delivery_) {
    setMessage(QStringLiteral("Pick nodes A and B before exporting."), true);
    return;
  }
  pending_export_path_ = path;
  pending_export_request_ = coordinator_->requestComparisonExport(
      *request, exportSettings(), path.toStdString());
  updateCommandLine();
  status_label_->setText(QStringLiteral("Exporting %1...").arg(path));
}

void PreviewCompareWindow::onComparisonExported(
    uint64_t request_id, orc::PreviewCompareExportResult result) {
  if (request_id != pending_export_request_) {
    return;
  }
  pending_export_request_ = 0;
  status_label_->clear();
  if (!result.success) {
    setMessage(QStringLiteral("The WebP was not written: %1")
                   .arg(QString::fromStdString(result.error)),
               true);
    return;
  }
  QString text = QStringLiteral(
                     "Wrote %1: two frames of %2 × %3, crop "
                     "%4,%5,%6,%7.")
                     .arg(pending_export_path_)
                     .arg(result.frame_width)
                     .arg(result.frame_height)
                     .arg(result.crop_x)
                     .arg(result.crop_y)
                     .arg(result.crop_width)
                     .arg(result.crop_height);
  if (result.identical) {
    text += QStringLiteral(
        " A and B are identical inside the crop; the frames differ only in "
        "their labels.");
  }
  for (const auto& warning : result.warnings) {
    text += QLatin1Char('\n') + QString::fromStdString(warning);
  }
  setMessage(text, false);
}

void PreviewCompareWindow::setMessage(const QString& text, bool is_error) {
  message_label_->setText(text);
  message_label_->setProperty("isError", is_error);
  // Errors in the theme's own warning colour would need the theme here; a
  // bold face is enough to tell them from warnings and notes.
  QFont font = message_label_->font();
  font.setBold(is_error);
  message_label_->setFont(font);
}

void PreviewCompareWindow::onHelpClicked() {
  QFile file(QString::fromLatin1(kHelpResource));
  const QString markdown = file.open(QIODevice::ReadOnly)
                               ? QString::fromUtf8(file.readAll())
                               : QString();
  auto* dialog =
      new StageHelpDialog(QStringLiteral("Preview Window"), markdown, this);
  dialog->show();
  dialog->scrollToHeading(helpHeading());
  emit helpShown();
}

// --- Window events ----------------------------------------------------------

void PreviewCompareWindow::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (queries_stale_) {
    startNodeQueries();
  }
  requestRender();
  viewport_->setFocus();
}

void PreviewCompareWindow::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  flicker_timer_->stop();
  // Answers that arrive while hidden are dropped; showing again asks anew.
  pending_compare_request_ = 0;
  if (pending_outputs_request_ != 0) {
    pending_outputs_request_ = 0;
    nodes_to_query_.clear();
    queries_stale_ = true;
  }
}

void PreviewCompareWindow::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  updateCommandLine();
}

}  // namespace orc::gui
