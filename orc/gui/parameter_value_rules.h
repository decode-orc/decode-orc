/*
 * File:        parameter_value_rules.h
 * Module:      orc-gui
 * Purpose:     Pure helper that reports parameter values whose relation to
 *              each other is wrong (Tier 1 / gui-logic testable)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef PARAMETER_VALUE_RULES_H
#define PARAMETER_VALUE_RULES_H

#include <orc/stage/params/parameter_types.h>

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace orc::gui {

/**
 * @brief Report parameter pairs that are the wrong way round.
 *
 * A parameter descriptor states one parameter's type and range, which is all
 * a spin box needs; it cannot say that one parameter must stay below another.
 * Those relations are listed here so the dialog can say which pair is wrong
 * before the stage is asked to accept it — the stage rejects the same
 * combinations, but its refusal carries no explanation a user can act on.
 *
 * Values are addressed by the parameter names the stages actually use: a rule
 * naming a parameter that is not in @p values is simply not applied, so this
 * is safe to call for any stage. (A rule keyed on a name no stage has would
 * be silently dead, which is why each one is covered by a test.)
 *
 * -1 means "inherit from source" for the video parameter overrides, and what
 * the source reports is not known here, so a pair is only judged when both
 * ends carry a real value.
 *
 * @param values Current values, as the parameter dialog would apply them
 * @return One sentence per broken relation, in a fixed order; empty when the
 *         values are consistent
 */
std::vector<std::string> crossParameterErrors(
    const std::map<std::string, orc::ParameterValue>& values);

/**
 * @brief Report what is wrong with a video sink decoder schedule as typed.
 *
 * The schedule is "range:decoder" entries plus at most one bare decoder for
 * the frames no range lists, e.g. "transform3d, 1001-1500:transform2d". Frame
 * numbers are 1-based here, as everywhere a user sees them. The stage checks
 * the stored form again when it exports; this says what is wrong while the
 * dialog is still open.
 *
 * @param display_schedule The schedule as shown in the dialog
 * @param decoders The decoder names it may use: the Decoder Type choices
 *        the project offers, less "multiple"
 * @return One sentence describing the first problem; empty when usable
 */
std::string decoderScheduleError(const std::string& display_schedule,
                                 const std::vector<std::string>& decoders);

}  // namespace orc::gui

#endif  // PARAMETER_VALUE_RULES_H
