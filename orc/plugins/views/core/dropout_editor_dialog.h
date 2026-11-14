/*
 * File:        dropout_editor_dialog.h
 * Module:      orc-view_core
 * Purpose:     Dropout Editor: mark and remove dropouts frame by frame,
 *              written back to the dropout_map stage through its session
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef DROPOUT_EDITOR_DIALOG_H
#define DROPOUT_EDITOR_DIALOG_H

#include <orc/view-support/frame_viewport_widget.h>
#include <orc/view-support/overlay_primitives.h>
#include <orc/view/stage_tool_session.h>

#include <QComboBox>
#include <QDialog>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QUndoStack>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../stages/common/dropout-map/dropout_map_text.h"
#include "frame_marker_slider.h"

namespace dropout_editor {

/// A dropout extent: frame-flat 0-based line, samples within the line. The
/// stage's own type, so what the editor stores is what the stage reads.
using Region = orc::dropout_map_text::Span;

/// One frame's additions and removals.
using FrameEdits = orc::dropout_map_text::FrameEdits;

}  // namespace dropout_editor

/**
 * @brief Interactive widget for displaying and editing dropout regions on a
 * frame image
 *
 * Displays a full video frame (non-interlaced, sequential lines) rendered by
 * the host's preview renderer with modeless direct-manipulation editing:
 * - Drag on an empty area to add a dropout region
 * - Click a region to select it; drag a selected addition to move it
 * - Drag the end handles of a selected addition to resize it
 * - Drag the end handles of a selected source dropout to resize it by
 *   replacement: the dialog marks the source dropout removed and creates an
 *   addition covering the new extent in one undoable step
 * - Right-click for context actions (delete / mark removed / restore)
 *
 * The widget does not mutate the dropout map itself: completed interactions
 * are emitted as intent signals (regionAddRequested,
 * additionModifyRequested, ...) and the dialog turns them into undoable
 * commands. During a drag the widget adjusts its local copy for live
 * feedback only.
 *
 * Hit-testing runs in widget space against the drawn overlay bands, so
 * regions remain clickable at any zoom level.
 *
 * All coordinates are frame-flat 0-based (line = 0-based frame line index,
 * start_sample/end_sample = sample within that line), matching the frames
 * IStageToolSession::render_frame() renders, whose image rows are frame-flat
 * lines.
 */
class DropoutFrameView : public orc::view::FrameViewportWidget {
  Q_OBJECT

 public:
  /**
   * @brief Kind of region an interaction refers to
   *
   * OrphanRemoval identifies a removal entry with no matching source dropout
   * (possible in hand-edited project files); it is shown struck-through and
   * can only be deleted.
   */
  enum class RegionKind { None, Source, Addition, OrphanRemoval };
  Q_ENUM(RegionKind)

  explicit DropoutFrameView(QWidget* parent = nullptr);
  ~DropoutFrameView() override = default;

  /**
   * @brief Set the frame to display
   * @param frame_image Rendered frame image (rows = frame-flat lines)
   * @param source_dropouts Existing dropout regions from source
   * @param additions Dropout regions to add
   * @param removals Dropout regions to remove
   */
  void setFrame(const QImage& frame_image,
                const std::vector<dropout_editor::Region>& source_dropouts,
                const std::vector<dropout_editor::Region>& additions,
                const std::vector<dropout_editor::Region>& removals);

  /**
   * @brief Replace the edit region lists without touching the frame image
   * (used after undoable commands change the frame's edit state).
   */
  void updateRegions(const std::vector<dropout_editor::Region>& additions,
                     const std::vector<dropout_editor::Region>& removals);

  const std::vector<dropout_editor::Region>& getAdditions() const {
    return additions_;
  }
  const std::vector<dropout_editor::Region>& getRemovals() const {
    return removals_;
  }
  const std::vector<dropout_editor::Region>& getSourceDropouts() const {
    return source_dropouts_;
  }

