/*
 * File:        smear_track.cpp
 * Module:      orc-stage-plugin-channel-equalise
 * Purpose:     An edge smear that varies along a source: knots of measured
 *              smear, interpolated per frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "smear_track.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace orc {

namespace {

namespace ce = channel_equalise;

bool valid_smear(const SmearKnot& k) {
  for (const double v :
       {k.fraction, k.tau_samples, k.fast_fraction, k.fast_tau_samples}) {
    if (!std::isfinite(v)) return false;
  }
  if (k.fraction < 0.0 || k.fraction > ce::kMaxFraction) return false;
  if (k.tau_samples < ce::kMinTauSamples ||
      k.tau_samples > ce::kMaxTauSamples) {
    return false;
  }
  if (k.fast_fraction < ce::kMinFastFraction ||
      k.fast_fraction > ce::kMaxFastFraction ||
      k.fast_tau_samples < ce::kMinFastTauSamples ||
      k.fast_tau_samples > ce::kMaxFastTauSamples) {
    return false;
  }
  return ce::invertible(
      {k.fraction, k.tau_samples, k.fast_fraction, k.fast_tau_samples});
}

std::optional<double> parse_double(const std::string& text) {
  if (text.empty()) return std::nullopt;
  std::istringstream in(text);
  in.imbue(std::locale::classic());
  double value = 0.0;
  in >> value;
  if (in.fail() || !in.eof()) return std::nullopt;
  return value;
}

std::optional<int64_t> parse_frame(const std::string& text) {
  int64_t value = 0;
  const char* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (ec != std::errc() || ptr != end) return std::nullopt;
  return value;
}

// The smear the straight line from |a| to |b| gives at |frame|. A knot with
// no smear lends the line its fractions but not its time constants, which
// mean nothing there.
channel_equalise::EdgeSmear between(const SmearKnot& a, const SmearKnot& b,
                                    int64_t frame) {
  const double t = static_cast<double>(frame - a.frame) /
                   static_cast<double>(b.frame - a.frame);
  SmearKnot from = a;
  SmearKnot to = b;
  if (!a.has_smear()) {
    from.tau_samples = b.tau_samples;
    from.fast_tau_samples = b.fast_tau_samples;
  }
  if (!b.has_smear()) {
    to.tau_samples = from.tau_samples;
    to.fast_tau_samples = from.fast_tau_samples;
  }
  const auto lerp = [t](double x, double y) { return x + (t * (y - x)); };
  return {lerp(from.fraction, to.fraction),
          lerp(from.tau_samples, to.tau_samples),
          lerp(from.fast_fraction, to.fast_fraction),
          lerp(from.fast_tau_samples, to.fast_tau_samples)};
}

channel_equalise::EdgeSmear smear_of(const SmearKnot& k) {
  return {k.fraction, k.tau_samples, k.fast_fraction, k.fast_tau_samples};
}

}  // namespace

std::optional<SmearTrack> SmearTrack::from_knots(std::vector<SmearKnot> knots) {
  for (size_t i = 0; i < knots.size(); ++i) {
    if (!valid_smear(knots[i])) {
      return std::nullopt;
    }
    if (i > 0 && knots[i].frame <= knots[i - 1].frame) return std::nullopt;
  }
  SmearTrack track;
  track.knots_ = std::move(knots);
  return track;
}

std::optional<SmearTrack> SmearTrack::parse(const std::string& text) {
  std::vector<SmearKnot> knots;
  if (text.empty()) return SmearTrack{};
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(';', start);
    if (end == std::string::npos) end = text.size();
    const std::string item = text.substr(start, end - start);
    std::vector<std::string> fields;
    size_t from = 0;
    while (true) {
      const size_t colon = item.find(':', from);
      fields.push_back(item.substr(
          from, colon == std::string::npos ? std::string::npos : colon - from));
      if (colon == std::string::npos) break;
      from = colon + 1;
    }
    if (fields.size() != 3 && fields.size() != 5) return std::nullopt;
    const auto frame = parse_frame(fields[0]);
    if (!frame) return std::nullopt;
    SmearKnot knot;
    knot.frame = *frame;
    std::vector<double> values;
    for (size_t i = 1; i < fields.size(); ++i) {
      const auto v = parse_double(fields[i]);
      if (!v) return std::nullopt;
      values.push_back(*v);
    }
    knot.fraction = values[0];
    knot.tau_samples = values[1];
    if (values.size() == 4) {
      knot.fast_fraction = values[2];
      knot.fast_tau_samples = values[3];
    }
    knots.push_back(knot);
    start = end + 1;
  }
  return from_knots(std::move(knots));
}

std::string SmearTrack::to_string() const {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed;
  for (size_t i = 0; i < knots_.size(); ++i) {
    if (i > 0) out << ';';
    out << knots_[i].frame << ':' << std::setprecision(4) << knots_[i].fraction
        << ':' << std::setprecision(3) << knots_[i].tau_samples;
    if (knots_[i].fast_fraction != 0.0) {
      out << ':' << std::setprecision(4) << knots_[i].fast_fraction << ':'
          << std::setprecision(3) << knots_[i].fast_tau_samples;
    }
  }
  return out.str();
}

channel_equalise::EdgeSmear SmearTrack::at(int64_t frame) const {
  if (knots_.empty()) return {0.0, channel_equalise::kMinTauSamples};
  if (frame <= knots_.front().frame) return smear_of(knots_.front());
  if (frame >= knots_.back().frame) return smear_of(knots_.back());
  const auto next = std::upper_bound(
      knots_.begin(), knots_.end(), frame,
      [](int64_t f, const SmearKnot& knot) { return f < knot.frame; });
  return between(*(next - 1), *next, frame);
}

SmearTrack SmearTrack::simplified(double fraction_tolerance,
                                  double tau_tolerance,
                                  const std::vector<bool>& pinned) const {
  const size_t n = knots_.size();
  if (n <= 2) return *this;
  std::vector<bool> keep(n, false);
  for (size_t i = 0; i < n && i < pinned.size(); ++i) keep[i] = pinned[i];
  keep.front() = true;
  keep.back() = true;
  // Where the smear turns on or off stays exact.
  for (size_t i = 1; i < n; ++i) {
    if (knots_[i].has_smear() != knots_[i - 1].has_smear()) {
      keep[i - 1] = true;
      keep[i] = true;
    }
  }
  // Douglas-Peucker between each pair of kept knots.
  std::vector<std::pair<size_t, size_t>> spans;
  size_t last = 0;
  for (size_t i = 1; i < n; ++i) {
    if (keep[i]) {
      spans.emplace_back(last, i);
      last = i;
    }
  }
  while (!spans.empty()) {
    const auto [a, b] = spans.back();
    spans.pop_back();
    if (b <= a + 1) continue;
    size_t worst = a;
    double worst_excess = 1.0;
    for (size_t i = a + 1; i < b; ++i) {
      const auto line = between(knots_[a], knots_[b], knots_[i].frame);
      const auto& k = knots_[i];
      double excess = std::max(std::abs(line.fraction - k.fraction),
                               std::abs(line.fast_fraction - k.fast_fraction)) /
                      fraction_tolerance;
      if (k.fraction > 0.0) {
        excess = std::max(
            excess, std::abs(line.tau_samples - k.tau_samples) / tau_tolerance);
      }
      if (k.fast_fraction != 0.0) {
        excess = std::max(excess,
                          std::abs(line.fast_tau_samples - k.fast_tau_samples) /
                              tau_tolerance);
      }
      if (excess > worst_excess) {
        worst_excess = excess;
        worst = i;
      }
    }
    if (worst != a) {
      keep[worst] = true;
      spans.emplace_back(a, worst);
      spans.emplace_back(worst, b);
    }
  }
  SmearTrack track;
  for (size_t i = 0; i < n; ++i) {
    if (keep[i]) track.knots_.push_back(knots_[i]);
  }
  return track;
}

}  // namespace orc
