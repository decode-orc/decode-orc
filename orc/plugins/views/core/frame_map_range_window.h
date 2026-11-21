/*
 * File:        frame_map_range_window.h
 * Module:      orc-view_core
 * Purpose:     The Frame Map Range Finder's window: start and end as picture
 *              numbers and as CLV time codes, each following the other
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/view-support/analysis_tool_window.h>
#include <orc/view/stage_tool_session.h>

#include <memory>

class QFormLayout;
class QSpinBox;
class QWidget;

/**
 * @brief The host's analysis window, with the Range Finder's own controls
 *
 * The tool takes its start and end addresses as text. The window offers
 * each as a picture number and as a CLV time code (h:m:s.p) that follow one
 * another, and passes the picture number. A time code counts 25 pictures a
 * second on a PAL project and 30 otherwise; 0:0:0.0 is picture 1.
 */
class FrameMapRangeWindow : public orc::view::AnalysisToolWindow {
  Q_OBJECT

 public:
  explicit FrameMapRangeWindow(
      std::shared_ptr<orc::view::IStageToolSession> session,
      QWidget* parent = nullptr);

  // ---- for tests ----
  QSpinBox* pictureSpin(bool start) const {
    return start ? start_.picture : end_.picture;
  }
  /// Hours, minutes, seconds and pictures, by @p part 0 to 3.
  QSpinBox* timecodeSpin(bool start, int part) const;
  int picturesPerSecond() const { return fps_; }

 protected:
  void buildParameters(const orc::view::ToolAnalysisInfo& info,
                       QFormLayout* form) override;
  orc::view::ToolParameterMap currentParameters() const override;
  void showParameters(const orc::view::ToolParameterMap& values) override;

 private:
  struct Address {
    QSpinBox* picture = nullptr;
    QSpinBox* hours = nullptr;
    QSpinBox* minutes = nullptr;
    QSpinBox* seconds = nullptr;
    QSpinBox* pictures = nullptr;
  };

  QWidget* makeTimecode(Address& address);
  void connectAddress(Address& address);
  void timecodeFromPicture(Address& address);
  void pictureFromTimecode(Address& address);

  Address start_;
  Address end_;
  int fps_ = 30;
  bool syncing_ = false;
};
