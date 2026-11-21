/*
 * File:        dropout_editor_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     The Dropout Editor (orc-view_core) through a fake tool
 *              session: frames, the edit round-trip, undo/redo, and what
 *              Apply and OK write
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "dropout_editor_dialog.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QStatusBar>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../../../orc/plugins/stages/common/dropout-map/dropout_map_text.h"
#include "mocks/no_analysis_session.h"

namespace gui_unit_test {

namespace {

/// The frame the fake session serves, and the size the tests give
/// the view so widget and image coordinates coincide.
constexpr QSize kFrameSize(100, 50);

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
 * A tool session serving synthetic 100x50 frames with one source dropout on
 * line 10, samples [20, 40). Replies arrive on a later turn of the event
 * loop, as the host's do, never inside the call that asked.
 */
class FakeSession final : public gui_unit_test::NoAnalysisSession {
 public:
  std::string contract_id() const override {
    return orc::dropout_map_text::kDropoutEditorContractId;
  }
  std::string title() const override {
    return "Dropout Editor - Dropout Map (node_2)";
  }
  bool is_open() const override { return open; }

  void request(const std::string&, std::vector<uint8_t>,
               orc::view::ToolReplyHandler) override {}
  void set_results_changed_handler(std::function<void()>) override {}
  std::shared_ptr<orc::view::IToolJob> run_job(
      const std::string&, std::vector<uint8_t>, orc::view::ToolProgressHandler,
      orc::view::ToolReplyHandler) override {
    return nullptr;
  }

  void apply_parameters(orc::view::ToolParameterMap values,
                        const std::string& label,
                        orc::view::ToolReplyHandler on_done) override {
    applied.push_back(values);
    apply_labels.push_back(label);
    applies.push_back(std::move(on_done));
  }

  void show_frame(uint64_t) override {}
  void set_preview_frame_handler(std::function<void(uint64_t)>) override {}

  void render_frame(uint64_t frame_index,
                    const orc::view::ToolFrameRenderOptions& options,
                    orc::view::ToolFrameHandler on_image) override {
    rendered.push_back(frame_index);
    from_input.push_back(options.from_input);
    orc::view::ToolFrameImage image;
    image.frame_index = frame_index;
    image.frame_count = frame_count;
    if (frame_count == 0) {
      image.error = "The node has no frames to render";
    } else {
      image.ok = true;
      image.width = static_cast<uint32_t>(kFrameSize.width());
      image.height = static_cast<uint32_t>(kFrameSize.height());
      image.rgb.assign(static_cast<size_t>(kFrameSize.width()) *
                           static_cast<size_t>(kFrameSize.height()) * 3,
                       128);
      image.dar_aspect_correction = 1.0;  // Identity mapping for tests
      image.dropouts = {orc::view::ToolFrameDropout{10, 20, 40}};
    }
    QTimer::singleShot(0, [on_image = std::move(on_image),
                           image = std::move(image)]() mutable {
      on_image(std::move(image));
    });
  }

  void set_graph_rebuilt_handler(std::function<void()> handler) override {
    graph_rebuilt = std::move(handler);
  }
  orc::view::AmplitudeUnit amplitude_unit() const override {
    return orc::view::AmplitudeUnit::IRE;
  }
  void set_amplitude_unit_handler(
      std::function<void(orc::view::AmplitudeUnit)>) override {}
  orc::view::ToolParameterMap parameters() const override { return stored; }
  std::string project_directory() const override { return {}; }

  static orc::view::ToolReply ok() {
    orc::view::ToolReply reply;
    reply.ok = true;
    return reply;
  }
  static orc::view::ToolReply refusal(const std::string& why) {
    orc::view::ToolReply reply;
    reply.error = why;
    return reply;
  }

