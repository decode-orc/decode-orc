/*
 * File:        previewdialog.h
 * Module:      orc-gui
 * Purpose:     Separate preview window for field/frame viewing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef PREVIEWDIALOG_H
#define PREVIEWDIALOG_H

#include <orc/stage/common_types.h>
#include <orc/stage/node_id.h>
#include <orc/stage/preview/orc_preview_types.h>
#include <orc/stage/preview/orc_vectorscope.h>
#include <orc_audio_views.h>
#include <orc_preview_views.h>

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QMenuBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "presenters/include/hints_view_models.h"  // For VideoParametersView
#include "presenters/include/project_presenter_types.h"  // For VideoFormat
#include "preview_audio_controller.h"  // PreviewAudioController, IAudioOutput

class FieldPreviewWidget;
class FrameScopeDialog;
class FrameTimingDialog;
class HistogramDialog;
class QHBoxLayout;
class QProgressDialog;
class VectorscopeDialog;
class WaveformMonitorDialog;

/**
 * @brief Separate dialog window for previewing field/frame outputs from DAG
 * nodes
 *
 * Provides a dedicated window for viewing video field/frame previews with
 * controls for:
 * - Field/frame navigation via slider
 * - Preview mode selection (field, frame, split, etc.)
 * - Aspect ratio control
 * - Export to PNG
 * - VBI and other metadata dialogs
 *
 * This is a thin GUI layer - all rendering logic is handled by
 * orc::PreviewRenderer.
 */
class PreviewDialog : public QDialog {
  Q_OBJECT

 public:
  explicit PreviewDialog(QWidget* parent = nullptr);
  ~PreviewDialog();

  /// @name Widget Accessors
  /// @{
  FieldPreviewWidget* previewWidget() {
    return preview_widget_;
  }  ///< Get preview widget
  QSlider* previewSlider() {
    return preview_slider_;
  }  ///< Get field/frame slider
  QLabel* previewInfoLabel() {
    return preview_info_label_;
  }  ///< Get info label
  QLabel* sliderMinLabel() {
    return slider_min_label_;
  }  ///< Get slider min label
  QLabel* sliderMaxLabel() {
    return slider_max_label_;
  }  ///< Get slider max label
  QComboBox* previewModeCombo() {
    return preview_mode_combo_;
  }  ///< Get preview mode selector
  QComboBox* signalCombo() {
    return signal_combo_;
  }  ///< Get channel selector (Y+C/Luma/Chroma for YC sources)
  QLabel* signalLabel() { return signal_label_; }  ///< Get signal label
  QComboBox* aspectRatioCombo() {
    return aspect_ratio_combo_;
  }  ///< Get aspect ratio selector
  QAction* ntscObserverAction() {
    return show_ntsc_observer_action_;
  }  ///< Get NTSC observer menu action
  QAction* closedCaptionAction() {
    return show_closed_caption_action_;
  }  ///< Get closed captions menu action
  QAction* frameTimingAction() {
    return show_frame_timing_action_;
  }  ///< Get frame timing menu action
  QAction* waveformMonitorAction() {
    return show_waveform_monitor_action_;
  }  ///< Get waveform monitor menu action
  QPushButton* dropoutsButton() {
    return dropouts_button_;
  }  ///< Get dropouts button for state control
  QSpinBox* frameJumpSpinBox() {
    return frame_jump_spinbox_;
  }  ///< Get frame/field jump spin box
  QPushButton* playPauseButton() {
    return play_pause_button_;
  }  ///< Get play/pause button
  QComboBox* audioPairCombo() {
    return audio_pair_combo_;
  }  ///< Get audio channel-pair selector
  QSlider* audioVolumeSlider() {
    return audio_volume_slider_;
  }  ///< Get audio volume slider
  /// @}

  /// @name Audio playback
  /// @{

  /**
   * @brief Playback session that makes audio the clock and video chase it.
   *
   * Owned by the dialogue and shared with tests, which drive it through a
   * mocked IAudioOutput rather than a real device.
   */
  orc::gui::PreviewAudioController* audioController() {
    return audio_controller_;
  }

