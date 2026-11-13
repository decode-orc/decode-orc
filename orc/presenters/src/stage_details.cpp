/*
 * File:        stage_details.cpp
 * Module:      orc-presenters
 * Purpose:     Implementation of the shared stage/parameter description
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * This translation unit deliberately depends only on the standard library, the
 * plugin SDK's parameter types and orc-common's presentation helpers, so it
 * can be tested in isolation from orc-core.
 */

#include "stage_details.h"

#include <frame_numbering.h>
#include <stage_ux_strings.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace orc::presenters {

namespace {

/// Lower-case copy, for case-insensitive matching of user-supplied words.
std::string to_lower(const std::string& text) {
  std::string lower = text;
  for (char& ch : lower) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return lower;
}

/// Split an allowed_strings entry into its stored value and its display label.
/// An entry with no separator is both.
std::pair<std::string, std::string> split_allowed_entry(
    const std::string& entry) {
  const auto pos = entry.find(stage_ux::kComboValueLabelSeparator);
  if (pos == std::string::npos) {
    return {entry, entry};
  }
  return {entry.substr(0, pos), entry.substr(pos + 1)};
}

/// Render a constraint value, which may hold any of the parameter types.
std::string constraint_to_string(
    const std::optional<orc::ParameterValue>& value) {
  return value.has_value() ? orc::parameter_util::value_to_string(*value)
                           : std::string();
}

/// The type-appropriate empty value for a parameter with no declared default.
/// Matches what the GUI dialog seeds its widget with, so the two agree on what
/// "unset" looks like.
std::string implicit_default(orc::ParameterType type) {
  switch (type) {
    case orc::ParameterType::INT32:
    case orc::ParameterType::UINT32:
    case orc::ParameterType::DOUBLE:
      return "0";
    case orc::ParameterType::BOOL:
      return "false";
    case orc::ParameterType::STRING:
    case orc::ParameterType::FILE_PATH:
      break;
  }
  return std::string();
}

/// Levenshtein distance, used only to rank a handful of stage names.
std::size_t edit_distance(const std::string& lhs, const std::string& rhs) {
  std::vector<std::size_t> previous(rhs.size() + 1);
  std::vector<std::size_t> current(rhs.size() + 1);
  for (std::size_t j = 0; j <= rhs.size(); ++j) {
    previous[j] = j;
  }
  for (std::size_t i = 1; i <= lhs.size(); ++i) {
    current[0] = i;
    for (std::size_t j = 1; j <= rhs.size(); ++j) {
      const std::size_t substitution =
          previous[j - 1] + (lhs[i - 1] == rhs[j - 1] ? 0 : 1);
      current[j] =
          std::min({previous[j] + 1, current[j - 1] + 1, substitution});
    }
    previous.swap(current);
  }
  return previous[rhs.size()];
}

}  // namespace

const char* stageKindLabel(const StageInfo& stage) {
  return orc::stage_category_label(orc::stage_category_for(stage.node_type));
}

const char* stageKindId(const StageInfo& stage) {
  switch (orc::stage_category_for(stage.node_type)) {
    case orc::StageCategory::SOURCE:
      return "source";
    case orc::StageCategory::TRANSFORM:
      return "transform";
    case orc::StageCategory::ANALYSIS:
      return "analysis";
    case orc::StageCategory::SINK:
      break;
  }
  return "sink";
}

bool parseStageKind(const std::string& text, orc::StageCategory* out) {
  const std::string kind = to_lower(text);
  if (kind == "source") {
    *out = orc::StageCategory::SOURCE;
    return true;
  }
  if (kind == "transform") {
    *out = orc::StageCategory::TRANSFORM;
    return true;
  }
  if (kind == "analysis") {
    *out = orc::StageCategory::ANALYSIS;
    return true;
  }
  if (kind == "sink") {
    *out = orc::StageCategory::SINK;
    return true;
  }
  return false;
}

bool stageMatchesKind(const StageInfo& stage, orc::StageCategory kind) {
  if (kind == orc::StageCategory::SINK) {
    // An analysis sink is a sink too, so "give me the sinks" includes them.
    // --kind analysis still narrows to the analysis sinks alone, and each
    // row's kind field says which it is.
    return stage.is_sink;
  }
  return orc::stage_category_for(stage.node_type) == kind;
}

std::vector<StageDetailField> makeStageDetails(const StageInfo& stage) {
  std::vector<StageDetailField> fields;
  auto add = [&fields](const char* label, std::string value) {
    if (!value.empty()) {
      fields.push_back(StageDetailField{label, std::move(value)});
    }
  };

  // The name leads, as the selector does for a plugin: it is what every
  // command that takes a stage accepts.
  add(stage_ux::kFieldName, stage.name);
  add(stage_ux::kFieldDisplayName, stage.display_name);
  add(stage_ux::kFieldKind, stageKindLabel(stage));
  // An owning plugin id is a plugin selector, so it is printed only when the
  // registry really has one; a host-registered stage prints no plugin field
  // rather than a placeholder no command would accept.
  add(stage_ux::kFieldPlugin, stage.owning_plugin_id);
  add(stage_ux::kFieldCore, stage.is_core_plugin ? "yes" : "no");
  add(stage_ux::kFieldDescription, stage.description);
  return fields;
}

