/*
 * File:        dropout_editor_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Edit round-trip and undo/redo tests for the dropout editor
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "dropout_editor_dialog.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QStatusBar>
#include <QTableWidget>
#include <QTest>
#include <memory>
#include <vector>

#include "mocks/mock_project_presenter.h"

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

/**
 * Fake render presenter serving synthetic 100x50 frames with one source
 * dropout on line 10, samples [20, 40). Called from the dialog's worker
 * thread; all methods are stateless apart from configuration set before the
 * dialog is constructed.
 */
class FakeRenderPresenter : public orc::presenters::IRenderPresenter {
 public:
  bool provide_outputs = true;
  uint64_t frame_count = 5;

  void setDAG(std::shared_ptr<void>) override {}
  bool getShowDropouts() const override { return false; }
  void setShowDropouts(bool) override {}
  void setBackgroundObservationEnabled(bool) override {}
  void setExecutionProgressCallback(
      orc::presenters::DagExecutionProgressCallback) override {}

  uint64_t subscribeInvalidation(
      orc::presenters::ObservationInvalidationCallback) override {
    return 0;
  }
  void unsubscribeInvalidation(uint64_t) override {}

  uint64_t requestObservations(
      orc::NodeID, orc::FieldID,
      orc::presenters::ObservationDataReadyCallback) override {
    return 0;
  }
  uint64_t subscribeObservationProgress(
      orc::presenters::ObservationProgressCallback) override {
    return 0;
  }
  void unsubscribeObservationProgress(uint64_t) override {}

  std::vector<orc::PreviewOutputInfo> getAvailableOutputs(
      orc::NodeID) override {
    if (!provide_outputs) {
      return {};
    }
    orc::PreviewOutputInfo info{};
    info.type = orc::PreviewOutputType::Frame_Field1_First;
    info.display_name = "Sequential Clamped";
    info.count = frame_count;
    info.is_available = true;
    info.dar_aspect_correction = 1.0;  // Identity mapping for tests
    info.option_id = "sequential_clamped";
    info.dropouts_available = true;
    info.has_separate_channels = false;
    info.first_field_offset = 0;
    return {info};
  }

  orc::PreviewRenderResult renderPreview(orc::NodeID node_id,
                                         orc::PreviewOutputType output_type,
                                         uint64_t output_index,
                                         const std::string&,
                                         orc::PreviewNavigationHint) override {
    orc::PreviewRenderResult result;
    result.node_id = node_id;
    result.output_type = output_type;
    result.output_index = output_index;
    result.image.width = 100;
    result.image.height = 50;
    result.image.rgb_data.assign(100 * 50 * 3, 128);

    orc::DropoutRegion source;
    source.line = 10;
    source.start_sample = 20;
    source.end_sample = 40;
    result.image.dropout_regions = {source};

    result.success = true;
    return result;
  }