  /**
   * @brief Get frame dimensions for external access
   */
  int getFrameWidth() const { return imageSize().width(); }
  int getFrameHeight() const { return imageSize().height(); }

  /**
   * @brief Programmatically set the selection (e.g., from the region table).
   * Does not emit selectionChanged.
   */
  void setSelectedRegion(RegionKind kind, int index);
  RegionKind selectedKind() const { return selected_kind_; }
  int selectedIndex() const { return selected_index_; }

  /// True when this removal entry has no matching source dropout.
  bool isOrphanRemoval(int removal_index) const;

 signals:
  /**
   * @brief Emitted when the user changes the selection by clicking
   */
  void selectionChanged(RegionKind kind, int index);

  /**
   * @brief Emitted when the user completes a drag on an empty area
   */
  void regionAddRequested(const dropout_editor::Region& region);

  /**
   * @brief Emitted when a move/resize drag of an addition completes
   */
  void additionModifyRequested(int index,
                               const dropout_editor::Region& new_region);

  /**
   * @brief Emitted when a resize drag of a source dropout's handle completes
   *
   * The source list itself is never modified; the dialog is expected to mark
   * the source dropout removed and add new_region as an addition in a single
   * undoable command.
   */
  void sourceResizeRequested(int index,
                             const dropout_editor::Region& new_region);

  /**
   * @brief Emitted on right-click; kind is None when no region was hit
   */
  void contextMenuRequested(RegionKind kind, int index,
                            const QPoint& global_pos);

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void buildOverlay(orc::view::OverlayPrimitives& out) const override;

 private:
  enum class DragMode {
    None,
    Adding,       ///< Dragging out a new region on empty area
    PendingMove,  ///< Pressed on an addition; becomes Moving past a threshold
    Moving,       ///< Dragging a selected addition (line + samples)
    ResizingLeft,
    ResizingRight,
    ResizingSourceLeft,  ///< Resizing a source dropout (replace-on-release)
    ResizingSourceRight
  };

  struct Hit {
    RegionKind kind = RegionKind::None;
    int index = -1;
    enum class Part { Body, LeftHandle, RightHandle };
    Part part = Part::Body;
  };

  /// Widget-space hit test against the drawn overlay bands (and the selected
  /// region's resize handles).
  Hit hitTest(const QPointF& widget_pos) const;

  /// Selected region whose end handles are active: the selected addition, or
  /// the selected source dropout when it is not marked removed.
  const dropout_editor::Region* resizableSelectedRegion() const;

  /// Widget-space rectangle of a region's overlay band.
  QRectF regionBandRect(const dropout_editor::Region& region,
                        bool emphasized) const;

  void appendRegionBand(orc::view::OverlayPrimitives& out,
                        const dropout_editor::Region& region,
                        const QColor& color, bool emphasized,
                        bool struck) const;
  bool isRegionMarkedForRemoval(const dropout_editor::Region& region) const;
  bool removalHasSource(const dropout_editor::Region& removal) const;

  /// Update selection state and emit selectionChanged when it changed.
  void selectFromInteraction(RegionKind kind, int index);
  void updateHoverState(const QPointF& widget_pos);
  void updateCursorShape();

  // Region data (all coordinates are frame-flat 0-based). additions_ is
  // adjusted locally during move/resize drags for live feedback; the
  // authoritative state lives in the dialog's dropout map.
  std::vector<dropout_editor::Region>
      source_dropouts_;  // What the input reports
  std::vector<dropout_editor::Region> additions_;
  std::vector<dropout_editor::Region> removals_;

  // Selection
  RegionKind selected_kind_ = RegionKind::None;
  int selected_index_ = -1;

  // Hover state
  Hit hover_;

