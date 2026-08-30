/*
 * File:        node_navigation_mapper.h
 * Module:      orc-gui
 * Purpose:     Keyboard navigation mapping between DAG nodes on the canvas
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef NODE_NAVIGATION_MAPPER_H
#define NODE_NAVIGATION_MAPPER_H

#include <cstdint>
#include <optional>
#include <vector>

namespace orc::gui {

/// Direction of a cursor-key move across the DAG canvas.
enum class NavigationDirection { Left, Right, Up, Down };

/// On-screen extent of one node, in scene coordinates.
struct NodeBounds {
  std::uint64_t id{0};
  double x{0.0};
  double y{0.0};
  double width{0.0};
  double height{0.0};
};

/**
 * @brief Find the node a cursor-key press should move the selection to.
 *
 * Selection is geometric, not topological: the node picked is the one a user
 * would point at, which on a left-to-right pipeline is also the next stage in
 * the chain. Two tiers are tried in turn:
 *
 * 1. Nodes ahead on the pressed axis whose extent overlaps the current node's
 *    band on the cross axis; the smallest edge gap wins. Walking a chain
 *    rightwards therefore never diverts to a branch that merely happens to be
 *    closer diagonally.
 * 2. If the band is empty, nodes ahead within a 90-degree cone, scored by
 *    distance along the axis plus twice the distance across it, so a slightly
 *    off-axis node beats a badly off-axis one at the same range.
 *
 * Ties are broken by the lowest node id so the result is deterministic for
 * nodes that sit on top of each other.
 *
 * @param nodes All nodes on the canvas (may include |current_id|)
 * @param current_id Node the selection is moving from
 * @param direction Direction of the move
 * @return Target node id, or nullopt if there is nothing in that direction
 */
std::optional<std::uint64_t> findAdjacentNode(
    const std::vector<NodeBounds>& nodes, std::uint64_t current_id,
    NavigationDirection direction);

/**
 * @brief Find the node whose centre is closest to a point.
 *
 * Used to pick an anchor when a cursor key arrives with nothing selected: the
 * viewport centre gives the most visible node rather than an arbitrary one.
 *
 * @param nodes All nodes on the canvas
 * @param x Scene x coordinate
 * @param y Scene y coordinate
 * @return Closest node id, or nullopt if |nodes| is empty
 */
std::optional<std::uint64_t> findNodeNearestPoint(
    const std::vector<NodeBounds>& nodes, double x, double y);

/**
 * @brief Find the next or previous node in a stable cycle over all nodes.
 *
 * Ordering is by node id, which for ORC node ids is creation order, so Tab
 * walks the graph in the order the stages were added and always terminates.
 * The cycle wraps at both ends; with no valid |current_id| the first (or, when
 * stepping backwards, the last) node is returned.
 *
 * @param nodes All nodes on the canvas
 * @param current_id Node the selection is moving from, if any
 * @param forward True to step to the next node, false for the previous one
 * @return Target node id, or nullopt if |nodes| is empty
 */
std::optional<std::uint64_t> findCycledNode(
    const std::vector<NodeBounds>& nodes,
    std::optional<std::uint64_t> current_id, bool forward);

}  // namespace orc::gui

#endif  // NODE_NAVIGATION_MAPPER_H
