/*
 * File:        dropout_editor_dialog.cpp
 * Module:      orc-gui
 * Purpose:     Dropout map editor dialog implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dropout_editor_dialog.h"

#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>

#include "gpu/overlay_primitive_builder.h"
#include "logging.h"
#include "preview_image_qt.h"

namespace {

// Overlay colours shared with the legend and region table. Source dropouts
// use the same red family as the preview dialog's dropout overlay.
const QColor kSourceColor(255, 48, 48);
const QColor kAdditionColor(48, 200, 48);
const QColor kRemovalColor(160, 160, 160);

// The "sequential" preview option renders image rows as frame-flat lines,
// which is the dropout map's coordinate system.
constexpr const char* kSequentialOptionId = "sequential_clamped";

// Interaction geometry (widget-space pixels)
constexpr double kHitSlop = 2.0;
/// Widget pixels an arrow key or a pan-bar step moves the view.
constexpr int kPanStep = 50;
constexpr int kMoveThreshold = 3;

bool sameRegion(const orc::presenters::DropoutRegion& a,
                const orc::presenters::DropoutRegion& b) {
  return a.line == b.line && a.start_sample == b.start_sample &&
         a.end_sample == b.end_sample;
}

/// The overlay builder's view of a region. Drawing and hit-testing both go
/// through it, so a band that is drawn and a band that can be grabbed cannot
/// drift apart.
orc::gui::gpu::OverlaySpan spanOf(const orc::presenters::DropoutRegion& r) {
  return orc::gui::gpu::OverlaySpan{static_cast<int>(r.line),
                                    static_cast<int>(r.start_sample),
                                    static_cast<int>(r.end_sample)};
}

QRectF leftHandleRect(const QRectF& band) {
  return orc::gui::gpu::OverlayPrimitiveBuilder::leftHandleRect(band);
}

QRectF rightHandleRect(const QRectF& band) {
  return orc::gui::gpu::OverlayPrimitiveBuilder::rightHandleRect(band);
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
    const std::vector<orc::presenters::DropoutRegion>& source_dropouts,
    const std::vector<orc::presenters::DropoutRegion>& additions,
    const std::vector<orc::presenters::DropoutRegion>& removals) {
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
    const std::vector<orc::presenters::DropoutRegion>& additions,
    const std::vector<orc::presenters::DropoutRegion>& removals) {
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
    const orc::presenters::DropoutRegion& region) const {
  return std::any_of(removals_.begin(), removals_.end(),
                     [&region](const orc::presenters::DropoutRegion& removal) {
                       return sameRegion(removal, region);
                     });
}

bool DropoutFrameView::removalHasSource(
    const orc::presenters::DropoutRegion& removal) const {
  return std::any_of(source_dropouts_.begin(), source_dropouts_.end(),
                     [&removal](const orc::presenters::DropoutRegion& source) {
                       return sameRegion(source, removal);
                     });
}

QRectF DropoutFrameView::regionBandRect(
    const orc::presenters::DropoutRegion& region, bool emphasized) const {
  return orc::gui::gpu::OverlayPrimitiveBuilder::regionBandRect(
      viewGeometry(), spanOf(region), emphasized);
}

void DropoutFrameView::appendRegionBand(
    orc::gui::gpu::OverlayPrimitives& out,
    const orc::presenters::DropoutRegion& region, const QColor& color,
    bool emphasized, bool struck) const {
  orc::gui::gpu::OverlayPrimitiveBuilder::appendRegionBand(
      out, viewGeometry(), spanOf(region), color, emphasized, struck);
}

void DropoutFrameView::buildOverlay(
    orc::gui::gpu::OverlayPrimitives& out) const {
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
    orc::gui::gpu::OverlayPrimitiveBuilder::appendResizeHandles(
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
    orc::presenters::DropoutRegion preview;
    preview.line = static_cast<uint32_t>(drag_start_image_.y());
    preview.start_sample = static_cast<uint32_t>(
        std::min(drag_start_image_.x(), drag_current_image_.x()));
    preview.end_sample = static_cast<uint32_t>(
        std::max(drag_start_image_.x(), drag_current_image_.x()) + 1);
    appendRegionBand(out, preview, kAdditionColor, true, false);
  }
}

const orc::presenters::DropoutRegion*
DropoutFrameView::resizableSelectedRegion() const {
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
        orc::presenters::DropoutRegion region;
        region.line = static_cast<uint32_t>(line);
        region.start_sample = static_cast<uint32_t>(start_sample);
        region.end_sample = static_cast<uint32_t>(end_sample);
        region.basis =
            orc::presenters::DropoutRegion::DetectionBasis::HINT_DERIVED;
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
                        orc::presenters::FrameDropoutMap before,
                        orc::presenters::FrameDropoutMap after,
                        const QString& text, int merge_key)
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
  orc::presenters::FrameDropoutMap before_;
  orc::presenters::FrameDropoutMap after_;
  int merge_key_;
};

// ============================================================================
// DropoutEditorRenderWorker Implementation
// ============================================================================

DropoutEditorRenderWorker::DropoutEditorRenderWorker(
    std::shared_ptr<orc::presenters::IRenderPresenter> render_presenter,
    orc::NodeID input_node_id)
    : render_presenter_(std::move(render_presenter)),
      input_node_id_(std::move(input_node_id)),
      frame_output_type_(orc::PreviewOutputType::Frame_Field1_First) {}

void DropoutEditorRenderWorker::initialize() {
  bool success = false;
  uint64_t total_frames = 0;
  double dar_aspect_correction = 1.0;

  render_option_id_ = kSequentialOptionId;
  try {
    // Executes the DAG up to the input node on this worker thread.
    const auto outputs = render_presenter_->getAvailableOutputs(input_node_id_);
    for (const auto& output : outputs) {
      if (output.option_id == kSequentialOptionId) {
        frame_output_type_ = output.type;
        total_frames = output.count;
        dar_aspect_correction = output.dar_aspect_correction;
        if (output.has_separate_channels) {
          // Y/C sources: display luma, matching the preview dialog's default
          // "Y+C" signal selection.
          render_option_id_ += "_yc";
        }
        success = total_frames > 0;
        break;
      }
    }
  } catch (const std::exception& e) {
    ORC_LOG_ERROR("DropoutEditorRenderWorker: initialization failed: {}",
                  e.what());
  }

  if (!success) {
    ORC_LOG_ERROR(
        "DropoutEditorRenderWorker: no '{}' preview output available on "
        "input node '{}'",
        kSequentialOptionId, input_node_id_.to_string());
  } else {
    ORC_LOG_DEBUG("DropoutEditorRenderWorker: {} total frames", total_frames);
  }

  Q_EMIT initialized(success, total_frames, dar_aspect_correction);
}

void DropoutEditorRenderWorker::renderFrame(uint64_t frame_id) {
  orc::PreviewRenderResult result;
  result.success = false;
  try {
    result = render_presenter_->renderPreview(
        input_node_id_, frame_output_type_, frame_id, render_option_id_,
        orc::PreviewNavigationHint::Random);
  } catch (const std::exception& e) {
    result.error_message = e.what();
    ORC_LOG_ERROR("DropoutEditorRenderWorker: render of frame {} failed: {}",
                  frame_id, e.what());
  }
  Q_EMIT frameReady(frame_id, std::move(result));
}

// ============================================================================
// DropoutEditorDialog Implementation
// ============================================================================

DropoutEditorDialog::DropoutEditorDialog(
    orc::NodeID node_id, orc::presenters::DropoutPresenter* presenter,
    std::shared_ptr<orc::presenters::IRenderPresenter> render_presenter,
    orc::NodeID input_node_id, QWidget* parent)
    : QDialog(parent),
      node_id_(node_id),
      presenter_(presenter),
      render_presenter_(std::move(render_presenter)),
      input_node_id_(input_node_id),
      worker_(nullptr),
      dar_aspect_correction_(1.0),
      current_frame_id_(0),
      total_frames_(0),
      undo_stack_(new QUndoStack(this)) {
  if (!presenter_) {
    throw std::invalid_argument("Presenter cannot be null");
  }
  if (!render_presenter_) {
    throw std::invalid_argument("Render presenter cannot be null");
  }

  dropout_map_ = presenter_->getDropoutMap(node_id_);

  setupUI();

  // DAG execution and frame rendering run on a dedicated worker thread so
  // the dialog opens immediately and stays responsive. The RenderPresenter
  // is used exclusively from that thread from here on.
  worker_ = new DropoutEditorRenderWorker(render_presenter_, input_node_id_);
  worker_->moveToThread(&worker_thread_);
  connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
  connect(worker_, &DropoutEditorRenderWorker::initialized, this,
          &DropoutEditorDialog::onWorkerInitialized);
  connect(worker_, &DropoutEditorRenderWorker::frameReady, this,
          &DropoutEditorDialog::onFrameReady);
  worker_thread_.start();

  status_bar_->showMessage("Executing pipeline…");
  QMetaObject::invokeMethod(worker_, &DropoutEditorRenderWorker::initialize,
                            Qt::QueuedConnection);
}

DropoutEditorDialog::~DropoutEditorDialog() {
  worker_thread_.quit();
  worker_thread_.wait();
}

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
  connect(frame_view_, &FrameViewportWidget::zoomChanged, this,
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
  connect(frame_view_, &FrameViewportWidget::panRangeChanged, this,
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
  connect(frame_view_, &FrameViewportWidget::visibleOriginChanged, this,
          [this](QPoint origin) {
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
  connect(button_box, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(button_box->button(QDialogButtonBox::Apply), &QPushButton::clicked,
          this, &DropoutEditorDialog::onApply);
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

void DropoutEditorDialog::onWorkerInitialized(bool success,
                                              uint64_t total_frames,
                                              double dar_aspect_correction) {
  total_frames_ = total_frames;
  dar_aspect_correction_ = dar_aspect_correction;
  worker_ready_ = success && total_frames > 0;

  if (!worker_ready_) {
    status_bar_->showMessage(
        "No frames available — ensure the dropout_map stage has a valid "
        "video source connected");
    return;
  }

  frame_view_->setAspectCorrection(currentAspectCorrection());

  frame_slider_->setEnabled(true);
  frame_spin_box_->setEnabled(true);
  {
    // Range setup must not fire a debounced navigation.
    const QSignalBlocker slider_blocker(frame_slider_);
    frame_slider_->setRange(0, static_cast<int>(total_frames_) - 1);
    frame_spin_box_->setRange(1, static_cast<int>(total_frames_));
  }
  slider_max_label_->setText(QString::number(total_frames_));
  first_button_->setEnabled(true);
  last_button_->setEnabled(true);

  navigateToIndex(0);
}

void DropoutEditorDialog::navigateToIndex(int zero_based) {
  if (!worker_ready_) {
    return;
  }
  const int clamped = std::clamp(zero_based, frame_slider_->minimum(),
                                 frame_slider_->maximum());
  nav_debounce_timer_->stop();  // cancel any pending debounced commit
  setIndex(clamped);

  current_frame_id_ = static_cast<orc::FrameID>(clamped);
  updateFrameInfo();
  requestRenderForCurrentFrame();
}

void DropoutEditorDialog::navigateToIndexDebounced(int zero_based) {
  if (!worker_ready_) {
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
  if (!worker_ready_) {
    return;
  }
  if (render_in_flight_) {
    // onFrameReady re-requests when the delivered frame is stale.
    return;
  }
  render_in_flight_ = true;
  status_bar_->showMessage(
      QString("Rendering frame %1…").arg(current_frame_id_ + 1));
  const uint64_t frame_id = current_frame_id_;
  QMetaObject::invokeMethod(
      worker_,
      [worker = worker_, frame_id]() { worker->renderFrame(frame_id); },
      Qt::QueuedConnection);
}

void DropoutEditorDialog::onFrameReady(uint64_t frame_id,
                                       orc::PreviewRenderResult result) {
  render_in_flight_ = false;

  if (frame_id != current_frame_id_) {
    // Stale result from an earlier navigation target; render the current one.
    requestRenderForCurrentFrame();
    return;
  }

  if (!result.is_valid()) {
    ORC_LOG_ERROR("Failed to render frame {}: {}", frame_id,
                  result.error_message);
    status_bar_->showMessage(
        QString("Failed to render frame %1").arg(frame_id + 1));
    return;
  }

  frame_image_ =
      orc::gui::previewImageToQImage(result.image, std::move(frame_image_));

  // The sequential render reports source dropouts in image coordinates,
  // which equal frame-flat coordinates for this layout.
  std::vector<orc::presenters::DropoutRegion> source_dropouts =
      result.image.dropout_regions;

  const orc::presenters::FrameDropoutMap state = frameEditState(frame_id);
  frame_view_->setFrame(frame_image_, source_dropouts, state.additions,
                        state.removals);
  loaded_frame_id_ = static_cast<orc::FrameID>(frame_id);
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

// ============================================================================
// Undoable editing
// ============================================================================

orc::presenters::FrameDropoutMap DropoutEditorDialog::frameEditState(
    uint64_t frame_id) const {
  auto it = dropout_map_.find(frame_id);
  if (it != dropout_map_.end()) {
    return it->second;
  }
  orc::presenters::FrameDropoutMap empty_state;
  empty_state.frame_id = frame_id;
  return empty_state;
}

void DropoutEditorDialog::pushEditCommand(
    uint64_t frame_id, orc::presenters::FrameDropoutMap after,
    const QString& text, int merge_key) {
  undo_stack_->push(
      new DropoutMapEditCommand(this, frame_id, frameEditState(frame_id),
                                std::move(after), text, merge_key));
}

void DropoutEditorDialog::applyFrameEditState(
    uint64_t frame_id, const orc::presenters::FrameDropoutMap& state) {
  const bool empty = state.additions.empty() && state.removals.empty();
  if (empty) {
    dropout_map_.erase(frame_id);
  } else {
    orc::presenters::FrameDropoutMap& entry = dropout_map_[frame_id];
    entry.frame_id = frame_id;
    entry.additions = state.additions;
    entry.removals = state.removals;
  }

  if (loaded_frame_id_.has_value() && *loaded_frame_id_ == frame_id) {
    frame_view_->updateRegions(state.additions, state.removals);
    refreshRegionTable();
    selectRegion(DropoutFrameView::RegionKind::None, -1);
  } else if (worker_ready_ &&
             frame_id != static_cast<uint64_t>(current_frame_id_)) {
    // Undo/redo touched a frame we are not looking at: go there so the user
    // sees the effect of the operation.
    navigateToIndex(static_cast<int>(frame_id));
  }

  updateFrameInfo();
}

void DropoutEditorDialog::onRegionAddRequested(
    const orc::presenters::DropoutRegion& region) {
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
    int index, const orc::presenters::DropoutRegion& new_region) {
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
    int index, const orc::presenters::DropoutRegion& new_region) {
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

  auto findRemoval = [&after](const orc::presenters::DropoutRegion& region) {
    return std::find_if(after.removals.begin(), after.removals.end(),
                        [&region](const orc::presenters::DropoutRegion& r) {
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
      const orc::presenters::DropoutRegion region = sources[index];
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
  orc::presenters::FrameDropoutMap empty_state;
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
        removed = std::any_of(
            removals.begin(), removals.end(),
            [&sources, index](const orc::presenters::DropoutRegion& r) {
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
                           : orc::presenters::FrameDropoutMap();
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
        removed = std::any_of(
            removals.begin(), removals.end(),
            [&sources, index](const orc::presenters::DropoutRegion& r) {
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
                       const orc::presenters::DropoutRegion& region,
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
                    [&sources, i](const orc::presenters::DropoutRegion& r) {
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
                         : orc::presenters::FrameDropoutMap();

  frame_info_label_->setText(
      QString("Frame %1 of %2 — Additions: %3, Removals: %4")
          .arg(current_frame_id_ + 1)
          .arg(total_frames_)
          .arg(state.additions.size())
          .arg(state.removals.size()));

  const bool has_frames = worker_ready_ && total_frames_ > 0;
  prev_button_->setEnabled(has_frames && current_frame_id_ > 0);
  next_button_->setEnabled(has_frames &&
                           current_frame_id_ <
                               static_cast<orc::FrameID>(total_frames_) - 1);

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
  prev_edit_button_->setEnabled(worker_ready_ && has_prev_edit);
  next_edit_button_->setEnabled(worker_ready_ && has_next_edit);
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

void DropoutEditorDialog::onApply() { Q_EMIT applied(); }
