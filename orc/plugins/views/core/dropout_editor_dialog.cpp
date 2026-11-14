/*
 * File:        dropout_editor_dialog.cpp
 * Module:      orc-view_core
 * Purpose:     Dropout map editor dialog implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dropout_editor_dialog.h"

#include <orc/view-support/overlay_primitive_builder.h>

#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {

// Overlay colours shared with the legend and region table. Source dropouts
// use the same red family as the preview dialog's dropout overlay.
const QColor kSourceColor(255, 48, 48);
const QColor kAdditionColor(48, 200, 48);
const QColor kRemovalColor(160, 160, 160);

// Interaction geometry (widget-space pixels)
constexpr double kHitSlop = 2.0;
/// Widget pixels an arrow key or a pan-bar step moves the view.
constexpr int kPanStep = 50;
constexpr int kMoveThreshold = 3;

bool sameRegion(const dropout_editor::Region& a,
                const dropout_editor::Region& b) {
  return a == b;
}

/// The frame as an image the view can draw; null when the reply's pixels do
/// not match its size.
QImage frameImage(const orc::view::ToolFrameImage& frame) {
  const auto width = static_cast<int>(frame.width);
  const auto height = static_cast<int>(frame.height);
  const size_t row_bytes = static_cast<size_t>(width) * 3;
  if (width <= 0 || height <= 0 ||
      frame.rgb.size() != row_bytes * static_cast<size_t>(height)) {
    return {};
  }
  QImage image(width, height, QImage::Format_RGB888);
  for (int y = 0; y < height; ++y) {
    std::memcpy(image.scanLine(y),
                frame.rgb.data() + row_bytes * static_cast<size_t>(y),
                row_bytes);
  }
  return image;
}

/// The overlay builder's view of a region. Drawing and hit-testing both go
/// through it, so a band that is drawn and a band that can be grabbed cannot
/// drift apart.
orc::view::OverlaySpan spanOf(const dropout_editor::Region& r) {
  return orc::view::OverlaySpan{static_cast<int>(r.line),
                                static_cast<int>(r.start_sample),
                                static_cast<int>(r.end_sample)};
}

QRectF leftHandleRect(const QRectF& band) {
  return orc::view::OverlayPrimitiveBuilder::leftHandleRect(band);
}

QRectF rightHandleRect(const QRectF& band) {
  return orc::view::OverlayPrimitiveBuilder::rightHandleRect(band);
}

}  // namespace

// ============================================================================
// DropoutFrameView Implementation
// ============================================================================

DropoutFrameView::DropoutFrameView(QWidget* parent)
    : FrameViewportWidget(parent) {
  setZoomRange(0.25, 8.0);
}

void DropoutFrameView::setFrame(
    const QImage& frame_image,
    const std::vector<dropout_editor::Region>& source_dropouts,
    const std::vector<dropout_editor::Region>& additions,
    const std::vector<dropout_editor::Region>& removals) {
  source_dropouts_ = source_dropouts;
  additions_ = additions;
  removals_ = removals;
  selected_kind_ = RegionKind::None;
  selected_index_ = -1;
  hover_ = Hit{};
  drag_mode_ = DragMode::None;
  setImage(frame_image);
}

void DropoutFrameView::updateRegions(
    const std::vector<dropout_editor::Region>& additions,
    const std::vector<dropout_editor::Region>& removals) {
  additions_ = additions;
  removals_ = removals;

  // The dialog resyncs the selection after edits; drop it locally when the
  // index no longer exists so painting never dereferences a stale index.
  if ((selected_kind_ == RegionKind::Addition &&
       selected_index_ >= static_cast<int>(additions_.size())) ||
      (selected_kind_ == RegionKind::OrphanRemoval &&
       selected_index_ >= static_cast<int>(removals_.size()))) {
    selected_kind_ = RegionKind::None;
    selected_index_ = -1;
  }
  hover_ = Hit{};
  refreshOverlay();
}

void DropoutFrameView::setSelectedRegion(RegionKind kind, int index) {
  selected_kind_ = kind;
  selected_index_ = index;
  refreshOverlay();
}

bool DropoutFrameView::isOrphanRemoval(int removal_index) const {
  return removal_index >= 0 &&
         removal_index < static_cast<int>(removals_.size()) &&
         !removalHasSource(removals_[removal_index]);
}

bool DropoutFrameView::isRegionMarkedForRemoval(
    const dropout_editor::Region& region) const {
  return std::any_of(removals_.begin(), removals_.end(),
                     [&region](const dropout_editor::Region& removal) {
                       return sameRegion(removal, region);
                     });
}

bool DropoutFrameView::removalHasSource(
    const dropout_editor::Region& removal) const {
  return std::any_of(source_dropouts_.begin(), source_dropouts_.end(),
                     [&removal](const dropout_editor::Region& source) {
                       return sameRegion(source, removal);
                     });
}

QRectF DropoutFrameView::regionBandRect(const dropout_editor::Region& region,
                                        bool emphasized) const {
  return orc::view::OverlayPrimitiveBuilder::regionBandRect(
      viewGeometry(), spanOf(region), emphasized);
}

void DropoutFrameView::appendRegionBand(orc::view::OverlayPrimitives& out,
                                        const dropout_editor::Region& region,
                                        const QColor& color, bool emphasized,
                                        bool struck) const {
  orc::view::OverlayPrimitiveBuilder::appendRegionBand(
      out, viewGeometry(), spanOf(region), color, emphasized, struck);
}

void DropoutFrameView::buildOverlay(orc::view::OverlayPrimitives& out) const {
  // Source dropouts (red); those marked for removal render struck-through in
  // the removal colour.
  for (size_t i = 0; i < source_dropouts_.size(); ++i) {
    const auto& region = source_dropouts_[i];
    const bool emphasized = (selected_kind_ == RegionKind::Source &&
                             selected_index_ == static_cast<int>(i)) ||
                            (hover_.kind == RegionKind::Source &&
                             hover_.index == static_cast<int>(i));
    if (isRegionMarkedForRemoval(region)) {
      appendRegionBand(out, region, kRemovalColor, emphasized, true);
    } else {
      appendRegionBand(out, region, kSourceColor, emphasized, false);
    }
  }

  // Orphan removal entries (no matching source dropout)
  for (size_t i = 0; i < removals_.size(); ++i) {
    if (removalHasSource(removals_[i])) {
      continue;  // Already drawn from the source list above
    }
    const bool emphasized = (selected_kind_ == RegionKind::OrphanRemoval &&
                             selected_index_ == static_cast<int>(i)) ||
                            (hover_.kind == RegionKind::OrphanRemoval &&
                             hover_.index == static_cast<int>(i));
    appendRegionBand(out, removals_[i], kRemovalColor, emphasized, true);
  }

  // Additions (green, on top)
  for (size_t i = 0; i < additions_.size(); ++i) {
    const bool emphasized = (selected_kind_ == RegionKind::Addition &&
                             selected_index_ == static_cast<int>(i)) ||
                            (hover_.kind == RegionKind::Addition &&
                             hover_.index == static_cast<int>(i));
    appendRegionBand(out, additions_[i], kAdditionColor, emphasized, false);
  }

  // Resize handles for the selected addition or source dropout (sources
  // marked removed are not resizable).
  if (const auto* handle_region = resizableSelectedRegion()) {
    orc::view::OverlayPrimitiveBuilder::appendResizeHandles(
        out, regionBandRect(*handle_region, true));
  }

  // In-progress source-resize preview: drawn in the addition colour because
  // releasing converts the resized extent into an addition.
  if (drag_mode_ == DragMode::ResizingSourceLeft ||
      drag_mode_ == DragMode::ResizingSourceRight) {
    appendRegionBand(out, source_resize_preview_, kAdditionColor, true, false);
  }

  // In-progress add-drag preview
  if (drag_mode_ == DragMode::Adding) {
    dropout_editor::Region preview;
    preview.line = static_cast<uint32_t>(drag_start_image_.y());
    preview.start_sample = static_cast<uint32_t>(
        std::min(drag_start_image_.x(), drag_current_image_.x()));
    preview.end_sample = static_cast<uint32_t>(
        std::max(drag_start_image_.x(), drag_current_image_.x()) + 1);
    appendRegionBand(out, preview, kAdditionColor, true, false);
  }
}

const dropout_editor::Region* DropoutFrameView::resizableSelectedRegion()
    const {
  if (selected_index_ < 0) {
    return nullptr;
  }
  if (selected_kind_ == RegionKind::Addition &&
      selected_index_ < static_cast<int>(additions_.size())) {
    return &additions_[selected_index_];
  }
  if (selected_kind_ == RegionKind::Source &&
      selected_index_ < static_cast<int>(source_dropouts_.size()) &&
      !isRegionMarkedForRemoval(source_dropouts_[selected_index_])) {
    return &source_dropouts_[selected_index_];
  }
  return nullptr;
}

DropoutFrameView::Hit DropoutFrameView::hitTest(
    const QPointF& widget_pos) const {
  Hit hit;

  // Resize handles of the selected region take priority.
  if (const auto* handle_region = resizableSelectedRegion()) {
    const QRectF band = regionBandRect(*handle_region, true);
    if (!band.isEmpty()) {
      const double slop = kHitSlop;
      if (leftHandleRect(band)
              .adjusted(-slop, -slop, slop, slop)
              .contains(widget_pos)) {
        return Hit{selected_kind_, selected_index_, Hit::Part::LeftHandle};
      }
      if (rightHandleRect(band)
              .adjusted(-slop, -slop, slop, slop)
              .contains(widget_pos)) {
        return Hit{selected_kind_, selected_index_, Hit::Part::RightHandle};
      }
    }
  }

  // Additions are drawn on top, so hit-test them first (topmost = last).
  for (int i = static_cast<int>(additions_.size()) - 1; i >= 0; --i) {
    const QRectF band = regionBandRect(additions_[i], true);
    if (!band.isEmpty() &&
        band.adjusted(-kHitSlop, -kHitSlop, kHitSlop, kHitSlop)
            .contains(widget_pos)) {
      return Hit{RegionKind::Addition, i, Hit::Part::Body};
    }
  }

  for (int i = static_cast<int>(removals_.size()) - 1; i >= 0; --i) {
    if (removalHasSource(removals_[i])) {
      continue;
    }
    const QRectF band = regionBandRect(removals_[i], true);
    if (!band.isEmpty() &&
        band.adjusted(-kHitSlop, -kHitSlop, kHitSlop, kHitSlop)
            .contains(widget_pos)) {
      return Hit{RegionKind::OrphanRemoval, i, Hit::Part::Body};
    }
  }

  for (int i = static_cast<int>(source_dropouts_.size()) - 1; i >= 0; --i) {
    const QRectF band = regionBandRect(source_dropouts_[i], true);
    if (!band.isEmpty() &&
        band.adjusted(-kHitSlop, -kHitSlop, kHitSlop, kHitSlop)
            .contains(widget_pos)) {
      return Hit{RegionKind::Source, i, Hit::Part::Body};
    }
  }

  return hit;
}

void DropoutFrameView::selectFromInteraction(RegionKind kind, int index) {
  if (selected_kind_ == kind && selected_index_ == index) {
    return;
  }
  selected_kind_ = kind;
  selected_index_ = index;
  refreshOverlay();
  Q_EMIT selectionChanged(kind, index);
}

void DropoutFrameView::mousePressEvent(QMouseEvent* event) {
  if (!hasImage()) {
    return;
  }

  if (event->button() == Qt::RightButton) {
    const Hit hit = hitTest(event->position());
    if (hit.kind != RegionKind::None) {
      selectFromInteraction(hit.kind, hit.index);
    }
    Q_EMIT contextMenuRequested(hit.kind, hit.index,
                                event->globalPosition().toPoint());
    return;
  }

  if (event->button() != Qt::LeftButton) {
    return;
  }

  const Hit hit = hitTest(event->position());
  const QPoint image_pos = imagePixelFromWidget(event->pos());

  if (hit.kind == RegionKind::Addition) {
    selectFromInteraction(RegionKind::Addition, hit.index);
    drag_index_ = hit.index;
    drag_original_ = additions_[hit.index];
    drag_start_image_ = image_pos;
    drag_start_widget_ = event->position();
    if (hit.part == Hit::Part::LeftHandle) {
      drag_mode_ = DragMode::ResizingLeft;
    } else if (hit.part == Hit::Part::RightHandle) {
      drag_mode_ = DragMode::ResizingRight;
    } else {
      drag_mode_ = DragMode::PendingMove;
    }
    return;
  }

  if (hit.kind == RegionKind::Source || hit.kind == RegionKind::OrphanRemoval) {
    selectFromInteraction(hit.kind, hit.index);
    if (hit.kind == RegionKind::Source &&
        (hit.part == Hit::Part::LeftHandle ||
         hit.part == Hit::Part::RightHandle)) {
      // Resize-by-replacement: track the new extent as a preview; the source
      // list stays untouched until the dialog applies the edit.
      drag_index_ = hit.index;
      drag_original_ = source_dropouts_[hit.index];
      source_resize_preview_ = drag_original_;
      drag_start_image_ = image_pos;
      drag_start_widget_ = event->position();
      drag_mode_ = hit.part == Hit::Part::LeftHandle
                       ? DragMode::ResizingSourceLeft
                       : DragMode::ResizingSourceRight;
    }
    return;
  }

  // Empty area: clear selection and start dragging out a new region.
  selectFromInteraction(RegionKind::None, -1);
  drag_mode_ = DragMode::Adding;
  drag_start_image_ = image_pos;
  drag_current_image_ = image_pos;
  refreshOverlay();
}

void DropoutFrameView::mouseMoveEvent(QMouseEvent* event) {
  if (!hasImage()) {
    return;
  }

  const QPoint image_pos = imagePixelFromWidget(event->pos());
  const int width = imageSize().width();
  const int height = imageSize().height();

  switch (drag_mode_) {
    case DragMode::Adding:
      drag_current_image_ = image_pos;
      refreshOverlay();
      return;

    case DragMode::PendingMove:
      if ((event->position() - drag_start_widget_).manhattanLength() <=
          kMoveThreshold) {
        return;
      }
      drag_mode_ = DragMode::Moving;
      [[fallthrough]];

    case DragMode::Moving: {
      if (drag_index_ < 0 ||
          drag_index_ >= static_cast<int>(additions_.size())) {
        return;
      }
      const int dy = image_pos.y() - drag_start_image_.y();
      const int dx = image_pos.x() - drag_start_image_.x();
      const int length = static_cast<int>(drag_original_.end_sample -
                                          drag_original_.start_sample);
      const int line =
          std::clamp(static_cast<int>(drag_original_.line) + dy, 0, height - 1);
      const int start =
          std::clamp(static_cast<int>(drag_original_.start_sample) + dx, 0,
                     width - length);
      auto& region = additions_[drag_index_];
      region.line = static_cast<uint32_t>(line);
      region.start_sample = static_cast<uint32_t>(start);
      region.end_sample = static_cast<uint32_t>(start + length);
      refreshOverlay();
      return;
    }

    case DragMode::ResizingLeft: {
      if (drag_index_ < 0 ||
          drag_index_ >= static_cast<int>(additions_.size())) {
        return;
      }
      auto& region = additions_[drag_index_];
      const int start = std::clamp(
          image_pos.x(), 0, static_cast<int>(drag_original_.end_sample) - 1);
      region.start_sample = static_cast<uint32_t>(start);
      region.end_sample = drag_original_.end_sample;
      refreshOverlay();
      return;
    }

    case DragMode::ResizingRight: {
      if (drag_index_ < 0 ||
          drag_index_ >= static_cast<int>(additions_.size())) {
        return;
      }
      auto& region = additions_[drag_index_];
      const int end =
          std::clamp(image_pos.x() + 1,
                     static_cast<int>(drag_original_.start_sample) + 1, width);
      region.start_sample = drag_original_.start_sample;
      region.end_sample = static_cast<uint32_t>(end);
      refreshOverlay();
      return;
    }

    case DragMode::ResizingSourceLeft: {
      const int start = std::clamp(
          image_pos.x(), 0, static_cast<int>(drag_original_.end_sample) - 1);
      source_resize_preview_ = drag_original_;
      source_resize_preview_.start_sample = static_cast<uint32_t>(start);
      refreshOverlay();
      return;
    }

    case DragMode::ResizingSourceRight: {
      const int end =
          std::clamp(image_pos.x() + 1,
                     static_cast<int>(drag_original_.start_sample) + 1, width);
      source_resize_preview_ = drag_original_;
      source_resize_preview_.end_sample = static_cast<uint32_t>(end);
      refreshOverlay();
      return;
    }

    case DragMode::None:
      updateHoverState(event->position());
      return;
  }
}

void DropoutFrameView::updateHoverState(const QPointF& widget_pos) {
  const Hit hit = hitTest(widget_pos);
  if (hit.kind != hover_.kind || hit.index != hover_.index ||
      hit.part != hover_.part) {
    hover_ = hit;
    updateCursorShape();
    refreshOverlay();
  }
}

void DropoutFrameView::updateCursorShape() {
  if ((hover_.kind == RegionKind::Addition ||
       hover_.kind == RegionKind::Source) &&
      (hover_.part == Hit::Part::LeftHandle ||
       hover_.part == Hit::Part::RightHandle)) {
    setCursor(Qt::SizeHorCursor);
  } else if (hover_.kind == RegionKind::Addition) {
    setCursor(Qt::SizeAllCursor);
  } else if (hover_.kind != RegionKind::None) {
    setCursor(Qt::PointingHandCursor);
  } else {
    setCursor(Qt::CrossCursor);
  }
}

void DropoutFrameView::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    return;
  }

  const DragMode finished_mode = drag_mode_;
  drag_mode_ = DragMode::None;

  switch (finished_mode) {
    case DragMode::Adding: {
      const int line = drag_start_image_.y();
      const int start_sample =
          std::min(drag_start_image_.x(), drag_current_image_.x());
      const int end_sample =
          std::max(drag_start_image_.x(), drag_current_image_.x());
      refreshOverlay();
      if (end_sample > start_sample) {
        dropout_editor::Region region;
        region.line = static_cast<uint32_t>(line);
        region.start_sample = static_cast<uint32_t>(start_sample);
        region.end_sample = static_cast<uint32_t>(end_sample);
        Q_EMIT regionAddRequested(region);
      }
      return;
    }

    case DragMode::Moving:
    case DragMode::ResizingLeft:
    case DragMode::ResizingRight: {
      if (drag_index_ >= 0 &&
          drag_index_ < static_cast<int>(additions_.size()) &&
          !sameRegion(additions_[drag_index_], drag_original_)) {
        Q_EMIT additionModifyRequested(drag_index_, additions_[drag_index_]);
      }
      return;
    }

    case DragMode::ResizingSourceLeft:
    case DragMode::ResizingSourceRight: {
      refreshOverlay();  // Clear the preview band
      if (drag_index_ >= 0 &&
          drag_index_ < static_cast<int>(source_dropouts_.size()) &&
          !sameRegion(source_resize_preview_, drag_original_)) {
        Q_EMIT sourceResizeRequested(drag_index_, source_resize_preview_);
      }
      return;
    }

    case DragMode::PendingMove:
    case DragMode::None:
      return;
  }
}

void DropoutFrameView::leaveEvent(QEvent* event) {
  if (hover_.kind != RegionKind::None) {
    hover_ = Hit{};
    updateCursorShape();
    refreshOverlay();
  }
  FrameViewportWidget::leaveEvent(event);
}

// ============================================================================
// DropoutMapEditCommand
// ============================================================================

/**
 * @brief Undoable transition of one frame's dropout edit state
 *
 * Stores before/after snapshots of a single frame's additions and removals.
 * Snapshots keep undo/redo exact without index bookkeeping; per-frame edit
 * lists are small. Commands with the same non-negative merge key on the same
 * frame coalesce (arrow-key nudges).
 */
