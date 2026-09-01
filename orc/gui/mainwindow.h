/*
 * File:        mainwindow.h
 * Module:      orc-gui
 * Purpose:     Main application window
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <orc/stage/common_types.h>  // For VideoSystem, SourceType
#include <orc/stage/node_id.h>
#include <orc/stage/preview/orc_preview_types.h>
#include <orc/stage/preview/orc_rendering.h>  // Public API rendering types
#include <orc_analysis.h>                     // For AnalysisToolInfo

#include <QElapsedTimer>
#include <QMainWindow>
#include <QPointer>
#include <QProgressDialog>
#include <QString>
#include <QTabWidget>
#include <QTimer>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "guiproject.h"
#include "orcgraphicsscene.h"
#include "orcgraphmodel.h"
#include "presenters/include/dropout_presenter.h"
#include "presenters/include/vbi_view_models.h"
#include "render_coordinator.h"

class OrcGraphicsView;
class PreviewDialog;
class VBIDialog;
class VideoParameterObserverDialog;
class NtscObserverDialog;
class CatalogueDialog;
class ClosedCaptionDialog;
class DropoutAnalysisDialog;
class SNRAnalysisDialog;
class BurstLevelAnalysisDialog;
class RenderCoordinator;

namespace orc {
class DropoutAnalysisDecoder;
enum class DropoutAnalysisMode;
class SNRAnalysisDecoder;
enum class SNRAnalysisMode;
}  // namespace orc

namespace orc {
class DAG;
class AnalysisTool;
class DropoutAnalysisDecoder;
}  // namespace orc

class FieldPreviewWidget;
class QLabel;
class QSlider;
class QToolBar;
class QMenu;
class QComboBox;
class QSplitter;
class QTimer;

/**
 * Main window for orc-gui
 *
 * Layout:
 * - Toolbar (file operations, source selection)
 * - Central preview area
 * - Bottom status/navigation bar
 *
 * Architecture: This window is a thin display client.
 * All rendering logic is in orc::PreviewRenderer (orc-core).
 */

