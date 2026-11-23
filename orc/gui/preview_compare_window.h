/*
 * File:        preview_compare_window.h
 * Module:      orc-gui
 * Purpose:     A/B Compare window: flicker two nodes' previews in place so
 *              small differences show, and export the pair as a WebP
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef PREVIEW_COMPARE_WINDOW_H
#define PREVIEW_COMPARE_WINDOW_H

#include <orc/stage/node_id.h>
#include <orc/stage/preview/orc_rendering.h>
#include <orc/view-support/frame_viewport_widget.h>
#include <orc_preview_comparison.h>

#include <QImage>
#include <QRect>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ab_flicker_state.h"
#include "render_coordinator.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollBar;
class QSpinBox;
class QTimer;

namespace orc::gui {

/// A node the window can compare, as the project names it.
struct CompareNodeChoice {
  orc::NodeID node_id;
  QString label;
};

/**
 * @brief The comparison's viewport: nearest-neighbour, power-of-two zoom.
 *
 * Ctrl+wheel steps through 1x, 2x, 4x and 8x at the cursor rather than the
 * base widget's continuous zoom, so every sample stays a whole block of
 * pixels and the zoom is always a scale the export can repeat.
 *
 * Thread safety: GUI thread only.
 */
class CompareViewport : public orc::view::FrameViewportWidget {
  Q_OBJECT

 public:
  static constexpr double kMinZoom = 1.0;
  static constexpr double kMaxZoom = 8.0;

  explicit CompareViewport(QWidget* parent = nullptr);

 protected:
  void wheelEvent(QWheelEvent* event) override;
};

/**
 * @brief The A/B Compare window, opened from the preview dialog.
 *
 * Shows the same output, option and frame of two nodes and alternates
 * between them, so a difference too small to see side by side shows as
 * movement. It follows the main preview's output and frame; the owner calls
 * setOutput() and setIndex() as those change, and refresh() when the project
 * changes.
 *
 * Nodes A and B are picked from the nodes that offer the current output and
 * option. Finding those means asking each node for its outputs, which runs
 * on the render worker; the window asks one node at a time, so the main
 * preview never waits behind more than one of these questions.
 *
 * Nothing is requested while the window is hidden.
 *
 * Problems (a frame beyond one node's end, images of different sizes, a
 * missing option, different output counts) are shown in the window, never
 * in a dialogue.
 *
 * Thread safety: GUI thread only. The coordinator must outlive the window.
 */
class PreviewCompareWindow : public QWidget {
  Q_OBJECT

 public:
  /// The heading of the window's section in the preview window's help.
  static const QString& helpHeading();

  explicit PreviewCompareWindow(RenderCoordinator* coordinator,
                                QWidget* parent = nullptr);
  ~PreviewCompareWindow() override;

  /// @name Context from the owner
  /// @{
  /// The project's nodes. Starts asking which offer the current output.
  void setNodes(std::vector<CompareNodeChoice> nodes);
  /// The output type and option to compare, and the name the preview shows
  /// for them.
  void setOutput(orc::PreviewOutputType type, const std::string& option_id,
                 const QString& output_name);
  /// The 0-based index of the output to compare (each node's own).
  void setIndex(uint64_t index);
  /// Make @p node A when it offers the output; the preview's node when the
  /// window opens.
  void setPreferredNodeA(orc::NodeID node);
  /// The project file, for the command line the window shows.
  void setProjectPath(const QString& path);
  /// The project changed: ask the nodes again and render again.
  void refresh();
  /// @}

  /// @name State
  /// @{
  /// The comparison the window shows, or nothing when A or B is not picked.
  std::optional<orc::PreviewCompareRequest> currentRequest() const;
  orc::PreviewCompareViewSettings viewSettings() const;
  /// What an export from the window now writes: the visible region as the
  /// crop, the zoom as the scale, the window's stretch and interval.
  orc::PreviewCompareExportSettings exportSettings() const;
  /// The region of each node's render the viewport shows, in native samples,
  /// or nothing when no comparison is shown.
  std::optional<QRect> visibleNativeRect() const;
  /// The `orc-cli preview compare` command that writes what an export to
  /// @p output_path would.
  QString commandLine(const QString& output_path) const;
  const ABFlickerState& flickerState() const { return flicker_; }
  /// The last comparison delivered, or null.
  const ComparisonDelivery* delivery() const { return delivery_.get(); }
  /// @}

  /// Export to @p path without asking for a file name.
  void exportTo(const QString& path);

