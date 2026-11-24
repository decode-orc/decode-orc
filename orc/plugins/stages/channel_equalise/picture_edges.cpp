/*
 * File:        picture_edges.cpp
 * Module:      orc-stage-plugin-channel-equalise
 * Purpose:     Sharp, isolated picture edges of a PAL field and the tails a
 *              smear correction leaves on them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "picture_edges.h"

#include <algorithm>
#include <cmath>

namespace orc::picture_edges {

namespace {

// The field rows searched (frame-flat field lines 25-306, the comb reading
// two lines below) and the samples (the active picture).
constexpr size_t kFirstRow = 24;
constexpr size_t kLastRow = 306;  // exclusive
// The second field has 312 lines, the first 313; edges are looked for on
// rows kFirstRow to kLastRow (and the comb two rows below) of either.
constexpr size_t kFieldLines = 312;
constexpr size_t kFirstColumn = 190;
constexpr size_t kLastColumn = 1100;  // exclusive

constexpr double kMinStep = 0.3;
constexpr double kFlat = 0.06;
constexpr double kSharp = 0.8;
constexpr size_t kMinSpacing = 10;

// Lead-in: 14..5 samples before; plateau: 8..19 after.
constexpr size_t kLeadFrom = 14;
constexpr size_t kLeadCount = 10;
constexpr size_t kPlateauFrom = 8;
constexpr size_t kPlateauCount = 12;

// scipy.ndimage.gaussian_filter1d(x, 1.0): radius 4, reflected ends.
std::vector<double> smooth(const std::vector<double>& x) {
  static const std::array<double, 9> kernel = [] {
    std::array<double, 9> k{};
    double sum = 0.0;
    for (size_t i = 0; i < k.size(); ++i) {
      const double d = static_cast<double>(i) - 4.0;
      k[i] = std::exp(-0.5 * d * d);
      sum += k[i];
    }
    for (double& v : k) v /= sum;
    return k;
  }();
  const auto n = static_cast<std::ptrdiff_t>(x.size());
  std::vector<double> y(x.size());
  for (std::ptrdiff_t i = 0; i < n; ++i) {
    double acc = 0.0;
    for (std::ptrdiff_t k = -4; k <= 4; ++k) {
      std::ptrdiff_t j = i + k;
      if (j < 0) j = -j - 1;
      if (j >= n) j = (2 * n) - j - 1;
      acc += kernel[static_cast<size_t>(k + 4)] * x[static_cast<size_t>(j)];
    }
    y[static_cast<size_t>(i)] = acc;
  }
  return y;
}

struct Window {
  double mean = 0.0;
  double spread = 0.0;
};

Window window(const std::vector<double>& y, size_t first, size_t count) {
  double lo = y[first];
  double hi = y[first];
  double sum = 0.0;
  for (size_t i = first; i < first + count; ++i) {
    lo = std::min(lo, y[i]);
    hi = std::max(hi, y[i]);
    sum += y[i];
  }
  return {sum / static_cast<double>(count), hi - lo};
}

double median(std::vector<double> v) {
  const size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid),
                   v.end());
  const double upper = v[mid];
  if (v.size() % 2 == 1) return upper;
  const double lower = *std::max_element(
      v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
  return (lower + upper) / 2.0;
}

}  // namespace

void Edges::append(const Edges& other) {
  samples.insert(samples.end(), other.samples.begin(), other.samples.end());
  crossing.insert(crossing.end(), other.crossing.begin(), other.crossing.end());
  field.insert(field.end(), other.field.begin(), other.field.end());
}

Edges find(const std::vector<double>& field, size_t width, uint8_t parity,
           size_t max_edges) {
  Edges all;
  if (field.size() < kFieldLines * width || width < kFirstColumn + kAfter + 2) {
    return all;
  }
  const size_t last_column = std::min(kLastColumn, width - kAfter - 2);
  std::vector<double> raw(width);
  for (size_t row = kFirstRow; row < kLastRow; ++row) {
    for (size_t i = 0; i < width; ++i) {
      raw[i] =
          0.5 * (field[(row * width) + i] + field[((row + 2) * width) + i]);
    }
    const auto y = smooth(raw);
    std::vector<double> step(width, 0.0);
    for (size_t i = 2; i + 2 < width; ++i) step[i] = y[i + 2] - y[i - 2];
    size_t last_kept = 0;
    bool kept_any = false;
    for (size_t i = std::max(kFirstColumn, kBefore + 2); i < last_column; ++i) {
      const double s = std::abs(step[i]);
      if (s < kMinStep || s < std::abs(step[i - 1]) ||
          s < std::abs(step[i + 1])) {
        continue;
      }
      const auto lead = window(y, i - kLeadFrom, kLeadCount);
      const auto plateau = window(y, i + kPlateauFrom, kPlateauCount);
      const double height = plateau.mean - lead.mean;
      const double size = std::abs(height);
      if (size < kMinStep || lead.spread > kFlat * size ||
          plateau.spread > 2.5 * kFlat * size || s < kSharp * size) {
        continue;
      }
      if (kept_any && i - last_kept < kMinSpacing) continue;
      const double mid = (lead.mean + plateau.mean) / 2.0;
      double crossing = -1.0;
      for (int d = -3; d < 3; ++d) {
        const auto j = static_cast<size_t>(static_cast<std::ptrdiff_t>(i) + d);
        const double x0 = y[j] - mid;
        const double x1 = y[j + 1] - mid;
        if (x0 * x1 <= 0.0 && x0 != x1) {
          crossing = static_cast<double>(j) + (x0 / (x0 - x1));
          break;
        }
      }
      if (crossing < 0.0) continue;
      kept_any = true;
      last_kept = i;
      const auto base = static_cast<std::ptrdiff_t>(std::floor(crossing)) -
                        static_cast<std::ptrdiff_t>(kBefore);
      if (base < 0 || static_cast<size_t>(base) + kLength > width) {
        continue;
      }
      for (size_t k = 0; k < kLength; ++k) {
        all.samples.push_back(
            static_cast<float>(raw[static_cast<size_t>(base) + k]));
      }
      all.crossing.push_back(crossing - static_cast<double>(base));
      all.field.push_back(parity);
    }
  }
  if (all.size() <= max_edges) return all;
  // Spread over the field: every n-th.
  Edges kept;
  const double stride =
      static_cast<double>(all.size()) / static_cast<double>(max_edges);
  for (size_t k = 0; k < max_edges; ++k) {
    const auto e = static_cast<size_t>(static_cast<double>(k) * stride);
    kept.samples.insert(
        kept.samples.end(),
        all.samples.begin() + static_cast<std::ptrdiff_t>(e * kLength),
        all.samples.begin() + static_cast<std::ptrdiff_t>((e + 1) * kLength));
    kept.crossing.push_back(all.crossing[e]);
    kept.field.push_back(all.field[e]);
  }
  return kept;
}

Profiles profiles(const Edges& edges,
                  const std::array<channel_equalise::EdgeSmear, 2>& smears) {
  return profiles_each(edges, [&edges, &smears](size_t e) {
    return smears[edges.field[e] == 0 ? 0 : 1];
  });
}

Profiles profiles_each(
    const Edges& edges,
    const std::function<channel_equalise::EdgeSmear(size_t edge)>& smear_of) {
  Profiles out;
  std::vector<double> snippet(kLength);
  for (size_t e = 0; e < edges.size(); ++e) {
    for (size_t k = 0; k < kLength; ++k) {
      snippet[k] = edges.samples[(e * kLength) + k];
    }
    channel_equalise::unsmear(snippet, smear_of(e));
    const double lead = window(snippet, kBefore - kLeadFrom, kLeadCount).mean;
    const double plateau =
        window(snippet, kBefore + kPlateauFrom, kPlateauCount).mean;
    const double height = plateau - lead;
    if (height == 0.0) continue;
    for (int o = kProfileFirst; o <= kProfileLast; ++o) {
      const double x = edges.crossing[e] + o;
      const auto i = static_cast<size_t>(std::floor(x));
      const double f = x - static_cast<double>(i);
      const double v = (snippet[i] * (1.0 - f)) + (snippet[i + 1] * f);
      out.values.push_back((v - lead) / height);
    }
    out.rising.push_back(height > 0.0 ? 1 : 0);
  }
  return out;
}

std::optional<Tails> tails(const Profiles& profiles,
                           const std::vector<size_t>& which) {
  constexpr size_t kMinEdges = 200;
  constexpr size_t kMinEachWay = 10;
  const size_t n = which.empty() ? profiles.size() : which.size();
  if (n < kMinEdges) return std::nullopt;
  std::array<std::vector<double>, kProfileLength> up;
  std::array<std::vector<double>, kProfileLength> down;
  for (size_t k = 0; k < n; ++k) {
    const size_t e = which.empty() ? k : which[k];
    auto& into = profiles.rising[e] != 0 ? up : down;
    for (size_t o = 0; o < kProfileLength; ++o) {
      into[o].push_back(profiles.values[(e * kProfileLength) + o]);
    }
  }
  if (up[0].size() < kMinEachWay || down[0].size() < kMinEachWay) {
    return std::nullopt;
  }
  Tails t;
  t.edges = n;
  double near = 0.0;
  double far = 0.0;
  for (int o = 3; o <= 20; ++o) {
    const auto i = static_cast<size_t>(o - kProfileFirst);
    const double linear = (median(up[i]) + median(down[i])) / 2.0;
    if (o <= 8) {
      near += linear;
    } else {
      far += linear;
    }
  }
  t.t3_8 = 1.0 - (near / 6.0);
  t.t9_20 = 1.0 - (far / 12.0);
  return t;
}

}  // namespace orc::picture_edges
