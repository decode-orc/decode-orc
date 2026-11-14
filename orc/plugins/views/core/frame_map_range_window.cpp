/*
 * File:        frame_map_range_window.cpp
 * Module:      orc-view_core
 * Purpose:     The Frame Map Range Finder's window: start and end as picture
 *              numbers and as CLV time codes, each following the other
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "frame_map_range_window.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QString>
#include <QWidget>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "../../stages/frame_map/frame_map_range_contract.h"

namespace {

QSpinBox* spin(int minimum, int maximum, const char* tip) {
  auto* box = new QSpinBox();
  box->setMinimum(minimum);
  box->setMaximum(maximum);
  box->setToolTip(tip);
  return box;
}

}  // namespace

FrameMapRangeWindow::FrameMapRangeWindow(
    std::shared_ptr<orc::view::IStageToolSession> session, QWidget* parent)
    : AnalysisToolWindow(std::move(session), parent) {}

QSpinBox* FrameMapRangeWindow::timecodeSpin(bool start, int part) const {
  const Address& address = start ? start_ : end_;
  switch (part) {
    case 0:
      return address.hours;
    case 1:
      return address.minutes;
    case 2:
      return address.seconds;
    default:
      return address.pictures;
  }
}

void FrameMapRangeWindow::buildParameters(
    const orc::view::ToolAnalysisInfo& info, QFormLayout* form) {
  fps_ = info.video_system == "PAL" ? 25 : 30;

  form->setContentsMargins(0, 0, 0, 0);
  form->setHorizontalSpacing(12);
  form->setVerticalSpacing(8);

  const int max_picture = std::numeric_limits<int32_t>::max();
  start_.picture = spin(1, max_picture, "Start picture number (1-based)");
  end_.picture = spin(1, max_picture, "End picture number (1-based)");
  form->addRow(new QLabel("Picture Number Start:"), start_.picture);
  form->addRow(new QLabel("Picture Number End:"), end_.picture);
  form->addRow(new QLabel("Timecode Start:"), makeTimecode(start_));
  form->addRow(new QLabel("Timecode End:"), makeTimecode(end_));

  start_.picture->setValue(1);
  end_.picture->setValue(1);
  timecodeFromPicture(start_);
  timecodeFromPicture(end_);
  connectAddress(start_);
  connectAddress(end_);
}

QWidget* FrameMapRangeWindow::makeTimecode(Address& address) {
  auto* container = new QWidget();
  auto* layout = new QHBoxLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(4);

  address.hours = spin(0, 999, "Hours");
  address.minutes = spin(0, 59, "Minutes");
  address.seconds = spin(0, 59, "Seconds");
  address.pictures = spin(0, fps_ - 1, "Picture (frame) within second");

  layout->addWidget(address.hours);
  layout->addWidget(new QLabel(":"));
  layout->addWidget(address.minutes);
  layout->addWidget(new QLabel(":"));
  layout->addWidget(address.seconds);
  layout->addWidget(new QLabel("."));
  layout->addWidget(address.pictures);
  layout->addStretch();
  return container;
}

void FrameMapRangeWindow::connectAddress(Address& address) {
  Address* target = &address;
  connect(address.picture, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [this, target](int) { timecodeFromPicture(*target); });
  for (QSpinBox* part :
       {address.hours, address.minutes, address.seconds, address.pictures}) {
    connect(part, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this, target](int) { pictureFromTimecode(*target); });
  }
}

void FrameMapRangeWindow::timecodeFromPicture(Address& address) {
  if (syncing_) {
    return;
  }
  syncing_ = true;
  int64_t frame_index = std::max(1, address.picture->value()) - 1;
  const int64_t per_hour = static_cast<int64_t>(fps_) * 3600;
  const int64_t per_minute = static_cast<int64_t>(fps_) * 60;
  const int64_t hours = frame_index / per_hour;
  frame_index %= per_hour;
  const int64_t minutes = frame_index / per_minute;
  frame_index %= per_minute;
  address.hours->setValue(static_cast<int>(hours));
  address.minutes->setValue(static_cast<int>(minutes));
  address.seconds->setValue(static_cast<int>(frame_index / fps_));
  address.pictures->setValue(static_cast<int>(frame_index % fps_));
  syncing_ = false;
}

void FrameMapRangeWindow::pictureFromTimecode(Address& address) {
  if (syncing_) {
    return;
  }
  syncing_ = true;
  const int64_t frame_index =
      static_cast<int64_t>(address.hours->value()) * 3600 * fps_ +
      static_cast<int64_t>(address.minutes->value()) * 60 * fps_ +
      static_cast<int64_t>(address.seconds->value()) * fps_ +
      static_cast<int64_t>(address.pictures->value());
  const int64_t picture = std::clamp<int64_t>(
      frame_index + 1, 1, std::numeric_limits<int32_t>::max());
  address.picture->setValue(static_cast<int>(picture));
  syncing_ = false;
}

orc::view::ToolParameterMap FrameMapRangeWindow::currentParameters() const {
  orc::view::ToolParameterMap values;
  if (start_.picture && end_.picture) {
    values[orc::frame_map_range_contract::kStartAddress] =
        std::to_string(start_.picture->value());
    values[orc::frame_map_range_contract::kEndAddress] =
        std::to_string(end_.picture->value());
  }
  return values;
}

void FrameMapRangeWindow::showParameters(
    const orc::view::ToolParameterMap& values) {
  // The stored addresses are picture numbers as text; the time codes follow
  // them. One that is not a number (a time code typed at the command line)
  // leaves the window's own value.
  const auto show = [&values](const char* name, Address& address) {
    const auto it = values.find(name);
    if (it == values.end() || !address.picture) {
      return;
    }
    bool ok = false;
    const int picture = QString::fromStdString(it->second).toInt(&ok);
    if (ok) {
      address.picture->setValue(picture);
    }
  };
  show(orc::frame_map_range_contract::kStartAddress, start_);
  show(orc::frame_map_range_contract::kEndAddress, end_);
}
