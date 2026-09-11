/*
 * File:        histogram_dialog.h
 * Module:      orc-gui
 * Purpose:     Video histogram visualization dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_PREVIEW_HISTOGRAM_DIALOG_H
#define ORC_GUI_PREVIEW_HISTOGRAM_DIALOG_H

#include <orc_histogram.h>

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <optional>

class PlotWidget;
class PlotSeries;

/**
 * @brief Video histogram visualization for chroma decoder output.
 *
 * For Y mode a single plot is shown.  For YUV / YIQ three vertically-stacked
 * plots are shown — one per component — so each has its own independent Y
 * scale and differences between components remain visible.
 *
 * The X axis is expressed in percent of full scale (0 % = blanking / black,
 * 100 % = white).  EBU R103 tolerance zones are rendered as shaded bands:
 *   - Red   : below 0 % or above 100 % (illegal / out-of-range)
 *   - Orange: NTSC 7.5 IRE black pedestal zone (0–7.5 %)
 */
class HistogramDialog : public QDialog {
  Q_OBJECT

 public:
  explicit HistogramDialog(QWidget* parent = nullptr);
  ~HistogramDialog() override;

  void updateHistogram(const orc::VideoHistogramData& data);
  void clearDisplay();

 Q_SIGNALS:
  void closed();

 protected:
  void closeEvent(QCloseEvent* event) override;

 private slots:
  void onChannelSelectionChanged(int index);

 private:
  enum class ChannelMode { Y, YUV, YIQ };

  /**
   * @brief What one plot's fixed furniture was last built for.
   *
   * The zones, guide lines and the trace item itself depend only on the
   * channel's kind, the video system and the theme - none of which change from
   * one displayed frame to the next. Rebuilding them per frame deleted and
   * re-created every QGraphicsItem in up to three plots, 25 times a second,
   * for a picture that differed only in the bin heights. Remembering what the
   * furniture was built for lets an ordinary update set the data and stop.
   */
  struct PlotFurniture {
    PlotSeries* series = nullptr;  ///< The trace; owned by the plot.
    bool built = false;
    bool is_chroma = false;
    bool is_ntsc = false;
    bool dark = false;
  };

  void setupUI();
  void rebuildPlots();
  void populatePlot(
      PlotWidget* plot, PlotFurniture& furniture, const QString& series_title,
      const QColor& color,
      const std::array<uint32_t, orc::VideoHistogramData::kBinCount>& bins,
      bool is_ntsc, bool is_chroma, bool dark);
  void addEbuR103Zones(PlotWidget* plot, bool is_ntsc);
  void addChromaOverrangeZones(PlotWidget* plot);
  ChannelMode currentChannelMode() const;
  void setThreePlotMode(bool three_plots);

  QLabel* info_label_;
  QComboBox* channel_combo_;

  // One plot per channel row; secondary_ and tertiary_ are hidden in Y mode.
  PlotWidget* primary_plot_;    // always Y (luma)
  PlotWidget* secondary_plot_;  // U or I
  PlotWidget* tertiary_plot_;   // V or Q

  // Furniture state, one entry per plot above.
  PlotFurniture primary_furniture_;
  PlotFurniture secondary_furniture_;
  PlotFurniture tertiary_furniture_;

  std::optional<orc::VideoHistogramData> last_data_;
};

#endif  // ORC_GUI_PREVIEW_HISTOGRAM_DIALOG_H
