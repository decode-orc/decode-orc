/*
 * File:        dropout_frame_view_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Interaction tests for the dropout editor frame view
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QMouseEvent>
#include <optional>
#include <vector>

#include "dropout_editor_dialog.h"

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

orc::presenters::DropoutRegion makeRegion(uint32_t line, uint32_t start,
                                          uint32_t end) {
  orc::presenters::DropoutRegion region;
  region.line = line;
  region.start_sample = start;
  region.end_sample = end;
  region.basis = orc::presenters::DropoutRegion::DetectionBasis::HINT_DERIVED;
  return region;
}

void sendMousePress(QWidget* widget, const QPointF& pos,
                    Qt::MouseButton button = Qt::LeftButton) {
  QMouseEvent event(QEvent::MouseButtonPress, pos,
                    widget->mapToGlobal(pos.toPoint()), button, button,
                    Qt::NoModifier);
  QCoreApplication::sendEvent(widget, &event);
}

void sendMouseMove(QWidget* widget, const QPointF& pos,
                   Qt::MouseButtons buttons = Qt::LeftButton) {
  QMouseEvent event(QEvent::MouseMove, pos, widget->mapToGlobal(pos.toPoint()),
                    Qt::NoButton, buttons, Qt::NoModifier);
  QCoreApplication::sendEvent(widget, &event);
}

void sendMouseRelease(QWidget* widget, const QPointF& pos,
                      Qt::MouseButton button = Qt::LeftButton) {
  QMouseEvent event(QEvent::MouseButtonRelease, pos,
                    widget->mapToGlobal(pos.toPoint()), button, Qt::NoButton,
                    Qt::NoModifier);
  QCoreApplication::sendEvent(widget, &event);
}

void sendDrag(QWidget* widget, const QPointF& from, const QPointF& to) {
  sendMousePress(widget, from);
  sendMouseMove(widget, to);
  sendMouseRelease(widget, to);
}

/**
 * Test fixture with a 100x50 frame at zoom 1.0 and square aspect, so widget
 * coordinates map 1:1 onto image coordinates.
 */
class DropoutFrameViewTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ensureApplication();
    view_ = std::make_unique<DropoutFrameView>();
    view_->setAspectCorrection(1.0);
  }

  void setFrame(
      const std::vector<orc::presenters::DropoutRegion>& sources,
      const std::vector<orc::presenters::DropoutRegion>& additions = {},
      const std::vector<orc::presenters::DropoutRegion>& removals = {}) {
    QImage image(100, 50, QImage::Format_RGB888);
    image.fill(Qt::gray);
    // The view no longer grows to the zoomed frame - it stays its own size and
    // pans - so the test says how big the viewport is. At exactly the image
    // size and zoom 1.0 widget coordinates equal image coordinates, which is
    // what every coordinate below assumes.
    view_->resize(100, 50);
    view_->setFrame(image, sources, additions, removals);
    ASSERT_EQ(view_->zoomLevel(), 1.0);
    ASSERT_EQ(view_->widgetFromImage(QPointF(0, 0)), QPointF(0, 0));
    ASSERT_EQ(view_->widgetFromImage(QPointF(100, 50)), QPointF(100, 50));
  }

  std::unique_ptr<DropoutFrameView> view_;
};

TEST_F(DropoutFrameViewTest, DragOnEmptyArea_EmitsRegionAddRequested) {
  setFrame({});

  std::optional<orc::presenters::DropoutRegion> added;
  QObject::connect(view_.get(), &DropoutFrameView::regionAddRequested,
                   [&added](const orc::presenters::DropoutRegion& region) {
                     added = region;
                   });

  sendDrag(view_.get(), QPointF(10, 30), QPointF(40, 30));

  ASSERT_TRUE(added.has_value());
  EXPECT_EQ(added->line, 30u);
  EXPECT_EQ(added->start_sample, 10u);
  EXPECT_EQ(added->end_sample, 40u);
}

TEST_F(DropoutFrameViewTest, ClickWithoutDrag_DoesNotAddRegion) {
  setFrame({});

  int add_count = 0;
  QObject::connect(
      view_.get(), &DropoutFrameView::regionAddRequested,
      [&add_count](const orc::presenters::DropoutRegion&) { ++add_count; });

  sendMousePress(view_.get(), QPointF(10, 30));
  sendMouseRelease(view_.get(), QPointF(10, 30));

  EXPECT_EQ(add_count, 0);
}

