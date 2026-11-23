/*
 * File:        preview_compare_window_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 3 offscreen tests of the A/B Compare window: picking
 *              nodes, flicker and hold, the export request and its help
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_compare_window.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QThread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "mocks/mock_render_presenter.h"
#include "previewdialog.h"
#include "stage_help_dialog.h"

// The help is a resource of the static GUI library; a test binary has to
// register it itself, as orc-gui's main() does.
static void initGuiResources() { Q_INIT_RESOURCE(orc_gui_resources); }

namespace gui_unit_test {

namespace {

using orc::gui::ABSide;
using orc::gui::PreviewCompareWindow;
using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;

using MockPresenter = NiceMock<orc::presenters::test::MockRenderPresenter>;

QApplication& ensureApplication() {
  if (auto* existing =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing;
  }
  static int argc = 3;
  static char app_name[] = "orc-gui-compare-window-test";
  static char platform_opt[] = "-platform";
  static char platform_val[] = "offscreen";
  static char* argv[] = {app_name, platform_opt, platform_val, nullptr};
  static QApplication* app = [] {
    auto* created = new QApplication(argc, argv);
    created->setQuitOnLastWindowClosed(false);
    initGuiResources();
    return created;
  }();
  return *app;
}

bool waitUntil(const std::function<bool()>& predicate, int timeout_ms = 3000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    if (predicate()) {
      return true;
    }
    QThread::msleep(5);
  }
  QCoreApplication::processEvents();
  return predicate();
}

constexpr uint32_t kWidth = 400;
constexpr uint32_t kHeight = 300;
constexpr uint8_t kLevelA = 40;
constexpr uint8_t kLevelB = 200;
const char* const kOption = "interlaced_clamped";

orc::PreviewImage grey(uint8_t level) {
  orc::PreviewImage image;
  image.width = kWidth;
  image.height = kHeight;
  image.rgb_data.assign(static_cast<size_t>(kWidth) * kHeight * 3, level);
  return image;
}

orc::PreviewOutputInfo frameOutput(const std::string& option) {
  orc::PreviewOutputInfo info{};
  info.type = orc::PreviewOutputType::Frame_Field1_First;
  info.display_name = "Frame";
  info.count = 5;
  info.is_available = true;
  info.dar_aspect_correction = 1.0;
  info.option_id = option;
  return info;
}

// A project of three nodes: 1 and 2 offer the frame output with the option
// compared, 3 (a sink) offers another option only.
class CompareWindowFixture {
 public:
  CompareWindowFixture()
      : presenter_(std::make_shared<MockPresenter>()),
        coordinator_([p = presenter_](void*)
                         -> std::shared_ptr<orc::presenters::IRenderPresenter> {
          return p;
        }) {
    ensureApplication();
    ON_CALL(*presenter_, getAvailableOutputs(_))
        .WillByDefault(Invoke([](orc::NodeID node) {
          return std::vector<orc::PreviewOutputInfo>{frameOutput(
              node.value() == 3 ? "phase2_colour_carrier" : kOption)};
        }));
    ON_CALL(*presenter_, renderComparison(_))
        .WillByDefault(Invoke([this](const orc::PreviewCompareRequest& r) {
          {
            const std::lock_guard<std::mutex> lock(mutex_);
            last_request_ = r;
          }
          orc::PreviewCompareResult result;
          result.success = true;
          result.request = r;
          result.image_a = grey(r.node_a.value() == 1 ? kLevelA : kLevelB);
          result.image_b = grey(r.node_b.value() == 1 ? kLevelA : kLevelB);
          result.count_a = 5;
          result.count_b = 5;
          return result;
        }));
    ON_CALL(*presenter_, deriveComparisonView(_, _))
        .WillByDefault(Invoke([](const orc::PreviewCompareResult& result,
                                 const orc::PreviewCompareViewSettings& s) {
          orc::PreviewCompareViewImages view;
          view.success = true;
          if (s.mode == orc::PreviewCompareViewMode::Difference) {
            view.difference = result.image_a;
          } else {
            view.image_a = result.image_a;
            view.image_b = result.image_b;
          }
          return view;
        }));
    ON_CALL(*presenter_, exportComparison(_, _, _))
        .WillByDefault(Invoke([this](const orc::PreviewCompareRequest& r,
                                     const orc::PreviewCompareExportSettings& s,
                                     const std::string& path) {
          {
            const std::lock_guard<std::mutex> lock(mutex_);
            export_request_ = r;
            export_settings_ = s;
            export_path_ = path;
          }
          orc::PreviewCompareExportResult result;
          result.success = true;
          result.crop_x = s.crop_x;
          result.crop_y = s.crop_y;
          result.crop_width = s.crop_width;
          result.crop_height = s.crop_height;
          return result;
        }));

    coordinator_.start();
    coordinator_.setProject(reinterpret_cast<void*>(0x1));
    coordinator_.updateDAG(std::make_shared<int>(1));

    window_ = std::make_unique<PreviewCompareWindow>(&coordinator_);
    window_->setNodes({{orc::NodeID(1), "CVBS Source"},
                       {orc::NodeID(2), "Video Parameters"},
                       {orc::NodeID(3), "Video Sink"}});
    window_->setOutput(orc::PreviewOutputType::Frame_Field1_First, kOption,
                       "Frame");
    window_->setIndex(2);
    window_->setPreferredNodeA(orc::NodeID(1));
    window_->setProjectPath("/projects/my capture.orcprj");
    window_->resize(800, 600);
  }

  ~CompareWindowFixture() {
    window_.reset();
    coordinator_.stop();
  }

  PreviewCompareWindow& window() { return *window_; }

  // Show the window and wait until the pair is on screen.
  void showAndWaitForPair() {
    window_->show();
    ASSERT_TRUE(waitUntil([this] {
      return window_->delivery() != nullptr && window_->delivery()->ok();
    }));
  }

  int shownLevel() const {
    return qGray(window_->viewport()->grab().toImage().pixel(
        window_->viewport()->width() / 2, window_->viewport()->height() / 2));
  }

  std::optional<orc::PreviewCompareRequest> lastRequest() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return last_request_;
  }
  std::optional<orc::PreviewCompareExportSettings> exportSettings() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return export_settings_;
  }
  std::string exportPath() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return export_path_;
  }

 private:
  std::shared_ptr<MockPresenter> presenter_;
  RenderCoordinator coordinator_;
  std::unique_ptr<PreviewCompareWindow> window_;
  std::mutex mutex_;
  std::optional<orc::PreviewCompareRequest> last_request_;
  std::optional<orc::PreviewCompareRequest> export_request_;
  std::optional<orc::PreviewCompareExportSettings> export_settings_;
  std::string export_path_;
};

}  // namespace

TEST(PreviewCompareWindowTest, ListsOnlyNodesThatOfferTheOutputAndPicksAPair) {
  CompareWindowFixture fixture;
  fixture.showAndWaitForPair();
  auto& window = fixture.window();

  ASSERT_EQ(window.nodeACombo()->count(), 2);
  ASSERT_EQ(window.nodeBCombo()->count(), 2);
  EXPECT_EQ(window.nodeACombo()->findData(3), -1)
      << "a node without the option was offered";
  EXPECT_EQ(window.nodeACombo()->currentData().toInt(), 1);
  EXPECT_EQ(window.nodeBCombo()->currentData().toInt(), 2);

  const auto request = fixture.lastRequest();
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->node_a, orc::NodeID(1));
  EXPECT_EQ(request->node_b, orc::NodeID(2));
  EXPECT_EQ(request->option_id, kOption);
  EXPECT_EQ(request->index, 2u);

  EXPECT_EQ(fixture.shownLevel(), kLevelA);
  EXPECT_TRUE(window.sideLabel()->isVisible());
  EXPECT_TRUE(window.sideLabel()->text().startsWith("A: CVBS Source"));
}

TEST(PreviewCompareWindowTest, FlickersBetweenTheSidesAndHoldsOnSpace) {
  CompareWindowFixture fixture;
  auto& window = fixture.window();
  window.intervalCombo()->setCurrentIndex(
      window.intervalCombo()->findText("0.5 s"));
  fixture.showAndWaitForPair();

  ASSERT_TRUE(waitUntil(
      [&] { return window.flickerState().shownSide() == ABSide::B; }, 2000));
  EXPECT_EQ(fixture.shownLevel(), kLevelB);
  EXPECT_TRUE(window.sideLabel()->text().startsWith("B: Video Parameters"));
  EXPECT_EQ(window.holdButton()->text(), "Animate");
  EXPECT_FALSE(window.holdButton()->isChecked());

  window.toggleHold();
  EXPECT_TRUE(window.flickerState().isHeld());
  EXPECT_TRUE(window.holdButton()->isChecked());
  EXPECT_EQ(window.holdButton()->text(), "Hold");
  const ABSide held = window.flickerState().shownSide();
  waitUntil([] { return false; }, 1200);
  EXPECT_EQ(window.flickerState().shownSide(), held);

  window.toggleHold();
  EXPECT_FALSE(window.flickerState().isHeld());
  EXPECT_EQ(window.holdButton()->text(), "Animate");
  ASSERT_TRUE(waitUntil(
      [&] { return window.flickerState().shownSide() != held; }, 2000));
}

TEST(PreviewCompareWindowTest, TheAAndBKeysShowOneSideAndHold) {
  CompareWindowFixture fixture;
  fixture.showAndWaitForPair();
  auto& window = fixture.window();

  window.showSide(ABSide::B);
  EXPECT_TRUE(window.flickerState().isHeld());
  EXPECT_EQ(fixture.shownLevel(), kLevelB);
  window.showSide(ABSide::A);
  EXPECT_EQ(fixture.shownLevel(), kLevelA);
}

TEST(PreviewCompareWindowTest, TheDifferenceViewDisablesTheFlickerControls) {
  CompareWindowFixture fixture;
  auto& window = fixture.window();
  window.intervalCombo()->setCurrentIndex(
      window.intervalCombo()->findText("0.5 s"));
  fixture.showAndWaitForPair();
  ASSERT_TRUE(window.showAButton()->isEnabled());
  ASSERT_TRUE(window.holdButton()->isEnabled());

  // Difference is one picture: there is no side to show or to flicker to.
  window.viewModeCombo()->setCurrentIndex(window.viewModeCombo()->findData(
      static_cast<int>(orc::PreviewCompareViewMode::Difference)));
  EXPECT_FALSE(window.showAButton()->isEnabled());
  EXPECT_FALSE(window.showBButton()->isEnabled());
  EXPECT_FALSE(window.holdButton()->isEnabled());
  EXPECT_FALSE(window.intervalCombo()->isEnabled());

  window.viewModeCombo()->setCurrentIndex(window.viewModeCombo()->findData(
      static_cast<int>(orc::PreviewCompareViewMode::Normal)));
  EXPECT_TRUE(window.showAButton()->isEnabled());
  EXPECT_TRUE(window.showBButton()->isEnabled());
  EXPECT_TRUE(window.holdButton()->isEnabled());
  EXPECT_TRUE(window.intervalCombo()->isEnabled());
}

TEST(PreviewCompareWindowTest, SwapKeepsThePictureAndRelabelsIt) {
  CompareWindowFixture fixture;
  fixture.showAndWaitForPair();
  auto& window = fixture.window();
  window.showSide(ABSide::A);

  window.swapButton()->click();
  EXPECT_EQ(window.nodeACombo()->currentData().toInt(), 2);
  EXPECT_EQ(window.nodeBCombo()->currentData().toInt(), 1);
  EXPECT_EQ(window.flickerState().shownSide(), ABSide::B);
  EXPECT_EQ(fixture.shownLevel(), kLevelA);
  EXPECT_TRUE(window.sideLabel()->text().startsWith("B: CVBS Source"));
  ASSERT_TRUE(waitUntil([&] {
    const auto request = fixture.lastRequest();
    return request && request->node_a == orc::NodeID(2);
  }));
}

TEST(PreviewCompareWindowTest, FollowsThePreviewsFrame) {
  CompareWindowFixture fixture;
  fixture.showAndWaitForPair();
  fixture.window().setIndex(4);
  ASSERT_TRUE(waitUntil([&] {
    const auto request = fixture.lastRequest();
    return request && request->index == 4;
  }));
}

TEST(PreviewCompareWindowTest, AnErrorIsShownInTheWindow) {
  CompareWindowFixture fixture;
  fixture.showAndWaitForPair();
  auto& window = fixture.window();
  // An option neither node offers leaves nothing to pick.
  window.setOutput(orc::PreviewOutputType::Frame_Field1_First, "nonesuch",
                   "Other");
  EXPECT_EQ(window.nodeACombo()->count(), 0);
  EXPECT_FALSE(window.currentRequest().has_value());
  EXPECT_TRUE(window.messageLabel()->text().contains("No stage offers"));
}

TEST(PreviewCompareWindowTest, TheExportCarriesTheVisibleCropAndSettings) {
  CompareWindowFixture fixture;
  fixture.showAndWaitForPair();
  auto& window = fixture.window();

  window.viewModeCombo()->setCurrentIndex(
      window.viewModeCombo()->findText("Stretched"));
  window.gainSpin()->setValue(6.0);
  window.intervalCombo()->setCurrentIndex(
      window.intervalCombo()->findText("1 s"));
  window.zoomCombo()->setCurrentIndex(window.zoomCombo()->findText("4×"));
  ASSERT_TRUE(waitUntil([&] {
    return window.delivery() != nullptr &&
           window.delivery()->view_settings.mode ==
               orc::PreviewCompareViewMode::Stretched;
  }));
  ASSERT_DOUBLE_EQ(window.viewport()->zoomLevel(), 4.0);
  window.viewport()->setVisibleOrigin(QPoint(400, 200));
  const QPoint origin = window.viewport()->visibleOrigin();
  ASSERT_EQ(origin, QPoint(400, 200));

  // At 4x, widget pixel 400 is sample 100.
  const int width = window.viewport()->width();
  const int height = window.viewport()->height();
  const int right = std::min<int>(
      kWidth, static_cast<int>(std::ceil((origin.x() + width) / 4.0)));
  const int bottom = std::min<int>(
      kHeight, static_cast<int>(std::ceil((origin.y() + height) / 4.0)));

  window.exportTo("ab.webp");
  ASSERT_TRUE(waitUntil([&] { return fixture.exportSettings().has_value(); }));
  const auto settings = *fixture.exportSettings();
  EXPECT_EQ(fixture.exportPath(), "ab.webp");
  EXPECT_EQ(settings.crop_mode, orc::PreviewCompareCropMode::Explicit);
  EXPECT_EQ(settings.crop_x, 100u);
  EXPECT_EQ(settings.crop_y, 50u);
  EXPECT_EQ(settings.crop_width, static_cast<uint32_t>(right - 100));
  EXPECT_EQ(settings.crop_height, static_cast<uint32_t>(bottom - 50));
  EXPECT_EQ(settings.scale, 4u);
  EXPECT_TRUE(settings.stretch);
  EXPECT_DOUBLE_EQ(settings.stretch_gain, 6.0);
  EXPECT_EQ(settings.stretch_window, orc::kPreviewCompareStretchWindow);
  EXPECT_EQ(settings.interval_ms, 1000u);

  const QString command = window.commandLineEdit()->text();
  EXPECT_TRUE(command.startsWith(
      "orc-cli preview compare '/projects/my capture.orcprj' --a 1 --b 2 "
      "--frame 3 --output-type frame --option interlaced_clamped"))
      << command.toStdString();
  EXPECT_TRUE(command.contains(
      QStringLiteral("--crop 100,50,%1,%2").arg(right - 100).arg(bottom - 50)))
      << command.toStdString();
  EXPECT_TRUE(command.contains("--stretch 6 --stretch-window 15"));
  EXPECT_TRUE(command.contains("--scale 4 --interval 1000 -o ab.webp"));

  ASSERT_TRUE(waitUntil([&] {
    return window.messageLabel()->text().startsWith("Wrote ab.webp");
  }));
}

TEST(PreviewCompareWindowTest, TheNormalViewExportsWithoutTheStretchedHalf) {
  CompareWindowFixture fixture;
  fixture.showAndWaitForPair();
  auto& window = fixture.window();
  const auto settings = window.exportSettings();
  EXPECT_FALSE(settings.stretch);
  EXPECT_EQ(settings.scale, 1u);
  EXPECT_EQ(settings.interval_ms, 800u);
  // At 1x the whole picture is in view, so the crop is all of it.
  EXPECT_EQ(settings.crop_x, 0u);
  EXPECT_EQ(settings.crop_width, kWidth);
  EXPECT_TRUE(window.commandLineEdit()->text().contains("--stretch 0"));
}

TEST(PreviewCompareWindowTest, TheHelpButtonOpensTheABCompareSection) {
  CompareWindowFixture fixture;
  fixture.showAndWaitForPair();
  auto& window = fixture.window();

  window.helpButton()->click();
  auto* help = window.findChild<StageHelpDialog*>();
  ASSERT_NE(help, nullptr);
  auto* browser = help->findChild<QTextBrowser*>();
  ASSERT_NE(browser, nullptr);
  EXPECT_TRUE(
      browser->toPlainText().contains(PreviewCompareWindow::helpHeading()));
  EXPECT_TRUE(help->scrollToHeading(PreviewCompareWindow::helpHeading()));
  help->close();
}

TEST(PreviewCompareWindowTest, TheCompareActionNeedsANodeOnShow) {
  ensureApplication();
  PreviewDialog dialog;
  ASSERT_NE(dialog.compareAction(), nullptr);
  EXPECT_FALSE(dialog.compareAction()->isEnabled());
  dialog.setCurrentNodeId(orc::NodeID(3));
  EXPECT_TRUE(dialog.compareAction()->isEnabled());
  dialog.setCurrentNodeId(orc::NodeID());
  EXPECT_FALSE(dialog.compareAction()->isEnabled());
}

}  // namespace gui_unit_test