  /**
   * @brief Populate the channel-pair selector for the viewed node.
   *
   * An empty list is the normal "this node has no audio" answer: the selector
   * shows a disabled "Mute/None" entry and the volume control is disabled with
   * it.
   *
   * The pair the user last picked is re-selected if this node carries it, so
   * switching stages keeps the audio playing rather than silently reverting to
   * "Mute/None"; a node without that pair falls back to "Mute/None" but does
   * not forget the choice.
   */
  void setAudioChannelPairs(const std::vector<orc::AudioPairView>& pairs);

  /**
   * @brief Install the audio device instead of the platform one.
   *
   * Production code never calls this: the dialogue creates the platform output
   * lazily when playback with a pair selected first starts, so a project with
   * no audio never touches the audio subsystem. Tests install a device-free
   * stand-in up front.
   */
  void setAudioOutput(std::unique_ptr<orc::gui::IAudioOutput> output);

  /**
   * @brief Deliver a reader requested via audioStreamReaderRequested().
   *
   * A null reader means the pair turned out to be unplayable; playback then
   * falls back to the timer-paced video-only path. Ignored unless the dialogue
   * is still waiting for this reader.
   */
  void setAudioStreamReader(
      std::shared_ptr<orc::presenters::IAudioStreamReader> reader);

  /**
   * @brief Preview items per video frame at the viewed output (1 or 2).
   *
   * Audio maps to whole frames, so a field-indexed output advances two preview
   * indices per frame period. Set whenever the output type changes.
   */
  void setAudioItemsPerFrame(uint32_t items_per_frame);

  /// Channel pair the user has selected, or empty for "Mute/None".
  std::optional<size_t> selectedAudioPair() const;

  /// True while the audio clock is driving the preview position.
  bool isAudioPlaybackActive() const;

  /**
   * @brief Drop the cached reader and stop any playback using it.
   *
   * The reader holds a resolved representation alive, so it is only valid for
   * the DAG version it was created from. Call whenever the DAG, its parameters
   * or the project change.
   */
  void invalidateAudioSource();
  /// @}

  /**
   * @brief Set visibility of signal controls (label and combo box)
   * @param visible True to show signal controls, false to hide them
   */
  void setSignalControlsVisible(bool visible);

  /**
   * @brief Enable the observer menu entries that only apply to one standard
   * @param format Video format of the project being previewed
   *
   * The NTSC observers (FM code, white flag) and EIA-608 closed captions are
   * 525-line NTSC features. Anything else (including PAL, PAL-M and an unknown
   * format) leaves those entries disabled.
   */
  void setObserverAvailabilityForFormat(orc::presenters::VideoFormat format);

  /**
   * @brief Set the currently previewed node
   * @param node_label Human-readable node label
   * @param node_id Node identifier string
   */
  void setCurrentNode(const QString& node_label, const QString& node_id);

  /**
   * @brief Set the current node used by preview-owned supplementary views.
   */
  void setCurrentNodeId(orc::NodeID node_id);

  /**
   * @brief Update view launcher availability using presenter/registry
   * descriptors.
   */
  void setAvailablePreviewViews(
      const std::vector<orc::PreviewViewDescriptor>& views);

  /**
   * @brief Check whether a registry view is currently available for this node.
   */
  bool hasAvailablePreviewView(const std::string& view_id) const;

  /**
   * @brief Return the currently selected vectorscope preview view id.
   */
  const std::string& activeVectorscopeViewId() const {
    return kComponentVectorscopeViewIdRef();
  }

  static const std::string& kComponentVectorscopeViewIdRef();

  /**
   * @brief Set and broadcast the shared preview coordinate for supplementary
   * tools.
   */
  void setSharedPreviewCoordinate(const orc::PreviewCoordinate& coordinate);

  /**
   * @brief Read the currently shared coordinate used by supplementary tools.
   */
  const std::optional<orc::PreviewCoordinate>& sharedPreviewCoordinate() const {
    return shared_preview_coordinate_;
  }

