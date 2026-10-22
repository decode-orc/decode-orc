/*
 * File:        analysis_tool_presenter_factory_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     Unit tests for the tool id to presenter selection shared by
 *              the GUI's analysis dialog and orc-cli
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/presenters/include/analysis_tool_presenter_factory.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include "../../../../orc/core/include/project.h"
#include "../../../../orc/presenters/include/disc_mapper_presenter.h"
#include "../../../../orc/presenters/include/dropout_editor_presenter.h"
#include "../../../../orc/presenters/include/ffmpeg_preset_presenter.h"
#include "../../../../orc/presenters/include/frame_corruption_presenter.h"
#include "../../../../orc/presenters/include/frame_map_range_presenter.h"
#include "../../../../orc/presenters/include/mask_line_presenter.h"
#include "../../../../orc/presenters/include/source_alignment_presenter.h"

namespace orc::presenters::test {

namespace {

orc::Project make_project() {
  return orc::project_io::create_empty_project(
      "factory-test", orc::VideoSystem::PAL, orc::SourceType::Composite);
}

template <typename Expected>
bool makes(const std::string& tool_id, orc::Project* project) {
  const auto presenter = makeAnalysisToolPresenter(tool_id, project);
  return dynamic_cast<Expected*>(presenter.get()) != nullptr;
}

}  // namespace

TEST(AnalysisToolPresenterFactoryTest, PicksTheDedicatedPresenter) {
  auto project = make_project();

  EXPECT_TRUE(makes<DiscMapperPresenter>("field_mapping", &project));
  EXPECT_TRUE(makes<DiscMapperPresenter>("disc_mapper", &project));
  EXPECT_TRUE(makes<FrameMapRangePresenter>("frame_map_range", &project));
  EXPECT_TRUE(makes<FrameCorruptionPresenter>("frame_corruption", &project));
  EXPECT_TRUE(makes<SourceAlignmentPresenter>("source_alignment", &project));
  EXPECT_TRUE(makes<MaskLinePresenter>("mask_line_config", &project));
  EXPECT_TRUE(makes<FFmpegPresetPresenter>("ffmpeg_preset_config", &project));
  EXPECT_TRUE(makes<DropoutEditorPresenter>("dropout_editor", &project));
}

TEST(AnalysisToolPresenterFactoryTest, FallsBackToTheGenericPresenter) {
  auto project = make_project();

  const auto presenter = makeAnalysisToolPresenter("snr_analysis", &project);
  ASSERT_NE(presenter, nullptr);
  EXPECT_EQ(dynamic_cast<DiscMapperPresenter*>(presenter.get()), nullptr);
  EXPECT_EQ(dynamic_cast<SourceAlignmentPresenter*>(presenter.get()), nullptr);
  EXPECT_FALSE(hasDedicatedAnalysisToolPresenter("snr_analysis"));
  EXPECT_TRUE(hasDedicatedAnalysisToolPresenter("field_mapping"));
}

TEST(AnalysisToolPresenterFactoryTest, GenericPresenterReportsAnUnknownTool) {
  auto project = make_project();

  const auto presenter =
      makeAnalysisToolPresenter("no_such_tool_registered", &project);
  const auto result =
      presenter->runAnalysis(orc::NodeID(1), {}, nullptr, nullptr);

  EXPECT_EQ(result.status, orc::AnalysisResult::Status::Failed);
  EXPECT_NE(result.summary.find("no_such_tool_registered"), std::string::npos);
}

TEST(AnalysisToolPresenterFactoryTest, RejectsANullProject) {
  EXPECT_THROW(makeAnalysisToolPresenter("field_mapping", nullptr),
               std::invalid_argument);
  EXPECT_THROW(makeAnalysisToolPresenter("snr_analysis", nullptr),
               std::invalid_argument);
}

}  // namespace orc::presenters::test
