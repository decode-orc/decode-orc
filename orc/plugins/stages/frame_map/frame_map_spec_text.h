/*
 * File:        frame_map_spec_text.h
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     A Frame Map range spec as the user reads it: 1-based
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_FRAME_MAP_FRAME_MAP_SPEC_TEXT_H
#define ORC_STAGE_FRAME_MAP_FRAME_MAP_SPEC_TEXT_H

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>

namespace orc::frame_map_spec_text {

namespace detail {

// A run of decimal digits; nullopt when empty, not digits, or too large.
inline std::optional<uint64_t> parse_index(const std::string& token) {
  if (token.empty()) return std::nullopt;
  uint64_t value = 0;
  for (char c : token) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    const auto digit = static_cast<uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
    value = value * 10 + digit;
  }
  return value;
}

// Each index of @p spec plus one, comma-joined with whitespace trimmed;
// PAD_<count> tokens hold counts and stay as they are. nullopt when a token
// is malformed.
inline std::optional<std::string> one_based(const std::string& spec) {
  std::string result;
  size_t pos = 0;
  bool first = true;
  while (pos <= spec.size()) {
    const size_t comma = spec.find(',', pos);
    std::string token = spec.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);
    const size_t start = token.find_first_not_of(" \t");
    if (start == std::string::npos) {
      token.clear();
    } else {
      token = token.substr(start, token.find_last_not_of(" \t") - start + 1);
    }

    if (!token.empty()) {
      std::string converted;
      if (token.rfind("PAD_", 0) == 0) {
        if (!parse_index(token.substr(4))) return std::nullopt;
        converted = token;
      } else {
        const size_t dash = token.find('-');
        if (dash == std::string::npos) {
          const auto value = parse_index(token);
          if (!value) return std::nullopt;
          converted = std::to_string(*value + 1);
        } else {
          const auto lhs = parse_index(token.substr(0, dash));
          const auto rhs = parse_index(token.substr(dash + 1));
          if (!lhs || !rhs) return std::nullopt;
          converted = std::to_string(*lhs + 1) + "-" + std::to_string(*rhs + 1);
        }
      }
      if (!first) result += ",";
      result += converted;
      first = false;
    }

    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return result;
}

}  // namespace detail

/**
 * @brief A stored (0-based) range spec as the user reads it (1-based)
 *
 * The same conversion the host's parameter dialog shows the `ranges`
 * parameter with. A malformed spec comes back as it is, so the user sees the
 * raw value.
 */
inline std::string to_presentation(const std::string& spec) {
  const auto shifted = detail::one_based(spec);
  return shifted ? *shifted : spec;
}

}  // namespace orc::frame_map_spec_text

#endif  // ORC_STAGE_FRAME_MAP_FRAME_MAP_SPEC_TEXT_H
