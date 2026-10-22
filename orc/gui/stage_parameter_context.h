/*
 * File:        stage_parameter_context.h
 * Module:      orc-gui
 * Purpose:     Pure helper deriving what a stage's parameter dialogue shows
 *              from the graph around the node (Tier 1 / gui-logic testable)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef STAGE_PARAMETER_CONTEXT_H
#define STAGE_PARAMETER_CONTEXT_H

#include <orc/stage/orc_source_parameters.h>
#include <orc/stage/params/parameter_types.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "source_join_notice.h"

namespace orc::gui {

/**
 * @brief Raw material for one stage's parameter dialogue.
 *
 * The stage's own descriptors and the node's stored values say what the form
 * holds; the rest describes the graph around the node, which is what decides
 * how some of those parameters should be presented — which channel pairs are
 * actually selectable, what "reset" should mean, what the header has to say.
 * Every field beyond the first four is optional: a stage that does not consult
 * the graph leaves them empty and passes through untouched.
 *
 * Gathering these needs a presenter; deriving the dialogue's contents from
 * them does not, which is the reason for the split. The parameter dialogue is
 * modeless, so this derivation is repeated whenever the graph changes under an
 * open window and cannot live inline in the code that opens one.
 */
struct StageParameterContextInputs {
  /// Stage identifier (e.g. "video_params"), which selects the rules applied.
  std::string stage_name;
  /// Parameter descriptors as the stage publishes them.
  std::vector<orc::ParameterDescriptor> descriptors;
  /// Values currently stored on the node.
  std::map<std::string, orc::ParameterValue> current_values;
  /// Stage description, before any note is appended.
  std::string stage_description;

  /// Nodes connected to a multi-input stage's input, in node-ID order.
  /// Consulted for Source Join only.
  std::vector<ConnectedInputNode> connected_inputs;

  /// Audio channel pairs carried by the node's input, in container order.
  /// Consulted for the audio stages only; nullopt where it was not looked up
  /// (which is not the same as an input carrying none).
  std::optional<std::vector<std::string>> input_audio_pair_names;

  /// Video parameters read from the node's input — that is, before this
  /// node's own overrides. Consulted for Video Parameters only.
  std::optional<orc::SourceParameters> input_video_parameters;
};

/**
 * @brief What the parameter dialogue should be built from or refreshed with.
 */
struct StageParameterContext {
  std::vector<orc::ParameterDescriptor> descriptors;
  std::map<std::string, orc::ParameterValue> current_values;
  std::string stage_description;
  /// Values the dialogue's reset button restores; nullopt falls back to the
  /// descriptor defaults, and the button names itself accordingly.
  std::optional<std::map<std::string, orc::ParameterValue>> reset_values;
};

/**
 * @brief Stages whose channel-pair dropdowns are narrowed to the input's pairs.
 *
 * The stage descriptor lists every container slot because a stage cannot know
 * what it will be connected to; the dialogue narrows that to the pairs the
 * upstream node actually carries, so a selection the input cannot satisfy is
 * not offered in the first place.
 */
bool stageNarrowsAudioChannelPairs(const std::string& stage_name);

/**
 * @brief Whether two descriptor lists would build the same form.
 *
 * Compares everything a parameter row is built from — the parameters, their
 * order, types, labels, bounds, defaults, allowed values and dependencies.
 * A refresh that leaves this unchanged can put new values into the widgets
 * already on screen instead of building them again, which is what keeps a
 * part-typed value and its caret intact through a refresh.
 *
 * The SDK types have no equality of their own; this compares only the fields
 * the dialogue reads, so a field added there without being added here would
 * show as "same form" rather than as a compile error.
 */
bool descriptorsMatch(const std::vector<orc::ParameterDescriptor>& lhs,
                      const std::vector<orc::ParameterDescriptor>& rhs);

/**
 * @brief Derive a stage's dialogue contents from the graph around its node.
 *
 * Touches no presenter, no widget and no filesystem: given the same inputs it
 * gives the same answer, which is what allows an open dialogue to be refreshed
 * against a rewired graph and the whole derivation to be tested on its own.
 *
 * @param inputs Stage descriptors, stored values and the graph context
 * @return Descriptors, values, header text and reset values for the dialogue
 */
StageParameterContext buildStageParameterContext(
    StageParameterContextInputs inputs);

}  // namespace orc::gui

#endif  // STAGE_PARAMETER_CONTEXT_H