  /**
   * @brief Show vectorscope dialog owned by the preview subsystem.
   */
  void showVectorscopeForNode(orc::NodeID node_id);

  /**
   * @brief Show video histogram dialog owned by the preview subsystem.
   */
  void showHistogramForNode(orc::NodeID node_id);

  /**
   * @brief Update histogram dialog content.
   */
  void updateHistogram(orc::NodeID node_id,
                       const std::optional<orc::VideoHistogramData>& data);

  /**
   * @brief Check if histogram is visible for node.
   */
  bool isHistogramVisibleForNode(orc::NodeID node_id) const;

  /**
   * @brief Return the histogram preview view id.
   */
  static const std::string& kHistogramViewIdRef();

  /**
   * @brief Update vectorscope dialog content for the given node.
   */
  void updateVectorscope(orc::NodeID node_id,
                         const std::optional<orc::VectorscopeData>& data);

  /**
   * @brief Check if vectorscope is visible for node.
   */
  bool isVectorscopeVisibleForNode(orc::NodeID node_id) const;

  /**
   * @brief Copy the vectorscope dialog's acquisition controls into a
   *        preview coordinate.
   *
   * No-op when the vectorscope dialog has not been created, leaving the
   * coordinate's defaults (decoded acquisition) in place.
   */
  void applyVectorscopeAcquisition(orc::PreviewCoordinate& coordinate) const;

  /**
   * @brief Acquisition the vectorscope dialog is currently set to.
   *
   * Returns DecodedComponent when the dialog has not been created.
   */
  orc::VectorscopeAcquisitionMode vectorscopeAcquisitionMode() const;

  /**
   * @brief State which vectorscope acquisition the current stage's output
   *        calls for.
   *
   * Remembered so it also applies to a vectorscope dialog opened later.
   */
  void setVectorscopeAcquisitionMode(orc::VectorscopeAcquisitionMode mode);

  /**
   * @brief Show frame scope dialog with sample data
   *
   * field_index is used as frame_id and line_number (1-based) is converted
   * to a 0-based frame-flat line index.
   *
   * @param node_id          Stage node identifier
   * @param stage_index      1-based pipeline stage number
   * @param field_index      0-based field index (used as frame_id)
   * @param line_number      1-based field line number
   * @param sample_x         Sample X position that was clicked
   * @param samples          CVBS_U10_4FSC composite samples
   * @param video_params     Optional video parameters for level markers
   * @param preview_image_width  Pixel width of preview image
   * @param original_sample_x    Preview-space X (for cross-hair sync)
   * @param original_image_y     Preview-space Y (for refresh)
   * @param preview_mode     Current preview mode (kept for callers; ignored
   * internally)
   * @param y_samples        Optional luma samples (YC sources)
   * @param c_samples        Optional chroma samples (YC sources)
   */
  void showLineScope(
      const QString& node_id, int stage_index, uint64_t field_index,
      int line_number, int sample_x, const std::vector<int16_t>& samples,
      const std::optional<orc::presenters::VideoParametersView>& video_params,
      int preview_image_width, int original_sample_x, int original_image_y,
      orc::PreviewOutputType preview_mode,
      const std::vector<int16_t>& y_samples = {},
      const std::vector<int16_t>& c_samples = {});

  /**
   * @brief Close all child dialogs (e.g., line scope)
   */
  void closeChildDialogs();

  /**
   * @brief Forward amplitude display unit to all owned supplementary dialogs.
   * Called by MainWindow::propagateAmplitudeUnit() whenever the unit changes.
   */
  void forwardAmplitudeUnit(orc::AmplitudeDisplayUnit unit);

  /**
   * @brief Check if line scope dialog is currently visible
   */
  bool isLineScopeVisible() const;

  /**
   * @brief True while the user wants the frame timing dialog open.
   *
   * Set when the user triggers the View menu action, cleared when the dialog
   * closes. Async data callbacks must consult this before auto-showing the
   * dialog: a request issued before the user closed it would otherwise
   * re-open the window when its response arrives.
   */
  bool isFrameTimingOpenRequested() const {
    return frame_timing_open_requested_;
  }