  bool open = true;
  uint64_t frame_count = 5;
  orc::view::ToolParameterMap stored;
  std::vector<uint64_t> rendered;
  std::vector<bool> from_input;
  std::vector<orc::view::ToolParameterMap> applied;
  std::vector<std::string> apply_labels;
  std::vector<orc::view::ToolReplyHandler> applies;
  std::function<void()> graph_rebuilt;
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
    session_ = std::make_shared<FakeSession>();
  }

  std::unique_ptr<DropoutEditorDialog> makeDialog() {
    // The dialog is intentionally never show()n: the initial fit-to-viewport
    // only runs on visible dialogs, so the view stays at zoom 1.0 with
    // square aspect and widget coordinates equal image coordinates.
    return std::make_unique<DropoutEditorDialog>(session_);
  }

  static QPushButton* standardButton(DropoutEditorDialog& dialog,
                                     QDialogButtonBox::StandardButton which) {
    auto* box = dialog.findChild<QDialogButtonBox*>();
    return box ? box->button(which) : nullptr;
  }

  static DropoutFrameView* waitForLoadedView(DropoutEditorDialog& dialog) {
    auto* view = dialog.findChild<DropoutFrameView*>();
    if (!view) {
      return nullptr;
    }
    // The view stays its own size and pans rather than growing to the zoomed
    // frame, so the test sizes it to the frame it is about to load: at zoom
    // 1.0 that makes widget coordinates equal image coordinates, which every
    // coordinate below assumes. Sized before the frame arrives because a
    // hidden widget's resize event is deferred until it is shown, and the
    // view takes its viewport size when the frame is set.
    view->setFixedSize(kFrameSize);
    if (!QTest::qWaitFor([view]() { return view->hasImage(); }, 5000)) {
      return nullptr;
    }
    return view;
  }

  /// True when widget coordinates and image coordinates coincide.
  static bool mapsOneToOne(DropoutFrameView& view) {
    return view.widgetFromImage(QPointF(0, 0)) == QPointF(0, 0) &&
           view.widgetFromImage(
               QPointF(view.imageSize().width(), view.imageSize().height())) ==
               QPointF(view.imageSize().width(), view.imageSize().height());
  }

  std::shared_ptr<FakeSession> session_;
};

TEST_F(DropoutEditorDialogTest, NullSession_Throws) {
  EXPECT_THROW(DropoutEditorDialog(nullptr), std::invalid_argument);
}

TEST_F(DropoutEditorDialogTest, NoOutputs_ReportsErrorAndKeepsNavDisabled) {
  session_->frame_count = 0;
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

  EXPECT_EQ(view->imageSize(), QSize(100, 50));
  EXPECT_TRUE(mapsOneToOne(*view));  // zoom 1.0, identity mapping
  ASSERT_EQ(view->getSourceDropouts().size(), 1u);
  EXPECT_EQ(view->getSourceDropouts()[0].line, 10u);

  // The region table lists the source dropout.
  auto* table = dialog->findChild<QTableWidget*>();
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->rowCount(), 1);
  EXPECT_EQ(table->item(0, 0)->text(), "Source");
  EXPECT_EQ(table->item(0, 1)->text(), "11");  // 1-based line display

  // Frames come from the node's input, before the stage's own edits.
  ASSERT_FALSE(session_->from_input.empty());
  EXPECT_TRUE(session_->from_input.front());
}

