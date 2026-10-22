/*
 * File:        analysis_dialog_refresh_option_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Offscreen tests for the stage tool dialog's "Use this tool
 *              when refreshing from the CLI" option
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <memory>
#include <string>
#include <variant>

#include "generic_analysis_dialog.h"
#include "presenters/include/analysis_presenter.h"
#include "presenters/include/project_presenter.h"
#include "presenters/include/tool_refresh.h"

namespace gui_unit_test {

namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-analysis-dialog-refresh-option-test";
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

const QString kRefreshOption = "Use this tool when refreshing from the CLI";

/// An in-memory project holding one stage. The GUI tests load no stage
/// plugins, so the stage is one built into core; the dialog stores the
/// option on whatever node it was opened for, so the Frame Map tools can be
/// driven against it.
struct OneStageProject {
  orc::presenters::ProjectPresenter presenter;
  orc::NodeID stage;

  OneStageProject() {
    presenter.setProjectName("refresh-option");
    presenter.setVideoFormat(orc::presenters::VideoFormat::PAL);
    stage = presenter.addNode("dropout_map", 0.0, 0.0);
  }

  void* handle() { return presenter.getCoreProjectHandle(); }

  /// The tool dialog for @p tool_id on the stage; it owns the analysis
  /// presenter it is given.
  std::unique_ptr<orc::gui::GenericAnalysisDialog> dialog(
      const std::string& tool_id) {
    auto* analysis = new orc::presenters::AnalysisPresenter(handle());
    const auto info = analysis->getToolInfo(tool_id);
    return std::make_unique<orc::gui::GenericAnalysisDialog>(
        tool_id, info, analysis, stage, handle());
  }
};

QCheckBox* refreshOption(QWidget* dialog) {
  for (auto* box : dialog->findChildren<QCheckBox*>()) {
    if (box->text() == kRefreshOption) {
      return box;
    }
  }
  return nullptr;
}

}  // namespace

TEST(AnalysisDialogRefreshOptionTest, TickingItStoresTheToolOnTheStage) {
  (void)ensureApplication();
  OneStageProject project;

  auto dialog = project.dialog("field_mapping");
  int changes = 0;
  QObject::connect(dialog.get(),
                   &orc::gui::GenericAnalysisDialog::refreshConfigChanged,
                   [&changes] { ++changes; });
  auto* option = refreshOption(dialog.get());
  ASSERT_NE(option, nullptr);
  EXPECT_FALSE(option->isChecked());

  option->setChecked(true);
  dialog->accept();

  const auto stored =
      orc::presenters::getNodeToolRefresh(project.handle(), project.stage);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->tool_id, "field_mapping");
  // The dialog's parameters are stored with it
  EXPECT_EQ(stored->parameters.count("include_lead_in_out"), 1u);
  EXPECT_EQ(changes, 1);
  EXPECT_TRUE(project.presenter.isModified());
}

TEST(AnalysisDialogRefreshOptionTest, ItRoundTripsThroughTheDialog) {
  (void)ensureApplication();
  OneStageProject project;
  orc::presenters::setNodeToolRefresh(
      project.handle(), project.stage,
      orc::presenters::ToolRefreshConfig{"field_mapping",
                                         {{"include_lead_in_out", true}}});

  auto dialog = project.dialog("field_mapping");
  auto* option = refreshOption(dialog.get());
  ASSERT_NE(option, nullptr);
  EXPECT_TRUE(option->isChecked());

  // Accepting unchanged keeps the stored parameters
  dialog->accept();
  auto stored =
      orc::presenters::getNodeToolRefresh(project.handle(), project.stage);
  ASSERT_TRUE(stored.has_value());
  EXPECT_TRUE(std::get<bool>(stored->parameters.at("include_lead_in_out")));

  // Unticking it clears the stage's refresh tool
  auto second = project.dialog("field_mapping");
  auto* second_option = refreshOption(second.get());
  ASSERT_NE(second_option, nullptr);
  second_option->setChecked(false);
  second->accept();
  EXPECT_FALSE(
      orc::presenters::getNodeToolRefresh(project.handle(), project.stage)
          .has_value());
}

TEST(AnalysisDialogRefreshOptionTest, AnotherToolsDialogLeavesItAlone) {
  (void)ensureApplication();
  OneStageProject project;
  const orc::presenters::ToolRefreshConfig config{"field_mapping", {}};
  orc::presenters::setNodeToolRefresh(project.handle(), project.stage, config);

  // The range finder's option starts unticked, and leaving it so does not
  // clear the Disc Mapper's refresh
  auto dialog = project.dialog("frame_map_range");
  auto* option = refreshOption(dialog.get());
  ASSERT_NE(option, nullptr);
  EXPECT_FALSE(option->isChecked());
  dialog->accept();

  EXPECT_EQ(
      orc::presenters::getNodeToolRefresh(project.handle(), project.stage),
      config);
}

}  // namespace gui_unit_test