  /// @name Widgets, for tests
  /// @{
  QComboBox* nodeACombo() const { return node_a_combo_; }
  QComboBox* nodeBCombo() const { return node_b_combo_; }
  QComboBox* intervalCombo() const { return interval_combo_; }
  QComboBox* zoomCombo() const { return zoom_combo_; }
  QComboBox* viewModeCombo() const { return view_mode_combo_; }
  QDoubleSpinBox* gainSpin() const { return gain_spin_; }
  QSpinBox* windowSpin() const { return window_spin_; }
  QPushButton* swapButton() const { return swap_button_; }
  QPushButton* showAButton() const { return show_a_button_; }
  QPushButton* showBButton() const { return show_b_button_; }
  QPushButton* holdButton() const { return hold_button_; }
  QPushButton* exportButton() const { return export_button_; }
  QPushButton* helpButton() const { return help_button_; }
  CompareViewport* viewport() const { return viewport_; }
  QLabel* sideLabel() const { return side_label_; }
  QLabel* messageLabel() const { return message_label_; }
  QLineEdit* commandLineEdit() const { return command_line_edit_; }
  /// @}

 public Q_SLOTS:
  /// Space: hold the flicker on the side shown, or resume it.
  void toggleHold();
  /// The A and B keys: show one side and hold there.
  void showSide(orc::gui::ABSide side);
  /// Swap nodes A and B; the picture stays, its label changes.
  void swapNodes();

 Q_SIGNALS:
  /// The window opened its help.
  void helpShown();

 protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  void setupUi();
  void onExportClicked();
  void onHelpClicked();

  // Asking the nodes which outputs they offer, one at a time.
  void startNodeQueries();
  void queryNextNode();
  void onAvailableOutputs(uint64_t request_id,
                          std::vector<orc::PreviewOutputInfo> outputs);
  bool nodeOffersOutput(orc::NodeID node) const;
  void rebuildNodeCombos();

  void requestRender();
  void onComparisonReady(uint64_t request_id, ComparisonDeliveryPtr delivery);
  void onComparisonExported(uint64_t request_id,
                            orc::PreviewCompareExportResult result);

  void showCurrentImage();
  void syncTimer();
  void onTimerTick();
  void updateSideLabel();
  /// A, B, Hold/Animate and the interval: what they say, and whether they
  /// apply (the Difference view is one picture, so nothing flickers).
  void updateFlickerControls();
  /// Whether the view shows A and B in turn, which the flicker controls act on.
  bool viewShowsSides() const;
  void updateCommandLine();
  void setMessage(const QString& text, bool is_error);

  QString labelFor(orc::NodeID node) const;
  orc::PreviewCompareViewMode viewMode() const;

  RenderCoordinator* coordinator_;

  // Context from the owner.
  std::vector<CompareNodeChoice> nodes_;
  orc::PreviewOutputType output_type_ =
      orc::PreviewOutputType::Frame_Field1_First;
  std::string option_id_;
  QString output_name_;
  uint64_t index_ = 0;
  std::optional<orc::NodeID> preferred_a_;
  QString project_path_;

  // Which outputs each node offers, keyed by NodeID::value().
  std::map<int32_t, std::vector<orc::PreviewOutputInfo>> node_outputs_;
  std::deque<orc::NodeID> nodes_to_query_;
  uint64_t pending_outputs_request_ = 0;
  orc::NodeID node_in_flight_;  ///< The node pending_outputs_request_ asks
  bool queries_stale_ = true;

  uint64_t pending_compare_request_ = 0;
  uint64_t pending_export_request_ = 0;
  QString pending_export_path_;
  ComparisonDeliveryPtr delivery_;

  ABFlickerState flicker_;
  QTimer* flicker_timer_ = nullptr;

  QComboBox* node_a_combo_ = nullptr;
  QComboBox* node_b_combo_ = nullptr;
  QPushButton* swap_button_ = nullptr;
  QPushButton* show_a_button_ = nullptr;
  QPushButton* show_b_button_ = nullptr;
  QPushButton* hold_button_ = nullptr;
  QComboBox* interval_combo_ = nullptr;
  QComboBox* zoom_combo_ = nullptr;
  QComboBox* view_mode_combo_ = nullptr;
  QDoubleSpinBox* gain_spin_ = nullptr;
  QSpinBox* window_spin_ = nullptr;
  QPushButton* export_button_ = nullptr;
  QPushButton* help_button_ = nullptr;
  CompareViewport* viewport_ = nullptr;
  QScrollBar* horizontal_pan_bar_ = nullptr;
  QScrollBar* vertical_pan_bar_ = nullptr;
  QLabel* side_label_ = nullptr;
  QLabel* status_label_ = nullptr;
  QLabel* message_label_ = nullptr;
  QLineEdit* command_line_edit_ = nullptr;
};

}  // namespace orc::gui

#endif  // PREVIEW_COMPARE_WINDOW_H