TEST_F(DropoutFrameViewTest, ClickSourceRegion_SelectsIt) {
  setFrame({makeRegion(10, 20, 40)});

  DropoutFrameView::RegionKind selected_kind =
      DropoutFrameView::RegionKind::None;
  int selected_index = -2;
  QObject::connect(view_.get(), &DropoutFrameView::selectionChanged,
                   [&](DropoutFrameView::RegionKind kind, int index) {
                     selected_kind = kind;
                     selected_index = index;
                   });

  // The overlay band is thicker than one image line, so a click on the line
  // itself must hit even though the region is only one line tall.
  sendMousePress(view_.get(), QPointF(30, 10));
  sendMouseRelease(view_.get(), QPointF(30, 10));

  EXPECT_EQ(selected_kind, DropoutFrameView::RegionKind::Source);
  EXPECT_EQ(selected_index, 0);
  EXPECT_EQ(view_->selectedKind(), DropoutFrameView::RegionKind::Source);
  EXPECT_EQ(view_->selectedIndex(), 0);
}

TEST_F(DropoutFrameViewTest, DragAdditionBody_EmitsModifyWithMovedRegion) {
  setFrame({}, {makeRegion(30, 10, 40)});

  std::optional<orc::presenters::DropoutRegion> modified;
  int modified_index = -1;
  QObject::connect(
      view_.get(), &DropoutFrameView::additionModifyRequested,
      [&](int index, const orc::presenters::DropoutRegion& region) {
        modified_index = index;
        modified = region;
      });

  // Press on the addition body, drag +5 samples and +2 lines.
  sendMousePress(view_.get(), QPointF(25, 30));
  sendMouseMove(view_.get(), QPointF(30, 32));
  sendMouseRelease(view_.get(), QPointF(30, 32));

  ASSERT_TRUE(modified.has_value());
  EXPECT_EQ(modified_index, 0);
  EXPECT_EQ(modified->line, 32u);
  EXPECT_EQ(modified->start_sample, 15u);
  EXPECT_EQ(modified->end_sample, 45u);
}

TEST_F(DropoutFrameViewTest, ClickAdditionWithoutMoving_DoesNotEmitModify) {
  setFrame({}, {makeRegion(30, 10, 40)});

  int modify_count = 0;
  QObject::connect(view_.get(), &DropoutFrameView::additionModifyRequested,
                   [&modify_count](int, const orc::presenters::DropoutRegion&) {
                     ++modify_count;
                   });

  sendMousePress(view_.get(), QPointF(25, 30));
  sendMouseRelease(view_.get(), QPointF(25, 30));

  EXPECT_EQ(modify_count, 0);
  EXPECT_EQ(view_->selectedKind(), DropoutFrameView::RegionKind::Addition);
}

TEST_F(DropoutFrameViewTest, DragRightHandle_ResizesAddition) {
  setFrame({}, {makeRegion(30, 10, 40)});
  view_->setSelectedRegion(DropoutFrameView::RegionKind::Addition, 0);

  std::optional<orc::presenters::DropoutRegion> modified;
  QObject::connect(view_.get(), &DropoutFrameView::additionModifyRequested,
                   [&](int, const orc::presenters::DropoutRegion& region) {
                     modified = region;
                   });

  // The right resize handle sits at the band's right edge (x = end_sample).
  sendMousePress(view_.get(), QPointF(40, 30.5));
  sendMouseMove(view_.get(), QPointF(50, 30.5));
  sendMouseRelease(view_.get(), QPointF(50, 30.5));

  ASSERT_TRUE(modified.has_value());
  EXPECT_EQ(modified->line, 30u);
  EXPECT_EQ(modified->start_sample, 10u);
  EXPECT_EQ(modified->end_sample, 51u);  // Pixel 50 included (exclusive end)
}