  // Drag state
  DragMode drag_mode_ = DragMode::None;
  int drag_index_ = -1;
  dropout_editor::Region drag_original_;
  QPoint drag_start_image_;    // Image pixel at press
  QPoint drag_current_image_;  // For Adding preview
  QPointF drag_start_widget_;
  // Live extent during a source-resize drag; sources stay untouched so
  // removal entries keep matching them by exact region identity.
  dropout_editor::Region source_resize_preview_;
};

/**
 * @brief The Dropout Editor window for a dropout_map node
 *
 * This dialog allows the user to:
 * - Navigate through frames with preview-style controls (first/prev/next/last
 *   buttons, scrub slider with debounce, 1-based jump spin box)
 * - Jump between frames that carry edits (slider tick marks, Prev/Next Edit)
 * - Edit dropouts modelessly: drag to add, drag selected regions to
 *   move/resize, Delete to delete or toggle removal, right-click for a
 *   context menu, arrow keys to nudge the selected addition
 * - Undo/redo every edit (Ctrl+Z / Ctrl+Shift+Z), including across frames
 * - Write the map back to the node (Apply, or OK to write and close)
 *
 * Everything goes through the tool session: the stored map comes from
 * parameters(), frames of the node's input (with the dropouts the input
 * reports) from render_frame(), and Apply/OK write the "dropout_map"
 * parameter with apply_parameters(). The window stays open across graph
 * rebuilds, its own applies included, and re-renders the frame it shows.
 *
 * The dialog's dropout_map_ is the authoritative edit state; every mutation
 * goes through a QUndoCommand holding before/after snapshots of one frame's
 * edits, so undo/redo is exact and navigation never needs to "save" edits.
 * The undo stack is the window's own: an apply cannot be undone from the
 * project.
 *
 * All displayed and stored coordinates are frame-flat 0-based, matching the
 * DropoutMapStage serialisation format; user-visible frame and line numbers
 * are 1-based per the frame/field presentation convention.
 */
class DropoutEditorDialog : public QDialog {
  Q_OBJECT

 public:
  /// @param session The tool session of the dropout_map node
  explicit DropoutEditorDialog(
      std::shared_ptr<orc::view::IStageToolSession> session,
      QWidget* parent = nullptr);

  ~DropoutEditorDialog() override;

  /// The edited map, by frame.
  const orc::dropout_map_text::EditMap& getDropoutMap() const {
    return dropout_map_;
  }

 private slots:
  void onViewSelectionChanged(DropoutFrameView::RegionKind kind, int index);
  void onRegionAddRequested(const dropout_editor::Region& region);
  void onAdditionModifyRequested(int index,
                                 const dropout_editor::Region& new_region);
  void onSourceResizeRequested(int index,
                               const dropout_editor::Region& new_region);
  void onContextMenuRequested(DropoutFrameView::RegionKind kind, int index,
                              const QPoint& global_pos);
  void onTableSelectionChanged();
  void onClearCurrentFrame();
  void onZoomIn();
  void onZoomOut();
  void onZoomReset();
  void onZoomFit();
  void onAspectRatioChanged(int index);
  void onPreviousEditedFrame();
  void onNextEditedFrame();
  void onFrameViewZoomChanged(double zoom_level);

 private:
  friend class DropoutMapEditCommand;

  void setupUI();

  /// @name Frames from the session
  /// @{
  /// Request a render of current_frame_id_, coalescing while one is in
  /// flight.
  void requestRenderForCurrentFrame();
  void onFrameRendered(orc::view::ToolFrameImage image);
  /// Size the navigation for @p frame_count frames.
  void setFrameCount(uint64_t frame_count);
  /// The graph was rebuilt (an apply, or an edit elsewhere): render again.
  void onGraphRebuilt();
  /// @}

  /// Write the map to the node; close afterwards when @p close_after.
  void submit(bool close_after);
  void setSubmitEnabled(bool enabled);

