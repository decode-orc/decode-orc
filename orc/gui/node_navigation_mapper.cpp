/*
 * File:        node_navigation_mapper.cpp
 * Module:      orc-gui
 * Purpose:     Keyboard navigation mapping between DAG nodes on the canvas
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "node_navigation_mapper.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace orc::gui {

namespace {

// Nodes closer together than this along the axis of travel count as level with
// each other rather than ahead, so a cursor key never "moves" sideways to a
// node that is really beside the current one.
constexpr double kAheadEpsilon = 1.0;

// Weight applied to off-axis distance in the tier 2 cone score. Anything above
// 1.0 makes an aligned node win over an equally distant diagonal one; 2.0 is
// firm enough that the cone only ever picks a diagonal when the band is empty.
constexpr double kAcrossWeight = 2.0;

struct Centre {
  double x{0.0};
  double y{0.0};
};

Centre centreOf(const NodeBounds& node) {
  return Centre{node.x + node.width / 2.0, node.y + node.height / 2.0};
}

bool isHorizontal(NavigationDirection direction) {
  return direction == NavigationDirection::Left ||
         direction == NavigationDirection::Right;
}

// Signed distance from |from| to |to| along the direction of travel; positive
// means |to| lies ahead.
double alongDistance(const Centre& from, const Centre& to,
                     NavigationDirection direction) {
  switch (direction) {
    case NavigationDirection::Left:
      return from.x - to.x;
    case NavigationDirection::Right:
      return to.x - from.x;
    case NavigationDirection::Up:
      return from.y - to.y;
    case NavigationDirection::Down:
      return to.y - from.y;
  }
  return 0.0;
}

double acrossDistance(const Centre& from, const Centre& to,
                      NavigationDirection direction) {
  return isHorizontal(direction) ? std::abs(to.y - from.y)
                                 : std::abs(to.x - from.x);
}

// True when the two nodes overlap on the axis perpendicular to the direction
// of travel, i.e. the candidate sits in the current node's band.
bool overlapsBand(const NodeBounds& current, const NodeBounds& candidate,
                  NavigationDirection direction) {
  if (isHorizontal(direction)) {
    return candidate.y < current.y + current.height &&
           current.y < candidate.y + candidate.height;
  }
  return candidate.x < current.x + current.width &&
         current.x < candidate.x + candidate.width;
}

const NodeBounds* findNode(const std::vector<NodeBounds>& nodes,
                           std::uint64_t id) {
  const auto it =
      std::find_if(nodes.begin(), nodes.end(),
                   [id](const NodeBounds& node) { return node.id == id; });
  return it == nodes.end() ? nullptr : &(*it);
}

}  // namespace

std::optional<std::uint64_t> findAdjacentNode(
    const std::vector<NodeBounds>& nodes, std::uint64_t current_id,
    NavigationDirection direction) {
  const NodeBounds* current = findNode(nodes, current_id);
  if (!current) {
    return std::nullopt;
  }

  const Centre current_centre = centreOf(*current);

  std::optional<std::uint64_t> banded_best;
  double banded_best_score = std::numeric_limits<double>::max();
  std::optional<std::uint64_t> cone_best;
  double cone_best_score = std::numeric_limits<double>::max();

  for (const NodeBounds& candidate : nodes) {
    if (candidate.id == current_id) {
      continue;
    }

    const Centre candidate_centre = centreOf(candidate);
    const double along =
        alongDistance(current_centre, candidate_centre, direction);
    if (along <= kAheadEpsilon) {
      continue;  // Level with, or behind, the current node
    }

    const double across =
        acrossDistance(current_centre, candidate_centre, direction);

    if (overlapsBand(*current, candidate, direction)) {
      // Tier 1: distance along the axis alone, so the nearest node in the
      // band wins regardless of how it is offset within that band.
      if (along < banded_best_score ||
          (along == banded_best_score && banded_best &&
           candidate.id < *banded_best)) {
        banded_best_score = along;
        banded_best = candidate.id;
      }
      continue;
    }

    // Tier 2: a 90 degree cone centred on the direction of travel.
    if (across > along) {
      continue;
    }

    const double score = along + kAcrossWeight * across;
    if (score < cone_best_score ||
        (score == cone_best_score && cone_best && candidate.id < *cone_best)) {
      cone_best_score = score;
      cone_best = candidate.id;
    }
  }

  return banded_best ? banded_best : cone_best;
}

std::optional<std::uint64_t> findNodeNearestPoint(
    const std::vector<NodeBounds>& nodes, double x, double y) {
  std::optional<std::uint64_t> best;
  double best_distance = std::numeric_limits<double>::max();

  for (const NodeBounds& node : nodes) {
    const Centre centre = centreOf(node);
    const double dx = centre.x - x;
    const double dy = centre.y - y;
    const double distance = (dx * dx) + (dy * dy);

    if (distance < best_distance ||
        (distance == best_distance && best && node.id < *best)) {
      best_distance = distance;
      best = node.id;
    }
  }

  return best;
}

std::optional<std::uint64_t> findCycledNode(
    const std::vector<NodeBounds>& nodes,
    std::optional<std::uint64_t> current_id, bool forward) {
  if (nodes.empty()) {
    return std::nullopt;
  }

  std::vector<std::uint64_t> ids;
  ids.reserve(nodes.size());
  for (const NodeBounds& node : nodes) {
    ids.push_back(node.id);
  }
  std::sort(ids.begin(), ids.end());

  if (!current_id) {
    return forward ? ids.front() : ids.back();
  }

  const auto it = std::find(ids.begin(), ids.end(), *current_id);
  if (it == ids.end()) {
    return forward ? ids.front() : ids.back();
  }

  const auto index = static_cast<std::size_t>(std::distance(ids.begin(), it));
  const std::size_t next = forward ? (index + 1) % ids.size()
                                   : (index + ids.size() - 1) % ids.size();
  return ids[next];
}

}  // namespace orc::gui
