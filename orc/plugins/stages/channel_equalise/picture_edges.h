/*
 * File:        picture_edges.h
 * Module:      orc-stage-plugin-channel-equalise
 * Purpose:     Sharp, isolated picture edges of a PAL field and the tails a
 *              smear correction leaves on them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_CHANNEL_EQUALISE_PICTURE_EDGES_H
#define ORC_STAGE_CHANNEL_EQUALISE_PICTURE_EDGES_H

#include <orc/support/channel_equalise.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace orc {

// The picture's own edges show what a correction does to the picture, which
// the VITS cannot: a disc's picture can carry a tail of its own (a
// telecine's) that the VITS, inserted after it, does not. Edges are found on
// the luma of a two-line comb within the field ((F[n] + F[n+2]) / 2, which
// cancels PAL chroma on vertically uniform content): a step of at least 0.3
// of white - blanking with 80 % of it within 2 samples, flat (within 6 % of
// the step) 4-14 samples before it and (within 15 %) 8-20 samples after it,
// found on a copy smoothed by a Gaussian of 1 sample. Soft and natural edges
// fail those tests, so they do not bias the measurement.
namespace picture_edges {

// A snippet of the comb luma around each edge: from kBefore samples before
// its crossing to kAfter after it, so any correction along the line can be
// applied to it and the edge measured again.
inline constexpr size_t kBefore = 120;
inline constexpr size_t kAfter = 40;
inline constexpr size_t kLength = kBefore + kAfter + 1;

struct Edges {
  // kLength samples per edge, in units of white - blanking.
  std::vector<float> samples;
  // The crossing within each snippet (kBefore plus a fraction).
  std::vector<double> crossing;
  // The field parity each edge was found in (0 first, 1 second).
  std::vector<uint8_t> field;

  size_t size() const { return crossing.size(); }
  void append(const Edges& other);
};

// The edges of one field (its frame-flat rows of |width| samples, in units
// of white - blanking: 313 lines for the first field, 312 for the second),
// at most |max_edges| of them, spread over the field.
Edges find(const std::vector<double>& field, size_t width, uint8_t parity,
           size_t max_edges);

struct Tails {
  // The linear part of the edges (the mean of the median rising and the
  // median falling profile, each normalised to its own lead-in and
  // plateau): the share of the step missing 3-8 and 9-20 samples after the
  // crossing.
  double t3_8 = 0.0;
  double t9_20 = 0.0;
  size_t edges = 0;
};

// Each edge's normalised profile at -14..20 samples from its crossing after
// its field's correction (|smears| indexed by field parity).
inline constexpr int kProfileFirst = -14;
inline constexpr int kProfileLast = 20;
inline constexpr size_t kProfileLength = kProfileLast - kProfileFirst + 1;
struct Profiles {
  std::vector<double> values;  // kProfileLength per edge
  std::vector<uint8_t> rising;
  size_t size() const { return rising.size(); }
};
Profiles profiles(const Edges& edges,
                  const std::array<channel_equalise::EdgeSmear, 2>& smears);
// The same with each edge's smear given by |smear_of| (its index).
Profiles profiles_each(
    const Edges& edges,
    const std::function<channel_equalise::EdgeSmear(size_t edge)>& smear_of);

// The tails of |profiles| (those listed in |which|, every one when empty).
// std::nullopt with fewer than 200 edges, or fewer than 10 rising or
// falling.
std::optional<Tails> tails(const Profiles& profiles,
                           const std::vector<size_t>& which = {});

}  // namespace picture_edges
}  // namespace orc

#endif  // ORC_STAGE_CHANNEL_EQUALISE_PICTURE_EDGES_H