  /// @name Navigation (mirrors PreviewDialog semantics)
  /// @{
  /// Navigate immediately: retarget and request a render.
  void navigateToIndex(int zero_based);
  /// Update UI immediately, commit navigation after the debounce settles.
  void navigateToIndexDebounced(int zero_based);
  /// Silently sync slider + spinbox display without emitting signals.
  void setIndex(int zero_based);
  /// @}

  /// @name Undoable editing
  /// @{
  /// Current edit state for a frame (empty when the frame has no edits).
  dropout_editor::FrameEdits frameEditState(uint64_t frame_id) const;
  /// Push an undoable transition of one frame's edit state onto the stack.
  /// merge_key >= 0 makes consecutive commands with the same key coalesce
  /// (used for arrow-key nudges).
  void pushEditCommand(uint64_t frame_id, dropout_editor::FrameEdits after,
                       const QString& text, int merge_key = -1);
  /// Apply a frame's edit state (called by undo commands and pushes).
  void applyFrameEditState(uint64_t frame_id,
                           const dropout_editor::FrameEdits& state);
  /// Delete the selected addition/orphan removal, or toggle removal of the
  /// selected source dropout.
  void deleteOrToggleSelection();
  /// Nudge the selected addition by (dx samples, dy lines) as a mergeable
  /// undo command.
  void nudgeSelectedAddition(int dx, int dy);
  /// @}

  /// Select a region in both the frame view and the region table.
  void selectRegion(DropoutFrameView::RegionKind kind, int index);
  /// Rebuild the region table from the loaded frame's regions.
  void refreshRegionTable();

  /// Sorted 0-based indices of frames carrying edits.
  std::vector<int> editedFrameIndices() const;

  void updateFrameInfo();
  void updateEditedFrameNavigation();
  void updateZoomLabel(double zoom_level);
  double currentAspectCorrection() const;
  void keyPressEvent(QKeyEvent* event) override;
  void showEvent(QShowEvent* event) override;

  std::shared_ptr<orc::view::IStageToolSession> session_;

  // Frames from the session
  double dar_aspect_correction_ = 1.0;
  bool frames_ready_ = false;
  bool render_in_flight_ = false;
  // The graph was rebuilt while a render was in flight, so its frame is
  // from the old graph.
  bool render_stale_ = false;

  // Current state
  uint64_t current_frame_id_ = 0;            // Navigation target
  std::optional<uint64_t> loaded_frame_id_;  // Frame shown in the view
  size_t total_frames_ = 0;
  // Authoritative edit state; mutated only via applyFrameEditState()
  orc::dropout_map_text::EditMap dropout_map_;
  bool initial_fit_done_ = false;
  bool syncing_selection_ = false;  // Guard against table<->view echo

  // Undo/redo
  QUndoStack* undo_stack_;

  // UI elements
  QTimer* nav_debounce_timer_;
  QPushButton* first_button_;
  QPushButton* prev_button_;
  QPushButton* next_button_;
  QPushButton* last_button_;
  QSpinBox* frame_spin_box_;
  QLabel* slider_min_label_;
  QLabel* slider_max_label_;
  FrameMarkerSlider* frame_slider_;
  QPushButton* prev_edit_button_;
  QPushButton* next_edit_button_;
  QStatusBar* status_bar_;
  QLabel* frame_info_label_;
  QPushButton* undo_button_;
  QPushButton* redo_button_;
  QPushButton* delete_button_;
  QPushButton* clear_frame_button_;
  QTableWidget* region_table_;
  DropoutFrameView* frame_view_;
  QWidget* view_container_;
  QScrollBar* horizontal_pan_bar_;
  QScrollBar* vertical_pan_bar_;
  QComboBox* aspect_ratio_combo_;
  QPushButton* zoom_in_button_;
  QPushButton* zoom_out_button_;
  QPushButton* zoom_reset_button_;
  QPushButton* zoom_fit_button_;
  QLabel* zoom_label_;
  QPushButton* ok_button_;
  QPushButton* apply_button_;
};

#endif  // DROPOUT_EDITOR_DIALOG_H