  // Unused interface surface: inert stubs.
  std::vector<orc::AudioPairView> getAudioChannelPairs(orc::NodeID) override {
    return {};
  }
  std::shared_ptr<orc::presenters::IAudioStreamReader> createAudioStreamReader(
      orc::NodeID, size_t) override {
    return nullptr;
  }
  std::optional<orc::presenters::DropoutDisplaySeries> getDropoutAnalysisData(
      orc::NodeID) override {
    return std::nullopt;
  }
  std::optional<orc::presenters::SNRDisplaySeries> getSNRAnalysisData(
      orc::NodeID) override {
    return std::nullopt;
  }
  std::optional<orc::presenters::BurstLevelDisplaySeries>
  getBurstLevelAnalysisData(orc::NodeID) override {
    return std::nullopt;
  }
  std::optional<orc::CatalogueDataset> getCatalogueData(
      orc::NodeID, const std::string&,
      const std::optional<std::vector<std::string>>&) override {
    return std::nullopt;
  }
  LineSampleData getLineSamplesWithYC(orc::NodeID, orc::PreviewOutputType,
                                      uint64_t, int, int, int) override {
    return {};
  }
  std::optional<orc::SourceParameters> getVideoParameters(
      orc::NodeID) override {
    return std::nullopt;
  }
  LineSampleData getFieldSamplesForTiming(orc::NodeID, orc::PreviewOutputType,
                                          uint64_t) override {
    return {};
  }
  orc::FrameLineNavigationResult navigateFrameLine(orc::NodeID,
                                                   orc::PreviewOutputType,
                                                   uint64_t, int, int,
                                                   int) override {
    return {};
  }
  uint64_t triggerStage(orc::NodeID, TriggerProgressCallback) override {
    return 0;
  }
  uint64_t triggerStage(orc::NodeID, TriggerProgressCallback,
                        std::map<std::string, orc::ParameterValue>) override {
    return 0;
  }
  void cancelTrigger() override {}
  bool savePNG(orc::NodeID, orc::PreviewOutputType, uint64_t,
               const std::string&, const std::string&, double) override {
    return false;
  }
  orc::ImageToFieldMappingResult mapImageToField(orc::NodeID,
                                                 orc::PreviewOutputType,
                                                 uint64_t, int, int,
                                                 const std::string&) override {
    return {};
  }
  orc::FieldToImageMappingResult mapFieldToImage(orc::NodeID,
                                                 orc::PreviewOutputType,
                                                 uint64_t, uint64_t, int, int,
                                                 const std::string&) override {
    return {};
  }
  orc::FrameFieldsResult getFrameFields(orc::NodeID, uint64_t) override {
    return {};
  }
  std::vector<orc::PreviewViewDescriptor> getAvailablePreviewViews(
      orc::NodeID, orc::VideoDataType) override {
    return {};
  }
  std::vector<orc::VideoDataType> getStageDataTypes(orc::NodeID) override {
    return {};
  }
  orc::PreviewViewDataResult requestPreviewViewData(
      orc::NodeID, const std::string&, orc::VideoDataType,
      const orc::PreviewCoordinate&) override {
    return {};
  }
  orc::PreviewScopePayloads getPreviewScopes(
      orc::NodeID, const orc::PreviewScopeRequest&) override {
    return {};
  }
};

void sendMousePress(QWidget* widget, const QPointF& pos,
                    Qt::MouseButton button = Qt::LeftButton) {
  QMouseEvent event(QEvent::MouseButtonPress, pos,
                    widget->mapToGlobal(pos.toPoint()), button, button,
                    Qt::NoModifier);
  QCoreApplication::sendEvent(widget, &event);
}

void sendMouseMove(QWidget* widget, const QPointF& pos) {
  QMouseEvent event(QEvent::MouseMove, pos, widget->mapToGlobal(pos.toPoint()),
                    Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
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

void sendKey(QWidget* widget, Qt::Key key) {
  QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
  QCoreApplication::sendEvent(widget, &press);
  QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
  QCoreApplication::sendEvent(widget, &release);
}

QPushButton* buttonByText(QWidget& root, const QString& text) {
  const auto buttons = root.findChildren<QPushButton*>();
  for (QPushButton* button : buttons) {
    if (button->text() == text) {
      return button;
    }
  }
  return nullptr;
}

class DropoutEditorDialogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ensureApplication();
    mock_project_ = std::make_unique<
        ::testing::NiceMock<orc::presenters::test::MockProjectPresenter>>();
    dropout_presenter_ =
        std::make_unique<orc::presenters::DropoutPresenter>(*mock_project_);
    fake_render_ = std::make_shared<FakeRenderPresenter>();
  }

  std::unique_ptr<DropoutEditorDialog> makeDialog() {
    // The dialog is intentionally never show()n: the initial fit-to-viewport
    // only runs on visible dialogs, so the view stays at zoom 1.0 with
    // square aspect and widget coordinates equal image coordinates.
    return std::make_unique<DropoutEditorDialog>(
        orc::NodeID(2), dropout_presenter_.get(), fake_render_, orc::NodeID(1));
  }

  static DropoutFrameView* waitForLoadedView(DropoutEditorDialog& dialog) {
    auto* view = dialog.findChild<DropoutFrameView*>();
    if (!view) {
      return nullptr;
    }
    if (!QTest::qWaitFor([view]() { return view->hasImage(); }, 5000)) {
      return nullptr;
    }
    return view;
  }

  std::unique_ptr<
      ::testing::NiceMock<orc::presenters::test::MockProjectPresenter>>
      mock_project_;
  std::unique_ptr<orc::presenters::DropoutPresenter> dropout_presenter_;
  std::shared_ptr<FakeRenderPresenter> fake_render_;
};

