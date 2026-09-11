/*
 * File:        vectorscope_dialog.h
 * Module:      orc-gui
 * Purpose:     Vectorscope visualization dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ORC_GUI_ANALYSIS_VECTORSCOPE_DIALOG_H
#define ORC_GUI_ANALYSIS_VECTORSCOPE_DIALOG_H

#include <orc/stage/node_id.h>
#include <orc/stage/preview/orc_preview_types.h>  // PreviewCoordinate
#include <orc/stage/preview/orc_vectorscope.h>    // Public API types

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <memory>
#include <optional>
#include <string>

// Forward declaration for pimpl
class VectorscopeDialogPrivate;

/**
 * @brief QLabel subclass that maintains aspect ratio of the displayed pixmap
 */
class AspectRatioLabel : public QLabel {
  Q_OBJECT

 public:
  explicit AspectRatioLabel(QWidget* parent = nullptr);

  void setPixmap(const QPixmap& pixmap);

 protected:
  void resizeEvent(QResizeEvent* event) override;

 private:
  void updateScaledPixmap();

  QPixmap original_pixmap_;
};

/**
 * @brief Live vectorscope visualization for chroma decoder output
 *
 * This dialog displays U/V color components on a vectorscope for decoded
 * chroma output from a VideoSinkStage. It's a live visualization tool that
 * updates in real-time as the user navigates through fields.
 */
class VectorscopeDialog : public QDialog {
  Q_OBJECT

 public:
  explicit VectorscopeDialog(QWidget* parent = nullptr);
  ~VectorscopeDialog() override;

  void setScopeLabel(const QString& scope_label);
  void setStage(orc::NodeID node_id);
  bool isActiveAreaOnly() const;

  /// Acquisition in force: the decoded U/V planes (grading), or the composite
  /// carrier demodulated directly (measurement).  Not a user choice — it
  /// follows the data type the selected stage produces.
  orc::VectorscopeAcquisitionMode acquisitionMode() const;

  /**
   * @brief State which acquisition the selected stage's output calls for.
   *
   * A colour-domain output has decoder planes to plot; a signal-domain one has
   * a carrier to demodulate.  Switching re-acquires and re-lays the controls,
   * since the sampling window and line select only exist for the composite
   * acquisition.
   */
  void setAcquisitionMode(orc::VectorscopeAcquisitionMode mode);

  /// Portion of each line a composite acquisition samples.
  orc::VectorscopeSampleWindow sampleWindow() const;

  /// Inclusive frame-flat line range for a composite acquisition, 0-based.
  /// Returns {0, 0} when the whole frame is selected — 0 as the last line
  /// means "to the last line of the frame" in PreviewCoordinate.
  uint32_t firstLine() const;
  uint32_t lastLine() const;

  /// Copy the acquisition controls into a preview coordinate.
  void applyAcquisitionTo(orc::PreviewCoordinate& coordinate) const;

  /**
   * @brief Update vectorscope with new data
   * @param data Vectorscope data from renderer
   */
  void updateVectorscope(const orc::VectorscopeData& data);

  /**
   * @brief Render vectorscope from extracted U/V data
   * @param data Vectorscope data containing U/V samples
   */
  void renderVectorscope(const orc::VectorscopeData& data);

  /**
   * @brief Clear the vectorscope display
   */
  void clearDisplay();

 Q_SIGNALS:
  void closed();
  void dataRefreshRequested();

 protected:
  void closeEvent(QCloseEvent* event) override;

 private slots:
  void onBlendColorToggled();
  void onDefocusToggled();
  void onFieldSelectionChanged();
  void onGraticuleChanged();
  void onDrawLinesToggled();
  void onPointSizeChanged();
  void onActiveAreaOnlyToggled();
  void onSampleWindowChanged();
  void onLineRangeChanged();

 private:
  friend class VectorscopeDialogPrivate;

  void setupUI();
  void connectSignals();

  /// Show an image the CPU renderer produced, whichever path is in force.
  void showStaticImage(const QImage& image);

  /// Plot a decoded-component acquisition on the scope canvas.
  void renderVectorscopeOnCanvas(const orc::VectorscopeData& data,
                                 bool has_chroma);

  /// Give the window back to the CPU renderer after a canvas render failure.
  void downgradeIfCanvasFailed();

  /// What was acquired and how it was sampled, under the plot.
  void updateInfoLabel(const orc::VectorscopeData& data, int field_select);
  int getGraticuleMode() const;
  void updateWindowTitle();
  void updateAcquisitionControlState();
  void updateMeasurementReadout();

  // Pimpl - hides core types from header
  std::unique_ptr<VectorscopeDialogPrivate> d_;

  // UI components
  AspectRatioLabel* scope_label_;
  QLabel* info_label_;

  // Display options
  QCheckBox* blend_color_checkbox_;
  QCheckBox* defocus_checkbox_;
  QCheckBox* draw_lines_checkbox_;
  QSpinBox* point_size_spinbox_;

  // Field selection options
  QRadioButton* field_select_all_radio_;
  QRadioButton* field_select_first_radio_;
  QRadioButton* field_select_second_radio_;
  QButtonGroup* field_select_group_;

  // Acquisition in force, and the label that reports it.  Not a control: it
  // follows the selected stage's output.
  orc::VectorscopeAcquisitionMode acquisition_mode_{
      orc::VectorscopeAcquisitionMode::DecodedComponent};
  QLabel* acquisition_label_;

  // Sampling options.  The line select applies to both acquisitions;
  // window_options_ holds the radios that only a composite one can act on.
  QGroupBox* sampling_group_;
  QWidget* window_options_;
  QRadioButton* window_burst_radio_;
  QRadioButton* window_active_radio_;
  QRadioButton* window_whole_radio_;
  QButtonGroup* window_group_;
  QCheckBox* active_area_only_checkbox_;
  QCheckBox* all_lines_checkbox_;
  QSpinBox* first_line_spinbox_;
  QSpinBox* last_line_spinbox_;

  // Measurement readouts (composite acquisition only)
  QGroupBox* measurements_group_;
  QLabel* measurements_label_;

  // Graticule options
  QRadioButton* graticule_none_radio_;
  QRadioButton* graticule_full_radio_;
  QRadioButton* graticule_75_radio_;
  QRadioButton* graticule_both_radio_;
  QButtonGroup* graticule_group_;
};

#endif  // ORC_GUI_ANALYSIS_VECTORSCOPE_DIALOG_H
