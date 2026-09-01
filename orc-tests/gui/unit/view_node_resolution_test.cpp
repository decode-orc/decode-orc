/*
 * File:        view_node_resolution_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 (gui-logic) tests for the post-rebuild viewed-node
 *              decision
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "view_node_resolution.h"

#include <gtest/gtest.h>

namespace orc::gui {
namespace {

// The id MainWindow views while a project has no source. Negative, so it is
// never a node the project can resolve.
constexpr NodeID kNoSourcePlaceholder{-999};

TEST(ViewNodeResolution, KeepsTheViewedNodeWhenTheRebuildLeftItInPlace) {
  const auto decision = resolveViewNodeAfterRebuild(
      NodeID(2), /*current_exists=*/true, NodeID(1));

  EXPECT_EQ(decision.action, ViewNodeAction::Keep);
  EXPECT_EQ(decision.target, NodeID(2));
}

TEST(ViewNodeResolution, SwitchesAwayFromANodeTheRebuildDeleted) {
  // The regression this helper exists for: deleting the node being viewed
  // leaves an id that is still is_valid() (it is >= 0), so a check written in
  // terms of validity alone concluded there was nothing to do and left the
  // preview addressing a node the DAG no longer had.
  const auto decision = resolveViewNodeAfterRebuild(
      NodeID(3), /*current_exists=*/false, NodeID(1));

  EXPECT_EQ(decision.action, ViewNodeAction::Switch);
  EXPECT_EQ(decision.target, NodeID(1));
}

TEST(ViewNodeResolution, SwitchesToTheFirstNodeWhenNothingWasViewedYet) {
  const auto decision = resolveViewNodeAfterRebuild(
      NodeID(), /*current_exists=*/false, NodeID(1));

  EXPECT_EQ(decision.action, ViewNodeAction::Switch);
  EXPECT_EQ(decision.target, NodeID(1));
}

TEST(ViewNodeResolution, LeavesThePlaceholderAsSoonAsARealNodeExists) {
  const auto decision = resolveViewNodeAfterRebuild(
      kNoSourcePlaceholder, /*current_exists=*/false, NodeID(1));

  EXPECT_EQ(decision.action, ViewNodeAction::Switch);
  EXPECT_EQ(decision.target, NodeID(1));
}

TEST(ViewNodeResolution, ClearsWhenTheRebuiltProjectHasNoNodesAtAll) {
  // Deleting the last node: there is nothing to fall back to, and continuing
  // to name the deleted node would send every later render at a dead id.
  const auto decision = resolveViewNodeAfterRebuild(
      NodeID(3), /*current_exists=*/false, NodeID());

  EXPECT_EQ(decision.action, ViewNodeAction::Clear);
  EXPECT_FALSE(decision.target.is_valid());
}

TEST(ViewNodeResolution, ClearsAnEmptyProjectRatherThanKeepingThePlaceholder) {
  const auto decision = resolveViewNodeAfterRebuild(
      kNoSourcePlaceholder, /*current_exists=*/false, NodeID());

  EXPECT_EQ(decision.action, ViewNodeAction::Clear);
  EXPECT_FALSE(decision.target.is_valid());
}

TEST(ViewNodeResolution, NeverKeepsAnIdTheProjectCannotResolve) {
  // Whatever the id, "exists" is the only thing that licenses Keep.
  for (const NodeID id :
       {NodeID(), kNoSourcePlaceholder, NodeID(0), NodeID(7)}) {
    EXPECT_NE(
        resolveViewNodeAfterRebuild(id, /*current_exists=*/false, NodeID(1))
            .action,
        ViewNodeAction::Keep)
        << "id " << id.to_string();
  }
}

}  // namespace
}  // namespace orc::gui
