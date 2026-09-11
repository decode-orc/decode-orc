/*
 * File:        dropout_editor_dialog.h
 * Module:      orc-gui
 * Purpose:     Dropout map editor dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef DROPOUT_EDITOR_DIALOG_H
#define DROPOUT_EDITOR_DIALOG_H

#include <orc/stage/common_types.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/node_id.h>
#include <orc/stage/preview/orc_rendering.h>

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
#include <QThread>
#include <QTimer>
#include <QUndoStack>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "frame_marker_slider.h"
#include "frame_viewport_widget.h"
#include "presenters/include/dropout_presenter.h"
#include "render_coordinator.h"  // IRenderPresenter

/**
 * @brief Interactive widget for displaying and editing dropout regions on a
 * frame image
 *
 * Displays a full video frame (non-interlaced, sequential lines) rendered by
 * the shared preview pipeline with modeless direct-manipulation editing:
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
 * start_sample/end_sample = sample within that line), matching the
 * "sequential" preview render whose image rows are frame-flat lines.
 */
class DropoutFrameView : public FrameViewportWidget {
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
  void setFrame(
      const QImage& frame_image,
      const std::vector<orc::presenters::DropoutRegion>& source_dropouts,
      const std::vector<orc::presenters::DropoutRegion>& additions,
      const std::vector<orc::presenters::DropoutRegion>& removals);

  /**
   * @brief Replace the edit region lists without touching the frame image
   * (used after undoable commands change the frame's edit state).
   */
  void updateRegions(
      const std::vector<orc::presenters::DropoutRegion>& additions,
      const std::vector<orc::presenters::DropoutRegion>& removals);

  const std::vector<orc::presenters::DropoutRegion>& getAdditions() const {
    return additions_;
  }
  const std::vector<orc::presenters::DropoutRegion>& getRemovals() const {
    return removals_;
  }
  const std::vector<orc::presenters::DropoutRegion>& getSourceDropouts() const {
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
  void regionAddRequested(const orc::presenters::DropoutRegion& region);

  /**
   * @brief Emitted when a move/resize drag of an addition completes
   */
  void additionModifyRequested(
      int index, const orc::presenters::DropoutRegion& new_region);

  /**
   * @brief Emitted when a resize drag of a source dropout's handle completes
   *
   * The source list itself is never modified; the dialog is expected to mark
   * the source dropout removed and add new_region as an addition in a single
   * undoable command.
   */
  void sourceResizeRequested(int index,
                             const orc::presenters::DropoutRegion& new_region);

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
  void buildOverlay(orc::gui::gpu::OverlayPrimitives& out) const override;

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
  const orc::presenters::DropoutRegion* resizableSelectedRegion() const;

  /// Widget-space rectangle of a region's overlay band.
  QRectF regionBandRect(const orc::presenters::DropoutRegion& region,
                        bool emphasized) const;

  void appendRegionBand(orc::gui::gpu::OverlayPrimitives& out,
                        const orc::presenters::DropoutRegion& region,
                        const QColor& color, bool emphasized,
                        bool struck) const;
  bool isRegionMarkedForRemoval(
      const orc::presenters::DropoutRegion& region) const;
  bool removalHasSource(const orc::presenters::DropoutRegion& removal) const;

  /// Update selection state and emit selectionChanged when it changed.
  void selectFromInteraction(RegionKind kind, int index);
  void updateHoverState(const QPointF& widget_pos);
  void updateCursorShape();

  // Region data (all coordinates are frame-flat 0-based). additions_ is
  // adjusted locally during move/resize drags for live feedback; the
  // authoritative state lives in the dialog's dropout map.
  std::vector<orc::presenters::DropoutRegion>
      source_dropouts_;  // From source TBC
  std::vector<orc::presenters::DropoutRegion> additions_;
  std::vector<orc::presenters::DropoutRegion> removals_;

  // Selection
  RegionKind selected_kind_ = RegionKind::None;
  int selected_index_ = -1;

  // Hover state
  Hit hover_;

  // Drag state
  DragMode drag_mode_ = DragMode::None;
  int drag_index_ = -1;
  orc::presenters::DropoutRegion drag_original_;
  QPoint drag_start_image_;    // Image pixel at press
  QPoint drag_current_image_;  // For Adding preview
  QPointF drag_start_widget_;
  // Live extent during a source-resize drag; sources stay untouched so
  // removal entries keep matching them by exact region identity.
  orc::presenters::DropoutRegion source_resize_preview_;
};

/**
 * @brief Worker that runs DAG execution and frame rendering off the GUI thread
 *
 * Lives on a dedicated QThread owned by DropoutEditorDialog. The
 * RenderPresenter is used exclusively from this worker's thread (it is not
 * thread-safe); the dialog communicates with the worker only through queued
 * signal/slot connections.
 */
class DropoutEditorRenderWorker : public QObject {
  Q_OBJECT

 public:
  DropoutEditorRenderWorker(
      std::shared_ptr<orc::presenters::IRenderPresenter> render_presenter,
      orc::NodeID input_node_id);

 public slots:
  /**
   * @brief Execute the DAG to the input node and discover frame outputs.
   *
   * Picks the "sequential" preview option (image rows = frame-flat lines)
   * and emits initialized() with the outcome.
   */
  void initialize();

  /**
   * @brief Render one frame; emits frameReady() with the result.
   */
  void renderFrame(uint64_t frame_id);

 signals:
  /**
   * @brief Emitted when initialization completes
   * @param success False when no sequential frame output is available
   * @param total_frames Number of frames at the input node
   * @param dar_aspect_correction Width scale factor for 4:3 DAR display
   */
  void initialized(bool success, uint64_t total_frames,
                   double dar_aspect_correction);