  /**
   * @brief True while the user wants the waveform monitor dialog open.
   * @see isFrameTimingOpenRequested()
   */
  bool isWaveformMonitorOpenRequested() const {
    return waveform_monitor_open_requested_;
  }

  /**
   * @brief Notify that the preview frame/mode has changed
   *
   * Emits previewFrameChanged signal to notify line scope and other listeners
   * that they should refresh their data for the new frame context.
   */
  void notifyFrameChanged();

  /**
   * @brief Get frame scope dialog (for updating when stage changes)
   */
  FrameScopeDialog* frameScopeDialog() { return frame_scope_dialog_; }

  /**
   * @brief Get frame timing dialog (for updating when frame changes)
   */
  FrameTimingDialog* frameTimingDialog() { return frame_timing_dialog_; }

  /**
   * @brief Get waveform monitor dialog (for updating when frame changes)
   */
  WaveformMonitorDialog* waveformMonitorDialog() {
    return waveform_monitor_dialog_;
  }

  /**
   * @brief Returns the current 0-based navigation index (always matches
   * slider/spinbox display).
   */
  int currentIndex() const { return preview_slider_->value(); }

  /**
   * @brief Navigate immediately to \p zero_based.
   * Clamps to range, updates slider + spinbox, emits positionChanged and
   * renderRequested. Call this from outside the dialog (keyboard,
   * MainWindow::onNavigatePreview).
   */
  void navigateToIndex(int zero_based);

  /**
   * @brief Navigate with debounce (slider scrub, spinbox typing).
   * Updates UI immediately for visual feedback; emits renderRequested only
   * after the debounce timer fires.
   */
  void navigateToIndexDebounced(int zero_based);

  /**
   * @brief Silently set the displayed index — updates slider and spinbox
   * without emitting any navigation signals. Used by MainWindow when restoring
   * position after a range change (refreshViewerControls /
   * onPreviewModeChanged).
   */
  void setIndex(int zero_based);

  /**
   * @brief Set the playback timer interval in milliseconds.
   * Must be called by MainWindow whenever the video standard is known.
   * PAL: 40 ms (25 fps). NTSC: 33 ms (~30 fps).
   */
  void setPlaybackFrameRateMs(int ms);

  /**
   * @brief Stop playback and reset the play/pause button to the play state.
   * Call this whenever the source or project changes.
   */
  void stopPlayback();

  /**
   * @brief True while playback is running (or being prepared).
   *
   * The owner uses this to render with PreviewNavigationHint::Sequential, so
   * stages know a run of adjacent frames is coming and may pre-fetch.
   */
  bool isPlaying() const { return is_playing_; }

 Q_SIGNALS:
  /**
   * Emitted whenever the current index changes (every navigate/scrub).
   * MainWindow connects this to its updatePreviewInfo() to keep the info
   * label in sync even during a scrub before the render fires.
   */
  void positionChanged(int index);
  /**
   * Emitted when a render should happen — either immediately (buttons,
   * slider release) or after the debounce timer settles (slider drag,
   * spinbox typing).  MainWindow renders now if not in-flight, otherwise
   * the completion callback will re-render for currentIndex().
   */
  void renderRequested(int index);
  void previewModeChanged(int index);
  void signalChanged(
      int index);  // Emitted when signal selection changes (Y/C/Y+C)
  void aspectRatioModeChanged(int index);
  void exportPNGRequested();
  void showVBIDialogRequested();  // Emitted when VBI Decoder menu item selected
  void
  showVideoParameterObserverDialogRequested();  // Emitted when Video Parameter
                                                // Observer menu item selected
  void showNtscObserverDialogRequested();   // Emitted when NTSC Observer menu
                                            // item selected
  void showClosedCaptionDialogRequested();  // Emitted when Closed Captions menu
                                            // item selected
  void showDropoutsChanged(
      bool show);  // Emitted when dropout visibility changes
  void lineScopeRequested(int image_x,
                          int image_y);  // Emitted when user clicks a line
  void lineNavigationRequested(
      int direction, uint64_t current_field, int current_line, int sample_x,
      int preview_image_width);  // Emitted when navigating lines
  void sampleMarkerMovedInLineScope(
      int sample_x);  // Emitted when sample marker moves in line scope
  void
  previewFrameChanged();  // Emitted when preview frame/output type changes -
                          // tells line scope to refresh at current position
  void frameTimingRequested();  // Emitted when user requests frame timing view
  void
  waveformMonitorRequested();  // Emitted when user requests waveform monitor
  void vectorscopeRequested(const orc::PreviewCoordinate&
                                coordinate);  // Emitted when vectorscope should
                                              // refresh via presenter contract
  void histogramRequested(const orc::PreviewCoordinate&
                              coordinate);  // Emitted when histogram should
                                            // refresh via presenter contract
  void previewCoordinateChanged(
      const orc::PreviewCoordinate&
          coordinate);  // Emitted whenever shared coordinate changes
  /**
   * Emitted when playback needs a reader for the selected channel pair.
   * Creating one executes the DAG, so the owner routes it through the render
   * coordinator and answers with setAudioStreamReader().
   */
  void audioStreamReaderRequested(size_t pair);

