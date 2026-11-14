/*
 * File:        analysis_tool_presenter_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     Running a stage's analysis tool and applying its result, as
 *              orc-cli and a project refresh do
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/presenters/include/analysis_tool_presenter.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>

#include "../../../../orc/core/include/project.h"

namespace orc::presenters::test {

namespace {

orc::Project make_project() {
  return orc::project_io::create_empty_project(
      "presenter-test", orc::VideoSystem::PAL, orc::SourceType::Composite);
}

}  // namespace

TEST(AnalysisToolPresenterTest, ReportsAToolTheNodeDoesNotHave) {
  auto project = make_project();

  AnalysisToolPresenter presenter("no_such_tool", &project);
  const auto result =
      presenter.runAnalysis(orc::NodeID(1), {}, nullptr, nullptr);

  EXPECT_EQ(result.status, orc::AnalysisResult::Status::Failed);
  EXPECT_FALSE(result.summary.empty());
}

TEST(AnalysisToolPresenterTest, RejectsANullProject) {
  EXPECT_THROW(AnalysisToolPresenter("field_mapping", nullptr),
               std::invalid_argument);
}

namespace {

// A Frame Map whose padding settings differ from the defaults, so a lost
// setting shows.
constexpr const char* kFrameMapProject = R"yaml(
project:
  name: apply-merge
  version: "2.0"
  video_format: PAL
  source_format: Composite
  amplitude_unit: mV
dag:
  nodes:
    - id: 2
      stage: frame_map
      node_type: TRANSFORM
      display_name: Frame Map
      user_label: Frame Map
      x: 0
      y: 0
      parameters:
        pad_gaps:
          type: bool
          value: false
        ranges:
          type: string
          value: "0-9"
  edges: []
)yaml";

/// What the Frame Map Range Finder hands Apply after a successful run.
orc::AnalysisResult range_result(const std::string& spec) {
  orc::AnalysisResult result;
  result.status = orc::AnalysisResult::Status::Success;
  result.parameterChanges["ranges"] = spec;
  return result;
}

std::map<std::string, orc::ParameterValue> stored_parameters(
    const orc::Project& project) {
  return project.get_nodes().front().parameters;
}

}  // namespace

TEST(AnalysisToolPresenterTest, PrepareReportsChangesWithoutWriting) {
  auto project =
      orc::project_io::load_project_from_yaml(kFrameMapProject, "test.orcprj");
  AnalysisToolPresenter presenter("frame_map_range", &project);

  std::map<std::string, orc::ParameterValue> changes;
  ASSERT_TRUE(presenter.prepareResultChanges(range_result("100-199"),
                                             orc::NodeID(2), &changes));

  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(std::get<std::string>(changes.at("ranges")), "100-199");
  EXPECT_EQ(std::get<std::string>(stored_parameters(project).at("ranges")),
            "0-9");
}

TEST(AnalysisToolPresenterTest, ApplyKeepsTheNodesOtherParameters) {
  auto project =
      orc::project_io::load_project_from_yaml(kFrameMapProject, "test.orcprj");
  AnalysisToolPresenter presenter("frame_map_range", &project);

  ASSERT_TRUE(
      presenter.applyResultToGraph(range_result("100-199"), orc::NodeID(2)));

  const auto stored = stored_parameters(project);
  EXPECT_EQ(std::get<std::string>(stored.at("ranges")), "100-199");
  ASSERT_EQ(stored.count("pad_gaps"), 1u);
  EXPECT_FALSE(std::get<bool>(stored.at("pad_gaps")));
}

TEST(AnalysisToolPresenterTest, AFailedResultHasNothingToApply) {
  auto project =
      orc::project_io::load_project_from_yaml(kFrameMapProject, "test.orcprj");
  AnalysisToolPresenter presenter("frame_map_range", &project);

  auto failed = range_result("100-199");
  failed.status = orc::AnalysisResult::Status::Failed;
  EXPECT_FALSE(presenter.prepareResultChanges(failed, orc::NodeID(2), nullptr));
  EXPECT_EQ(std::get<std::string>(stored_parameters(project).at("ranges")),
            "0-9");
}

// A capture with no readable picture numbers maps no frame, and the Disc
// Mapper then proposes no ranges. Applying would empty the Frame Map's
// ranges and let the disc through unmapped, so a script would stack it
// silently: there is nothing to apply.
TEST(AnalysisToolPresenterTest, ADiscMapperResultMappingNoFrameIsRefused) {
  auto project =
      orc::project_io::load_project_from_yaml(kFrameMapProject, "test.orcprj");
  AnalysisToolPresenter presenter("field_mapping", &project);

  orc::AnalysisResult unmapped;
  unmapped.status = orc::AnalysisResult::Status::Success;
  std::map<std::string, orc::ParameterValue> changes;
  EXPECT_FALSE(
      presenter.prepareResultChanges(unmapped, orc::NodeID(2), &changes));
  EXPECT_FALSE(presenter.applyResultToGraph(unmapped, orc::NodeID(2)));
  EXPECT_EQ(std::get<std::string>(stored_parameters(project).at("ranges")),
            "0-9");

  orc::AnalysisResult mapped = unmapped;
  mapped.parameterChanges["ranges"] = std::string("0-4,PAD_2,7-9");
  ASSERT_TRUE(presenter.prepareResultChanges(mapped, orc::NodeID(2), &changes));
  EXPECT_EQ(std::get<std::string>(changes.at("ranges")), "0-4,PAD_2,7-9");
}

}  // namespace orc::presenters::test
