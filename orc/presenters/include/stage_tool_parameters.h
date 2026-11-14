/*
 * File:        stage_tool_parameters.h
 * Module:      orc-presenters
 * Purpose:     Turn a plugin view's parameter write-back (names and text
 *              values) into the node's new parameters, checked against what
 *              the stage declares
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/params/parameter_types.h>
#include <orc/view/stage_tool_session.h>

#include <map>
#include <string>
#include <vector>

namespace orc::presenters {

/**
 * @brief Merge a view's parameter values into a node's stored parameters
 *
 * Every name in @p values must be one of @p descriptors (and not the
 * host-owned input_node_ids), and every value must read as the declared type,
 * within its range and, for a string with a fixed list, one of the list. If
 * any one fails nothing is merged.
 *
 * Pure and thread-agnostic.
 *
 * @param descriptors The stage's declared parameters
 * @param current The node's stored parameters
 * @param values What the view asked to write, as text
 * @param merged Receives @p current with @p values applied (may be null)
 * @param error Receives the reason when false is returned (may be null)
 * @return true when every value was accepted
 */
bool mergeToolParameters(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::map<std::string, orc::ParameterValue>& current,
    const orc::view::ToolParameterMap& values,
    std::map<std::string, orc::ParameterValue>* merged, std::string* error);

/**
 * @brief A node's stored parameters as a view reads them
 *
 * Each value as text in the form mergeToolParameters() reads back to the same
 * value: "true"/"false", decimal integers, and reals with as many digits as
 * they need to read back exactly. The host-owned input_node_ids is left out.
 *
 * Pure and thread-agnostic.
 */
orc::view::ToolParameterMap toolParameterText(
    const std::map<std::string, orc::ParameterValue>& stored);

}  // namespace orc::presenters
