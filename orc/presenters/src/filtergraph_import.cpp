/*
 * File:        filtergraph_import.cpp
 * Module:      orc-presenters
 * Purpose:     Implementation of the shared filtergraph-to-project builder.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "filtergraph_import.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "filtergraph_parser.h"
#include "i_project_presenter.h"
#include "project_presenter_types.h"

namespace orc {
namespace presenters {

namespace {

/// Result of auto-detecting the project's video format from its stages.
struct VideoFormatInference {
  VideoFormat format = VideoFormat::Unknown;
  bool conflict = false;
  std::string conflict_detail;
};

/**
 * Auto-detect the project's video format from the stages actually used,
 * with no naming assumptions: a stage's format exclusivity comes from its
 * formally declared VideoFormatCompatibility (NTSC_ONLY / PAL_ONLY /
 * PAL_M_ONLY), the same metadata the GUI uses to filter its "Add Stage"
 * menu. Stages compatible with every format (the common case: transforms,
 * sinks, and dual-mode sources like tbc_source) contribute no signal.
 */
VideoFormatInference infer_video_format(
    IProjectPresenter& presenter, const std::vector<FilterStage>& stages) {
  VideoFormatInference result;

  std::set<std::string> ntsc_names, pal_names, pal_m_names;
  for (auto& s : presenter.listAvailableStagesForFormat(VideoFormat::NTSC)) {
    ntsc_names.insert(s.name);
  }
  for (auto& s : presenter.listAvailableStagesForFormat(VideoFormat::PAL)) {
    pal_names.insert(s.name);
  }
  for (auto& s : presenter.listAvailableStagesForFormat(VideoFormat::PAL_M)) {
    pal_m_names.insert(s.name);
  }

  std::string detected_from;
  for (const auto& stage : stages) {
    const bool in_ntsc = ntsc_names.count(stage.stage_name) != 0;
    const bool in_pal = pal_names.count(stage.stage_name) != 0;
    const bool in_pal_m = pal_m_names.count(stage.stage_name) != 0;
    const int membership =
        (in_ntsc ? 1 : 0) + (in_pal ? 1 : 0) + (in_pal_m ? 1 : 0);
    if (membership == 0 || membership == 3) {
      continue;  // Not a registered stage, or compatible with every format.
    }
    const VideoFormat this_format =
        in_ntsc ? VideoFormat::NTSC
                : (in_pal ? VideoFormat::PAL : VideoFormat::PAL_M);
    if (result.format == VideoFormat::Unknown) {
      result.format = this_format;
      detected_from = stage.stage_name;
    } else if (result.format != this_format) {
      result.conflict = true;
      result.conflict_detail = "stage '" + detected_from +
                               "' requires one video format but '" +
                               stage.stage_name + "' requires another";
      return result;
    }
  }
  return result;
}

/// Result of auto-detecting the project's source signal type from its stages.
struct SourceFormatInference {
  SourceType format = SourceType::Unknown;
  bool conflict = false;
  std::string conflict_detail;
};

/**
 * Auto-detect the project's source signal type (composite vs Y/C) from the
 * parameters actually supplied to source stages: y_path AND c_path both
 * present and non-empty implies Y/C; input_path present and non-empty
 * implies composite — the same convention, and the same presence-and-
 * non-empty check, the core's own set_node_parameters() uses (project.cpp),
 * so this works without hardcoding any specific stage name.
 */
/// True if `stage` has a non-empty string value for parameter `key`. A key
/// that is merely *present* with an empty value (e.g. a composite source
/// that still lists `y_path=''` for symmetry) must not count — matching
/// the core's own `has_nonempty_str()` check exactly (project.cpp).
bool has_nonempty_param(const FilterStage& stage, const std::string& key) {
  const auto it = stage.params.find(key);
  return it != stage.params.end() && !it->second.empty();
}

