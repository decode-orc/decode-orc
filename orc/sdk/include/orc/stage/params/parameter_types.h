/*
 * File:        parameter_types.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Stage parameter type definitions shared across all layers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

// SDK TIER: stage/params — stage contract type crossing the plugin boundary.
// A layout change here bumps the host ABI version.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace orc {

/// Parameter value types supported by stages
using ParameterValue = std::variant<int32_t,     // Integer values
                                    uint32_t,    // Unsigned integer values
                                    double,      // Floating point values
                                    bool,        // Boolean flags
                                    std::string  // String values
                                    >;

/// Type of parameter
enum class ParameterType {
  INT32,
  UINT32,
  DOUBLE,
  BOOL,
  STRING,
  FILE_PATH  // String representing a file path (GUI shows file browser)
};

/// Parameter dependency specification
struct ParameterDependency {
  std::string parameter_name;  // Name of parameter this depends on
  std::vector<std::string>
      required_values;  // Values that enable this parameter (empty = any
                        // non-default)
  // When true (default), hides the widget when dependency is not met.
  // When false, keeps it visible but grays it out (disabled).
  bool hide_when_disabled = true;
};

/// Parameter constraints
struct ParameterConstraints {
  // For numeric types
  std::optional<ParameterValue> min_value;
  std::optional<ParameterValue> max_value;
  std::optional<ParameterValue> default_value;

  // For string types (allowed values)
  std::vector<std::string> allowed_strings;

  // Whether parameter is required
  bool required = false;

  // Parameter dependency (optional)
  std::optional<ParameterDependency> depends_on;
};

/// Description of a stage parameter
struct ParameterDescriptor {
  std::string name;  // Parameter internal name (e.g., "overcorrect_extension")
  std::string
      display_name;  // Human-readable name (e.g., "Overcorrect Extension")
  std::string description;  // Detailed description of what parameter does
  ParameterType type;       // Parameter value type
  ParameterConstraints constraints;  // Value constraints and defaults
  std::string file_extension_hint =
      "";  // File extension hint for FILE_PATH types (e.g., ".cvbs", ".pcm",
           // ".rgb", ".mp4")
  // For FILE_PATH types: when true, the GUI browse button opens a "save" dialog
  // (the file is written, may not exist yet) instead of an "open" dialog. Sink
  // stages are treated as output by default via a name heuristic; set this
  // explicitly for output paths on stages that are not sinks (e.g. a report
  // file written by a transform stage).
  bool output_path = false;
};

/// Reserved parameter name carrying the host-supplied identity of a node's
/// inputs.
///
/// A stage that must know which upstream node feeds each entry of execute()'s
/// |inputs| vector declares a STRING parameter with this name. The host
/// overwrites the value when it builds the execution graph with the source
/// node IDs of the node's incoming connections, comma-separated, in the same
/// order as |inputs| (for example "2,4,16"). Stages that do not declare the
/// parameter are never handed it.
///
/// The value is host-owned: the parameter is hidden from the GUI and CLI
/// parameter surfaces, is never edited by the user, and is not written to the
/// project file. A stage handed an empty value (no host support, or no
/// connections yet) must fall back to positional input handling.
inline constexpr const char kInputNodeIdsParameter[] = "input_node_ids";

/// Helper functions to work with parameter values
namespace parameter_util {
/// Convert ParameterValue to string for display
std::string value_to_string(const ParameterValue& value);

/// Convert string to ParameterValue based on type
std::optional<ParameterValue> string_to_value(const std::string& str,
                                              ParameterType type);

/// Get type name as string
const char* type_name(ParameterType type);
}  // namespace parameter_util

}  // namespace orc