TEST_F(DropoutEditorDialogTest, NullPresenters_Throw) {
  EXPECT_THROW(DropoutEditorDialog(orc::NodeID(2), nullptr, fake_render_,
                                   orc::NodeID(1)),
               std::invalid_argument);
  EXPECT_THROW(DropoutEditorDialog(orc::NodeID(2), dropout_presenter_.get(),
                                   nullptr, orc::NodeID(1)),
               std::invalid_argument);
}

TEST_F(DropoutEditorDialogTest, NoOutputs_ReportsErrorAndKeepsNavDisabled) {
  fake_render_->provide_outputs = false;
  auto dialog = makeDialog();

  auto* status_bar = dialog->findChild<QStatusBar*>();
  ASSERT_NE(status_bar, nullptr);
  ASSERT_TRUE(QTest::qWaitFor(
      [status_bar]() {
        return status_bar->currentMessage().contains("No frames");
      },
      5000));

  auto* slider = dialog->findChild<FrameMarkerSlider*>();
  ASSERT_NE(slider, nullptr);
  EXPECT_FALSE(slider->isEnabled());
}

TEST_F(DropoutEditorDialogTest, LoadsFirstFrameWithSourceDropout) {
  auto dialog = makeDialog();
  auto* view = waitForLoadedView(*dialog);
  ASSERT_NE(view, nullptr);

  EXPECT_EQ(view->size(), QSize(100, 50));  // zoom 1.0, identity mapping
  ASSERT_EQ(view->getSourceDropouts().size(), 1u);
  EXPECT_EQ(view->getSourceDropouts()[0].line, 10u);

  // The region table lists the source dropout.
  auto* table = dialog->findChild<QTableWidget*>();
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->rowCount(), 1);
  EXPECT_EQ(table->item(0, 0)->text(), "Source");
  EXPECT_EQ(table->item(0, 1)->text(), "11");  // 1-based line display
}