/**
 * Convert one `key=value` text from a filtergraph into the ParameterValue
 * alternative the stage actually reads.
 *
 * A filtergraph is text, so every value arrives as a string; stages read
 * their parameters with std::holds_alternative against the type they
 * declared. Storing everything as a string therefore made every non-string
 * parameter invisible to its stage, which silently fell back to the default.
 * The declared type is the authority for the conversion, and a value that
 * does not fit it is an error rather than a default.
 *
 * @param descriptor Declared parameter, whose `type` selects the alternative
 * @param text       Value exactly as typed in the filtergraph
 * @param error      Set to a user-facing explanation when conversion fails
 * @return The typed value, or nullopt when @p text does not fit the type
 */
std::optional<orc::ParameterValue> coerce_parameter(
    const ParameterDescriptor& descriptor, const std::string& text,
    std::string* error) {
  // Leading/trailing spaces survive shell quoting more often than they are
  // meant, and no numeric or boolean literal needs them.
  const auto first = text.find_first_not_of(" \t");
  const auto last = text.find_last_not_of(" \t");
  const std::string trimmed = first == std::string::npos
                                  ? std::string()
                                  : text.substr(first, last - first + 1);

  const auto whole_number = [&](int64_t min_value,
                                int64_t max_value) -> std::optional<int64_t> {
    if (trimmed.empty()) {
      *error = "expected a whole number";
      return std::nullopt;
    }
    std::size_t consumed = 0;
    int64_t value = 0;
    try {
      value = static_cast<int64_t>(std::stoll(trimmed, &consumed));
    } catch (const std::exception&) {
      *error = "'" + text + "' is not a whole number";
      return std::nullopt;
    }
    if (consumed != trimmed.size()) {
      *error = "'" + text + "' is not a whole number";
      return std::nullopt;
    }
    if (value < min_value || value > max_value) {
      *error = "'" + text + "' is out of range for this parameter";
      return std::nullopt;
    }
    return value;
  };

  switch (descriptor.type) {
    case ParameterType::BOOL: {
      // The spellings a person actually types on a command line; the project
      // writer emits "true"/"false", which round-trips through the first two.
      std::string lowered = trimmed;
      std::transform(
          lowered.begin(), lowered.end(), lowered.begin(),
          [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (lowered == "true" || lowered == "1" || lowered == "yes" ||
          lowered == "on") {
        return orc::ParameterValue{true};
      }
      if (lowered == "false" || lowered == "0" || lowered == "no" ||
          lowered == "off") {
        return orc::ParameterValue{false};
      }
      *error = "'" + text + "' is not true or false";
      return std::nullopt;
    }
    case ParameterType::INT32: {
      const auto value = whole_number(INT32_MIN, INT32_MAX);
      if (!value) {
        return std::nullopt;
      }
      return orc::ParameterValue{static_cast<int32_t>(*value)};
    }
    case ParameterType::UINT32: {
      const auto value = whole_number(0, UINT32_MAX);
      if (!value) {
        return std::nullopt;
      }
      return orc::ParameterValue{static_cast<uint32_t>(*value)};
    }
    case ParameterType::DOUBLE: {
      if (trimmed.empty()) {
        *error = "expected a number";
        return std::nullopt;
      }
      std::size_t consumed = 0;
      double value = 0.0;
      try {
        value = std::stod(trimmed, &consumed);
      } catch (const std::exception&) {
        *error = "'" + text + "' is not a number";
        return std::nullopt;
      }
      if (consumed != trimmed.size()) {
        *error = "'" + text + "' is not a number";
        return std::nullopt;
      }
      return orc::ParameterValue{value};
    }
    case ParameterType::STRING:
    case ParameterType::FILE_PATH:
      break;
  }
  // Strings and paths are stored exactly as typed: trimming a path would
  // change what it means.
  return orc::ParameterValue{text};
}

SourceFormatInference infer_source_format(
    const std::vector<FilterStage>& stages,
    const std::map<std::string, StageInfo>& stage_index) {
  SourceFormatInference result;

  std::string detected_from;
  for (const auto& stage : stages) {
    auto it = stage_index.find(stage.stage_name);
    if (it == stage_index.end() || !it->second.is_source) {
      continue;
    }

    SourceType this_format = SourceType::Unknown;
    // Y/C requires *both* y_path and c_path — matching the core's own
    // has_nonempty_str("y_path") && has_nonempty_str("c_path") exactly.
    // Either alone is not enough: a composite source may still carry an
    // empty y_path/c_path key for parameter symmetry, and that must not be
    // mistaken for Y/C.
    if (has_nonempty_param(stage, "y_path") &&
        has_nonempty_param(stage, "c_path")) {
      this_format = SourceType::YC;
    } else if (has_nonempty_param(stage, "input_path")) {
      this_format = SourceType::Composite;
    }
    if (this_format == SourceType::Unknown) {
      continue;
    }

    if (result.format == SourceType::Unknown) {
      result.format = this_format;
      detected_from = stage.stage_name;
    } else if (result.format != this_format) {
      result.conflict = true;
      result.conflict_detail = "source '" + detected_from +
                               "' looks like one signal type but '" +
                               stage.stage_name + "' looks like another";
      return result;
    }
  }
  return result;
}

}  // namespace

FiltergraphImportResult import_filtergraph_into_project(
    IProjectPresenter& presenter, const std::string& filtergraph) {
  FiltergraphImportResult result;

  FilterGraphParseResult parsed = parse_filtergraph(filtergraph);
  if (!parsed.ok) {
    result.errors.push_back("Failed to parse filtergraph: " + parsed.error);
    return result;
  }

  // Validate stage names up front so callers get a clear message rather
  // than an opaque failure deep in DAG construction.
  for (const auto& stage : parsed.graph.stages) {
    if (!presenter.stageExists(stage.stage_name)) {
      result.errors.push_back("Unknown stage '" + stage.stage_name + "'.");
    }
  }
  if (!result.errors.empty()) {
    return result;
  }

  // Build a stage-name -> StageInfo index once, reused for source-format
  // inference and parameter validation below.
  std::map<std::string, StageInfo> stage_index;
  for (auto& info : presenter.listAllStages()) {
    stage_index.emplace(info.name, info);
  }

  // Auto-detect video format and source signal type from the stages
  // actually used — these are properties of the modules, not something the
  // caller should have to state separately.
  const VideoFormatInference video_inference =
      infer_video_format(presenter, parsed.graph.stages);
  if (video_inference.conflict) {
    result.errors.push_back("Conflicting video formats between stages: " +
                            video_inference.conflict_detail);
  }
  const SourceFormatInference source_inference =
      infer_source_format(parsed.graph.stages, stage_index);
  if (source_inference.conflict) {
    result.errors.push_back("Conflicting source signal types between stages: " +
                            source_inference.conflict_detail);
  }
  if (!result.errors.empty()) {
    return result;
  }

  // Set the detected format/source-type *before* fetching parameter
  // descriptors below: some stages narrow their descriptor set by format or
  // source type (see FixedFormatCVBSSourceStage::get_parameter_descriptors),
  // so descriptors must be fetched with the real context already in place,
  // not with Unknown.
  if (video_inference.format != VideoFormat::Unknown) {
    presenter.setVideoFormat(video_inference.format);
  }
  if (source_inference.format != SourceType::Unknown) {
    presenter.setSourceType(source_inference.format);
  }

  // Validate parameters against each stage's descriptors. This uses only
  // the generic getStageParameters() API, so it works identically for core
  // stages and for third-party plugin stages: missing required parameters
  // are reported as errors (instead of an opaque failure later inside the
  // stage's trigger()), and unrecognised parameter names are reported as
  // warnings (likely typos) without blocking the build.
  //
  // The same pass converts each value to the alternative its descriptor
  // declares. Filtergraph text is untyped, so this is the only point at which
  // a bool or a number can be recognised as one; without it every non-string
  // parameter reached the stage as a string and was silently ignored.
  std::vector<std::map<std::string, orc::ParameterValue>> typed_params(
      parsed.graph.stages.size());
  for (std::size_t index = 0; index < parsed.graph.stages.size(); ++index) {
    const auto& stage = parsed.graph.stages[index];
    const auto descriptors = presenter.getStageParameters(stage.stage_name);
    if (descriptors.empty()) {
      // Nothing declares these, so there is no type to convert to; pass them
      // through unchanged and let the stage decide.
      for (const auto& [key, value] : stage.params) {
        typed_params[index][key] = value;
      }
      continue;
    }

    for (const auto& descriptor : descriptors) {
      const bool supplied =
          stage.params.find(descriptor.name) != stage.params.end();
      if (descriptor.constraints.required && !supplied) {
        result.errors.push_back("Stage '" + stage.stage_name +
                                "': missing required parameter '" +
                                descriptor.name + "'.");
      }
    }

    // Named rather than destructured: the descriptor lookup below captures
    // the key, and capturing a structured binding is only legal from C++20.
    for (const auto& entry : stage.params) {
      const auto& key = entry.first;
      const auto& value = entry.second;
      const auto descriptor =
          std::find_if(descriptors.begin(), descriptors.end(),
                       [&key](const auto& d) { return d.name == key; });
      if (descriptor == descriptors.end()) {
        result.warnings.push_back(
            "Stage '" + stage.stage_name + "': parameter '" + key +
            "' is not recognised by this stage (check for typos).");
        typed_params[index][key] = value;
        continue;
      }
      std::string conversion_error;
      const auto coerced =
          coerce_parameter(*descriptor, value, &conversion_error);
      if (!coerced.has_value()) {
        result.errors.push_back("Stage '" + stage.stage_name +
                                "': parameter '" + key +
                                "': " + conversion_error + ".");
        continue;
      }
      typed_params[index][key] = *coerced;
    }
  }
  if (!result.errors.empty()) {
    // Parameter validation failed after all: undo the format/source-type
    // set above, so a failed import leaves the presenter completely
    // unchanged either way (see the contract documented in
    // filtergraph_import.h).
    if (video_inference.format != VideoFormat::Unknown) {
      presenter.setVideoFormat(VideoFormat::Unknown);
    }
    if (source_inference.format != SourceType::Unknown) {
      presenter.setSourceType(SourceType::Unknown);
    }
    return result;
  }

  // Add nodes (auto-layout left to right) and their parameters, then wire up
  // edges. Core-side validation (e.g. source format compatibility) can throw
  // for any stage, including third-party plugin stages, so surface those as
  // a clean error rather than letting the exception propagate.
  try {
    std::vector<orc::NodeID> node_ids;
    node_ids.reserve(parsed.graph.stages.size());
    double x = 0.0;
    for (std::size_t index = 0; index < parsed.graph.stages.size(); ++index) {
      const auto& stage = parsed.graph.stages[index];
      orc::NodeID id = presenter.addNode(stage.stage_name, x, 0.0);
      node_ids.push_back(id);
      if (!typed_params[index].empty() &&
          !presenter.setNodeParameters(id, typed_params[index])) {
        // Same failure path as any other core-side rejection below, so the
        // rollback in the catch block covers it too.
        throw std::runtime_error("stage '" + stage.stage_name +
                                 "': parameters were rejected");
      }
      x += 200.0;
    }

    for (const auto& edge : parsed.graph.edges) {
      presenter.addEdge(node_ids[edge.first], node_ids[edge.second]);
    }
  } catch (const std::exception& e) {
    result.errors.push_back(std::string("Failed to build filtergraph: ") +
                            e.what());
    // Same rollback as the parameter-validation failure above.
    if (video_inference.format != VideoFormat::Unknown) {
      presenter.setVideoFormat(VideoFormat::Unknown);
    }
    if (source_inference.format != SourceType::Unknown) {
      presenter.setSourceType(SourceType::Unknown);
    }
    return result;
  }

  result.ok = true;
  return result;
}

}  // namespace presenters
}  // namespace orc