using orc::NodeID;  // Make NodeID available for Qt signals/slots

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow();

  // Project operations
  void newProject(orc::VideoSystem video_format = orc::VideoSystem::Unknown,
                  orc::SourceType source_format = orc::SourceType::Unknown);
  void openProject(const QString& filename);
  void quickProject(const QString& filename);  ///< Create a quick project from
                                               ///< a TBC/TBCC/TBCY file
  bool saveProject();    ///< Returns true if the project was saved
  bool saveProjectAs();  ///< Returns true if the project was saved

 protected:
  void keyPressEvent(QKeyEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  void closeEvent(QCloseEvent* event) override;

 private slots:
  void onNewProject();    // Shows selection dialog for all four project types
  void onQuickProject();  // Load a TBC/TBCC/TBCY file and create project
                          // automatically
  void onOpenProject();
  void onSaveProject();
  void onSaveProjectAs();
  void onEditProject();
  void onReloadAllSources();
  void onPreviewIndexChanged(int index);
  void onNavigatePreview(int delta);
  void onPreviewModeChanged(int index);
  void onAspectRatioModeChanged(int index);
  void onNodeSelectedForView(const orc::NodeID& node_id);
  void onDAGModified();
  void onPreviewDialogExportPNG();
  void onNodeContextMenu(QtNodes::NodeId nodeId, const QPointF& pos);
  void onArrangeDAGToGrid();
  void onQtNodeSelected(QtNodes::NodeId nodeId);
  void onShowVBIDialog();
  void updateVBIDialog();
  void onShowVideoParameterObserverDialog();
  void updateVideoParameterObserverDialog();
  void onShowNtscObserverDialog();
  void updateNtscObserverDialog();
  void onShowClosedCaptionDialog();
  void updateClosedCaptionDialog();
  /// Issue the next batch of closed caption observation requests for the
  /// dialog's window. Called on every frame change and again as each delivery
  /// lands, because the dialog paces the frames it asks for.
  void issueClosedCaptionRequests();
  /// Issue async observation requests for whichever observer dialogs are open
  /// (Phase 5). Replaces the synchronous per-dialog render-and-extract path.
  void refreshObserverDialogs();
  void onLineScopeRequested(int image_x, int image_y);
  void onLineScopeRefreshAtFieldLine();  ///< Refresh line scope at stored
                                         ///< field/line (for frame changes)
  void onLineNavigation(int direction, uint64_t current_field, int current_line,
                        int sample_x, int preview_image_width);
  void onSampleMarkerMoved(int sample_x);
  void
  refreshLineScopeForCurrentStage();  ///< Refresh line scope when stage changes
  void onFrameTimingRequested();
  void onWaveformMonitorRequested();
  void onSetCrosshairsFromFrameTiming();
  void onFrameScopeDialogClosed();
  void onPreviewVectorscopeRequested(const orc::PreviewCoordinate& coordinate);
  void onPreviewHistogramRequested(const orc::PreviewCoordinate& coordinate);

  // Coordinator response slots
  void onPreviewReady(uint64_t request_id, orc::PreviewRenderResult result);
  void onVBIDataReady(uint64_t request_id,
                      orc::presenters::VBIFieldInfoView info);
  // Phase 5: async observation delivery + background-workload progress.
  void onObservationDataReady(
      uint64_t request_id, bool available, qulonglong field_id_value,
      orc::presenters::VideoParameterObservationView video_params,
      orc::presenters::NtscFieldObservationsView ntsc);
  void onClosedCaptionDataReady(
      uint64_t request_id, bool available, qulonglong field1_id_value,
      orc::presenters::ClosedCaptionFieldDataView field1,
      qulonglong field2_id_value,
      orc::presenters::ClosedCaptionFieldDataView field2);
  void onObservationProgress(bool active, int percent_complete, bool computing,
                             qulonglong outstanding_nodes);
  void onExecutionProgress(int node_id_value, qulonglong current,
                           qulonglong total);
  void onObservationsInvalidated(QVector<int> changed_node_ids);
  void onAvailableOutputsReady(uint64_t request_id,
                               std::vector<orc::PreviewOutputInfo> outputs);
  // Preview audio playback: the pair list feeds the dialogue's selector, the
  // reader is what playback pulls samples from.
  void onAudioChannelPairsReady(uint64_t request_id,
                                std::vector<orc::AudioPairView> pairs);
  void onAudioStreamReaderReady(
      uint64_t request_id,
      std::shared_ptr<orc::presenters::IAudioStreamReader> reader);
  void onLineSamplesReady(uint64_t request_id, uint64_t field_index,
                          int line_number, int sample_x,
                          std::vector<int16_t> samples,
                          std::optional<orc::SourceParameters> video_params,
                          std::vector<int16_t> y_samples,
                          std::vector<int16_t> c_samples);
  void onFrameTimingDataReady(uint64_t request_id, uint64_t field_index,
                              std::optional<uint64_t> field_index_2,
                              std::vector<int16_t> samples,
                              std::vector<int16_t> samples_2,
                              std::vector<int16_t> y_samples,
                              std::vector<int16_t> c_samples,
                              std::vector<int16_t> y_samples_2,
                              std::vector<int16_t> c_samples_2,
                              int first_field_height, int second_field_height);
  void onWaveformMonitorDataReady(uint64_t request_id,
                                  std::vector<int16_t> composite_samples,
                                  std::vector<int16_t> y_samples,
                                  std::vector<int16_t> c_samples,
                                  int first_field_height,
                                  int second_field_height);
  void onFrameLineNavigationReady(uint64_t request_id,
                                  orc::FrameLineNavigationResult result);
  void onDropoutDataReady(uint64_t request_id,
                          orc::presenters::DropoutDisplaySeries series);
  void onSNRDataReady(uint64_t request_id,
                      orc::presenters::SNRDisplaySeries series);
  void onBurstLevelDataReady(uint64_t request_id,
                             orc::presenters::BurstLevelDisplaySeries series);
  void onCatalogueDataReady(uint64_t request_id, orc::CatalogueDataset data);
  void onResultsNotAvailable(uint64_t request_id);
  void onTriggerProgress(size_t current, size_t total, QString message);
  void onTriggerComplete(uint64_t request_id, bool success, QString status);
  void onCoordinatorError(uint64_t request_id, QString message);
  void onAbout();

  // Tools > Logging...: edits the diagnostic logging configuration and applies
  // it to the running application through LoggingController.
  void onConfigureLogging();

 signals:

 private:
  bool
  checkUnsavedChanges();  // Returns true if safe to proceed, false if cancelled
  void setupUI();
  void setupMenus();
  void setupToolbar();
  void syncThemeUi();  ///< Refresh theme-dependent toolbar icons and the
                       ///< Tools > Themes checkmarks for the active mode
  void reportPluginRuntimeDiagnostics(bool show_error_dialog);
  void
  connectDAGSignals();  ///< Connect DAG model/scene signals to their handlers
  void recreateDAGModelScene();  ///< Delete and recreate DAG model/scene with
                                 ///< signals reconnected
  void updateWindowTitle();
  void updatePreviewInfo();
  void updateUIState();
  void updatePreview();
  void updatePreviewRenderer();
  void updatePreviewModeCombo();
  void updateAspectRatioCombo();  // Populate aspect ratio combo from core
  void refreshViewerControls(
      bool skip_preview =
          false);  // Update slider, combo, preview, and info for current node
  void updateAllPreviewComponents();  // Update preview image, info label, VBI
                                      // dialog, and vectorscope(s)
  /// Frame the observer dialogs should follow, from the previewer's position
  /// and output type. Nullopt when the position does not resolve to a frame
  /// of the current view node.
  std::optional<uint64_t> previewFrameForObservers() const;
  void loadProjectDAG();         // Load DAG into embedded viewer
  void positionViewToTopLeft();  // Position view to show top-left node
  void
  selectLowestSourceStage();  // Auto-select source stage with lowest node ID
  void applyStageSelection(
      const orc::NodeID& node_id);  // Centralized stage selection handling
  void selectStageInDAG(
      const orc::NodeID&
          node_id);  // Select stage in DAG view (same as user click)
  void onEditParameters(const orc::NodeID& node_id);
  void onTriggerStage(const orc::NodeID& node_id);
  void runAnalysisForNode(const orc::AnalysisToolInfo& tool_info,
                          const orc::NodeID& node_id,
                          const std::string& stage_name);
  QProgressDialog* createAnalysisProgressDialog(
      const QString& title, const QString& message,
      QPointer<QProgressDialog>& existingDialog);

  // Project-load feedback. Opening a project hands the real work (rebuilding
  // renderers, then executing the DAG far enough to know what can be previewed)
  // to the coordinator's worker thread, which for a large source takes long
  // enough that the window otherwise looks hung — the preview only appears when
  // the available-outputs response lands. begin/update/end drive a modal
  // progress dialog across exactly that window.
  //
  // Editing the DAG pays the same cost for the same reason (the rebuild
  // replaces every stage object, so the source is opened again), so
  // updatePreviewRenderer() arms this too; |title| names which of the two the
  // user is waiting on.
  void beginProjectLoadProgress(const QString& title = "Opening Project");
  void updateProjectLoadProgressLabel();
  void endProjectLoadProgress();
  void closeAllDialogs();  ///< Close all open dialogs when switching projects

  /// Close every analysis and catalogue viewer, and drop the reads in flight
  ///
  /// Called whenever the DAG is rebuilt: the results these windows display
  /// belong to stage objects the rebuild has replaced.
  void closeResultViewers();
  void createAndShowAnalysisDialog(const orc::NodeID& node_id,
                                   const std::string& stage_name);

  /// Tell a failed results read's viewer why nothing arrived
  ///
  /// Returns true when |request_id| was one of the four results reads, so the
  /// caller knows the failure has been reported.
  bool reportFailedResultsRead(uint64_t request_id, const QString& message);

  /// Drop |request_id| from whichever results-read map holds it
  ///
  /// Returns the node it was issued for, or nullopt when the id is not a
  /// results read (a stale response, or some other request's).
  std::optional<orc::NodeID> takePendingResultsRead(uint64_t request_id);

  // Line scope helpers
  void requestLineSamplesForNavigation(uint64_t field_index, int line_number,
                                       int sample_x, int preview_image_width);
  orc::VideoDataType inferCurrentVideoDataType() const;
  void refreshPreviewViewAvailability();
  orc::PreviewCoordinate buildCurrentPreviewCoordinate() const;
  void refreshVectorscopeForCurrentCoordinate();
  void refreshHistogramForCurrentCoordinate();

  // In-flight render state helpers — all "rendering" UX lives here
  void beginPreviewRenderInFlight();  // Set flag + start slow-title timer
  void endPreviewRenderInFlight();    // Clear flag + stop timer + restore title

  // Settings helpers
  QString getLastProjectDirectory() const;
  void setLastProjectDirectory(const QString& path);
  QString getLastSourceDirectory() const;
  void setLastSourceDirectory(const QString& path);
  QString getLastExportDirectory() const;
  void setLastExportDirectory(const QString& path);
  void saveSettings();
  void restoreSettings();

  void propagateAmplitudeUnit();  ///< Push current project amplitude unit to
                                  ///< all open dialogs

  // Project management
  GUIProject project_;
  std::unique_ptr<RenderCoordinator>
      render_coordinator_;            // Owns all core rendering state
  orc::NodeID current_view_node_id_;  // Which node is being viewed
  QtNodes::NodeId
      last_selected_qt_node_id_;  // Last selected node in DAG for DEL key

  // Pending request tracking
  uint64_t pending_preview_request_id_{0};
  // VBI requests go through the same async observation path as the observer
  // dialogs, so the two frame-mode responses may arrive in either order; each
  // field is cached until both are ready.
  uint64_t pending_vbi_request_id_field1_{0};
  uint64_t pending_vbi_request_id_field2_{0};
  bool pending_vbi_is_frame_mode_{false};
  bool pending_vbi_field1_ready_{false};
  bool pending_vbi_field2_ready_{false};
  orc::presenters::VBIFieldInfoView pending_vbi_field1_info_;
  orc::presenters::VBIFieldInfoView pending_vbi_field2_info_;
  // Phase 5: async observer-dialog requests. One frame yields one (field mode)
  // or two (frame mode) requests whose responses carry both observer view
  // models; field1 is cached until field2 arrives, matching the VBI flow.
  uint64_t pending_obs_request_id_field1_{0};
  uint64_t pending_obs_request_id_field2_{0};
  bool pending_obs_frame_mode_{false};
  bool pending_obs_field1_ready_{false};
  bool pending_obs_field2_ready_{false};
  bool pending_obs_field1_available_{false};
  bool pending_obs_field2_available_{false};
  orc::FieldID pending_obs_field1_id_{0};
  orc::FieldID pending_obs_field2_id_{0};
  orc::presenters::VideoParameterObservationView pending_obs_video_field1_;
  orc::presenters::VideoParameterObservationView pending_obs_video_field2_;
  orc::presenters::NtscFieldObservationsView pending_obs_ntsc_field1_;
  orc::presenters::NtscFieldObservationsView pending_obs_ntsc_field2_;

  // Closed caption dialog: one request per window frame still lacking caption
  // data (request_id -> frame index); unknown ids in responses are stale.
  std::unordered_map<uint64_t, uint64_t> pending_closed_caption_requests_;
  // View node the closed caption cache was filled from; a change clears it.
  orc::NodeID closed_caption_cache_node_id_;

  uint64_t pending_outputs_request_id_{0};
  // Preview audio queries. The coordinator already answers only the newest of
  // each, so these exist to drop a response that the view has moved past
  // before the worker got to it.
  uint64_t pending_audio_pairs_request_id_{0};
  uint64_t pending_audio_reader_request_id_{0};
  uint64_t pending_trigger_request_id_{0};
  orc::NodeID pending_trigger_node_id_;  // Track which node is being triggered
  uint64_t pending_line_sample_request_id_{0};
  uint64_t pending_frame_timing_request_id_{0};
  uint64_t pending_waveform_monitor_request_id_{0};
  std::unordered_map<uint64_t, orc::NodeID>
      pending_dropout_requests_;  // request_id -> node_id
  std::unordered_map<uint64_t, orc::NodeID>
      pending_snr_requests_;  // request_id -> node_id
  std::unordered_map<uint64_t, orc::NodeID>
      pending_burst_level_requests_;  // request_id -> node_id
  std::unordered_map<uint64_t, orc::NodeID>
      pending_catalogue_requests_;  // request_id -> node_id

  // Dropout analysis state tracking
  orc::NodeID last_dropout_node_id_;
  orc::DropoutAnalysisMode last_dropout_mode_;
  orc::PreviewOutputType last_dropout_output_type_;

  // SNR analysis state tracking
  orc::NodeID last_snr_node_id_;
  orc::SNRAnalysisMode last_snr_mode_;
  orc::PreviewOutputType last_snr_output_type_;

  // UI components
  PreviewDialog* preview_dialog_;
  VBIDialog* vbi_dialog_;
  VideoParameterObserverDialog* video_parameter_observer_dialog_;
  std::unique_ptr<orc::presenters::DropoutPresenter> dropout_presenter_;
  // Note: project_presenter_ removed - use project_.presenter() instead
  NtscObserverDialog* ntsc_observer_dialog_;
  ClosedCaptionDialog* closed_caption_dialog_;
  std::unordered_map<orc::NodeID, DropoutAnalysisDialog*>
      dropout_analysis_dialogs_;
  std::unordered_map<orc::NodeID, SNRAnalysisDialog*> snr_analysis_dialogs_;
  std::unordered_map<orc::NodeID, BurstLevelAnalysisDialog*>
      burst_level_analysis_dialogs_;
  // Catalogue browsers, one per node whose stage offers one (stage tool)
  std::unordered_map<orc::NodeID, CatalogueDialog*> catalogue_dialogs_;
  OrcGraphModel* dag_model_;
  OrcGraphicsView* dag_view_;
  OrcGraphicsScene* dag_scene_;
  QAction* save_project_action_;
  QAction* save_project_as_action_;
  QAction* edit_project_action_;
  QAction* reload_sources_action_ = nullptr;  // shared by File menu + toolbar
  QAction* plugin_manager_action_ = nullptr;
  QAction* logging_action_ = nullptr;
  QAction* show_preview_action_;
  QAction* auto_show_preview_action_;

  // Toolbar (mirrors a few menu actions with simple icons; the menu items are
  // retained). See setupToolbar()/syncThemeUi().
  QToolBar* main_toolbar_ = nullptr;
  QMenu* view_menu_ = nullptr;
  QAction* arrange_dag_action_ = nullptr;  // shared by View menu + toolbar
  QAction* theme_cycle_action_ = nullptr;  // toolbar: cycles Auto->Light->Dark
  QAction* theme_auto_action_ = nullptr;   // Tools > Themes > Auto
  QAction* theme_dark_action_ = nullptr;   // Tools > Themes > Dark
  QAction* theme_light_action_ = nullptr;  // Tools > Themes > Light

  // Preview state (UI only - all data comes from core)
  orc::PreviewOutputType current_output_type_;
  std::string
      current_option_id_;  ///< Current option ID for custom preview rendering
  orc::AspectRatioMode
      current_aspect_ratio_mode_;  ///< Current aspect ratio mode
  std::vector<orc::PreviewOutputInfo>
      available_outputs_;  ///< Cached outputs for current node
  // Line scope tracking - store the actual field/line being displayed
  // All visual positions are derived from these via orc-core mapping functions
  // Note: line numbers are stored as 0-based (matching core API), converted to
  // 1-based for display
  uint64_t last_line_scope_field_index_;  ///< Current field being displayed in
                                          ///< line scope
  int last_line_scope_line_number_;  ///< Current line in line scope (0-based: 0
                                     ///< to field_height-1)
  int last_line_scope_image_x_;  ///< Store original preview-space X coordinate
                                 ///< for line scope navigation
  int last_line_scope_image_y_;  ///< Store original preview-space Y coordinate
                                 ///< for line scope navigation
  int last_line_scope_preview_width_;  ///< Store preview width for coordinate
                                       ///< mapping
  int last_line_scope_samples_count_;  ///< Store samples count for coordinate
                                       ///< mapping

  bool preview_render_in_flight_{
      false};                     // True while a render request is in-flight
  int pending_render_index_{-1};  // Index passed to the most recent render call
  QTimer* render_slow_timer_{
      nullptr};  // Fires after 2 s to update preview title during long renders

  // Trigger progress tracking (now via coordinator signals)
  // Use QPointer to auto-null when dialog is deleted
  QPointer<QProgressDialog> trigger_progress_dialog_;

  // Project-load progress state (see beginProjectLoadProgress()). The dialog is
  // created up front but only shown once the load has outlived
  // project_load_show_timer_, so projects that open quickly never flash it.
  bool project_load_in_progress_{false};
  QPointer<QProgressDialog> project_load_progress_dialog_;
  QTimer* project_load_show_timer_{nullptr};  ///< Delays first appearance
  QTimer* project_load_tick_timer_{nullptr};  ///< Refreshes the elapsed counter
  QElapsedTimer project_load_elapsed_;
  QString project_load_stage_label_;  ///< Stage the worker last reported
  qulonglong project_load_current_{0};
  qulonglong project_load_total_{0};
};

#endif  // MAINWINDOW_H
