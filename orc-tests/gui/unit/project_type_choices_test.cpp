/*
 * File:        project_type_choices_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     The project types the New Project dialog offers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "project_type_choices.h"

#include <gtest/gtest.h>

#include <set>
#include <utility>

namespace gui_unit_test {
namespace {

using orc::SourceType;
using orc::VideoSystem;
using orc::gui::kProjectTypeChoices;
using orc::gui::projectTypeChoiceForLabel;

}  // namespace

// Every system a project can have is offered as composite and as Y/C, once
// each, and Unknown is never a project type.
TEST(ProjectTypeChoicesTest, EverySystemOnceAsCompositeAndYc) {
  std::set<std::pair<VideoSystem, SourceType>> seen;
  for (const auto& choice : kProjectTypeChoices) {
    EXPECT_NE(choice.system, VideoSystem::Unknown) << choice.label;
    EXPECT_NE(choice.source, SourceType::Unknown) << choice.label;
    EXPECT_TRUE(seen.insert({choice.system, choice.source}).second)
        << choice.label;
  }
  for (const auto system : {VideoSystem::NTSC, VideoSystem::PAL,
                            VideoSystem::PAL_M, VideoSystem::SECAM}) {
    EXPECT_EQ(seen.count({system, SourceType::Composite}), 1u);
    EXPECT_EQ(seen.count({system, SourceType::YC}), 1u);
  }
}

TEST(ProjectTypeChoicesTest, SecamLabelsSelectSecam) {
  const auto composite = projectTypeChoiceForLabel("SECAM Composite");
  ASSERT_TRUE(composite.has_value());
  EXPECT_EQ(composite->system, VideoSystem::SECAM);
  EXPECT_EQ(composite->source, SourceType::Composite);

  const auto yc = projectTypeChoiceForLabel("SECAM YC");
  ASSERT_TRUE(yc.has_value());
  EXPECT_EQ(yc->system, VideoSystem::SECAM);
  EXPECT_EQ(yc->source, SourceType::YC);
}

// Each label round-trips to its own entry, so the dialog's text is all it
// needs to remember.
TEST(ProjectTypeChoicesTest, EveryLabelRoundTrips) {
  for (const auto& choice : kProjectTypeChoices) {
    const auto found = projectTypeChoiceForLabel(choice.label);
    ASSERT_TRUE(found.has_value()) << choice.label;
    EXPECT_EQ(found->system, choice.system) << choice.label;
    EXPECT_EQ(found->source, choice.source) << choice.label;
  }
  EXPECT_FALSE(projectTypeChoiceForLabel("MESECAM Composite").has_value());
  EXPECT_FALSE(projectTypeChoiceForLabel("").has_value());
}

}  // namespace gui_unit_test