TEST_F(DropoutEditorDialogTest, EditRoundTrip_AddMoveNudgeRemoveUndoRedo) {
  auto dialog = makeDialog();
  auto* view = waitForLoadedView(*dialog);
  ASSERT_NE(view, nullptr);
  ASSERT_TRUE(mapsOneToOne(*view));

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
  ASSERT_TRUE(mapsOneToOne(*view));

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

TEST_F(DropoutEditorDialogTest, LoadsTheStoredMap) {
  session_->stored = {
      {"dropout_map", "[{frame:2,add:[{line:5,start:1,end:9}]}]"},
      {"other", "ignored"}};
  auto dialog = makeDialog();
  auto* view = waitForLoadedView(*dialog);
  ASSERT_NE(view, nullptr);

  const auto& map = dialog->getDropoutMap();
  ASSERT_EQ(map.size(), 1u);
  ASSERT_EQ(map.at(2).additions.size(), 1u);
  EXPECT_EQ(map.at(2).additions[0].line, 5u);
  auto* slider = dialog->findChild<FrameMarkerSlider*>();
  ASSERT_NE(slider, nullptr);
  EXPECT_EQ(slider->markedValues(), std::vector<int>{2});
}

TEST_F(DropoutEditorDialogTest, ApplyWritesTheMapAndStaysOpen) {
  auto dialog = makeDialog();
  auto* view = waitForLoadedView(*dialog);
  ASSERT_NE(view, nullptr);
  sendDrag(view, QPointF(10, 30), QPointF(40, 30));

  auto* apply = standardButton(*dialog, QDialogButtonBox::Apply);
  ASSERT_NE(apply, nullptr);
  apply->click();

  ASSERT_EQ(session_->applied.size(), 1u);
  EXPECT_EQ(
      session_->applied[0],
      (orc::view::ToolParameterMap{
          {"dropout_map", "[{frame:0,add:[{line:30,start:10,end:40}]}]"}}));
  EXPECT_EQ(session_->apply_labels[0], "Edited dropout map");
  EXPECT_FALSE(apply->isEnabled());  // Until the host answers

  // The host rebuilds the graph, then answers.
  ASSERT_TRUE(static_cast<bool>(session_->graph_rebuilt));
  const size_t renders_before = session_->rendered.size();
  session_->graph_rebuilt();
  session_->applies[0](FakeSession::ok());
  EXPECT_TRUE(apply->isEnabled());
  EXPECT_EQ(dialog->result(), 0);

  // The frame is drawn again from the new graph, and the edits survive.
  ASSERT_TRUE(QTest::qWaitFor(
      [&]() { return session_->rendered.size() > renders_before; }, 5000));
  EXPECT_EQ(session_->rendered.back(), 0u);
  EXPECT_EQ(dialog->getDropoutMap().count(0), 1u);
}

TEST_F(DropoutEditorDialogTest, OkWritesTheMapAndCloses) {
  auto dialog = makeDialog();
  auto* view = waitForLoadedView(*dialog);
  ASSERT_NE(view, nullptr);
  dialog->show();
  sendDrag(view, QPointF(10, 30), QPointF(40, 30));

  auto* ok = standardButton(*dialog, QDialogButtonBox::Ok);
  ASSERT_NE(ok, nullptr);
  ok->click();
  ASSERT_EQ(session_->applies.size(), 1u);
  EXPECT_TRUE(dialog->isVisible());  // Not until the host has written it

  session_->applies[0](FakeSession::ok());
  EXPECT_FALSE(dialog->isVisible());
  EXPECT_EQ(dialog->result(), QDialog::Accepted);
}

TEST_F(DropoutEditorDialogTest, ARefusedApplyKeepsTheEditsAndTheWindow) {
  auto dialog = makeDialog();
  auto* view = waitForLoadedView(*dialog);
  ASSERT_NE(view, nullptr);
  dialog->show();
  sendDrag(view, QPointF(10, 30), QPointF(40, 30));

  auto* ok = standardButton(*dialog, QDialogButtonBox::Ok);
  ASSERT_NE(ok, nullptr);
  ok->click();
  ASSERT_EQ(session_->applies.size(), 1u);

  // The warning the dialog raises is closed again at once.
  QTimer::singleShot(0, []() {
    for (auto* widget : QApplication::topLevelWidgets()) {
      if (widget->isVisible() && widget->inherits("QMessageBox")) {
        qobject_cast<QDialog*>(widget)->reject();
      }
    }
  });
  session_->applies[0](FakeSession::refusal("The project is closing"));

  EXPECT_TRUE(dialog->isVisible());
  EXPECT_TRUE(ok->isEnabled());
  EXPECT_EQ(dialog->getDropoutMap().count(0), 1u);
}

TEST_F(DropoutEditorDialogTest, AClosedSessionWritesNothing) {
  auto dialog = makeDialog();
  auto* view = waitForLoadedView(*dialog);
  ASSERT_NE(view, nullptr);
  dialog->show();

  session_->open = false;
  auto* apply = standardButton(*dialog, QDialogButtonBox::Apply);
  ASSERT_NE(apply, nullptr);
  apply->click();
  EXPECT_TRUE(session_->applied.empty());
  EXPECT_FALSE(dialog->isVisible());
}

}  // namespace
}  // namespace gui_unit_test
