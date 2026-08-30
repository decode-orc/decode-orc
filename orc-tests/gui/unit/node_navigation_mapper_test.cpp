/*
 * File:        node_navigation_mapper_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Unit tests for DAG canvas keyboard navigation mapping
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "node_navigation_mapper.h"

#include <gtest/gtest.h>

#include <vector>

namespace gui_unit_test {

using orc::gui::findAdjacentNode;
using orc::gui::findCycledNode;
using orc::gui::findNodeNearestPoint;
using orc::gui::NavigationDirection;
using orc::gui::NodeBounds;

namespace {

// Node size the graph model reports for every stage node.
constexpr double kNodeWidth = 140.0;
constexpr double kNodeHeight = 80.0;

NodeBounds node(std::uint64_t id, double x, double y) {
  return NodeBounds{id, x, y, kNodeWidth, kNodeHeight};
}

// A straight left-to-right pipeline: source -> transform -> sink.
std::vector<NodeBounds> chain() {
  return {node(1, 0.0, 0.0), node(2, 300.0, 0.0), node(3, 600.0, 0.0)};
}

}  // namespace

TEST(NodeNavigationMapperTest, MovesRightAlongChain) {
  const auto nodes = chain();

  EXPECT_EQ(findAdjacentNode(nodes, 1, NavigationDirection::Right), 2u);
  EXPECT_EQ(findAdjacentNode(nodes, 2, NavigationDirection::Right), 3u);
}

TEST(NodeNavigationMapperTest, MovesLeftAlongChain) {
  const auto nodes = chain();

  EXPECT_EQ(findAdjacentNode(nodes, 3, NavigationDirection::Left), 2u);
  EXPECT_EQ(findAdjacentNode(nodes, 2, NavigationDirection::Left), 1u);
}

TEST(NodeNavigationMapperTest, StopsAtTheEndOfTheChain) {
  const auto nodes = chain();

  EXPECT_FALSE(findAdjacentNode(nodes, 3, NavigationDirection::Right));
  EXPECT_FALSE(findAdjacentNode(nodes, 1, NavigationDirection::Left));
}

TEST(NodeNavigationMapperTest, VerticalMovesPickParallelBranches) {
  // One source feeding two branches stacked above and below it.
  const std::vector<NodeBounds> nodes = {
      node(1, 300.0, 0.0), node(2, 300.0, 200.0), node(3, 300.0, 400.0)};

  EXPECT_EQ(findAdjacentNode(nodes, 2, NavigationDirection::Up), 1u);
  EXPECT_EQ(findAdjacentNode(nodes, 2, NavigationDirection::Down), 3u);
  EXPECT_FALSE(findAdjacentNode(nodes, 1, NavigationDirection::Up));
}

TEST(NodeNavigationMapperTest, AlignedNodeBeatsNearerDiagonalNode) {
  // Node 3 is closer in a straight line, but node 2 is the one the user is
  // pointing at when they press Right from node 1.
  const std::vector<NodeBounds> nodes = {
      node(1, 0.0, 0.0), node(2, 400.0, 20.0), node(3, 150.0, 400.0)};

  EXPECT_EQ(findAdjacentNode(nodes, 1, NavigationDirection::Right), 2u);
}

TEST(NodeNavigationMapperTest, ConeIsUsedWhenTheBandIsEmpty) {
  // Nothing sits level with node 1, so the off-axis node ahead is taken.
  const std::vector<NodeBounds> nodes = {node(1, 0.0, 0.0),
                                         node(2, 300.0, 250.0)};

  EXPECT_EQ(findAdjacentNode(nodes, 1, NavigationDirection::Right), 2u);
}

TEST(NodeNavigationMapperTest, ConeExcludesNodesOutsideIt) {
  // Node 2 is barely ahead but far off to the side: not a Right move.
  const std::vector<NodeBounds> nodes = {node(1, 0.0, 0.0),
                                         node(2, 20.0, 800.0)};

  EXPECT_FALSE(findAdjacentNode(nodes, 1, NavigationDirection::Right));
  EXPECT_EQ(findAdjacentNode(nodes, 1, NavigationDirection::Down), 2u);
}

TEST(NodeNavigationMapperTest, NodesLevelWithEachOtherAreNotAhead) {
  const std::vector<NodeBounds> nodes = {node(1, 0.0, 0.0), node(2, 0.0, 0.0)};

  EXPECT_FALSE(findAdjacentNode(nodes, 1, NavigationDirection::Right));
  EXPECT_FALSE(findAdjacentNode(nodes, 1, NavigationDirection::Down));
}

TEST(NodeNavigationMapperTest, TiesBreakOnTheLowestNodeId) {
  // Two candidates at the same distance in the band and two more in the cone.
  const std::vector<NodeBounds> banded = {
      node(1, 0.0, 0.0), node(3, 300.0, 0.0), node(2, 300.0, 0.0)};
  EXPECT_EQ(findAdjacentNode(banded, 1, NavigationDirection::Right), 2u);

  const std::vector<NodeBounds> coned = {
      node(1, 0.0, 0.0), node(3, 300.0, 250.0), node(2, 300.0, -250.0)};
  EXPECT_EQ(findAdjacentNode(coned, 1, NavigationDirection::Right), 2u);
}

TEST(NodeNavigationMapperTest, UnknownOrEmptyInputYieldsNoTarget) {
  EXPECT_FALSE(findAdjacentNode({}, 1, NavigationDirection::Right));
  EXPECT_FALSE(findAdjacentNode(chain(), 99, NavigationDirection::Right));
}

TEST(NodeNavigationMapperTest, NearestPointPicksTheClosestCentre) {
  const auto nodes = chain();

  // Node centres sit at x = 70, 370 and 670.
  EXPECT_EQ(findNodeNearestPoint(nodes, 80.0, 40.0), 1u);
  EXPECT_EQ(findNodeNearestPoint(nodes, 360.0, 500.0), 2u);
  EXPECT_EQ(findNodeNearestPoint(nodes, 5000.0, 40.0), 3u);
  EXPECT_FALSE(findNodeNearestPoint({}, 0.0, 0.0));
}

TEST(NodeNavigationMapperTest, NearestPointTiesBreakOnTheLowestNodeId) {
  const std::vector<NodeBounds> nodes = {node(2, 0.0, 0.0), node(1, 0.0, 0.0)};

  EXPECT_EQ(findNodeNearestPoint(nodes, 0.0, 0.0), 1u);
}

TEST(NodeNavigationMapperTest, CycleWalksNodesInIdOrderAndWraps) {
  // Deliberately unordered, and positioned so geometry cannot be the ordering.
  const std::vector<NodeBounds> nodes = {
      node(7, 600.0, 0.0), node(2, 0.0, 400.0), node(5, 300.0, 100.0)};

  EXPECT_EQ(findCycledNode(nodes, 2, true), 5u);
  EXPECT_EQ(findCycledNode(nodes, 5, true), 7u);
  EXPECT_EQ(findCycledNode(nodes, 7, true), 2u);

  EXPECT_EQ(findCycledNode(nodes, 2, false), 7u);
  EXPECT_EQ(findCycledNode(nodes, 7, false), 5u);
}

TEST(NodeNavigationMapperTest, CycleWithoutAnAnchorStartsAtAnEnd) {
  const auto nodes = chain();

  EXPECT_EQ(findCycledNode(nodes, std::nullopt, true), 1u);
  EXPECT_EQ(findCycledNode(nodes, std::nullopt, false), 3u);

  // An anchor that is no longer on the canvas behaves the same way.
  EXPECT_EQ(findCycledNode(nodes, 99, true), 1u);
  EXPECT_EQ(findCycledNode(nodes, 99, false), 3u);
}

TEST(NodeNavigationMapperTest, CycleOverAnEmptyCanvasYieldsNoTarget) {
  EXPECT_FALSE(findCycledNode({}, std::nullopt, true));
  EXPECT_FALSE(findCycledNode({}, 1, false));
}

}  // namespace gui_unit_test