  /**
   * Emitted when playback starts or stops (never repeated for the same
   * state). The owner tells the render coordinator, which holds back the
   * background observation sweep for the duration: it would otherwise run on
   * half the machine's cores in competition with the worker that has to
   * deliver the next frame.
   */
  void playbackActiveChanged(bool active);

 private slots:
  void onSampleMarkerMoved(int sample_x);
  void onComponentVectorscopeActionTriggered();
  void onHistogramActionTriggered();

 private:
  void setupUI();

  // The one place is_playing_ changes: keeps the play/pause glyph and the
  // playbackActiveChanged() listeners in step with it. A no-op when the state
  // is already what is asked for.
  void setPlaying(bool playing);

  // Audio playback session state. Reader creation and the deferred whole-
  // stream decode behind it both take an unbounded amount of time, so pressing
  // Play with a pair selected walks through them before any audio starts.
  enum class AudioState {
    kIdle,            ///< No audio session (video-only playback or stopped)
    kAwaitingReader,  ///< Reader requested from the owner, not yet delivered
    kPriming,         ///< Deferred decode running on the prime thread
    kPlaying          ///< Audio is the clock and the video is chasing it
  };

  // Progress published by the prime thread. Polled from the GUI thread rather
  // than signalled, so the worker never touches a Qt object it does not own.
  struct AudioPrimeProgressState {
    std::mutex mutex;
    uint64_t done = 0;
    uint64_t total = 0;
    std::string message;
  };

  void setupAudioControls(QHBoxLayout* layout);
  void updateAudioControlStates();

  // Selection carried across the repopulate that follows a stage change.
  void rememberSelectedAudioPair();
  int comboIndexForRememberedPair() const;

  // Playback entry points. startPlayback() picks the audio or the legacy
  // video-only path from the current selection; the others are its steps.
  void startPlayback();
  void beginVideoOnlyPlayback();
  void beginAudioPlayback();
  bool ensureAudioOutput();
  void beginAudioPrime();
  void finishAudioPrime(uint64_t generation);
  void endAudioPreparation();
  void showAudioPrimeDialog();
  void updateAudioPrimeDialog();
  void onPlaybackTick();
  void suspendAudioForScrub();
  void resumeAudioAtCurrentIndex();

