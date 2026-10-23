/*
 * File:        node_editing.cpp
 * Module:      orc-presenters
 * Purpose:     Node addressing and text-to-parameter conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "node_editing.h"

#include <stage_ux_strings.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace orc {
namespace presenters {

namespace {

bool is_all_digits(const std::string& text) {
  return !text.empty() &&
         std::all_of(text.begin(), text.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string list_labels(const std::vector<NodeInfo>& nodes) {
  std::string listing;
  for (const auto& node : nodes) {
    listing += "\n  " + node.node_id.to_string() + "  " + node.label + "  (" +
               node.stage_name + ")";
  }
  return listing;
}

/// A numeric constraint as a double, so int32, uint32 and double limits
/// compare the same way. nullopt when the constraint is absent or not numeric.
std::optional<double> as_number(const std::optional<ParameterValue>& value) {
  if (!value) {
    return std::nullopt;
  }
  if (const auto* v = std::get_if<int32_t>(&*value)) {
    return static_cast<double>(*v);
  }
  if (const auto* v = std::get_if<uint32_t>(&*value)) {
    return static_cast<double>(*v);
  }
  if (const auto* v = std::get_if<double>(&*value)) {
    return *v;
  }
  return std::nullopt;
}

std::string number_text(double value) {
  // Whole limits read as whole numbers; std::to_string would add ".000000"
  if (value == static_cast<double>(static_cast<int64_t>(value))) {
    return std::to_string(static_cast<int64_t>(value));
  }
  return std::to_string(value);
}

const ParameterDescriptor* find_descriptor(
    const std::vector<ParameterDescriptor>& descriptors,
    const std::string& name) {
  const auto it = std::find_if(
      descriptors.begin(), descriptors.end(),
      [&name](const ParameterDescriptor& d) { return d.name == name; });
  return it == descriptors.end() ? nullptr : &*it;
}

}  // namespace

NodeAddressResolution resolveNodeAddress(const std::vector<NodeInfo>& nodes,
                                         const std::string& address) {
  NodeAddressResolution result;

  if (is_all_digits(address)) {
    for (const auto& node : nodes) {
      if (node.node_id.to_string() == address) {
        result.found = true;
        result.node_id = node.node_id;
        return result;
      }
    }
    result.error = "No stage has the id " + address + ". The project's stages:";
  } else {
    for (const auto& node : nodes) {
      if (node.label == address) {
        result.found = true;
        result.node_id = node.node_id;
        return result;
      }
    }
    result.error =
        "No stage is named '" + address + "'. The project's stages (id, name):";
  }

  if (nodes.empty()) {
    result.error += " none";
  } else {
    result.error += list_labels(nodes);
  }
  return result;
}

std::optional<ParameterValue> parseParameterText(
    const ParameterDescriptor& descriptor, const std::string& text,
    std::string* error) {
  // A filtergraph or command line is text, so every value arrives as a
  // string; stages read their parameters with std::holds_alternative against
  // the type they declared. The declared type is the authority for the
  // conversion, and a value that does not fit it is an error rather than a
  // silent fall back to the default.
  //
  // Leading/trailing spaces survive shell quoting more often than they are
  // meant, and no numeric or boolean literal needs them.
  const auto first = text.find_first_not_of(" \t");
  const auto last = text.find_last_not_of(" \t");
  const std::string trimmed = first == std::string::npos
                                  ? std::string()
                                  : text.substr(first, last - first + 1);

  const auto whole_number = [&](int64_t min_value,
                                int64_t max_value) -> std::optional<int64_t> {
    if (trimmed.empty()) {
      *error = "expected a whole number";
      return std::nullopt;
    }
    std::size_t consumed = 0;
    int64_t value = 0;
    try {
      value = static_cast<int64_t>(std::stoll(trimmed, &consumed));
    } catch (const std::exception&) {
      *error = "'" + text + "' is not a whole number";
      return std::nullopt;
    }
    if (consumed != trimmed.size()) {
      *error = "'" + text + "' is not a whole number";
      return std::nullopt;
    }
    if (value < min_value || value > max_value) {
      *error = "'" + text + "' is out of range for this parameter";
      return std::nullopt;
    }
    return value;
  };

  switch (descriptor.type) {
    case ParameterType::BOOL: {
      // The spellings a person actually types on a command line; the project
      // writer emits "true"/"false", which round-trips through the first two.
      std::string lowered = trimmed;
      std::transform(
          lowered.begin(), lowered.end(), lowered.begin(),
          [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (lowered == "true" || lowered == "1" || lowered == "yes" ||
          lowered == "on") {
        return ParameterValue{true};
      }
      if (lowered == "false" || lowered == "0" || lowered == "no" ||
          lowered == "off") {
        return ParameterValue{false};
      }
      *error = "'" + text + "' is not true or false";
      return std::nullopt;
    }
    case ParameterType::INT32: {
      const auto value = whole_number(INT32_MIN, INT32_MAX);
      if (!value) {
        return std::nullopt;
      }
      return ParameterValue{static_cast<int32_t>(*value)};
    }
    case ParameterType::UINT32: {
      const auto value = whole_number(0, UINT32_MAX);
      if (!value) {
        return std::nullopt;
      }
      return ParameterValue{static_cast<uint32_t>(*value)};
    }
    case ParameterType::DOUBLE: {
      if (trimmed.empty()) {
        *error = "expected a number";
        return std::nullopt;
      }
      std::size_t consumed = 0;
      double value = 0.0;
      try {
        value = std::stod(trimmed, &consumed);
      } catch (const std::exception&) {
        *error = "'" + text + "' is not a number";
        return std::nullopt;
      }
      if (consumed != trimmed.size()) {
        *error = "'" + text + "' is not a number";
        return std::nullopt;
      }
      return ParameterValue{value};
    }
    case ParameterType::STRING:
    case ParameterType::FILE_PATH:
      break;
  }
  // Strings and paths are stored exactly as typed: trimming a path would
  // change what it means.
  return ParameterValue{text};
}

bool checkParameterConstraints(const ParameterDescriptor& descriptor,
                               const ParameterValue& value,
                               std::string* error) {
  const auto& constraints = descriptor.constraints;

  if (const auto* text = std::get_if<std::string>(&value)) {
    if (constraints.allowed_strings.empty()) {
      return true;
    }
    // An entry may pack a display label after the stored value; only the
    // stored value is what a project holds.
    std::vector<std::string> allowed;
    for (const auto& entry : constraints.allowed_strings) {
      allowed.push_back(
          entry.substr(0, entry.find(stage_ux::kComboValueLabelSeparator)));
    }
    if (std::find(allowed.begin(), allowed.end(), *text) == allowed.end()) {
      *error = "'" + *text + "' is not one of:";
      for (const auto& choice : allowed) {
        *error += " '" + choice + "'";
      }
      return false;
    }
    return true;
  }

  const auto number = as_number(value);
  if (!number) {
    return true;  // bool: nothing to check
  }
  const auto min_value = as_number(constraints.min_value);
  const auto max_value = as_number(constraints.max_value);
  if ((min_value && *number < *min_value) ||
      (max_value && *number > *max_value)) {
    *error = number_text(*number) + " is outside the range ";
    *error += min_value ? number_text(*min_value) : std::string("-");
    *error += " to ";
    *error += max_value ? number_text(*max_value) : std::string("-");
    return false;
  }
  return true;
}

ParameterAssignments parseParameterAssignments(
    const std::vector<ParameterDescriptor>& descriptors,
    const std::vector<ParameterDescriptor>& any_format_descriptors,
    const std::string& source_format_name,
    const std::vector<std::string>& assignments) {
  ParameterAssignments result;

  for (const auto& assignment : assignments) {
    const auto equals = assignment.find('=');
    if (equals == std::string::npos || equals == 0) {
      result.errors.push_back("'" + assignment +
                              "' is not a key=value assignment");
      continue;
    }
    const std::string key = assignment.substr(0, equals);
    const std::string text = assignment.substr(equals + 1);

    const auto* descriptor = find_descriptor(descriptors, key);
    if (!descriptor) {
      if (find_descriptor(any_format_descriptors, key)) {
        result.unknown_keys.push_back("'" + key + "' does not apply to a " +
                                      source_format_name + " project");
      } else {
        result.unknown_keys.push_back("'" + key +
                                      "' is not a parameter of this stage");
      }
      continue;
    }

    std::string error;
    const auto value = parseParameterText(*descriptor, text, &error);
    if (!value) {
      result.errors.push_back("'" + key + "': " + error);
      continue;
    }
    if (!checkParameterConstraints(*descriptor, *value, &error)) {
      result.errors.push_back("'" + key + "': " + error);
      continue;
    }
    result.values[key] = *value;
  }
  return result;
}

std::string configurationStatusName(orc::ConfigurationStatus status) {
  switch (status) {
    case orc::ConfigurationStatus::Green:
      return "green";
    case orc::ConfigurationStatus::Yellow:
      return "yellow";
    case orc::ConfigurationStatus::Red:
      return "red";
  }
  return "red";
}

}  // namespace presenters
}  // namespace orc
