/*
 * File:        view_node_resolution.h
 * Module:      orc-gui
 * Purpose:     Pure helper that decides which node the preview should view
 *              after the DAG has been rebuilt (Tier 1 / gui-logic testable)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef VIEW_NODE_RESOLUTION_H
#define VIEW_NODE_RESOLUTION_H

#include <orc/stage/node_id.h>

namespace orc::gui {

/// What should happen to the viewed node once the DAG has been rebuilt.
enum class ViewNodeAction {
  Keep,    ///< The viewed node survived; refresh its outputs in place.
  Switch,  ///< It did not; view ViewNodeDecision::target instead.
  Clear,   ///< There is nothing left to view; stop naming a node at all.
};

struct ViewNodeDecision {
  ViewNodeAction action{ViewNodeAction::Clear};
  /// The node to view. Meaningful for Keep and Switch; invalid for Clear.
  NodeID target{};
};

/**
 * @brief Decide which node the preview should be looking at after a rebuild.
 *
 * Rebuilding the DAG can delete the node being viewed, and a project open
 * rebuilds into a project that has nothing in common with the previous one.
 * Exactly one question decides the outcome: does the rebuilt project still
 * have the node whose output is on screen?
 *
 * The distinction that matters is between an id that is *unset* and one that
 * is *stale*. NodeID::is_valid() only asks whether the id is non-negative, so
 * a node that has just been deleted still reports valid — it names something
 * real, just not something that exists any more. Treating "valid" as "usable"
 * is what let a delete leave the preview addressing a node the DAG no longer
 * had, so |current_exists| is asked for separately and both must hold.
 *
 * The placeholder id used when a project has no source is negative, so it is
 * covered by the same "not usable" path and switches to a real node as soon
 * as one exists.
 *
 * @param current        Node currently being viewed; may be unset or stale.
 * @param current_exists Whether the rebuilt project still has |current|.
 * @param first_node     Fallback node to view, or an invalid id if the project
 *                       has no nodes at all.
 */
inline ViewNodeDecision resolveViewNodeAfterRebuild(NodeID current,
                                                    bool current_exists,
                                                    NodeID first_node) {
  if (current.is_valid() && current_exists) {
    return {ViewNodeAction::Keep, current};
  }
  if (first_node.is_valid()) {
    return {ViewNodeAction::Switch, first_node};
  }
  return {ViewNodeAction::Clear, NodeID()};
}

}  // namespace orc::gui

#endif  // VIEW_NODE_RESOLUTION_H