class DropoutMapEditCommand : public QUndoCommand {
 public:
  DropoutMapEditCommand(DropoutEditorDialog* dialog, uint64_t frame_id,
                        dropout_editor::FrameEdits before,
                        dropout_editor::FrameEdits after, const QString& text,
                        int merge_key)
      : QUndoCommand(text),
        dialog_(dialog),
        frame_id_(frame_id),
        before_(std::move(before)),
        after_(std::move(after)),
        merge_key_(merge_key) {}

  int id() const override { return merge_key_ >= 0 ? 1 : -1; }

  bool mergeWith(const QUndoCommand* other) override {
    const auto* command = dynamic_cast<const DropoutMapEditCommand*>(other);
    if (!command || merge_key_ < 0 || command->merge_key_ != merge_key_ ||
        command->frame_id_ != frame_id_) {
      return false;
    }
    after_ = command->after_;
    return true;
  }

  void redo() override { dialog_->applyFrameEditState(frame_id_, after_); }
  void undo() override { dialog_->applyFrameEditState(frame_id_, before_); }

 private:
  DropoutEditorDialog* dialog_;
  uint64_t frame_id_;
  dropout_editor::FrameEdits before_;
  dropout_editor::FrameEdits after_;
  int merge_key_;
};

// ============================================================================
// DropoutEditorDialog Implementation
// ============================================================================

