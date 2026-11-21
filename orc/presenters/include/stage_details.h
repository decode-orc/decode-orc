/*
 * File:        stage_details.h
 * Module:      orc-presenters
 * Purpose:     One ordered description of a stage and its parameters
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/node_type.h>
#include <orc/stage/params/parameter_types.h>

#include <cstddef>
#include <string>
#include <vector>

#include "project_presenter_types.h"

namespace orc::presenters {

/// One labelled line of a stage description. Mirrors PluginDetailField so both
/// kinds of listing render through the same printer.
struct StageDetailField {
  std::string label;  ///< Canonical field label from stage_ux_strings.h.
  std::string value;  ///< Rendered value; never empty (fields with nothing to
                      ///< say are omitted from the list instead).
};

/**
 * @brief Menu-category label for a stage ("Source", "Transform", ...).
 *
 * Derived from the stage's NodeType through the SDK's stage_category_for(), so
 * a stage reads under the same category word in the CLI as in the GUI's Add
 * Stage menu.
 */
const char* stageKindLabel(const StageInfo& stage);

/// Stable lower-case identifier for the same category, for machine-readable
/// output and for `--kind`.
const char* stageKindId(const StageInfo& stage);

/**
 * @brief Parse a `--kind` argument.
 *
 * Accepts the lower-case form of every category label the listing prints.
 *
 * @param text User-supplied kind.
 * @param out Receives the category on success.
 * @return false when @p text names no category.
 */
bool parseStageKind(const std::string& text, orc::StageCategory* out);

/// Whether @p stage belongs to @p kind.
bool stageMatchesKind(const StageInfo& stage, orc::StageCategory kind);

/// Describe a stage's identity as an ordered field list. The name leads: it is
/// the token every command that takes a stage accepts.
std::vector<StageDetailField> makeStageDetails(const StageInfo& stage);

/**
 * @brief Describe one parameter as an ordered field list.
 *
 * The same descriptor data the GUI's parameter dialog renders, in the order it
 * renders it. Values of parameters that hold 0-based index specifications are
 * converted to their 1-based presentation form, so a default reads here exactly
 * as it reads in the dialog.
 *
 * @param stage_name Internal stage name; selects the indexed-spec conversion.
 * @param descriptor Parameter to describe.
 */
std::vector<StageDetailField> makeStageParameterDetails(
    const std::string& stage_name, const orc::ParameterDescriptor& descriptor);

/**
 * @brief The parameter's default in the form a project file stores.
 *
 * Always 0-based: this feeds machine-readable output and `nodes reset`, which
 * are read back by a script or the project loader rather than by a person.
 */
std::string stageParameterStoredDefault(
    const orc::ParameterDescriptor& descriptor);

/// True when any of @p descriptors holds a 0-based index specification, so a
/// caller knows whether the 1-based presentation note applies.
bool stageHasIndexedSpecParameter(
    const std::string& stage_name,
    const std::vector<orc::ParameterDescriptor>& descriptors);

/**
 * @brief Registered stage names close to a name that did not resolve.
 *
 * Ranked by substring containment first, then by edit distance, so a typo and a
 * half-remembered name both land on the same suggestion list.
 *
 * @param stage_name Name the user supplied.
 * @param stages Every registered stage.
 * @param max_results Upper bound on suggestions returned.
 */
std::vector<std::string> findNearbyStageNames(
    const std::string& stage_name, const std::vector<StageInfo>& stages,
    std::size_t max_results);

}  // namespace orc::presenters