TEST_F(DropoutEditorDialogTest, EditRoundTrip_AddMoveNudgeRemoveUndoRedo) {
  auto dialog = makeDialog();
  auto* view = waitForLoadedView(*dialog);
  ASSERT_NE(view, nullptr);
  ASSERT_EQ(view->size(), QSize(100, 50));

  auto* undo_button =
      dialog->findChild<QPushButton*>("dropoutEditorUndoButton");
  auto* redo_button =
      dialog->findChild<QPushButton*>("dropoutEditorRedoButton");
  auto* delete_button =
      dialog->findChild<QPushButton*>("dropoutEditorDeleteButton");
  ASSERT_NE(undo_button, nullptr);
  ASSERT_NE(redo_button, nullptr);
  ASSERT_NE(delete_button, nullptr);

  // --- Add: drag on an empty area ---
  sendDrag(view, QPointF(10, 30), QPointF(40, 30));
  auto map = dialog->getDropoutMap();
  ASSERT_EQ(map.count(0), 1u);
  ASSERT_EQ(map[0].additions.size(), 1u);
  EXPECT_EQ(map[0].additions[0].line, 30u);
  EXPECT_EQ(map[0].additions[0].start_sample, 10u);
  EXPECT_EQ(map[0].additions[0].end_sample, 40u);

  // The new addition is selected and the frame gets an edited marker.
  EXPECT_EQ(view->selectedKind(), DropoutFrameView::RegionKind::Addition);
  auto* slider = dialog->findChild<FrameMarkerSlider*>();
  ASSERT_NE(slider, nullptr);
  EXPECT_EQ(slider->markedValues(), std::vector<int>{0});

  // --- Move: drag the addition body by +5 samples, +2 lines ---
  sendMousePress(view, QPointF(25, 30));
  sendMouseMove(view, QPointF(30, 32));
  sendMouseRelease(view, QPointF(30, 32));
  map = dialog->getDropoutMap();
  ASSERT_EQ(map[0].additions.size(), 1u);
  EXPECT_EQ(map[0].additions[0].line, 32u);
  EXPECT_EQ(map[0].additions[0].start_sample, 15u);
  EXPECT_EQ(map[0].additions[0].end_sample, 45u);

  // --- Nudge: two arrow-key nudges merge into a single undo step ---
  sendKey(dialog.get(), Qt::Key_Right);
  sendKey(dialog.get(), Qt::Key_Right);
  map = dialog->getDropoutMap();
  EXPECT_EQ(map[0].additions[0].start_sample, 17u);
  EXPECT_EQ(map[0].additions[0].end_sample, 47u);

  undo_button->click();  // Undoes BOTH nudges (merged command)
  map = dialog->getDropoutMap();
  EXPECT_EQ(map[0].additions[0].start_sample, 15u);
  EXPECT_EQ(map[0].additions[0].end_sample, 45u);

  // --- Toggle removal of the source dropout ---
  sendMousePress(view, QPointF(30, 10));
  sendMouseRelease(view, QPointF(30, 10));
  ASSERT_EQ(view->selectedKind(), DropoutFrameView::RegionKind::Source);
  EXPECT_EQ(delete_button->text(), "Mark Removed");
  delete_button->click();
  map = dialog->getDropoutMap();
  ASSERT_EQ(map[0].removals.size(), 1u);
  EXPECT_EQ(map[0].removals[0].line, 10u);
  EXPECT_EQ(map[0].removals[0].start_sample, 20u);
  EXPECT_EQ(map[0].removals[0].end_sample, 40u);

  // --- Undo everything back to a pristine map ---
  int undo_guard = 0;
  while (undo_button->isEnabled() && undo_guard++ < 16) {
    undo_button->click();
  }
  EXPECT_TRUE(dialog->getDropoutMap().empty());
  EXPECT_TRUE(slider->markedValues().empty());

  // --- Redo restores the first addition ---
  ASSERT_TRUE(redo_button->isEnabled());
  redo_button->click();
  map = dialog->getDropoutMap();
  ASSERT_EQ(map.count(0), 1u);
  ASSERT_EQ(map[0].additions.size(), 1u);
  EXPECT_EQ(map[0].additions[0].line, 30u);
  EXPECT_EQ(map[0].additions[0].start_sample, 10u);
  EXPECT_EQ(map[0].additions[0].end_sample, 40u);
}

TEST_F(DropoutEditorDialogTest, TableSelectionSyncsWithView) {
  auto dialog = makeDialog();
  auto* view = waitForLoadedView(*dialog);
  ASSERT_NE(view, nullptr);

  // Add one region so the table holds a source row and an added row.
  sendDrag(view, QPointF(10, 30), QPointF(40, 30));

  auto* table = dialog->findChild<QTableWidget*>();
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->rowCount(), 2);

  // Table -> view
  table->selectRow(0);
  EXPECT_EQ(view->selectedKind(), DropoutFrameView::RegionKind::Source);

  // View -> table (click the addition band)
  sendMousePress(view, QPointF(25, 30));
  sendMouseRelease(view, QPointF(25, 30));
  EXPECT_EQ(view->selectedKind(), DropoutFrameView::RegionKind::Addition);
  const auto selected_rows = table->selectionModel()->selectedRows();
  ASSERT_EQ(selected_rows.size(), 1);
  EXPECT_EQ(selected_rows.first().row(), 1);  // Addition row
}

