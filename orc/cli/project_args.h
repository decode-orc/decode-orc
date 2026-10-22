/*
 * File:        project_args.h
 * Module:      orc-cli
 * Purpose:     Project and node arguments shared by the project subcommands
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/params/parameter_types.h>

#include <optional>
#include <string>

#include "json_writer.h"
#include "project_presenter.h"

namespace orc {
namespace cli {

// `nodes` and `tools` both take a project file and a node address, and must
// refuse them the same way, so a script sees one message and one exit code
// for "no such project" or "no such node" whichever group it called.

/// Load @p path into @p presenter, reporting why on stderr when it cannot be
/// loaded.
bool load_project_or_report(const std::string& path,
                            orc::presenters::ProjectPresenter* presenter);

/// Resolve @p address (a node id or label) to a node, reporting the project's
/// stages on stderr when it names none.
std::optional<orc::presenters::NodeInfo> find_node_or_report(
    const orc::presenters::ProjectPresenter& presenter,
    const std::string& address);

/// Write @p value as a member named @p name, typed as the parameter is.
void write_parameter_value_json(JsonWriter* json, const std::string& name,
                                const orc::ParameterValue& value);

}  // namespace cli
}  // namespace orc
