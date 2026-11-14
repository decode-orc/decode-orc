/*
 * File:        frame_marker_slider.h
 * Module:      orc-view_core
 * Purpose:     Slider with tick markers for values of interest
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef FRAME_MARKER_SLIDER_H
#define FRAME_MARKER_SLIDER_H

#include <QSlider>
#include <vector>

/**
 * @brief QSlider that draws tick markers at specific values
 *
 * Used by the dropout editor to mark frames that carry edits so the user can
 * find their work when scrubbing a long capture. Markers are drawn across the
 * groove in an accent colour; the handle renders on top as usual.
 *
 * Thread safety: GUI thread only.
 */
class FrameMarkerSlider : public QSlider {
  Q_OBJECT

 public:
  explicit FrameMarkerSlider(Qt::Orientation orientation,
                             QWidget* parent = nullptr);

  /// Set the slider values to mark (values outside the range are ignored).
  void setMarkedValues(std::vector<int> values);
  const std::vector<int>& markedValues() const { return marked_values_; }

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  std::vector<int> marked_values_;
};

#endif  // FRAME_MARKER_SLIDER_H
