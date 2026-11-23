/*
 * File:        field_preview_message_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     The preview says why it is empty when a render fails
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/preview/orc_rendering.h>

#include <QApplication>
#include <QCoreApplication>
#include <QImage>

#include "fieldpreviewwidget.h"

namespace gui_unit_test {

namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-widget-test";
  static char platform_opt[] = "-platform";
  static char platform_val[] = "offscreen";
  static char* argv[] = {app_name, platform_opt, platform_val, nullptr};
  static QApplication* app = [] {
    auto* created_app = new QApplication(argc, argv);
    created_app->setQuitOnLastWindowClosed(false);
    return created_app;
  }();
  return *app;
}

orc::PreviewImage makeGrayImage() {
  orc::PreviewImage image;
  image.width = 100;
  image.height = 50;
  image.rgb_data.assign(static_cast<size_t>(image.width) * image.height * 3,
                        128);
  return image;
}

}  // namespace

TEST(FieldPreviewMessage, ShowMessage_ClearsFrameAndKeepsText) {
  ensureApplication();
  FieldPreviewWidget widget;
  widget.resize(400, 300);
  widget.setImage(makeGrayImage());

  widget.showMessage("Alignment map references invalid input ID: 4");

  EXPECT_EQ(widget.message(), "Alignment map references invalid input ID: 4");
  EXPECT_TRUE(widget.originalImageSize().isEmpty());
}

TEST(FieldPreviewMessage, ArrivingFrame_RemovesMessage) {
  ensureApplication();
  FieldPreviewWidget widget;
  widget.resize(400, 300);
  widget.showMessage("render failed");

  widget.setImage(makeGrayImage());

  EXPECT_TRUE(widget.message().isEmpty());
  EXPECT_EQ(widget.originalImageSize(), QSize(100, 50));
}

TEST(FieldPreviewMessage, ClearImage_RemovesMessage) {
  ensureApplication();
  FieldPreviewWidget widget;
  widget.showMessage("render failed");

  widget.clearImage();

  EXPECT_TRUE(widget.message().isEmpty());
}

TEST(FieldPreviewMessage, NoMessage_ByDefault) {
  ensureApplication();
  FieldPreviewWidget widget;

  EXPECT_TRUE(widget.message().isEmpty());
}

}  // namespace gui_unit_test