TEST_F(DropoutFrameViewTest, DragSourceRightHandle_EmitsSourceResizeRequested) {
  setFrame({makeRegion(10, 20, 40)});
  view_->setSelectedRegion(DropoutFrameView::RegionKind::Source, 0);

  std::optional<orc::presenters::DropoutRegion> resized;
  int resized_index = -1;
  QObject::connect(
      view_.get(), &DropoutFrameView::sourceResizeRequested,
      [&](int index, const orc::presenters::DropoutRegion& region) {
        resized_index = index;
        resized = region;
      });

  // The right resize handle sits at the band's right edge (x = end_sample).
  sendMousePress(view_.get(), QPointF(40, 10.5));
  sendMouseMove(view_.get(), QPointF(55, 10.5));
  sendMouseRelease(view_.get(), QPointF(55, 10.5));

  ASSERT_TRUE(resized.has_value());
  EXPECT_EQ(resized_index, 0);
  EXPECT_EQ(resized->line, 10u);
  EXPECT_EQ(resized->start_sample, 20u);
  EXPECT_EQ(resized->end_sample, 56u);  // Pixel 55 included (exclusive end)

  // The source list itself must stay untouched; the dialog applies the edit.
  ASSERT_EQ(view_->getSourceDropouts().size(), 1u);
  EXPECT_EQ(view_->getSourceDropouts()[0].end_sample, 40u);
}

TEST_F(DropoutFrameViewTest, RemovedSourceDropout_HasNoActiveHandles) {
  const auto source = makeRegion(10, 20, 40);
  setFrame({source}, {}, {source});  // Source already marked removed
  view_->setSelectedRegion(DropoutFrameView::RegionKind::Source, 0);

  int resize_count = 0;
  QObject::connect(view_.get(), &DropoutFrameView::sourceResizeRequested,
                   [&resize_count](int, const orc::presenters::DropoutRegion&) {
                     ++resize_count;
                   });

  sendMousePress(view_.get(), QPointF(40, 10.5));
  sendMouseMove(view_.get(), QPointF(55, 10.5));
  sendMouseRelease(view_.get(), QPointF(55, 10.5));

  EXPECT_EQ(resize_count, 0);
}

TEST_F(DropoutFrameViewTest, MoveDragClampsToImageBounds) {
  setFrame({}, {makeRegion(30, 10, 40)});

  std::optional<orc::presenters::DropoutRegion> modified;
  QObject::connect(view_.get(), &DropoutFrameView::additionModifyRequested,
                   [&](int, const orc::presenters::DropoutRegion& region) {
                     modified = region;
                   });

  // Drag far past the top-left corner: region must clamp, keeping its length.
  sendMousePress(view_.get(), QPointF(25, 30));
  sendMouseMove(view_.get(), QPointF(-200, -200));
  sendMouseRelease(view_.get(), QPointF(-200, -200));

  ASSERT_TRUE(modified.has_value());
  EXPECT_EQ(modified->line, 0u);
  EXPECT_EQ(modified->start_sample, 0u);
  EXPECT_EQ(modified->end_sample, 30u);
}

TEST_F(DropoutFrameViewTest, UpdateRegions_ClearsStaleSelection) {
  setFrame({}, {makeRegion(30, 10, 40)});
  view_->setSelectedRegion(DropoutFrameView::RegionKind::Addition, 0);

  view_->updateRegions({}, {});

  EXPECT_EQ(view_->selectedKind(), DropoutFrameView::RegionKind::None);
  EXPECT_EQ(view_->selectedIndex(), -1);
}

TEST_F(DropoutFrameViewTest, RightClick_EmitsContextMenuRequestWithHit) {
  setFrame({makeRegion(10, 20, 40)});

  DropoutFrameView::RegionKind menu_kind = DropoutFrameView::RegionKind::None;
  int menu_index = -2;
  QObject::connect(
      view_.get(), &DropoutFrameView::contextMenuRequested,
      [&](DropoutFrameView::RegionKind kind, int index, const QPoint&) {
        menu_kind = kind;
        menu_index = index;
      });

  sendMousePress(view_.get(), QPointF(30, 10), Qt::RightButton);

  EXPECT_EQ(menu_kind, DropoutFrameView::RegionKind::Source);
  EXPECT_EQ(menu_index, 0);
  // Right-click also selects the hit region.
  EXPECT_EQ(view_->selectedKind(), DropoutFrameView::RegionKind::Source);
}

TEST_F(DropoutFrameViewTest, OrphanRemoval_DetectedAndSelectable) {
  // A removal entry with no matching source dropout.
  setFrame({makeRegion(10, 20, 40)}, {}, {makeRegion(20, 5, 15)});

  EXPECT_TRUE(view_->isOrphanRemoval(0));

  sendMousePress(view_.get(), QPointF(10, 20));
  sendMouseRelease(view_.get(), QPointF(10, 20));

  EXPECT_EQ(view_->selectedKind(), DropoutFrameView::RegionKind::OrphanRemoval);
  EXPECT_EQ(view_->selectedIndex(), 0);
}

}  // namespace
}  // namespace gui_unit_test
