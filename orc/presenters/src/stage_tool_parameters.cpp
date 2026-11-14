/*
 * File:        stage_tool_parameters.cpp
 * Module:      orc-presenters
 * Purpose:     Check and merge a plugin view's parameter write-back
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../include/stage_tool_parameters.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <variant>

namespace orc::presenters {

namespace {

// The whole of the text must be the number: std::stoi alone reads "12abc"
// as 12, which would write a value the view never meant.
std::optional<orc::ParameterValue> parseStrict(const std::string& text,
                                               orc::ParameterType type) {
  try {
    size_t used = 0;
    switch (type) {
      case orc::ParameterType::INT32: {
        const int64_t value = std::stoll(text, &used);
        if (used != text.size() ||
            value < std::numeric_limits<int32_t>::min() ||
            value > std::numeric_limits<int32_t>::max()) {
          return std::nullopt;
        }
        return static_cast<int32_t>(value);
      }
      case orc::ParameterType::UINT32: {
        if (!text.empty() && text[0] == '-') {
          return std::nullopt;
        }
        const uint64_t value = std::stoull(text, &used);
        if (used != text.size() ||
            value > std::numeric_limits<uint32_t>::max()) {
          return std::nullopt;
        }
        return static_cast<uint32_t>(value);
      }
      case orc::ParameterType::DOUBLE: {
        const double value = std::stod(text, &used);
        if (used != text.size()) {
          return std::nullopt;
        }
        return value;
      }
      case orc::ParameterType::BOOL:
      case orc::ParameterType::STRING:
      case orc::ParameterType::FILE_PATH:
        return orc::parameter_util::string_to_value(text, type);
    }
  } catch (const std::exception&) {
    return std::nullopt;
  }
  return std::nullopt;
}

// Compare two values of the same numeric alternative as doubles.
std::optional<double> asNumber(const orc::ParameterValue& value) {
  if (const auto* v = std::get_if<int32_t>(&value)) {
    return static_cast<double>(*v);
  }
  if (const auto* v = std::get_if<uint32_t>(&value)) {
    return static_cast<double>(*v);
  }
  if (const auto* v = std::get_if<double>(&value)) {
    return *v;
  }
  return std::nullopt;
}

}  // namespace

bool mergeToolParameters(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::map<std::string, orc::ParameterValue>& current,
    const orc::view::ToolParameterMap& values,
    std::map<std::string, orc::ParameterValue>* merged, std::string* error) {
  const auto refuse = [error](std::string reason) {
    if (error) {
      *error = std::move(reason);
    }
    return false;
  };

  std::map<std::string, orc::ParameterValue> result = current;
  for (const auto& [name, text] : values) {
    if (name == orc::kInputNodeIdsParameter) {
      return refuse("'" + name + "' is set by Decode-Orc, not by a tool");
    }
    auto descriptor = std::find_if(
        descriptors.begin(), descriptors.end(),
        [&name](const orc::ParameterDescriptor& d) { return d.name == name; });
    if (descriptor == descriptors.end()) {
      return refuse("The stage has no parameter '" + name + "'");
    }
    auto value = parseStrict(text, descriptor->type);
    if (!value) {
      return refuse("'" + text + "' is not a valid " +
                    orc::parameter_util::type_name(descriptor->type) +
                    " for '" + name + "'");
    }
    const auto& limits = descriptor->constraints;
    const auto number = asNumber(*value);
    if (number && limits.min_value) {
      const auto low = asNumber(*limits.min_value);
      if (low && *number < *low) {
        return refuse("'" + name + "' must be at least " +
                      orc::parameter_util::value_to_string(*limits.min_value));
      }
    }
    if (number && limits.max_value) {
      const auto high = asNumber(*limits.max_value);
      if (high && *number > *high) {
        return refuse("'" + name + "' must be at most " +
                      orc::parameter_util::value_to_string(*limits.max_value));
      }
    }
    if (descriptor->type == orc::ParameterType::STRING &&
        !limits.allowed_strings.empty() &&
        std::find(limits.allowed_strings.begin(), limits.allowed_strings.end(),
                  text) == limits.allowed_strings.end()) {
      return refuse("'" + text + "' is not one of the values '" + name +
                    "' accepts");
    }
    result[name] = std::move(*value);
  }

  if (merged) {
    *merged = std::move(result);
  }
  return true;
}

orc::view::ToolParameterMap toolParameterText(
    const std::map<std::string, orc::ParameterValue>& stored) {
  orc::view::ToolParameterMap text;
  for (const auto& [name, value] : stored) {
    if (name == orc::kInputNodeIdsParameter) {
      continue;
    }
    if (const auto* real = std::get_if<double>(&value)) {
      // The fewest digits that read back as the same double: 0.1 stays
      // "0.1" rather than std::to_string's "0.100000", and a value
      // std::to_string would round away keeps its digits.
      std::string written;
      for (int digits = 15; digits <= 17; ++digits) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out.precision(digits);
        out << *real;
        written = out.str();
        std::istringstream in(written);
        in.imbue(std::locale::classic());
        double read_back = 0.0;
        if (in >> read_back && read_back == *real) {
          break;
        }
      }
      text[name] = written;
    } else {
      text[name] = orc::parameter_util::value_to_string(value);
    }
  }
  return text;
}

}  // namespace orc::presenters