DropoutEditorDialog::DropoutEditorDialog(
    std::shared_ptr<orc::view::IStageToolSession> session, QWidget* parent)
    : QDialog(parent),
      session_(std::move(session)),
      undo_stack_(new QUndoStack(this)) {
  if (!session_) {
    throw std::invalid_argument("The Dropout Editor needs a tool session");
  }

  const auto stored = session_->parameters();
  const auto it = stored.find(orc::dropout_map_text::kParameterName);
  if (it != stored.end()) {
    dropout_map_ = orc::dropout_map_text::parse(it->second);
  }

  setupUI();

  // An apply rebuilds the graph; the window stays open across it and draws
  // the frame again from the new one.
  QPointer<DropoutEditorDialog> self(this);
  session_->set_graph_rebuilt_handler([self]() {
    if (self) self->onGraphRebuilt();
  });

  // The first frame also says how many there are.
  status_bar_->showMessage("Executing pipeline…");
  requestRenderForCurrentFrame();
}

DropoutEditorDialog::~DropoutEditorDialog() = default;

void DropoutEditorDialog::setupUI() {
  setWindowTitle("Dropout Map Editor");
  setWindowFlags(Qt::Window | Qt::WindowMinimizeButtonHint |
                 Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
  resize(1000, 750);

  auto* main_layout = new QVBoxLayout(this);

  // Navigation debounce (same semantics as the preview dialog): scrubbing
  // and spinbox typing update the UI immediately but commit the render only
  // once movement settles.
  nav_debounce_timer_ = new QTimer(this);
  nav_debounce_timer_->setSingleShot(true);
  nav_debounce_timer_->setInterval(100);  // ms: coalesces rapid scrub moves
  connect(nav_debounce_timer_, &QTimer::timeout,
          [this]() { navigateToIndex(frame_slider_->value()); });

  // Slider row with navigation buttons (mirrors the preview dialog)
  auto* slider_layout = new QHBoxLayout();

  first_button_ = new QPushButton("<<");
  prev_button_ = new QPushButton("<");
  next_button_ = new QPushButton(">");
  last_button_ = new QPushButton(">>");

  prev_button_->setAutoRepeat(true);
  prev_button_->setAutoRepeatDelay(200);
  prev_button_->setAutoRepeatInterval(30);
  next_button_->setAutoRepeat(true);
  next_button_->setAutoRepeatDelay(200);
  next_button_->setAutoRepeatInterval(30);

  for (QPushButton* button :
       {first_button_, prev_button_, next_button_, last_button_}) {
    button->setAutoDefault(false);
    button->setFixedWidth(40);
    button->setEnabled(false);
    slider_layout->addWidget(button);
  }
  first_button_->setToolTip("First frame");
  prev_button_->setToolTip("Previous frame");
  next_button_->setToolTip("Next frame");
  last_button_->setToolTip("Last frame");

  frame_spin_box_ = new QSpinBox();
  frame_spin_box_->setRange(1, 1);
  frame_spin_box_->setEnabled(false);
  {
    // Size the spinbox to comfortably hold up to 5-digit numbers
    QFontMetrics fm(frame_spin_box_->font());
    frame_spin_box_->setMinimumWidth(fm.horizontalAdvance("99999") + 36);
  }
  frame_spin_box_->setToolTip("Jump directly to a frame number");
  slider_layout->addWidget(frame_spin_box_);

  slider_min_label_ = new QLabel("1");
  slider_max_label_ = new QLabel("-");
  frame_slider_ = new FrameMarkerSlider(Qt::Horizontal);
  frame_slider_->setEnabled(false);
  frame_slider_->setToolTip(
      "Scrub through frames; orange marks show frames with edits");
  slider_layout->addWidget(slider_min_label_);
  slider_layout->addWidget(frame_slider_, 1);
  slider_layout->addWidget(slider_max_label_);

  prev_edit_button_ = new QPushButton("◀ Edit");
  next_edit_button_ = new QPushButton("Edit ▶");
  prev_edit_button_->setAutoDefault(false);
  next_edit_button_->setAutoDefault(false);
  prev_edit_button_->setEnabled(false);
  next_edit_button_->setEnabled(false);
  prev_edit_button_->setToolTip("Go to the previous frame with edits");
  next_edit_button_->setToolTip("Go to the next frame with edits");
  connect(prev_edit_button_, &QPushButton::clicked, this,
          &DropoutEditorDialog::onPreviousEditedFrame);
  connect(next_edit_button_, &QPushButton::clicked, this,
          &DropoutEditorDialog::onNextEditedFrame);
  slider_layout->addWidget(prev_edit_button_);
  slider_layout->addWidget(next_edit_button_);

  main_layout->addLayout(slider_layout);

  // Navigation wiring: buttons commit immediately; slider and spinbox are
  // debounced; slider release commits immediately.
  connect(first_button_, &QPushButton::clicked,
          [this]() { navigateToIndex(frame_slider_->minimum()); });
  connect(prev_button_, &QPushButton::clicked,
          [this]() { navigateToIndex(frame_slider_->value() - 1); });
  connect(next_button_, &QPushButton::clicked,
          [this]() { navigateToIndex(frame_slider_->value() + 1); });
  connect(last_button_, &QPushButton::clicked,
          [this]() { navigateToIndex(frame_slider_->maximum()); });
  connect(frame_slider_, &QSlider::valueChanged,
          [this](int value) { navigateToIndexDebounced(value); });
  connect(frame_slider_, &QSlider::sliderReleased,
          [this]() { navigateToIndex(frame_slider_->value()); });
  connect(frame_slider_, &QSlider::rangeChanged, [this](int min, int max) {
    frame_spin_box_->setRange(min + 1, max + 1);
  });
  connect(frame_spin_box_, QOverload<int>::of(&QSpinBox::valueChanged),
          [this](int one_based) { navigateToIndexDebounced(one_based - 1); });

  // Display control row: aspect ratio and zoom
  auto* display_layout = new QHBoxLayout();

  display_layout->addWidget(new QLabel("Aspect:"));
  aspect_ratio_combo_ = new QComboBox();
  aspect_ratio_combo_->addItem("1:1 (Square)");
  aspect_ratio_combo_->addItem("4:3 (Display)");
  aspect_ratio_combo_->setCurrentIndex(1);  // Default to 4:3 like the preview
  connect(aspect_ratio_combo_, QOverload<int>::of(&QComboBox::activated), this,
          &DropoutEditorDialog::onAspectRatioChanged);
  display_layout->addWidget(aspect_ratio_combo_);

  display_layout->addWidget(new QLabel("Zoom:"));

  zoom_out_button_ = new QPushButton("-");
  zoom_out_button_->setMaximumWidth(40);
  zoom_out_button_->setAutoDefault(false);
  connect(zoom_out_button_, &QPushButton::clicked, this,
          &DropoutEditorDialog::onZoomOut);
  display_layout->addWidget(zoom_out_button_);

  zoom_reset_button_ = new QPushButton("1:1");
  zoom_reset_button_->setMaximumWidth(60);
  zoom_reset_button_->setAutoDefault(false);
  connect(zoom_reset_button_, &QPushButton::clicked, this,
          &DropoutEditorDialog::onZoomReset);
  display_layout->addWidget(zoom_reset_button_);

  zoom_fit_button_ = new QPushButton("Fit");
  zoom_fit_button_->setMaximumWidth(60);
  zoom_fit_button_->setAutoDefault(false);
  connect(zoom_fit_button_, &QPushButton::clicked, this,
          &DropoutEditorDialog::onZoomFit);
  display_layout->addWidget(zoom_fit_button_);

  zoom_in_button_ = new QPushButton("+");
  zoom_in_button_->setMaximumWidth(40);
  zoom_in_button_->setAutoDefault(false);
  connect(zoom_in_button_, &QPushButton::clicked, this,
          &DropoutEditorDialog::onZoomIn);
  display_layout->addWidget(zoom_in_button_);

  zoom_label_ = new QLabel("100%");
  display_layout->addWidget(zoom_label_);
  display_layout->addStretch();

  main_layout->addLayout(display_layout);

  // Frame view with its own pan bars. The view stays viewport-sized and moves
  // what it shows rather than growing to the zoomed frame inside a scroll
  // area: at 8x zoom that widget is several thousand pixels across, which is
  // not something a GPU surface can be. The bars are controls that drive the
  // view's visible origin, not a scroll area's idea of a large child.
  view_container_ = new QWidget();
  view_container_->setContentsMargins(0, 0, 0, 0);
  auto* view_grid = new QGridLayout(view_container_);
  view_grid->setContentsMargins(0, 0, 0, 0);
  view_grid->setSpacing(0);

  horizontal_pan_bar_ = new QScrollBar(Qt::Horizontal);
  vertical_pan_bar_ = new QScrollBar(Qt::Vertical);
  horizontal_pan_bar_->setObjectName(QStringLiteral("horizontalPanBar"));
  vertical_pan_bar_->setObjectName(QStringLiteral("verticalPanBar"));

  frame_view_ = new DropoutFrameView();
  frame_view_->setAspectCorrection(currentAspectCorrection());
  connect(frame_view_, &orc::view::FrameViewportWidget::zoomChanged, this,
          &DropoutEditorDialog::onFrameViewZoomChanged);
  connect(frame_view_, &DropoutFrameView::selectionChanged, this,
          &DropoutEditorDialog::onViewSelectionChanged);
  connect(frame_view_, &DropoutFrameView::regionAddRequested, this,
          &DropoutEditorDialog::onRegionAddRequested);
  connect(frame_view_, &DropoutFrameView::additionModifyRequested, this,
          &DropoutEditorDialog::onAdditionModifyRequested);
  connect(frame_view_, &DropoutFrameView::sourceResizeRequested, this,
          &DropoutEditorDialog::onSourceResizeRequested);
  connect(frame_view_, &DropoutFrameView::contextMenuRequested, this,
          &DropoutEditorDialog::onContextMenuRequested);

  // The bars are the only writers of the origin from outside the view, and
  // the view is the only writer of their ranges: zoom-at-cursor and wheel
  // panning both come back here through visibleOriginChanged.
  connect(frame_view_, &orc::view::FrameViewportWidget::panRangeChanged, this,
          [this](QSize max_origin) {
            const QSignalBlocker block_h(horizontal_pan_bar_);
            const QSignalBlocker block_v(vertical_pan_bar_);
            horizontal_pan_bar_->setRange(0, max_origin.width());
            horizontal_pan_bar_->setPageStep(frame_view_->width());
            horizontal_pan_bar_->setSingleStep(kPanStep);
            vertical_pan_bar_->setRange(0, max_origin.height());
            vertical_pan_bar_->setPageStep(frame_view_->height());
            vertical_pan_bar_->setSingleStep(kPanStep);
            horizontal_pan_bar_->setVisible(max_origin.width() > 0);
            vertical_pan_bar_->setVisible(max_origin.height() > 0);
          });
  connect(frame_view_, &orc::view::FrameViewportWidget::visibleOriginChanged,
          this, [this](QPoint origin) {
            const QSignalBlocker block_h(horizontal_pan_bar_);
            const QSignalBlocker block_v(vertical_pan_bar_);
            horizontal_pan_bar_->setValue(origin.x());
            vertical_pan_bar_->setValue(origin.y());
          });
  connect(horizontal_pan_bar_, &QScrollBar::valueChanged, this,
          [this](int value) {
            frame_view_->setVisibleOrigin(
                QPoint(value, frame_view_->visibleOrigin().y()));
          });
  connect(vertical_pan_bar_, &QScrollBar::valueChanged, this,
          [this](int value) {
            frame_view_->setVisibleOrigin(
                QPoint(frame_view_->visibleOrigin().x(), value));
          });

  view_grid->addWidget(frame_view_, 0, 0);
  view_grid->addWidget(vertical_pan_bar_, 0, 1);
  view_grid->addWidget(horizontal_pan_bar_, 1, 0);
  view_grid->setRowStretch(0, 1);
  view_grid->setColumnStretch(0, 1);
  horizontal_pan_bar_->setVisible(false);
  vertical_pan_bar_->setVisible(false);

  main_layout->addWidget(view_container_, 3);

  // Overlay legend and interaction hints
  auto* legend_label = new QLabel(
      QString("<span style='color:%1'>&#9632;</span> Source dropout&nbsp;&nbsp;"
              "<span style='color:%2'>&#9632;</span> Addition&nbsp;&nbsp;"
              "<span style='color:%3'>&#9632;</span> Removal "
              "(struck-through)<br>"
              "Drag empty area to add &bull; drag a selected addition to move, "
              "handles resize, arrow keys nudge &bull; handles on a source "
              "dropout resize it into an addition &bull; Del deletes / toggles "
              "removal &bull; right-click for menu &bull; Ctrl+wheel zooms")
          .arg(kSourceColor.name(), kAdditionColor.name(),
               kRemovalColor.name()));
  legend_label->setTextFormat(Qt::RichText);
  // Wrap so the long hint text doesn't dictate the dialog's minimum width.
  legend_label->setWordWrap(true);
  main_layout->addWidget(legend_label);

  // Edit panel: undo/redo/delete/clear + unified region table
  auto* control_layout = new QHBoxLayout();

  auto* edit_group = new QGroupBox("Edit");
  auto* edit_vlayout = new QVBoxLayout(edit_group);

  undo_button_ = new QPushButton("Undo");
  undo_button_->setObjectName("dropoutEditorUndoButton");
  undo_button_->setEnabled(false);
  undo_button_->setAutoDefault(false);
  undo_button_->setToolTip("Undo the last edit (Ctrl+Z)");
  connect(undo_button_, &QPushButton::clicked, undo_stack_, &QUndoStack::undo);
  connect(undo_stack_, &QUndoStack::canUndoChanged, undo_button_,
          &QPushButton::setEnabled);
  edit_vlayout->addWidget(undo_button_);

  redo_button_ = new QPushButton("Redo");
  redo_button_->setObjectName("dropoutEditorRedoButton");
  redo_button_->setEnabled(false);
  redo_button_->setAutoDefault(false);
  redo_button_->setToolTip("Redo the last undone edit (Ctrl+Shift+Z)");
  connect(redo_button_, &QPushButton::clicked, undo_stack_, &QUndoStack::redo);
  connect(undo_stack_, &QUndoStack::canRedoChanged, redo_button_,
          &QPushButton::setEnabled);
  edit_vlayout->addWidget(redo_button_);

  delete_button_ = new QPushButton("Delete");
  delete_button_->setObjectName("dropoutEditorDeleteButton");
  delete_button_->setEnabled(false);
  delete_button_->setAutoDefault(false);
  delete_button_->setToolTip(
      "Delete the selected addition, or mark/restore the selected source "
      "dropout (Del)");
  connect(delete_button_, &QPushButton::clicked,
          [this]() { deleteOrToggleSelection(); });
  edit_vlayout->addWidget(delete_button_);

  clear_frame_button_ = new QPushButton("Clear Frame Edits");
  clear_frame_button_->setAutoDefault(false);
  connect(clear_frame_button_, &QPushButton::clicked, this,
          &DropoutEditorDialog::onClearCurrentFrame);
  edit_vlayout->addWidget(clear_frame_button_);
  edit_vlayout->addStretch();

  control_layout->addWidget(edit_group);

  auto* regions_group = new QGroupBox("Dropout Regions");
  auto* regions_layout = new QVBoxLayout(regions_group);
  region_table_ = new QTableWidget(0, 4);
  region_table_->setHorizontalHeaderLabels({"Status", "Line", "Start", "End"});
  region_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  region_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  region_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  region_table_->verticalHeader()->setVisible(false);
  region_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  region_table_->setMaximumHeight(160);
  connect(region_table_, &QTableWidget::itemSelectionChanged, this,
          &DropoutEditorDialog::onTableSelectionChanged);
  regions_layout->addWidget(region_table_);
  control_layout->addWidget(regions_group, 1);

  main_layout->addLayout(control_layout);

  auto* button_box =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply |
                           QDialogButtonBox::Cancel);
  ok_button_ = button_box->button(QDialogButtonBox::Ok);
  apply_button_ = button_box->button(QDialogButtonBox::Apply);
  ok_button_->setToolTip("Write the dropout map to the node and close");
  apply_button_->setToolTip("Write the dropout map to the node");
  connect(button_box, &QDialogButtonBox::accepted, this,
          [this]() { submit(true); });
  connect(button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(apply_button_, &QPushButton::clicked, this,
          [this]() { submit(false); });
  main_layout->addWidget(button_box);

  // Status bar: transient messages on the left, frame info always visible on
  // the right.
  status_bar_ = new QStatusBar(this);
  status_bar_->setSizeGripEnabled(false);
  frame_info_label_ = new QLabel();
  status_bar_->addPermanentWidget(frame_info_label_);
  main_layout->addWidget(status_bar_);

  // Editing shortcuts work regardless of which child has focus.
  auto* undo_shortcut = new QShortcut(QKeySequence::Undo, this);
  undo_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(undo_shortcut, &QShortcut::activated, undo_stack_, &QUndoStack::undo);
  auto* redo_shortcut = new QShortcut(QKeySequence::Redo, this);
  redo_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(redo_shortcut, &QShortcut::activated, undo_stack_, &QUndoStack::redo);
  auto* delete_shortcut = new QShortcut(QKeySequence::Delete, this);
  delete_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(delete_shortcut, &QShortcut::activated,
          [this]() { deleteOrToggleSelection(); });
}

double DropoutEditorDialog::currentAspectCorrection() const {
  // Index 0 = SAR 1:1, index 1 = DAR 4:3 (same order as the preview dialog).
  if (aspect_ratio_combo_ && aspect_ratio_combo_->currentIndex() == 0) {
    return 1.0;
  }
  return dar_aspect_correction_;
}

void DropoutEditorDialog::setFrameCount(uint64_t frame_count) {
  total_frames_ = static_cast<size_t>(frame_count);
  frames_ready_ = frame_count > 0;
  {
    // Range setup must not fire a debounced navigation.
    const QSignalBlocker slider_blocker(frame_slider_);
    const QSignalBlocker spin_blocker(frame_spin_box_);
    frame_slider_->setRange(0, static_cast<int>(total_frames_) - 1);
    frame_spin_box_->setRange(1, static_cast<int>(total_frames_));
  }
  slider_max_label_->setText(QString::number(total_frames_));
  frame_slider_->setEnabled(frames_ready_);
  frame_spin_box_->setEnabled(frames_ready_);
  first_button_->setEnabled(frames_ready_);
  last_button_->setEnabled(frames_ready_);
}

void DropoutEditorDialog::navigateToIndex(int zero_based) {
  if (!frames_ready_) {
    return;
  }
  const int clamped = std::clamp(zero_based, frame_slider_->minimum(),
                                 frame_slider_->maximum());
  nav_debounce_timer_->stop();  // cancel any pending debounced commit
  setIndex(clamped);

  current_frame_id_ = static_cast<uint64_t>(clamped);
  updateFrameInfo();
  requestRenderForCurrentFrame();
}

void DropoutEditorDialog::navigateToIndexDebounced(int zero_based) {
  if (!frames_ready_) {
    return;
  }
  const int clamped = std::clamp(zero_based, frame_slider_->minimum(),
                                 frame_slider_->maximum());
  setIndex(clamped);             // update UI immediately for visual feedback
  nav_debounce_timer_->start();  // (re)starts; commits when settled
}

void DropoutEditorDialog::setIndex(int zero_based) {
  // Silently sync both slider and spinbox without emitting any signals.
  const QSignalBlocker slider_blocker(frame_slider_);
  const QSignalBlocker spin_blocker(frame_spin_box_);
  frame_slider_->setValue(zero_based);
  frame_spin_box_->setValue(zero_based + 1);  // 1-indexed display
}

void DropoutEditorDialog::requestRenderForCurrentFrame() {
  if (render_in_flight_) {
    // onFrameRendered re-requests when the delivered frame is stale.
    return;
  }
  render_in_flight_ = true;
  if (frames_ready_) {
    status_bar_->showMessage(
        QString("Rendering frame %1…").arg(current_frame_id_ + 1));
  }
  QPointer<DropoutEditorDialog> self(this);
  // The picture before the stage: the edits are drawn over it as overlays.
  orc::view::ToolFrameRenderOptions options;
  options.from_input = true;
  session_->render_frame(current_frame_id_, options,
                         [self](orc::view::ToolFrameImage image) {
                           if (self) self->onFrameRendered(std::move(image));
                         });
}

void DropoutEditorDialog::onFrameRendered(orc::view::ToolFrameImage image) {
  render_in_flight_ = false;

  if (render_stale_) {
    // Rendered from the graph before the last rebuild.
    render_stale_ = false;
    requestRenderForCurrentFrame();
    return;
  }

  // Every reply says how many frames the input has, rendered or not; it can
  // change across a rebuild.
  if (image.frame_count != total_frames_ || !frames_ready_) {
    setFrameCount(image.frame_count);
    if (!frames_ready_) {
      status_bar_->showMessage(
          QString("No frames available — ensure the dropout_map stage has a "
                  "valid video source connected (%1)")
              .arg(QString::fromStdString(image.error)));
      updateFrameInfo();
      return;
    }
    dar_aspect_correction_ = image.dar_aspect_correction;
    frame_view_->setAspectCorrection(currentAspectCorrection());
    if (current_frame_id_ >= total_frames_) {
      navigateToIndex(static_cast<int>(total_frames_) - 1);
      return;
    }
    setIndex(static_cast<int>(current_frame_id_));
  }

  if (image.frame_index != current_frame_id_) {
    // Stale result from an earlier navigation target; render the current one.
    requestRenderForCurrentFrame();
    return;
  }

  QImage frame = image.ok ? frameImage(image) : QImage();
  if (frame.isNull()) {
    status_bar_->showMessage(
        QString("Failed to render frame %1: %2")
            .arg(image.frame_index + 1)
            .arg(QString::fromStdString(
                image.error.empty() ? std::string("the image was not valid")
                                    : image.error)));
    updateFrameInfo();
    return;
  }

  // The rows are frame-flat lines, so the input's dropouts are already in
  // the map's coordinates.
  std::vector<dropout_editor::Region> source_dropouts;
  source_dropouts.reserve(image.dropouts.size());
  for (const auto& dropout : image.dropouts) {
    source_dropouts.push_back(dropout_editor::Region{
        dropout.line, dropout.start_sample, dropout.end_sample});
  }

  const dropout_editor::FrameEdits state = frameEditState(image.frame_index);
  frame_view_->setFrame(frame, source_dropouts, state.additions,
                        state.removals);
  loaded_frame_id_ = image.frame_index;
  refreshRegionTable();
  selectRegion(DropoutFrameView::RegionKind::None, -1);

  if (!initial_fit_done_ && isVisible()) {
    initial_fit_done_ = true;
    frame_view_->fitToViewport();
    updateZoomLabel(frame_view_->zoomLevel());
  }

  status_bar_->clearMessage();
  updateFrameInfo();
}

void DropoutEditorDialog::onGraphRebuilt() {
  if (render_in_flight_) {
    render_stale_ = true;
    return;
  }
  requestRenderForCurrentFrame();
}

void DropoutEditorDialog::setSubmitEnabled(bool enabled) {
  ok_button_->setEnabled(enabled);
  apply_button_->setEnabled(enabled);
}

void DropoutEditorDialog::submit(bool close_after) {
  if (!session_->is_open()) {
    reject();
    return;
  }
  setSubmitEnabled(false);
  status_bar_->showMessage("Writing the dropout map…");
  QPointer<DropoutEditorDialog> self(this);
  session_->apply_parameters(
      {{orc::dropout_map_text::kParameterName,
        orc::dropout_map_text::encode(dropout_map_)}},
      "Edited dropout map", [self, close_after](orc::view::ToolReply reply) {
        if (!self) return;
        self->setSubmitEnabled(true);
        if (!reply.ok) {
          self->status_bar_->clearMessage();
          QMessageBox::warning(self, "Dropout Editor",
                               QString("The dropout map was not written: %1")
                                   .arg(QString::fromStdString(reply.error)));
          return;
        }
        if (close_after) {
          self->accept();
          return;
        }
        self->status_bar_->showMessage("Dropout map written", 3000);
      });
}

// ============================================================================
// Undoable editing
// ============================================================================

dropout_editor::FrameEdits DropoutEditorDialog::frameEditState(
    uint64_t frame_id) const {
  auto it = dropout_map_.find(frame_id);
  if (it != dropout_map_.end()) {
    return it->second;
  }
  dropout_editor::FrameEdits empty_state;
  empty_state.frame_id = frame_id;
  return empty_state;
}

void DropoutEditorDialog::pushEditCommand(uint64_t frame_id,
                                          dropout_editor::FrameEdits after,
                                          const QString& text, int merge_key) {
  undo_stack_->push(
      new DropoutMapEditCommand(this, frame_id, frameEditState(frame_id),
                                std::move(after), text, merge_key));
}

void DropoutEditorDialog::applyFrameEditState(
    uint64_t frame_id, const dropout_editor::FrameEdits& state) {
  const bool empty = state.additions.empty() && state.removals.empty();
  if (empty) {
    dropout_map_.erase(frame_id);
  } else {
    dropout_editor::FrameEdits& entry = dropout_map_[frame_id];
    entry.frame_id = frame_id;
    entry.additions = state.additions;
    entry.removals = state.removals;
  }

  if (loaded_frame_id_.has_value() && *loaded_frame_id_ == frame_id) {
    frame_view_->updateRegions(state.additions, state.removals);
    refreshRegionTable();
    selectRegion(DropoutFrameView::RegionKind::None, -1);
  } else if (frames_ready_ && frame_id != current_frame_id_) {
    // Undo/redo touched a frame we are not looking at: go there so the user
    // sees the effect of the operation.
    navigateToIndex(static_cast<int>(frame_id));
  }

  updateFrameInfo();
}

void DropoutEditorDialog::onRegionAddRequested(
    const dropout_editor::Region& region) {
  if (!loaded_frame_id_.has_value()) {
    return;
  }
  const uint64_t frame_id = *loaded_frame_id_;
  auto after = frameEditState(frame_id);
  after.additions.push_back(region);
  const int new_index = static_cast<int>(after.additions.size()) - 1;
  pushEditCommand(frame_id, std::move(after), "Add dropout");
  selectRegion(DropoutFrameView::RegionKind::Addition, new_index);
}

void DropoutEditorDialog::onAdditionModifyRequested(
    int index, const dropout_editor::Region& new_region) {
  if (!loaded_frame_id_.has_value()) {
    return;
  }
  const uint64_t frame_id = *loaded_frame_id_;
  auto after = frameEditState(frame_id);
  if (index < 0 || index >= static_cast<int>(after.additions.size())) {
    return;
  }
  after.additions[index] = new_region;
  pushEditCommand(frame_id, std::move(after), "Move dropout");
  selectRegion(DropoutFrameView::RegionKind::Addition, index);
}

void DropoutEditorDialog::onSourceResizeRequested(
    int index, const dropout_editor::Region& new_region) {
  if (!loaded_frame_id_.has_value()) {
    return;
  }
  const uint64_t frame_id = *loaded_frame_id_;
  const auto& sources = frame_view_->getSourceDropouts();
  if (index < 0 || index >= static_cast<int>(sources.size())) {
    return;
  }

  // Resize-by-replacement: mark the source dropout removed and add a region
  // covering the resized extent as one undoable command. The result is a
  // normal addition, so further move/resize/delete work as usual.
  auto after = frameEditState(frame_id);
  after.removals.push_back(sources[index]);
  after.additions.push_back(new_region);
  const int new_index = static_cast<int>(after.additions.size()) - 1;
  pushEditCommand(frame_id, std::move(after), "Resize source dropout");
  selectRegion(DropoutFrameView::RegionKind::Addition, new_index);
}

void DropoutEditorDialog::deleteOrToggleSelection() {
  if (!loaded_frame_id_.has_value()) {
    return;
  }
  const uint64_t frame_id = *loaded_frame_id_;
  const DropoutFrameView::RegionKind kind = frame_view_->selectedKind();
  const int index = frame_view_->selectedIndex();
  auto after = frameEditState(frame_id);

  auto findRemoval = [&after](const dropout_editor::Region& region) {
    return std::find_if(after.removals.begin(), after.removals.end(),
                        [&region](const dropout_editor::Region& r) {
                          return sameRegion(r, region);
                        });
  };

  switch (kind) {
    case DropoutFrameView::RegionKind::Addition: {
      if (index < 0 || index >= static_cast<int>(after.additions.size())) {
        return;
      }
      after.additions.erase(after.additions.begin() + index);
      pushEditCommand(frame_id, std::move(after), "Delete addition");
      selectRegion(DropoutFrameView::RegionKind::None, -1);
      return;
    }

    case DropoutFrameView::RegionKind::OrphanRemoval: {
      if (index < 0 || index >= static_cast<int>(after.removals.size())) {
        return;
      }
      after.removals.erase(after.removals.begin() + index);
      pushEditCommand(frame_id, std::move(after), "Delete removal entry");
      selectRegion(DropoutFrameView::RegionKind::None, -1);
      return;
    }

    case DropoutFrameView::RegionKind::Source: {
      const auto& sources = frame_view_->getSourceDropouts();
      if (index < 0 || index >= static_cast<int>(sources.size())) {
        return;
      }
      const dropout_editor::Region region = sources[index];
      auto it = findRemoval(region);
      if (it != after.removals.end()) {
        after.removals.erase(it);
        pushEditCommand(frame_id, std::move(after), "Restore dropout");
      } else {
        after.removals.push_back(region);
        pushEditCommand(frame_id, std::move(after), "Mark dropout removed");
      }
      selectRegion(DropoutFrameView::RegionKind::Source, index);
      return;
    }

    case DropoutFrameView::RegionKind::None:
      return;
  }
}

void DropoutEditorDialog::nudgeSelectedAddition(int dx, int dy) {
  if (!loaded_frame_id_.has_value() ||
      frame_view_->selectedKind() != DropoutFrameView::RegionKind::Addition) {
    return;
  }
  const uint64_t frame_id = *loaded_frame_id_;
  const int index = frame_view_->selectedIndex();
  auto after = frameEditState(frame_id);
  if (index < 0 || index >= static_cast<int>(after.additions.size())) {
    return;
  }

  auto& region = after.additions[index];
  const int width = frame_view_->getFrameWidth();
  const int height = frame_view_->getFrameHeight();
  const int length = static_cast<int>(region.end_sample - region.start_sample);
  const int line =
      std::clamp(static_cast<int>(region.line) + dy, 0, height - 1);
  const int start =
      std::clamp(static_cast<int>(region.start_sample) + dx, 0, width - length);

  if (line == static_cast<int>(region.line) &&
      start == static_cast<int>(region.start_sample)) {
    return;  // Clamped into no-op
  }
  region.line = static_cast<uint32_t>(line);
  region.start_sample = static_cast<uint32_t>(start);
  region.end_sample = static_cast<uint32_t>(start + length);

  // merge_key = addition index so consecutive nudges of the same region
  // collapse into one undo step.
  pushEditCommand(frame_id, std::move(after), "Nudge dropout", index);
  selectRegion(DropoutFrameView::RegionKind::Addition, index);
}

void DropoutEditorDialog::onClearCurrentFrame() {
  if (!loaded_frame_id_.has_value()) {
    return;
  }
  const uint64_t frame_id = *loaded_frame_id_;
  const auto state = frameEditState(frame_id);
  if (state.additions.empty() && state.removals.empty()) {
    return;
  }
  dropout_editor::FrameEdits empty_state;
  empty_state.frame_id = frame_id;
  pushEditCommand(frame_id, std::move(empty_state), "Clear frame edits");
  selectRegion(DropoutFrameView::RegionKind::None, -1);
}

void DropoutEditorDialog::onContextMenuRequested(
    DropoutFrameView::RegionKind kind, int index, const QPoint& global_pos) {
  QMenu menu(this);

  switch (kind) {
    case DropoutFrameView::RegionKind::Addition:
      menu.addAction("Delete addition",
                     [this]() { deleteOrToggleSelection(); });
      break;
    case DropoutFrameView::RegionKind::OrphanRemoval:
      menu.addAction("Delete removal entry",
                     [this]() { deleteOrToggleSelection(); });
      break;
    case DropoutFrameView::RegionKind::Source: {
      const auto& sources = frame_view_->getSourceDropouts();
      const auto& removals = frame_view_->getRemovals();
      bool removed = false;
      if (index >= 0 && index < static_cast<int>(sources.size())) {
        removed =
            std::any_of(removals.begin(), removals.end(),
                        [&sources, index](const dropout_editor::Region& r) {
                          return sameRegion(r, sources[index]);
                        });
      }
      menu.addAction(removed ? "Restore dropout" : "Mark dropout removed",
                     [this]() { deleteOrToggleSelection(); });
      break;
    }
    case DropoutFrameView::RegionKind::None:
      break;
  }

  if (!menu.isEmpty()) {
    menu.addSeparator();
  }
  QAction* clear_action =
      menu.addAction("Clear frame edits", [this]() { onClearCurrentFrame(); });
  {
    const auto state = loaded_frame_id_.has_value()
                           ? frameEditState(*loaded_frame_id_)
                           : dropout_editor::FrameEdits();
    clear_action->setEnabled(!state.additions.empty() ||
                             !state.removals.empty());
  }
  menu.addSeparator();
  QAction* undo_action = menu.addAction("Undo", undo_stack_, &QUndoStack::undo);
  undo_action->setEnabled(undo_stack_->canUndo());
  QAction* redo_action = menu.addAction("Redo", undo_stack_, &QUndoStack::redo);
  redo_action->setEnabled(undo_stack_->canRedo());

  menu.exec(global_pos);
}

// ============================================================================
// Selection / region table
// ============================================================================

void DropoutEditorDialog::onViewSelectionChanged(
    DropoutFrameView::RegionKind kind, int index) {
  if (syncing_selection_) {
    return;
  }
  selectRegion(kind, index);
}

void DropoutEditorDialog::selectRegion(DropoutFrameView::RegionKind kind,
                                       int index) {
  syncing_selection_ = true;
  frame_view_->setSelectedRegion(kind, index);

  region_table_->clearSelection();
  if (kind != DropoutFrameView::RegionKind::None) {
    for (int row = 0; row < region_table_->rowCount(); ++row) {
      QTableWidgetItem* item = region_table_->item(row, 0);
      if (item && item->data(Qt::UserRole).toInt() == static_cast<int>(kind) &&
          item->data(Qt::UserRole + 1).toInt() == index) {
        region_table_->selectRow(row);
        break;
      }
    }
  }
  syncing_selection_ = false;

  // Delete button context
  switch (kind) {
    case DropoutFrameView::RegionKind::Addition:
      delete_button_->setText("Delete Addition");
      delete_button_->setEnabled(true);
      break;
    case DropoutFrameView::RegionKind::OrphanRemoval:
      delete_button_->setText("Delete Removal");
      delete_button_->setEnabled(true);
      break;
    case DropoutFrameView::RegionKind::Source: {
      const auto& sources = frame_view_->getSourceDropouts();
      const auto& removals = frame_view_->getRemovals();
      bool removed = false;
      if (index >= 0 && index < static_cast<int>(sources.size())) {
        removed =
            std::any_of(removals.begin(), removals.end(),
                        [&sources, index](const dropout_editor::Region& r) {
                          return sameRegion(r, sources[index]);
                        });
      }
      delete_button_->setText(removed ? "Restore Dropout" : "Mark Removed");
      delete_button_->setEnabled(true);
      break;
    }
    case DropoutFrameView::RegionKind::None:
      delete_button_->setText("Delete");
      delete_button_->setEnabled(false);
      break;
  }
}

void DropoutEditorDialog::onTableSelectionChanged() {
  if (syncing_selection_) {
    return;
  }
  const auto selected = region_table_->selectedItems();
  if (selected.isEmpty()) {
    selectRegion(DropoutFrameView::RegionKind::None, -1);
    return;
  }
  QTableWidgetItem* item = region_table_->item(selected.first()->row(), 0);
  if (!item) {
    return;
  }
  const auto kind = static_cast<DropoutFrameView::RegionKind>(
      item->data(Qt::UserRole).toInt());
  const int index = item->data(Qt::UserRole + 1).toInt();
  selectRegion(kind, index);
}

void DropoutEditorDialog::refreshRegionTable() {
  syncing_selection_ = true;
  region_table_->setRowCount(0);

  auto addRow = [this](const QString& status, const QColor& color,
                       const dropout_editor::Region& region,
                       DropoutFrameView::RegionKind kind, int index) {
    const int row = region_table_->rowCount();
    region_table_->insertRow(row);

    auto* status_item = new QTableWidgetItem(status);
    status_item->setForeground(color);
    status_item->setData(Qt::UserRole, static_cast<int>(kind));
    status_item->setData(Qt::UserRole + 1, index);
    region_table_->setItem(row, 0, status_item);

    // Line shown 1-based per the presentation convention.
    region_table_->setItem(
        row, 1, new QTableWidgetItem(QString::number(region.line + 1)));
    region_table_->setItem(
        row, 2, new QTableWidgetItem(QString::number(region.start_sample)));
    region_table_->setItem(
        row, 3, new QTableWidgetItem(QString::number(region.end_sample)));
  };

  const auto& sources = frame_view_->getSourceDropouts();
  const auto& additions = frame_view_->getAdditions();
  const auto& removals = frame_view_->getRemovals();

  for (size_t i = 0; i < sources.size(); ++i) {
    const bool removed =
        std::any_of(removals.begin(), removals.end(),
                    [&sources, i](const dropout_editor::Region& r) {
                      return sameRegion(r, sources[i]);
                    });
    addRow(removed ? "Removed" : "Source",
           removed ? kRemovalColor : kSourceColor, sources[i],
           DropoutFrameView::RegionKind::Source, static_cast<int>(i));
  }

  for (size_t i = 0; i < additions.size(); ++i) {
    addRow("Added", kAdditionColor, additions[i],
           DropoutFrameView::RegionKind::Addition, static_cast<int>(i));
  }

  for (size_t i = 0; i < removals.size(); ++i) {
    if (frame_view_->isOrphanRemoval(static_cast<int>(i))) {
      addRow("Removed (no source)", kRemovalColor, removals[i],
             DropoutFrameView::RegionKind::OrphanRemoval, static_cast<int>(i));
    }
  }

  syncing_selection_ = false;
}

// ============================================================================
// Frame info / edited-frame navigation
// ============================================================================

std::vector<int> DropoutEditorDialog::editedFrameIndices() const {
  std::vector<int> indices;
  indices.reserve(dropout_map_.size());
  for (const auto& [frame_id, frame_map] : dropout_map_) {
    indices.push_back(static_cast<int>(frame_id));
  }
  return indices;  // Map iteration order keeps this sorted
}

void DropoutEditorDialog::updateFrameInfo() {
  const auto state = loaded_frame_id_.has_value()
                         ? frameEditState(*loaded_frame_id_)
                         : dropout_editor::FrameEdits();

  frame_info_label_->setText(
      QString("Frame %1 of %2 — Additions: %3, Removals: %4")
          .arg(current_frame_id_ + 1)
          .arg(total_frames_)
          .arg(state.additions.size())
          .arg(state.removals.size()));

  const bool has_frames = frames_ready_ && total_frames_ > 0;
  prev_button_->setEnabled(has_frames && current_frame_id_ > 0);
  next_button_->setEnabled(has_frames && current_frame_id_ + 1 < total_frames_);

  updateEditedFrameNavigation();
}

void DropoutEditorDialog::updateEditedFrameNavigation() {
  const std::vector<int> edited = editedFrameIndices();
  frame_slider_->setMarkedValues(edited);

  const int current = static_cast<int>(current_frame_id_);
  const bool has_prev_edit =
      !edited.empty() &&
      std::lower_bound(edited.begin(), edited.end(), current) != edited.begin();
  const bool has_next_edit =
      !edited.empty() &&
      std::upper_bound(edited.begin(), edited.end(), current) != edited.end();
  prev_edit_button_->setEnabled(frames_ready_ && has_prev_edit);
  next_edit_button_->setEnabled(frames_ready_ && has_next_edit);
}

void DropoutEditorDialog::onPreviousEditedFrame() {
  const std::vector<int> edited = editedFrameIndices();
  const int current = static_cast<int>(current_frame_id_);
  auto it = std::lower_bound(edited.begin(), edited.end(), current);
  if (it != edited.begin()) {
    navigateToIndex(*std::prev(it));
  }
}

void DropoutEditorDialog::onNextEditedFrame() {
  const std::vector<int> edited = editedFrameIndices();
  const int current = static_cast<int>(current_frame_id_);
  auto it = std::upper_bound(edited.begin(), edited.end(), current);
  if (it != edited.end()) {
    navigateToIndex(*it);
  }
}

// ============================================================================
// Zoom / aspect / keyboard
// ============================================================================

void DropoutEditorDialog::onZoomIn() { frame_view_->zoomIn(); }

void DropoutEditorDialog::onZoomOut() { frame_view_->zoomOut(); }

void DropoutEditorDialog::onZoomReset() { frame_view_->setZoomLevel(1.0); }

void DropoutEditorDialog::onZoomFit() { frame_view_->fitToViewport(); }

void DropoutEditorDialog::onAspectRatioChanged(int index) {
  Q_UNUSED(index);
  frame_view_->setAspectCorrection(currentAspectCorrection());
}

void DropoutEditorDialog::updateZoomLabel(double zoom_level) {
  zoom_label_->setText(QString("%1%").arg(qRound(zoom_level * 100)));
}

void DropoutEditorDialog::onFrameViewZoomChanged(double zoom_level) {
  updateZoomLabel(zoom_level);
}

void DropoutEditorDialog::keyPressEvent(QKeyEvent* event) {
  const bool addition_selected =
      frame_view_->selectedKind() == DropoutFrameView::RegionKind::Addition;

  // Arrow keys nudge the selected addition; with no addition selected they
  // pan the frame.
  switch (event->key()) {
    case Qt::Key_Left:
      if (addition_selected) {
        nudgeSelectedAddition(-1, 0);
      } else {
        frame_view_->setVisibleOrigin(frame_view_->visibleOrigin() -
                                      QPoint(kPanStep, 0));
      }
      event->accept();
      break;
    case Qt::Key_Right:
      if (addition_selected) {
        nudgeSelectedAddition(1, 0);
      } else {
        frame_view_->setVisibleOrigin(frame_view_->visibleOrigin() +
                                      QPoint(kPanStep, 0));
      }
      event->accept();
      break;
    case Qt::Key_Up:
      if (addition_selected) {
        nudgeSelectedAddition(0, -1);
      } else {
        frame_view_->setVisibleOrigin(frame_view_->visibleOrigin() -
                                      QPoint(0, kPanStep));
      }
      event->accept();
      break;
    case Qt::Key_Down:
      if (addition_selected) {
        nudgeSelectedAddition(0, 1);
      } else {
        frame_view_->setVisibleOrigin(frame_view_->visibleOrigin() +
                                      QPoint(0, kPanStep));
      }
      event->accept();
      break;
    case Qt::Key_PageUp:
      navigateToIndex(frame_slider_->value() - 1);
      event->accept();
      break;
    case Qt::Key_PageDown:
      navigateToIndex(frame_slider_->value() + 1);
      event->accept();
      break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
      onZoomIn();
      event->accept();
      break;
    case Qt::Key_Minus:
      onZoomOut();
      event->accept();
      break;
    case Qt::Key_0:
      onZoomReset();
      event->accept();
      break;
    default:
      QDialog::keyPressEvent(event);
      break;
  }
}

void DropoutEditorDialog::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  // Fit the frame to the viewport once the dialog has its real size (covers
  // the case where the first frame arrived before the dialog was shown).
  if (!initial_fit_done_ && frame_view_->hasImage()) {
    initial_fit_done_ = true;
    frame_view_->fitToViewport();
    updateZoomLabel(frame_view_->zoomLevel());
  }
}
