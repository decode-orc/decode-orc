/*
 * File:        stage_parameter_context.cpp
 * Module:      orc-gui
 * Purpose:     Pure helper deriving what a stage's parameter dialogue shows
 *              from the graph around the node (Tier 1 / gui-logic testable)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "stage_parameter_context.h"

#include <stage_ux_strings.h>

#include <variant>

#include "audio_channel_pair_notice.h"
#include "presenters/include/node_reset.h"

namespace orc::gui {

namespace {

// A Video Parameters override left at -1 means "inherit from the source", so a
// value that reads that way is not a value the user has chosen and the source
// metadata stands in for it.
bool isUnsetVideoParamsStageValue(const orc::ParameterValue& value) {
  if (const auto* int_value = std::get_if<int32_t>(&value)) {
    return *int_value == -1;
  }
  return false;
}

void applyMetadataFallbackValues(
    std::map<std::string, orc::ParameterValue>& current_values,
    const std::map<std::string, orc::ParameterValue>& metadata_values) {
  for (const auto& [param_name, metadata_value] : metadata_values) {
    auto current_it = current_values.find(param_name);
    if (current_it == current_values.end() ||
        isUnsetVideoParamsStageValue(current_it->second)) {
      current_values[param_name] = metadata_value;
    }
  }
}

// Narrows the channel-pair dropdowns in |descriptors| to |pair_names|. The
// stored value stays the bare index; the label carries the pair description
// alongside it. target_pair keeps its "new" entry, since mapping into a pair
// the input does not have yet is the point of that parameter.
void narrowChannelPairChoices(
    std::vector<orc::ParameterDescriptor>& descriptors,
    const std::vector<std::string>& pair_names) {
  const char sep = orc::stage_ux::kComboValueLabelSeparator;
  const auto pair_entry = [&](std::size_t p) {
    return audioChannelPairComboEntry(p, pair_names[p], sep);
  };

  for (auto& desc : descriptors) {
    if (desc.name == "channel_pair" || desc.name == "audio_channel_pair") {
      desc.constraints.allowed_strings.clear();
      for (std::size_t p = 0; p < pair_names.size(); ++p) {
        desc.constraints.allowed_strings.push_back(pair_entry(p));
      }
    } else if (desc.name == "target_pair") {
      desc.constraints.allowed_strings.clear();
      desc.constraints.allowed_strings.push_back(std::string("new") + sep +
                                                 "New channel pair");
      for (std::size_t p = 0; p < pair_names.size(); ++p) {
        desc.constraints.allowed_strings.push_back(pair_entry(p));
      }
    }
  }
}

}  // namespace

bool descriptorsMatch(const std::vector<orc::ParameterDescriptor>& lhs,
                      const std::vector<orc::ParameterDescriptor>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  const auto dependencies_match = [](const auto& a, const auto& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a.has_value()) return true;
    return a->parameter_name == b->parameter_name &&
           a->required_values == b->required_values &&
           a->hide_when_disabled == b->hide_when_disabled;
  };

  for (std::size_t i = 0; i < lhs.size(); ++i) {
    const auto& a = lhs[i];
    const auto& b = rhs[i];
    if (a.name != b.name || a.display_name != b.display_name ||
        a.description != b.description || a.type != b.type ||
        a.file_extension_hint != b.file_extension_hint ||
        a.output_path != b.output_path) {
      return false;
    }
    if (a.constraints.min_value != b.constraints.min_value ||
        a.constraints.max_value != b.constraints.max_value ||
        a.constraints.default_value != b.constraints.default_value ||
        a.constraints.allowed_strings != b.constraints.allowed_strings ||
        a.constraints.required != b.constraints.required) {
      return false;
    }
    if (!dependencies_match(a.constraints.depends_on,
                            b.constraints.depends_on)) {
      return false;
    }
  }

  return true;
}

bool stageNarrowsAudioChannelPairs(const std::string& stage_name) {
  return stage_name == "audio_channel_map" || stage_name == "audio_align" ||
         stage_name == "AudioSink" || stage_name == "tbc_sink";
}

StageParameterContext buildStageParameterContext(
    StageParameterContextInputs inputs) {
  StageParameterContext context;
  context.descriptors = std::move(inputs.descriptors);
  context.current_values = std::move(inputs.current_values);
  context.stage_description = std::move(inputs.stage_description);

  // Audio stages: offer the pairs the input carries, and say so when it
  // carries none. An audio stage whose input has no channel pairs cannot be
  // configured usefully — better to say that in the header than to let the
  // user pick a pair the input does not have. The TBC sink is the exception:
  // its export succeeds either way and only the optional .pcm sidecar is
  // affected, so it gets the milder note.
  if (stageNarrowsAudioChannelPairs(inputs.stage_name) &&
      inputs.input_audio_pair_names.has_value()) {
    const auto& pair_names = *inputs.input_audio_pair_names;
    if (!pair_names.empty()) {
      narrowChannelPairChoices(context.descriptors, pair_names);
    }
    context.stage_description =
        (inputs.stage_name == "tbc_sink")
            ? withAudioChannelPairSidecarNotice(context.stage_description,
                                                pair_names.size())
            : withAudioChannelPairNotice(context.stage_description,
                                         pair_names.size());
  }

  // Video Parameters: reset restores what the source reports, and a parameter
  // the user has not overridden shows that figure rather than the -1 that
  // stands for "inherit".
  if (orc::presenters::stageResetsToSourceMetadata(inputs.stage_name) &&
      inputs.input_video_parameters.has_value()) {
    auto metadata_values =
        orc::presenters::sourceParametersToVideoParamsStageValues(
            *inputs.input_video_parameters);
    applyMetadataFallbackValues(context.current_values, metadata_values);
    context.reset_values = std::move(metadata_values);
  }

  // Source Join orders its inputs by node ID, and nothing in the form says
  // which numbers those are — a multi-input stage has one input port, so the
  // connections do not name their sources. Put the connected nodes, with the
  // IDs the graph draws on them, at the top of the dialogue.
  if (inputs.stage_name == "source_join") {
    context.stage_description = withSourceJoinInputNodesNotice(
        context.stage_description, inputs.connected_inputs);
  }

  return context;
}

}  // namespace orc::gui
