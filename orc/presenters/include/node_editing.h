/*
 * File:        node_editing.h
 * Module:      orc-presenters
 * Purpose:     Front-end-neutral helpers for addressing project nodes and
 *              setting their parameters from text (orc-cli `nodes` commands,
 *              filtergraph import)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc/stage/node_type.h>
#include <orc/stage/params/parameter_types.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "project_presenter_types.h"

namespace orc {
namespace presenters {

/// Outcome of resolving a node address typed by a user.
struct NodeAddressResolution {
  bool found = false;
  NodeID node_id;
  /// User-facing reason when not found, listing the project's labels.
  std::string error;
};

/**
 * @brief Resolve a node address to a node
 *
 * An address made only of digits is a node id; anything else is a stage label.
 * Labels are unique and never all digits, so either form names at most one
 * node.
 */
NodeAddressResolution resolveNodeAddress(const std::vector<NodeInfo>& nodes,
                                         const std::string& address);

/**
 * @brief Convert parameter text into the value type its descriptor declares
 *
 * Booleans accept true/false, 1/0, yes/no and on/off; numbers must be whole
 * values of the declared type. Strings and paths are stored exactly as typed.
 * Constraints (range, allowed strings) are not checked here.
 *
 * @param descriptor Declared parameter, whose `type` selects the alternative
 * @param text       Value exactly as typed
 * @param error      Set to a user-facing explanation when conversion fails
 * @return The typed value, or nullopt when @p text does not fit the type
 */
std::optional<ParameterValue> parseParameterText(
    const ParameterDescriptor& descriptor, const std::string& text,
    std::string* error);

/**
 * @brief Check a typed value against its descriptor's constraints
 *
 * Numbers must lie within min_value / max_value when those are declared, and a
 * string must be one of allowed_strings when that list is not empty.
 *
 * @param error Set to a user-facing explanation when the check fails
 * @return true when the value is acceptable
 */
bool checkParameterConstraints(const ParameterDescriptor& descriptor,
                               const ParameterValue& value, std::string* error);

/// Parameter assignments parsed and checked against a stage's descriptors.
struct ParameterAssignments {
  /// The typed values, ready for setNodeParameters (merged by the caller).
  std::map<std::string, ParameterValue> values;
  /// Keys the stage does not offer in this project, one message each.
  std::vector<std::string> unknown_keys;
  /// Malformed assignments and values that fail their type or constraints.
  std::vector<std::string> errors;
};

/**
 * @brief Parse `key=value` assignments for one stage
 *
 * @param descriptors The stage's descriptors in the project's context (video
 *                    format and source type), which decide the valid keys
 * @param any_format_descriptors The stage's descriptors with no source type,
 *                    used only to explain why a key the stage has in another
 *                    source format is refused here
 * @param source_format_name The project's source format as the user knows it
 *                    ("composite", "Y/C"), for that explanation
 * @param assignments Each `key=value` as typed; the value may contain '='
 */
ParameterAssignments parseParameterAssignments(
    const std::vector<ParameterDescriptor>& descriptors,
    const std::vector<ParameterDescriptor>& any_format_descriptors,
    const std::string& source_format_name,
    const std::vector<std::string>& assignments);

/// Lower-case name of a configuration status: "green", "yellow" or "red".
std::string configurationStatusName(orc::ConfigurationStatus status);

}  // namespace presenters
}  // namespace orc