std::vector<StageDetailField> makeStageParameterDetails(
    const std::string& stage_name, const orc::ParameterDescriptor& descriptor) {
  std::vector<StageDetailField> fields;
  auto add = [&fields](const char* label, std::string value) {
    if (!value.empty()) {
      fields.push_back(StageDetailField{label, std::move(value)});
    }
  };

  const auto spec_kind = orc::indexed_spec_kind(stage_name, descriptor.name);
  auto present = [spec_kind](const std::string& stored) {
    return orc::indexed_spec_to_presentation(spec_kind, stored);
  };

  add(stage_ux::kParamFieldDisplayName, descriptor.display_name);
  add(stage_ux::kParamFieldDescription, descriptor.description);
  add(stage_ux::kParamFieldType,
      orc::parameter_util::type_name(descriptor.type));
  add(stage_ux::kParamFieldRequired,
      descriptor.constraints.required ? "yes" : "no");

  const std::string stored_default =
      constraint_to_string(descriptor.constraints.default_value);
  if (!stored_default.empty()) {
    add(stage_ux::kParamFieldDefault, present(stored_default));
  }
  add(stage_ux::kParamFieldMinimum,
      present(constraint_to_string(descriptor.constraints.min_value)));
  add(stage_ux::kParamFieldMaximum,
      present(constraint_to_string(descriptor.constraints.max_value)));

  if (!descriptor.constraints.allowed_strings.empty()) {
    // The stored value leads; a display label that differs follows it, because
    // it is the value that has to be typed into a project file or
    // `nodes set`, not the label the GUI combo box shows.
    std::string allowed;
    for (const auto& entry : descriptor.constraints.allowed_strings) {
      const auto [value, label] = split_allowed_entry(entry);
      if (!allowed.empty()) {
        allowed += ", ";
      }
      allowed += value;
      if (label != value) {
        allowed += " (" + label + ")";
      }
    }
    add(stage_ux::kParamFieldAllowed, allowed);
  }

  if (descriptor.constraints.depends_on.has_value()) {
    const auto& dependency = *descriptor.constraints.depends_on;
    std::string detail = dependency.parameter_name;
    if (dependency.required_values.empty()) {
      detail += " is set";
    } else {
      detail += " = ";
      for (std::size_t i = 0; i < dependency.required_values.size(); ++i) {
        detail += (i == 0 ? "" : " | ") + dependency.required_values[i];
      }
    }
    add(stage_ux::kParamFieldDependsOn, detail);
  }

  add(stage_ux::kParamFieldFileExtension, descriptor.file_extension_hint);
  if (descriptor.output_path) {
    add(stage_ux::kParamFieldWritesFile, "yes");
  }
  return fields;
}

std::string stageParameterStoredDefault(
    const orc::ParameterDescriptor& descriptor) {
  const std::string declared =
      constraint_to_string(descriptor.constraints.default_value);
  return declared.empty() ? implicit_default(descriptor.type) : declared;
}

bool stageHasIndexedSpecParameter(
    const std::string& stage_name,
    const std::vector<orc::ParameterDescriptor>& descriptors) {
  return std::any_of(descriptors.begin(), descriptors.end(),
                     [&stage_name](const orc::ParameterDescriptor& descriptor) {
                       return orc::indexed_spec_kind(stage_name,
                                                     descriptor.name) !=
                              orc::IndexedSpecKind::kNone;
                     });
}

std::vector<std::string> findNearbyStageNames(
    const std::string& stage_name, const std::vector<StageInfo>& stages,
    std::size_t max_results) {
  const std::string needle = to_lower(stage_name);

  // Rank containment above distance: a user who remembers "cvbs" wants every
  // CVBS stage, not the one name that happens to be a short hop away.
  struct Candidate {
    std::size_t rank;
    std::size_t distance;
    std::string name;
  };
  std::vector<Candidate> candidates;
  for (const auto& stage : stages) {
    const std::string name = to_lower(stage.name);
    const bool contains =
        !needle.empty() && (name.find(needle) != std::string::npos ||
                            needle.find(name) != std::string::npos);
    const std::size_t distance = edit_distance(needle, name);
    // A distance beyond a third of the name is not a typo, it is a different
    // word; offering it would bury the suggestions that are worth reading.
    if (!contains && distance > std::max<std::size_t>(2, name.size() / 3)) {
      continue;
    }
    candidates.push_back(Candidate{contains ? 0u : 1u, distance, stage.name});
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& lhs, const Candidate& rhs) {
              if (lhs.rank != rhs.rank) {
                return lhs.rank < rhs.rank;
              }
              if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
              }
              return lhs.name < rhs.name;
            });

  std::vector<std::string> names;
  for (const auto& candidate : candidates) {
    if (names.size() >= max_results) {
      break;
    }
    names.push_back(candidate.name);
  }
  return names;
}

}  // namespace orc::presenters