  /**
   * @brief Emitted when a frame render completes (check result.is_valid())
   */
  void frameReady(uint64_t frame_id, orc::PreviewRenderResult result);

 private:
  std::shared_ptr<orc::presenters::IRenderPresenter> render_presenter_;
  orc::NodeID input_node_id_;
  orc::PreviewOutputType frame_output_type_;
  std::string render_option_id_;
};

/**
 * @brief Dialog for editing dropout map for a stage
 *
 * This dialog allows the user to:
 * - Navigate through frames with preview-style controls (first/prev/next/last
 *   buttons, scrub slider with debounce, 1-based jump spin box)
 * - Jump between frames that carry edits (slider tick marks, Prev/Next Edit)
 * - Edit dropouts modelessly: drag to add, drag selected regions to
 *   move/resize, Delete to delete or toggle removal, right-click for a
 *   context menu, arrow keys to nudge the selected addition
 * - Undo/redo every edit (Ctrl+Z / Ctrl+Shift+Z), including across frames
 * - Save changes back to the dropout map stage parameter
 *
 * The dialog's dropout_map_ is the authoritative edit state; every mutation
 * goes through a QUndoCommand holding before/after snapshots of one frame's
 * edits, so undo/redo is exact and navigation never needs to "save" edits.
 *
 * The frame image is rendered through the shared preview pipeline
 * (RenderPresenter, "sequential" frame layout) on a worker thread so the GUI
 * stays responsive; the dialog opens immediately and shows progress in its
 * status bar while the pipeline executes.
 *
 * All displayed and stored coordinates are frame-flat 0-based, matching the
 * DropoutMapStage serialisation format; user-visible frame and line numbers
 * are 1-based per the frame/field presentation convention.
 */
class DropoutEditorDialog : public QDialog {
  Q_OBJECT

 public:
  /**
   * @brief Create a dropout editor dialog
   * @param node_id Node ID of the dropout_map stage
   * @param presenter Dropout presenter for map parameter access
   * @param render_presenter Render presenter for frame rendering (must have
   * dropout overlay rendering disabled; used exclusively from the dialog's
   * render worker thread after construction)
   * @param input_node_id Node ID of the dropout_map stage's video input
   * @param parent Parent widget
   */
  explicit DropoutEditorDialog(
      orc::NodeID node_id, orc::presenters::DropoutPresenter* presenter,
      std::shared_ptr<orc::presenters::IRenderPresenter> render_presenter,
      orc::NodeID input_node_id, QWidget* parent = nullptr);

  ~DropoutEditorDialog() override;

  /**
   * @brief Get the edited dropout map
   * @return Map of frame IDs to dropout modifications
   */
  std::map<uint64_t, orc::presenters::FrameDropoutMap> getDropoutMap() const {
    return dropout_map_;
  }

 signals:
  /**
   * @brief Emitted when the user clicks Apply (map saved but dialog stays open)
   */
  void applied();

 private slots:
  void onWorkerInitialized(bool success, uint64_t total_frames,
                           double dar_aspect_correction);
  void onFrameReady(uint64_t frame_id, orc::PreviewRenderResult result);
  void onViewSelectionChanged(DropoutFrameView::RegionKind kind, int index);
  void onRegionAddRequested(const orc::presenters::DropoutRegion& region);
  void onAdditionModifyRequested(
      int index, const orc::presenters::DropoutRegion& new_region);
  void onSourceResizeRequested(
      int index, const orc::presenters::DropoutRegion& new_region);
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
  void onApply();

 private:
  friend class DropoutMapEditCommand;

  void setupUI();

  /// @name Navigation (mirrors PreviewDialog semantics)
  /// @{
  /// Navigate immediately: retarget and request a render.
  void navigateToIndex(int zero_based);
  /// Update UI immediately, commit navigation after the debounce settles.
  void navigateToIndexDebounced(int zero_based);
  /// Silently sync slider + spinbox display without emitting signals.
  void setIndex(int zero_based);
  /// @}

  /// Request a render of current_frame_id_, coalescing while one is in
  /// flight.
  void requestRenderForCurrentFrame();

  /// @name Undoable editing
  /// @{
  /// Current edit state for a frame (empty when the frame has no edits).
  orc::presenters::FrameDropoutMap frameEditState(uint64_t frame_id) const;
  /// Push an undoable transition of one frame's edit state onto the stack.
  /// merge_key >= 0 makes consecutive commands with the same key coalesce
  /// (used for arrow-key nudges).
  void pushEditCommand(uint64_t frame_id,
                       orc::presenters::FrameDropoutMap after,
                       const QString& text, int merge_key = -1);
  /// Apply a frame's edit state (called by undo commands and pushes).
  void applyFrameEditState(uint64_t frame_id,
                           const orc::presenters::FrameDropoutMap& state);
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

  // Presenter and node info
  orc::NodeID node_id_;
  orc::presenters::DropoutPresenter* presenter_;

  // Frame rendering (shared preview pipeline, worker thread)
  std::shared_ptr<orc::presenters::IRenderPresenter> render_presenter_;
  orc::NodeID input_node_id_;
  QThread worker_thread_;
  DropoutEditorRenderWorker* worker_;  // Owned by worker_thread_ finish hook
  double dar_aspect_correction_;
  bool worker_ready_ = false;
  bool render_in_flight_ = false;

  // Current state
  orc::FrameID current_frame_id_;                // Navigation target
  std::optional<orc::FrameID> loaded_frame_id_;  // Frame shown in the view
  size_t total_frames_;
  // Authoritative edit state; mutated only via applyFrameEditState()
  std::map<uint64_t, orc::presenters::FrameDropoutMap> dropout_map_;
  QImage frame_image_;  // Reused buffer for PreviewImage conversion
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
};

#endif  // DROPOUT_EDITOR_DIALOG_H
