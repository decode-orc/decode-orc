/*
 * File:        stage_json.h
 * Module:      orc-cli
 * Purpose:     Machine-readable projection of the stage presenter types
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_CLI_STAGE_JSON_H
#define ORC_CLI_STAGE_JSON_H

#include <orc/stage/params/parameter_types.h>

#include <string>

#include "json_writer.h"
#include "project_presenter_types.h"

namespace orc {
namespace cli {

// The sibling projection of makeStageDetails() / makeStageParameterDetails():
// same StageInfo and ParameterDescriptor, different audience. The table renders
// for a person — canonical labels, 1-based frame and line numbers, fields
// omitted when they have nothing to say — while this renders for a script, so
// keys are struct field names, the kind is the stable id `--kind` accepts, and
// every parameter default is the 0-based stored form a project file holds.

/**
 * @brief Write one stage's identity as members of the caller's open object.
 *
 * Members only, with no enclosing braces, because `stages list` wraps each
 * stage in an object of its own while `stages info` follows the same members
 * with the stage's parameters — one description, two enclosings.
 *
 * The name leads: it is the token `stages info` and `stages help` accept and
 * a project's `stage:` key holds.
 */
void write_stage_fields_json(JsonWriter* json,
                             const orc::presenters::StageInfo& stage);

/**
 * @brief Write one parameter descriptor.
 *
 * @param json Receives the object.
 * @param descriptor Parameter to describe; its declared type decides whether
 *                   each constrained value is written as a JSON number,
 *                   boolean or string.
 */
void write_stage_parameter_json(JsonWriter* json,
                                const orc::ParameterDescriptor& descriptor);

}  // namespace cli
}  // namespace orc

#endif  // ORC_CLI_STAGE_JSON_H
