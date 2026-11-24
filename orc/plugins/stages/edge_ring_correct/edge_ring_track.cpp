/*
 * File:        edge_ring_track.cpp
 * Module:      orc-stage-plugin-edge-ring-correct
 * Purpose:     The certified windows of an edge-ring correction along a
 *              source, and their text form
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "edge_ring_track.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace orc::edge_ring {

namespace {

constexpr double kTableUnit = 1e-5;

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

bool parse_int(const std::string& s, long long& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  v = std::strtoll(s.c_str(), &end, 10);
  return end && *end == '\0';
}

bool parse_double(const std::string& s, double& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  v = std::strtod(s.c_str(), &end);
  return end && *end == '\0' && std::isfinite(v);
}

template <size_t N>
bool parse_table(const std::string& s, std::array<double, N>& t) {
  const auto parts = split(s, ',');
  if (parts.size() != N) return false;
  for (size_t j = 0; j < N; ++j) {
    long long v = 0;
    if (!parse_int(parts[j], v)) return false;
    t[j] = static_cast<double>(v) * kTableUnit;
  }
  return true;
}

template <size_t N>
void write_table(std::ostringstream& o, const std::array<double, N>& t) {
  for (size_t j = 0; j < N; ++j) {
    if (j) o << ',';
    o << static_cast<long long>(std::llround(t[j] / kTableUnit));
  }
}

template <size_t N>
void quantise(std::array<double, N>& t) {
  for (auto& v : t)
    v = static_cast<double>(std::llround(v / kTableUnit)) * kTableUnit;
}

std::string gain_text(double g) {
  std::ostringstream o;
  o << std::llround(g * 1000.0) / 1000.0;
  return o.str();
}

}  // namespace

std::optional<RingTrack> RingTrack::from_windows(
    std::vector<WindowCorrection> windows) {
  int64_t last_end = -1;
  for (auto& w : windows) {
    if (w.end_frame <= w.first_frame || w.first_frame < last_end) {
      return std::nullopt;
    }
    last_end = w.end_frame;
    for (double g : {w.gains.rise, w.gains.fall, w.gains.line}) {
      if (!(g >= 0.0 && g <= 1.0)) return std::nullopt;
    }
    for (const auto* t : {&w.templates.rise, &w.templates.fall}) {
      for (double v : *t) {
        if (!(std::abs(v) <= 1.0)) return std::nullopt;
      }
    }
    for (double v : w.templates.line) {
      if (!(std::abs(v) <= 1.0)) return std::nullopt;
    }
    // as the text form holds them, so a track behaves the same written out
    // and read back
    quantise(w.templates.rise);
    quantise(w.templates.fall);
    quantise(w.templates.line);
    w.gains.rise = std::llround(w.gains.rise * 1000.0) / 1000.0;
    w.gains.fall = std::llround(w.gains.fall * 1000.0) / 1000.0;
    w.gains.line = std::llround(w.gains.line * 1000.0) / 1000.0;
  }
  RingTrack t;
  t.windows_ = std::move(windows);
  return t;
}

std::optional<RingTrack> RingTrack::parse(const std::string& raw) {
  // surrounding white space (a trailing newline from a file) is ignored
  const auto first = raw.find_first_not_of(" \t\r\n");
  const auto last = raw.find_last_not_of(" \t\r\n");
  const std::string text = first == std::string::npos
                               ? std::string()
                               : raw.substr(first, last - first + 1);
  if (text.empty()) return RingTrack{};
  std::vector<WindowCorrection> windows;
  for (const auto& item : split(text, ';')) {
    const auto f = split(item, ':');
    if (f.size() != 6) return std::nullopt;
    WindowCorrection w;
    const auto range = split(f[0], '-');
    long long a = 0, b = 0;
    if (range.size() != 2 || !parse_int(range[0], a) ||
        !parse_int(range[1], b)) {
      return std::nullopt;
    }
    w.first_frame = a;
    w.end_frame = b;
    if (f[1] != "f" && f[1] != "w") return std::nullopt;
    w.adapt = f[1] == "f";
    const auto g = split(f[2], ',');
    if (g.size() != 3 || !parse_double(g[0], w.gains.rise) ||
        !parse_double(g[1], w.gains.fall) ||
        !parse_double(g[2], w.gains.line)) {
      return std::nullopt;
    }
    if (!parse_table(f[3], w.templates.rise) ||
        !parse_table(f[4], w.templates.fall) ||
        !parse_table(f[5], w.templates.line)) {
      return std::nullopt;
    }
    windows.push_back(w);
  }
  return from_windows(std::move(windows));
}

std::string RingTrack::to_string() const {
  std::ostringstream o;
  for (size_t k = 0; k < windows_.size(); ++k) {
    const auto& w = windows_[k];
    if (k) o << ';';
    o << w.first_frame << '-' << w.end_frame << ':' << (w.adapt ? 'f' : 'w')
      << ':' << gain_text(w.gains.rise) << ',' << gain_text(w.gains.fall) << ','
      << gain_text(w.gains.line) << ':';
    write_table(o, w.templates.rise);
    o << ':';
    write_table(o, w.templates.fall);
    o << ':';
    write_table(o, w.templates.line);
  }
  return o.str();
}

const WindowCorrection* RingTrack::at(int64_t frame) const {
  auto it = std::upper_bound(
      windows_.begin(), windows_.end(), frame,
      [](int64_t f, const WindowCorrection& w) { return f < w.first_frame; });
  if (it == windows_.begin()) return nullptr;
  --it;
  return frame < it->end_frame ? &*it : nullptr;
}

}  // namespace orc::edge_ring