TEST_F(DropoutEditorDialogTest, ResizeSourceDropout_ReplacesWithAddition) {
  auto dialog = makeDialog();
  auto* view = waitForLoadedView(*dialog);
  ASSERT_NE(view, nullptr);
  ASSERT_EQ(view->size(), QSize(100, 50));

  // Select the source dropout (line 10, samples [20, 40)).
  sendMousePress(view, QPointF(30, 10));
  sendMouseRelease(view, QPointF(30, 10));
  ASSERT_EQ(view->selectedKind(), DropoutFrameView::RegionKind::Source);

  // Drag its right handle from x=40 to x=60.
  sendMousePress(view, QPointF(40, 10.5));
  sendMouseMove(view, QPointF(60, 10.5));
  sendMouseRelease(view, QPointF(60, 10.5));

  // One command: the source dropout is marked removed and an addition
  // covering the extended extent is created.
  auto map = dialog->getDropoutMap();
  ASSERT_EQ(map.count(0), 1u);
  ASSERT_EQ(map[0].removals.size(), 1u);
  EXPECT_EQ(map[0].removals[0].line, 10u);
  EXPECT_EQ(map[0].removals[0].start_sample, 20u);
  EXPECT_EQ(map[0].removals[0].end_sample, 40u);
  ASSERT_EQ(map[0].additions.size(), 1u);
  EXPECT_EQ(map[0].additions[0].line, 10u);
  EXPECT_EQ(map[0].additions[0].start_sample, 20u);
  EXPECT_EQ(map[0].additions[0].end_sample, 61u);

  // The replacement addition is selected for immediate further editing.
  EXPECT_EQ(view->selectedKind(), DropoutFrameView::RegionKind::Addition);

  // A single undo restores the pristine map.
  auto* undo_button =
      dialog->findChild<QPushButton*>("dropoutEditorUndoButton");
  ASSERT_NE(undo_button, nullptr);
  undo_button->click();
  EXPECT_TRUE(dialog->getDropoutMap().empty());
}

TEST_F(DropoutEditorDialogTest, MinimumSize_FitsSmallScreens) {
  auto dialog = makeDialog();
  auto* view = waitForLoadedView(*dialog);
  ASSERT_NE(view, nullptr);

  // The layout-driven minimum must not exceed the preview dialog's default
  // size (800x700), so the editor fits on small screens.
  const QSize min_hint = dialog->minimumSizeHint();
  EXPECT_LE(min_hint.width(), 800) << "minimum width " << min_hint.width();
  EXPECT_LE(min_hint.height(), 700) << "minimum height " << min_hint.height();
}

TEST_F(DropoutEditorDialogTest, ClearFrameEdits_IsUndoable) {
  auto dialog = makeDialog();
  auto* view = waitForLoadedView(*dialog);
  ASSERT_NE(view, nullptr);

  sendDrag(view, QPointF(10, 30), QPointF(40, 30));
  ASSERT_EQ(dialog->getDropoutMap().count(0), 1u);

  auto* clear_button = buttonByText(*dialog, "Clear Frame Edits");
  ASSERT_NE(clear_button, nullptr);
  clear_button->click();
  EXPECT_TRUE(dialog->getDropoutMap().empty());

  auto* undo_button =
      dialog->findChild<QPushButton*>("dropoutEditorUndoButton");
  ASSERT_NE(undo_button, nullptr);
  undo_button->click();
  EXPECT_EQ(dialog->getDropoutMap().count(0), 1u);
}

}  // namespace
}  // namespace gui_unit_test