  // UI components
  FieldPreviewWidget* preview_widget_;
  QSlider* preview_slider_;
  QLabel* preview_info_label_;
  QLabel* slider_min_label_;
  QLabel* slider_max_label_;
  QComboBox* preview_mode_combo_;
  QComboBox* signal_combo_;  // Signal selection for YC sources (Y/C/Y+C)
  QLabel* signal_label_;     // Label for signal combo box
  QComboBox* aspect_ratio_combo_;
  QMenuBar* menu_bar_;
  QStatusBar* status_bar_;
  QAction* export_png_action_;
  QAction* show_vbi_action_;
  QAction* show_video_parameter_observer_action_;
  QAction* show_ntsc_observer_action_;
  QAction* show_closed_caption_action_;
  QAction* show_frame_timing_action_;
  QAction* show_waveform_monitor_action_;
  QAction* show_component_vectorscope_action_;
  QAction* show_histogram_action_;
  FrameScopeDialog* frame_scope_dialog_;
  FrameTimingDialog* frame_timing_dialog_;
  WaveformMonitorDialog* waveform_monitor_dialog_;
  VectorscopeDialog* vectorscope_dialog_{nullptr};
  orc::NodeID vectorscope_node_id_;
  // Acquisition the current stage's output calls for.  Defaults to the
  // decoded plot, which is what a colour stage gets.
  orc::VectorscopeAcquisitionMode vectorscope_acquisition_mode_{
      orc::VectorscopeAcquisitionMode::DecodedComponent};
  HistogramDialog* histogram_dialog_{nullptr};
  orc::NodeID histogram_node_id_;
  orc::NodeID current_node_id_;
  std::optional<orc::PreviewCoordinate> shared_preview_coordinate_;
  std::unordered_set<std::string> available_preview_view_ids_;

  // User intent to have each supplementary dialog open. Data for these
  // dialogs is fetched asynchronously, so a response can arrive after the
  // user has closed the window; without these flags the response re-opens it.
  bool line_scope_open_requested_{false};
  bool frame_timing_open_requested_{false};
  bool waveform_monitor_open_requested_{false};

  // Current line scope context for cross-hair updates
  int current_line_scope_line_ =
      -1;  // Image Y coordinate of current line being scoped
  int current_line_scope_preview_width_ = 0;
  int current_line_scope_samples_count_ = 0;

  // Navigation
  QTimer* nav_debounce_timer_;
  QPushButton* first_button_;
  QPushButton* prev_button_;
  QPushButton* play_pause_button_;
  QPushButton* next_button_;
  QPushButton* last_button_;
  QPushButton* zoom1to1_button_;
  QPushButton* dropouts_button_;
  QSpinBox* frame_jump_spinbox_;

  // Playback
  QTimer* playback_timer_;
  bool is_playing_{false};

  // Audio playback
  QLabel* audio_label_;
  QComboBox* audio_pair_combo_;
  QSlider* audio_volume_slider_;
  orc::gui::PreviewAudioController* audio_controller_;
  std::vector<orc::AudioPairView> audio_pairs_;

  // Last pair the user picked, held as a descriptor rather than an index so it
  // can be found again in another node's list. Survives nodes that offer no
  // audio at all, so stepping past one does not lose the choice.
  std::optional<orc::AudioPairView> remembered_audio_pair_;

  // Reader for audio_reader_pair_, kept across pause/resume so restarting
  // costs nothing; dropped whenever the representation behind it goes stale.
  std::shared_ptr<orc::presenters::IAudioStreamReader> audio_reader_;
  std::optional<size_t> audio_reader_pair_;

  AudioState audio_state_{AudioState::kIdle};

  // Set while the playback tick is driving navigation, so the chase target is
  // not mistaken for a user seek and fed straight back into the audio clock.
  bool chase_navigation_{false};

  // Remembers that this machine has no audio backend, so playback does not
  // retry the (failed) device creation on every press of Play.
  bool audio_output_unavailable_{false};

  // Bumped whenever a preparation step is abandoned. A prime thread cannot be
  // interrupted, so its completion is matched against this instead.
  uint64_t audio_prime_generation_{0};
  std::shared_ptr<AudioPrimeProgressState> audio_prime_state_;
  QProgressDialog* audio_prime_dialog_{nullptr};
  QTimer* audio_prime_show_timer_;
  QTimer* audio_prime_tick_timer_;

  void closeVectorscopeDialogs();
  void closeHistogramDialog();

 protected:
  void closeEvent(QCloseEvent* event) override;
};

#endif  // PREVIEWDIALOG_H
