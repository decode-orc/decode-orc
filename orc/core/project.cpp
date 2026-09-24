/*
 * File:        project.cpp
 * Module:      orc-core
 * Purpose:     Project
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 *
 * ARCHITECTURE NOTE - STRICT ENCAPSULATION:
 * ==========================================
 * This file implements the ONLY functions that can modify Project state.
 *
 * All Project fields are PRIVATE. This file accesses them via friend
 * declarations in project.h.
 *
 * CRITICAL RULES:
 * 1. ALL Project modifications MUST go through project_io:: functions
 * 2. GUI/CLI code CANNOT directly modify Project fields
 * 3. All project_io functions MUST set is_modified_ = true when changing state
 * 4. Project fields can ONLY be read via public const getters externally
 *
 * When adding new functionality:
 * - Add a new project_io:: function here
 * - Add friend declaration in project.h
 * - Forward-declare the function in project.h before the Project class
 * - Update GUI/CLI to use the new function
 *
 * DO NOT bypass this architecture by making Project fields public.
 */

#include "project.h"

#include <orc/plugin/orc_stage_tooling.h>
#include <orc/stage/common_types.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/support/logging.h>
#include <sqlite3.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>

#include "dag_executor.h"
#include "include/stage_plugin_registry.h"
#include "project_to_dag.h"
#include "stage_registry.h"

namespace orc {

namespace {

std::filesystem::path resolve_project_root_for_filename(
    const std::string& filename) {
  std::filesystem::path yaml_path;
  try {
    yaml_path = std::filesystem::absolute(filename);
  } catch (const std::filesystem::filesystem_error& e) {
    throw std::runtime_error("Failed to resolve project file path '" +
                             filename + "': " + e.what());
  }

  return yaml_path.parent_path();
}

ProjectPluginRequirement requirement_from_registry_entry(
    const StagePluginRegistryEntry& entry) {
  ProjectPluginRequirement requirement;
  requirement.plugin_id = entry.plugin_id;
  requirement.plugin_version = entry.plugin_version;
  requirement.source_repo_url = entry.source_repo_url;
  requirement.artifact_source = entry.artifact_source;
  requirement.release_asset_url = entry.release_asset_url;
  requirement.release_tag = entry.release_tag;
  requirement.release_asset_name = entry.release_asset_name;
  requirement.target_platform = entry.target_platform;
  requirement.local_dev_path = entry.local_dev_path;
  requirement.license_spdx = entry.license_spdx;
  requirement.is_core_plugin = entry.is_core_plugin;
  requirement.required_host_abi = entry.required_host_abi;
  return requirement;
}

ProjectPluginRequirement requirement_from_loaded_plugin(
    const LoadedStagePlugin& plugin) {
  ProjectPluginRequirement requirement;
  requirement.plugin_id = plugin.plugin_id;
  requirement.plugin_version = plugin.plugin_version;
  requirement.license_spdx = plugin.license_spdx;
  requirement.is_core_plugin = plugin.is_core_plugin;
  return requirement;
}

void merge_requirement_metadata(ProjectPluginRequirement& target,
                                const ProjectPluginRequirement& source) {
  if (target.plugin_version.empty()) {
    target.plugin_version = source.plugin_version;
  }
  if (target.source_repo_url.empty()) {
    target.source_repo_url = source.source_repo_url;
  }
  if (target.artifact_source == "local_path" &&
      source.artifact_source != "local_path") {
    target.artifact_source = source.artifact_source;
  }
  if (target.release_asset_url.empty()) {
    target.release_asset_url = source.release_asset_url;
  }
  if (target.release_tag.empty()) {
    target.release_tag = source.release_tag;
  }
  if (target.release_asset_name.empty()) {
    target.release_asset_name = source.release_asset_name;
  }
  if (target.target_platform.empty()) {
    target.target_platform = source.target_platform;
  }
  if (target.local_dev_path.empty()) {
    target.local_dev_path = source.local_dev_path;
  }
  if (target.license_spdx.empty()) {
    target.license_spdx = source.license_spdx;
  }
  if (target.required_host_abi == 0) {
    target.required_host_abi = source.required_host_abi;
  }
  target.is_core_plugin = target.is_core_plugin || source.is_core_plugin;
}

void add_unique_stage_name(ProjectPluginRequirement& requirement,
                           const std::string& stage_name) {
  if (std::find(requirement.stage_names.begin(), requirement.stage_names.end(),
                stage_name) == requirement.stage_names.end()) {
    requirement.stage_names.push_back(stage_name);
  }
}

std::vector<ProjectPluginRequirement> collect_required_plugins_for_project(
    const Project& project) {
  std::set<std::string> used_stage_names;
  for (const auto& node : project.get_nodes()) {
    if (!node.stage_name.empty()) {
      used_stage_names.insert(node.stage_name);
    }
  }

  const auto& registry = StageRegistry::instance();
  std::map<std::string, std::string> stage_to_plugin_id;
  std::map<std::string, ProjectPluginRequirement>
      current_requirement_by_plugin_id;

  for (const auto& plugin : registry.get_loaded_plugins()) {
    if (plugin.is_core_plugin || plugin.plugin_id.empty()) {
      continue;
    }

    current_requirement_by_plugin_id.emplace(
        plugin.plugin_id, requirement_from_loaded_plugin(plugin));
    for (const auto& stage_name : plugin.registered_stage_names) {
      stage_to_plugin_id[stage_name] = plugin.plugin_id;
    }
  }

  for (const auto& entry : registry.get_plugin_registry_entries()) {
    if (entry.is_core_plugin || entry.plugin_id.empty()) {
      continue;
    }

    auto [it, inserted] = current_requirement_by_plugin_id.emplace(
        entry.plugin_id, requirement_from_registry_entry(entry));
    if (!inserted) {
      merge_requirement_metadata(it->second,
                                 requirement_from_registry_entry(entry));
    }
  }

  std::map<std::string, ProjectPluginRequirement> required_by_plugin_id;
  for (const auto& stage_name : used_stage_names) {
    const auto stage_it = stage_to_plugin_id.find(stage_name);
    if (stage_it == stage_to_plugin_id.end()) {
      continue;
    }

    const std::string& plugin_id = stage_it->second;
    auto current_requirement_it =
        current_requirement_by_plugin_id.find(plugin_id);
    if (current_requirement_it == current_requirement_by_plugin_id.end()) {
      continue;
    }

    auto [required_it, inserted] = required_by_plugin_id.emplace(
        plugin_id, current_requirement_it->second);
    if (!inserted) {
      merge_requirement_metadata(required_it->second,
                                 current_requirement_it->second);
    }
    add_unique_stage_name(required_it->second, stage_name);
  }

  for (const auto& loaded_requirement : project.get_required_plugins()) {
    if (loaded_requirement.is_core_plugin ||
        loaded_requirement.plugin_id.empty()) {
      continue;
    }

    std::vector<std::string> surviving_stage_names;
    for (const auto& stage_name : loaded_requirement.stage_names) {
      if (used_stage_names.find(stage_name) != used_stage_names.end()) {
        surviving_stage_names.push_back(stage_name);
      }
    }

    if (surviving_stage_names.empty()) {
      continue;
    }

    auto [required_it, inserted] = required_by_plugin_id.emplace(
        loaded_requirement.plugin_id, loaded_requirement);
    if (!inserted) {
      merge_requirement_metadata(required_it->second, loaded_requirement);
      required_it->second.stage_names.clear();
    }

    required_it->second.stage_names.clear();
    for (const auto& stage_name : surviving_stage_names) {
      add_unique_stage_name(required_it->second, stage_name);
    }

    auto current_requirement_it =
        current_requirement_by_plugin_id.find(loaded_requirement.plugin_id);
    if (current_requirement_it != current_requirement_by_plugin_id.end()) {
      merge_requirement_metadata(required_it->second,
                                 current_requirement_it->second);
    }
  }

  std::vector<ProjectPluginRequirement> requirements;
  requirements.reserve(required_by_plugin_id.size());
  for (auto& [plugin_id, requirement] : required_by_plugin_id) {
    std::sort(requirement.stage_names.begin(), requirement.stage_names.end());
    requirements.push_back(std::move(requirement));
  }

  std::sort(requirements.begin(), requirements.end(),
            [](const auto& left, const auto& right) {
              return left.plugin_id < right.plugin_id;
            });
  return requirements;
}

YAML::Node to_yaml_node(const ProjectPluginRequirement& requirement) {
  YAML::Node node;
  node["plugin_id"] = requirement.plugin_id;
  node["plugin_version"] = requirement.plugin_version;
  node["source_repo_url"] = requirement.source_repo_url;
  node["artifact_source"] = requirement.artifact_source;
  node["release_asset_url"] = requirement.release_asset_url;
  node["release_tag"] = requirement.release_tag;
  node["release_asset_name"] = requirement.release_asset_name;
  node["target_platform"] = requirement.target_platform;
  node["local_dev_path"] = requirement.local_dev_path;
  node["license_spdx"] = requirement.license_spdx;
  node["is_core_plugin"] = requirement.is_core_plugin;
  node["required_host_abi"] = requirement.required_host_abi;

  YAML::Node stage_names(YAML::NodeType::Sequence);
  for (const auto& stage_name : requirement.stage_names) {
    stage_names.push_back(stage_name);
  }
  node["stage_names"] = stage_names;
  return node;
}

std::optional<ProjectPluginRequirement> parse_required_plugin_node(
    const YAML::Node& node) {
  if (!node.IsMap()) {
    return std::nullopt;
  }

  ProjectPluginRequirement requirement;
  requirement.plugin_id = node["plugin_id"].as<std::string>("");
  requirement.plugin_version = node["plugin_version"].as<std::string>("");
  requirement.source_repo_url = node["source_repo_url"].as<std::string>("");
  requirement.artifact_source =
      node["artifact_source"].as<std::string>("local_path");
  requirement.release_asset_url = node["release_asset_url"].as<std::string>("");
  requirement.release_tag = node["release_tag"].as<std::string>("");
  requirement.release_asset_name =
      node["release_asset_name"].as<std::string>("");
  requirement.target_platform = node["target_platform"].as<std::string>("");
  requirement.local_dev_path = node["local_dev_path"].as<std::string>("");
  requirement.license_spdx = node["license_spdx"].as<std::string>("");
  requirement.is_core_plugin = node["is_core_plugin"].as<bool>(false);
  requirement.required_host_abi = node["required_host_abi"].as<uint32_t>(0);

  const auto stage_names = node["stage_names"];
  if (stage_names && stage_names.IsSequence()) {
    for (const auto& stage_name : stage_names) {
      const auto value = stage_name.as<std::string>("");
      if (!value.empty()) {
        requirement.stage_names.push_back(value);
      }
    }
  }

  if (requirement.plugin_id.empty() || requirement.stage_names.empty()) {
    return std::nullopt;
  }

  std::sort(requirement.stage_names.begin(), requirement.stage_names.end());
  requirement.stage_names.erase(std::unique(requirement.stage_names.begin(),
                                            requirement.stage_names.end()),
                                requirement.stage_names.end());
  return requirement;
}

// Returns true for source stages whose signal type (composite vs Y/C) is
// determined by their file-path parameters rather than by the stage name.
bool is_dual_mode_source(const std::string& stage_name) {
  return stage_name == "tbc_source" || stage_name == "NTSC_CVBS_Source" ||
         stage_name == "PAL_CVBS_Source" || stage_name == "PAL_M_CVBS_Source";
}

// Determine the signal type of a source node from its stored parameters.
// For dual-mode sources the type depends on which file-path parameters are set:
//   y_path + c_path → Y/C;  input_path → Composite;  neither → Unknown.
// For non-dual-mode sources the type is inferred from the stage name.
SourceType get_node_signal_type(const ProjectDAGNode& node) {
  auto has_nonempty_str = [&](const std::string& key) {
    const auto it = node.parameters.find(key);
    return it != node.parameters.end() &&
           std::holds_alternative<std::string>(it->second) &&
           !std::get<std::string>(it->second).empty();
  };
  if (is_dual_mode_source(node.stage_name)) {
    if (has_nonempty_str("y_path") && has_nonempty_str("c_path")) {
      return SourceType::YC;
    }
    if (has_nonempty_str("input_path")) {
      return SourceType::Composite;
    }
    return SourceType::Unknown;
  }
  if (node.stage_name.find("YC") != std::string::npos ||
      node.stage_name.find("Yc") != std::string::npos ||
      node.stage_name.find("yc") != std::string::npos) {
    return SourceType::YC;
  }
  return SourceType::Composite;
}

// Determine the video format of a source node from its stage name.
// PALM must be checked before PAL to avoid a substring false-match.
// Returns Unknown for tbc_source (format is determined by the .db metadata
// file, not the stage name itself).
VideoSystem get_node_video_format(const ProjectDAGNode& node) {
  const auto& name = node.stage_name;
  if (name.find("NTSC") != std::string::npos) {
    return VideoSystem::NTSC;
  }
  if (name.find("PALM") != std::string::npos) {
    return VideoSystem::PAL_M;
  }
  if (name.find("PAL") != std::string::npos) {
    return VideoSystem::PAL;
  }
  return VideoSystem::Unknown;
}

}  // namespace

// Project class method implementations
bool Project::has_source() const {
  for (const auto& node : nodes_) {
    if (node.node_type == NodeType::SOURCE) {
      return true;
    }
  }
  return false;
}

SourceType Project::get_source_type() const {
  for (const auto& node : nodes_) {
    if (node.node_type == NodeType::SOURCE) {
      const SourceType t = get_node_signal_type(node);
      if (t != SourceType::Unknown) return t;
    }
  }
  return SourceType::Unknown;
}

namespace project_io {

namespace {

// Convert NodeType to string for serialization
// We save the node type as its enum name for clarity
std::string node_type_to_string(NodeType type) {
  switch (type) {
    case NodeType::SOURCE:
      return "SOURCE";
    case NodeType::SINK:
      return "SINK";
    case NodeType::TRANSFORM:
      return "TRANSFORM";
    case NodeType::MERGER:
      return "MERGER";
    case NodeType::COMPLEX:
      return "COMPLEX";
    case NodeType::ANALYSIS_SINK:
      return "ANALYSIS_SINK";
    default:
      return "UNKNOWN";
  }
}

// Convert SourceType to string for serialization
std::string source_type_to_string(SourceType type) {
  switch (type) {
    case SourceType::Composite:
      return "Composite";
    case SourceType::YC:
      return "YC";
    case SourceType::Unknown:
    default:
      return "Unknown";
  }
}

// Convert string to SourceType for deserialization
SourceType source_type_from_string(const std::string& str) {
  if (str == "Composite") return SourceType::Composite;
  if (str == "YC") return SourceType::YC;
  return SourceType::Unknown;
}

// Convert string to NodeType for deserialization
NodeType string_to_node_type(const std::string& str) {
  if (str == "SOURCE") return NodeType::SOURCE;
  if (str == "SINK") return NodeType::SINK;
  if (str == "TRANSFORM") return NodeType::TRANSFORM;
  if (str == "MERGER") return NodeType::MERGER;
  if (str == "COMPLEX") return NodeType::COMPLEX;
  if (str == "ANALYSIS_SINK") return NodeType::ANALYSIS_SINK;
  // Default to TRANSFORM for unknown types (backward compatibility)
  return NodeType::TRANSFORM;
}

}  // anonymous namespace

Project load_project(const std::string& filename) {
  std::filesystem::path yaml_path;
  try {
    yaml_path = std::filesystem::weakly_canonical(filename);
  } catch (const std::filesystem::filesystem_error& e) {
    throw std::runtime_error("Failed to resolve project file path '" +
                             filename + "': " + e.what());
  }

  ORC_LOG_DEBUG("Loading project from: {}", yaml_path.string());
  ORC_LOG_DEBUG("Project root directory: {}", yaml_path.parent_path().string());

  std::string yaml_text;
  try {
    YAML::Node root = YAML::LoadFile(yaml_path.string());
    YAML::Emitter emitter;
    emitter << root;
    yaml_text = emitter.c_str();
  } catch (const YAML::Exception& e) {
    throw std::runtime_error("Failed to parse YAML file '" + filename +
                             "': " + e.what());
  }

  return load_project_from_yaml(yaml_text, yaml_path.string());
}

Project load_project_from_yaml(const std::string& yaml_text,
                               const std::string& filename_hint) {
  std::filesystem::path yaml_path;
  try {
    yaml_path = std::filesystem::absolute(filename_hint);
  } catch (const std::filesystem::filesystem_error& e) {
    throw std::runtime_error("Failed to resolve project file path '" +
                             filename_hint + "': " + e.what());
  }

  Project project;
  project.project_root_ = yaml_path.parent_path().string();

  YAML::Node root;
  try {
    root = YAML::Load(yaml_text);
  } catch (const YAML::Exception& e) {
    throw std::runtime_error("Failed to parse YAML file '" + filename_hint +
                             "': " + e.what());
  }

  // Validate project section exists
  if (!root["project"]) {
    throw std::runtime_error("Invalid project file '" + filename_hint +
                             "': missing required 'project' section");
  }

  // Validate project name (required)
  project.name_ = root["project"]["name"].as<std::string>("");
  if (project.name_.empty()) {
    throw std::runtime_error("Invalid project file '" + filename_hint +
                             "': project name is required");
  }

  project.description_ = root["project"]["description"].as<std::string>("");
  project.version_ = root["project"]["version"].as<std::string>("1.0");

  // Reject project files that are not v2.0. v1.x files are not supported.
  if (project.version_ != "2.0") {
    throw std::runtime_error(
        "Project format version '" + project.version_ +
        "' is not supported by Decode-Orc 2.x.\n"
        "Please recreate the project using Decode-Orc 2.0 or later.");
  }

  // Validate video format (required)
  if (!root["project"]["video_format"]) {
    throw std::runtime_error(
        "Invalid project file '" + filename_hint +
        "': missing required 'video_format' field. "
        "Please create a new project or manually add 'video_format: NTSC' or "
        "'video_format: PAL' to the project section.");
  }

  std::string format_str = root["project"]["video_format"].as<std::string>();
  project.video_format_ = video_system_from_string(format_str);
  if (project.video_format_ == VideoSystem::Unknown &&
      format_str != "Unknown") {
    throw std::runtime_error("Invalid project file '" + filename_hint +
                             "': invalid video_format '" + format_str +
                             "'. "
                             "Valid values are: NTSC, PAL, PAL-M, or Unknown");
  }

  // Load source format (required)
  if (!root["project"]["source_format"]) {
    throw std::runtime_error(
        "Invalid project file '" + filename_hint +
        "': missing required 'source_format' field. "
        "Please create a new project or manually add 'source_format: "
        "Composite' or 'source_format: YC' to the project section.");
  }

  std::string source_format_str =
      root["project"]["source_format"].as<std::string>();
  project.source_format_ = source_type_from_string(source_format_str);
  if (project.source_format_ == SourceType::Unknown &&
      source_format_str != "Unknown") {
    throw std::runtime_error("Invalid project file '" + filename_hint +
                             "': invalid source_format '" + source_format_str +
                             "'. "
                             "Valid values are: Composite, YC, or Unknown");
  }

  // Load amplitude display unit (required)
  if (!root["project"]["amplitude_unit"]) {
    throw std::runtime_error(
        "Invalid project file '" + filename_hint +
        "': missing required 'amplitude_unit' field. "
        "Please create a new project or manually add "
        "'amplitude_unit: IRE' (or 'mV' or '10bit') to the project section.");
  }

  std::string amplitude_unit_str =
      root["project"]["amplitude_unit"].as<std::string>();
  project.amplitude_unit_ = amplitude_unit_from_string(amplitude_unit_str);
  if (amplitude_unit_str != "IRE" && amplitude_unit_str != "mV" &&
      amplitude_unit_str != "10bit") {
    throw std::runtime_error("Invalid project file '" + filename_hint +
                             "': invalid amplitude_unit '" +
                             amplitude_unit_str +
                             "'. Valid values are: IRE, mV, or 10bit");
  }

  // Load DAG nodes
  if (root["dag"] && root["dag"]["nodes"] &&
      root["dag"]["nodes"].IsSequence()) {
    for (const auto& node_yaml : root["dag"]["nodes"]) {
      ProjectDAGNode node;
      node.node_id = NodeID(node_yaml["id"].as<int32_t>(0));
      node.stage_name = node_yaml["stage"].as<std::string>("");
      node.display_name = node_yaml["display_name"].as<std::string>("");

      // Migrate the legacy raw_video_sink and ffmpeg_video_sink stages to the
      // merged video_sink stage. The old stage name determines the output
      // mode; any stored output_format parameter is remapped further down
      // once parameters are loaded.
      std::string migrated_output_mode;
      if (node.stage_name == "raw_video_sink") {
        migrated_output_mode = "raw";
      } else if (node.stage_name == "ffmpeg_video_sink") {
        migrated_output_mode = "ffmpeg";
      }
      if (!migrated_output_mode.empty()) {
        ORC_LOG_INFO(
            "Migrating legacy stage '{}' (node {}) to 'video_sink' with "
            "output_mode '{}'",
            node.stage_name, node.node_id.to_string(), migrated_output_mode);
        node.stage_name = "video_sink";
        if (node.display_name == "Raw Video Sink" ||
            node.display_name == "FFmpeg Video Sink") {
          node.display_name = "Video Sink";
        }
        node.parameters["output_mode"] = migrated_output_mode;
      }

      // Migrate the legacy teletext_analysis_sink stage, which was renamed
      // when it was recategorised from an analysis sink to a sink. Its stored
      // node_type is corrected below, once the field has been parsed.
      const bool migrated_teletext_sink =
          (node.stage_name == "teletext_analysis_sink");
      if (migrated_teletext_sink) {
        ORC_LOG_INFO(
            "Migrating legacy stage 'teletext_analysis_sink' (node {}) to "
            "'teletext_sink'",
            node.node_id.to_string());
        node.stage_name = "teletext_sink";
        if (node.display_name == "Teletext Analysis Sink") {
          node.display_name = "Teletext Sink";
        }
      }

      // Migrate the legacy ld_sink stage, renamed to tbc_sink so it pairs
      // obviously with tbc_source (issue #258). Behaviour is unchanged, so
      // the node only needs its stage name and stored labels rewritten.
      const bool migrated_tbc_sink = (node.stage_name == "ld_sink");
      if (migrated_tbc_sink) {
        ORC_LOG_INFO("Migrating legacy stage 'ld_sink' (node {}) to 'tbc_sink'",
                     node.node_id.to_string());
        node.stage_name = "tbc_sink";
        if (node.display_name == "ld-decode Sink") {
          node.display_name = "TBC Sink";
        }
      }

      node.user_label = node_yaml["user_label"].as<std::string>(
          node.display_name);  // Default to display_name if not present

      // The old default label was stored verbatim; only an untouched one is
      // rewritten, so a label the user chose is left alone.
      if (migrated_tbc_sink && node.user_label == "ld-decode Sink") {
        node.user_label = node.display_name;
      }
      node.x_position = node_yaml["x"].as<double>(0.0);
      node.y_position = node_yaml["y"].as<double>(0.0);

      // Parse node_type if present (required field)
      if (node_yaml["node_type"]) {
        node.node_type = string_to_node_type(
            node_yaml["node_type"].as<std::string>("TRANSFORM"));
      } else {
        // Default to TRANSFORM if not specified
        node.node_type = NodeType::TRANSFORM;
      }

      // A project saved before the recategorisation carries ANALYSIS_SINK for
      // this node. The stored type decides which nodes the DAG treats as
      // outputs, so leaving it would keep the migrated node out of that set.
      if (migrated_teletext_sink) {
        node.node_type = NodeType::SINK;
      }

      // Load parameters
      if (node_yaml["parameters"]) {
        for (const auto& param : node_yaml["parameters"]) {
          std::string param_name = param.first.as<std::string>();
          auto param_map = param.second;
          std::string type = param_map["type"].as<std::string>("string");

          ORC_LOG_DEBUG("Loading parameter '{}' for node '{}', type={}",
                        param_name, node.node_id.to_string(), type);

          if (type == "int32" || type == "int" || type == "integer") {
            int value = param_map["value"].as<int>();
            node.parameters[param_name] = value;
            ORC_LOG_DEBUG("  Set to int: {}", value);
          } else if (type == "uint32") {
            node.parameters[param_name] = param_map["value"].as<uint32_t>();
          } else if (type == "double") {
            node.parameters[param_name] = param_map["value"].as<double>();
          } else if (type == "bool") {
            node.parameters[param_name] = param_map["value"].as<bool>();
          } else {
            // String parameter - store as-is to preserve original format
            // (relative/absolute/${PROJECT_ROOT})
            std::string value = param_map["value"].as<std::string>();
            node.parameters[param_name] = value;
          }
        }
      }

      // Legacy video sink migration: remap the stored output_format value to
      // the mode-specific parameter of the merged video_sink stage.
      if (!migrated_output_mode.empty()) {
        auto fmt_it = node.parameters.find("output_format");
        if (fmt_it != node.parameters.end()) {
          if (std::holds_alternative<std::string>(fmt_it->second)) {
            const std::string format_key = (migrated_output_mode == "raw")
                                               ? "raw_format"
                                               : "ffmpeg_format";
            node.parameters[format_key] = fmt_it->second;
          }
          node.parameters.erase("output_format");
        }
      }

      project.nodes_.push_back(node);
    }
  }

  // Load DAG edges
  if (root["dag"] && root["dag"]["edges"] &&
      root["dag"]["edges"].IsSequence()) {
    for (const auto& edge_yaml : root["dag"]["edges"]) {
      ProjectDAGEdge edge;
      edge.source_node_id = NodeID(edge_yaml["from"].as<int32_t>(0));
      edge.target_node_id = NodeID(edge_yaml["to"].as<int32_t>(0));
      project.edges_.push_back(edge);
    }
  }

  if (root["required_plugins"] && root["required_plugins"].IsSequence()) {
    for (const auto& plugin_yaml : root["required_plugins"]) {
      auto requirement = parse_required_plugin_node(plugin_yaml);
      if (requirement.has_value()) {
        project.required_plugins_.push_back(std::move(*requirement));
      }
    }
  }

  // Validate source stage nodes against the declared video_format and
  // source_format, mirroring the enforcement in add_node().
  for (const auto& node : project.nodes_) {
    if (node.node_type != NodeType::SOURCE) {
      continue;
    }

    if (project.video_format_ != VideoSystem::Unknown) {
      bool is_ntsc_stage = (node.stage_name.find("NTSC") != std::string::npos);
      bool is_pal_stage = (node.stage_name.find("PAL") != std::string::npos);
      if (is_ntsc_stage && project.video_format_ != VideoSystem::NTSC) {
        throw std::runtime_error(
            "Project video_format mismatch: project declares '" +
            video_system_to_string(project.video_format_) +
            "' but source stage '" + node.stage_name + "' (node " +
            node.node_id.to_string() + ") is NTSC.");
      }
      if (is_pal_stage && !(project.video_format_ == VideoSystem::PAL ||
                            project.video_format_ == VideoSystem::PAL_M)) {
        throw std::runtime_error(
            "Project video_format mismatch: project declares '" +
            video_system_to_string(project.video_format_) +
            "' but source stage '" + node.stage_name + "' (node " +
            node.node_id.to_string() + ") is PAL.");
      }
    }

    if (project.source_format_ != SourceType::Unknown) {
      // CVBS source stages and tbc_source are dual-mode; skip name-based check.
      const bool is_dual_mode = (node.stage_name == "tbc_source" ||
                                 node.stage_name == "NTSC_CVBS_Source" ||
                                 node.stage_name == "PAL_CVBS_Source" ||
                                 node.stage_name == "PAL_M_CVBS_Source");
      if (!is_dual_mode) {
        bool is_yc_stage = (node.stage_name.find("YC") != std::string::npos ||
                            node.stage_name.find("Yc") != std::string::npos ||
                            node.stage_name.find("_yc") != std::string::npos);
        if (is_yc_stage && project.source_format_ != SourceType::YC) {
          throw std::runtime_error(
              "Project source_format mismatch: project declares 'Composite' "
              "but source stage '" +
              node.stage_name + "' (node " + node.node_id.to_string() +
              ") is a YC source.");
        }
      }
    }
  }

  // Validate that all configured source nodes use a consistent signal type.
  // Mixing composite and Y/C sources within a project is not permitted.
  {
    SourceType reference_type = SourceType::Unknown;
    std::string reference_desc;
    for (const auto& node : project.nodes_) {
      if (node.node_type != NodeType::SOURCE) continue;
      const SourceType node_type = get_node_signal_type(node);
      if (node_type == SourceType::Unknown) continue;
      if (reference_type == SourceType::Unknown) {
        reference_type = node_type;
        reference_desc = "'" + node.display_name + "' (node " +
                         node.node_id.to_string() + ")";
      } else if (node_type != reference_type) {
        const std::string type_name =
            (node_type == SourceType::YC) ? "Y/C" : "composite";
        const std::string ref_name =
            (reference_type == SourceType::YC) ? "Y/C" : "composite";
        throw std::runtime_error(
            "Source signal type mismatch: source node '" + node.display_name +
            "' (node " + node.node_id.to_string() + ") is a " + type_name +
            " source, but " + reference_desc + " is a " + ref_name +
            " source. All sources in a project must use the same signal type.");
      }
    }
  }

  // Validate that all configured source nodes use a consistent video format.
  // PAL-M is distinct from PAL and NTSC; mixing formats within a project is
  // not permitted.
  {
    VideoSystem reference_fmt = VideoSystem::Unknown;
    std::string reference_desc;
    for (const auto& node : project.nodes_) {
      if (node.node_type != NodeType::SOURCE) continue;
      const VideoSystem fmt = get_node_video_format(node);
      if (fmt == VideoSystem::Unknown) continue;
      if (reference_fmt == VideoSystem::Unknown) {
        reference_fmt = fmt;
        reference_desc = "'" + node.display_name + "' (node " +
                         node.node_id.to_string() + ")";
      } else if (fmt != reference_fmt) {
        throw std::runtime_error(
            "Source video format mismatch: source node '" + node.display_name +
            "' (node " + node.node_id.to_string() + ") is a " +
            video_system_to_string(fmt) + " source, but " + reference_desc +
            " is a " + video_system_to_string(reference_fmt) +
            " source. All sources in a project must use the same video "
            "format.");
      }
    }
  }

  // Validate all loaded edges to ensure they comply with current connection
  // rules
  std::vector<std::string> validation_errors;
  for (const auto& edge : project.edges_) {
    // Find source and target nodes
    auto source_it = std::find_if(project.nodes_.begin(), project.nodes_.end(),
                                  [&edge](const ProjectDAGNode& n) {
                                    return n.node_id == edge.source_node_id;
                                  });
    auto target_it = std::find_if(project.nodes_.begin(), project.nodes_.end(),
                                  [&edge](const ProjectDAGNode& n) {
                                    return n.node_id == edge.target_node_id;
                                  });

    if (source_it == project.nodes_.end()) {
      validation_errors.push_back("Edge references non-existent source node: " +
                                  edge.source_node_id.to_string());
      continue;
    }
    if (target_it == project.nodes_.end()) {
      validation_errors.push_back("Edge references non-existent target node: " +
                                  edge.target_node_id.to_string());
      continue;
    }

    // Validate connection type compatibility
    if (!is_connection_valid(source_it->stage_name, target_it->stage_name)) {
      validation_errors.push_back(
          "Invalid connection: " + source_it->stage_name + " (" +
          edge.source_node_id.to_string() + ") -> " + target_it->stage_name +
          " (" + edge.target_node_id.to_string() +
          ") - incompatible stage types");
    }

    // Check fan-out constraint for MANY output stages
    const NodeTypeInfo* source_info = get_node_type_info(source_it->stage_name);
    if (source_info && source_info->min_outputs > 1) {
      // Count outgoing connections from this source
      uint32_t output_count = 0;
      for (const auto& e : project.edges_) {
        if (e.source_node_id == edge.source_node_id) {
          output_count++;
        }
      }
      if (output_count > 1) {
        validation_errors.push_back(
            "MANY output stage " + source_it->stage_name + " (" +
            edge.source_node_id.to_string() +
            ") has multiple outgoing connections (fan-out not allowed)");
      }
    }
  }

  // If there are validation errors, throw exception with details
  if (!validation_errors.empty()) {
    std::string error_msg = "Project file contains invalid connections:\n";
    for (const auto& err : validation_errors) {
      error_msg += "  - " + err + "\n";
    }
    error_msg +=
        "\nPlease fix these connections in the project file or recreate the "
        "project.";
    throw std::runtime_error(error_msg);
  }

  // Clear modification flag - project is freshly loaded
  project.clear_modified_flag();

  return project;
}

std::string serialize_project_to_yaml(const Project& project,
                                      const std::string& filename_hint) {
  (void)resolve_project_root_for_filename(filename_hint);

  YAML::Emitter out;
  out << YAML::BeginMap;

  // Project metadata
  out << YAML::Key << "project";
  out << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "name" << YAML::Value << project.name_;
  if (!project.description_.empty()) {
    out << YAML::Key << "description" << YAML::Value << project.description_;
  }
  out << YAML::Key << "version" << YAML::Value << "2.0";

  // Save video format if set
  if (project.video_format_ != VideoSystem::Unknown) {
    out << YAML::Key << "video_format" << YAML::Value
        << video_system_to_string(project.video_format_);
  }

  // Save source format if set
  if (project.source_format_ != SourceType::Unknown) {
    out << YAML::Key << "source_format" << YAML::Value
        << source_type_to_string(project.source_format_);
  }

  out << YAML::Key << "amplitude_unit" << YAML::Value
      << amplitude_unit_to_string(project.amplitude_unit_);

  out << YAML::EndMap;

  // DAG
  out << YAML::Key << "dag";
  out << YAML::Value << YAML::BeginMap;

  // Nodes
  out << YAML::Key << "nodes";
  out << YAML::Value << YAML::BeginSeq;
  for (const auto& node : project.nodes_) {
    out << YAML::BeginMap;
    out << YAML::Key << "id" << YAML::Value << node.node_id.value();
    out << YAML::Key << "stage" << YAML::Value << node.stage_name;
    out << YAML::Key << "node_type" << YAML::Value
        << node_type_to_string(node.node_type);
    if (!node.display_name.empty()) {
      out << YAML::Key << "display_name" << YAML::Value << node.display_name;
    }
    if (!node.user_label.empty()) {
      out << YAML::Key << "user_label" << YAML::Value << node.user_label;
    }
    out << YAML::Key << "x" << YAML::Value << node.x_position;
    out << YAML::Key << "y" << YAML::Value << node.y_position;

    // Parameters (if any)
    if (!node.parameters.empty()) {
      out << YAML::Key << "parameters";
      out << YAML::Value << YAML::BeginMap;
      for (const auto& [param_name, param_value] : node.parameters) {
        out << YAML::Key << param_name;
        out << YAML::Value << YAML::BeginMap;

        // Write parameter value based on type
        if (std::holds_alternative<int32_t>(param_value)) {
          out << YAML::Key << "type" << YAML::Value << "int32";
          out << YAML::Key << "value" << YAML::Value
              << std::get<int32_t>(param_value);
        } else if (std::holds_alternative<uint32_t>(param_value)) {
          out << YAML::Key << "type" << YAML::Value << "uint32";
          out << YAML::Key << "value" << YAML::Value
              << std::get<uint32_t>(param_value);
        } else if (std::holds_alternative<double>(param_value)) {
          out << YAML::Key << "type" << YAML::Value << "double";
          out << YAML::Key << "value" << YAML::Value
              << std::get<double>(param_value);
        } else if (std::holds_alternative<bool>(param_value)) {
          out << YAML::Key << "type" << YAML::Value << "bool";
          out << YAML::Key << "value" << YAML::Value
              << std::get<bool>(param_value);
        } else if (std::holds_alternative<std::string>(param_value)) {
          std::string value = std::get<std::string>(param_value);

          // Save string parameters as-is to preserve their original format
          // (relative paths, absolute paths, or ${PROJECT_ROOT} variables)
          out << YAML::Key << "type" << YAML::Value << "string";
          out << YAML::Key << "value" << YAML::Value << value;
        }
        out << YAML::EndMap;
      }
      out << YAML::EndMap;
    }
    out << YAML::EndMap;
  }
  out << YAML::EndSeq;

  // Edges
  out << YAML::Key << "edges";
  out << YAML::Value << YAML::BeginSeq;
  for (const auto& edge : project.edges_) {
    out << YAML::BeginMap;
    out << YAML::Key << "from" << YAML::Value << edge.source_node_id.value();
    out << YAML::Key << "to" << YAML::Value << edge.target_node_id.value();
    out << YAML::EndMap;
  }
  out << YAML::EndSeq;

  out << YAML::EndMap;  // end dag

  const auto required_plugins = collect_required_plugins_for_project(project);
  if (!required_plugins.empty()) {
    out << YAML::Key << "required_plugins";
    out << YAML::Value << YAML::BeginSeq;
    for (const auto& requirement : required_plugins) {
      out << to_yaml_node(requirement);
    }
    out << YAML::EndSeq;
  }

  out << YAML::EndMap;  // end root

  std::ostringstream file_text;
  file_text << "# ORC Project File\n";
  file_text << "# Version: 2.0\n\n";
  file_text << out.c_str();
  return file_text.str();
}

void save_project(Project& project, const std::string& filename) {
  std::string yaml_text = serialize_project_to_yaml(project, filename);

  // Write to file
  std::ofstream file(filename);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open file for writing: " + filename);
  }
  file << yaml_text;
  file.close();

  // Adopt the saved file's directory as the project root.  Relative file-path
  // parameters are stored relative to the project file, and are resolved
  // against project_root_ when the DAG is built (see project_to_dag.cpp).
  // Without this a project created in-session — which has no root until it is
  // reloaded from disk — leaves those paths unresolved, and every relative
  // path appears to be missing until the user closes and reopens the project.
  project.project_root_ = resolve_project_root_for_filename(filename).string();

  // Clear modification flag - project has been saved
  project.clear_modified_flag();
}

Project create_empty_project(const std::string& project_name,
                             VideoSystem video_format,
                             SourceType source_format) {
  Project project;
  project.name_ = project_name;
  project.version_ = "2.0";
  project.video_format_ = video_format;
  project.source_format_ = source_format;
  project.amplitude_unit_ = default_amplitude_unit(video_format);
  // Mark as modified since it's a newly created project
  project.is_modified_ = true;
  return project;
}

void update_project_dag(Project& project,
                        const std::vector<ProjectDAGNode>& nodes_,
                        const std::vector<ProjectDAGEdge>& edges) {
  // Preserve SOURCE nodes_
  std::vector<ProjectDAGNode> source_nodes;
  for (const auto& node : project.nodes_) {
    if (node.node_type == NodeType::SOURCE) {
      source_nodes.push_back(node);
    }
  }

  // Clear all nodes_ and edges
  project.nodes_.clear();
  project.edges_.clear();

  // Restore SOURCE nodes_
  for (const auto& source_node : source_nodes) {
    project.nodes_.push_back(source_node);
  }

  // Add new nodes_ (should not include SOURCE nodes_ - those are managed
  // separately)
  for (const auto& node : nodes_) {
    if (node.node_type != NodeType::SOURCE) {
      project.nodes_.push_back(node);
    }
  }

  // Add new edges
  for (const auto& edge : edges) {
    project.edges_.push_back(edge);
  }

  project.is_modified_ = true;
}

NodeID generate_unique_node_id(const Project& project) {
  int32_t max_id = 0;

  // Scan all existing nodes to find the highest ID
  for (const auto& node : project.nodes_) {
    if (node.node_id.value() > max_id) {
      max_id = node.node_id.value();
    }
  }

  // Return next available ID
  return NodeID(max_id + 1);
}

NodeID add_node(Project& project, const std::string& stage_name,
                double x_position, double y_position) {
  // Validate that project has been initialized
  if (project.name_.empty()) {
    throw std::runtime_error(
        "Cannot add node to uninitialized project. Create or load a project "
        "first.");
  }

  // Validate stage name
  const NodeTypeInfo* type_info = get_node_type_info(stage_name);
  if (!type_info) {
    throw std::runtime_error("Invalid stage name: " + stage_name);
  }

  // Validate source stage compatibility with project's video and source format
  if (type_info->type == NodeType::SOURCE) {
    // Check source_format if set.
    // tbc_source and the CVBS source stages are dual-mode: whether they carry
    // composite or YC signal is determined by their file-path parameters, not
    // by the stage name, so they are valid for any source format.
    const bool is_dual_mode_source =
        (stage_name == "tbc_source" || stage_name == "NTSC_CVBS_Source" ||
         stage_name == "PAL_CVBS_Source" || stage_name == "PAL_M_CVBS_Source");
    if (!is_dual_mode_source && project.source_format_ != SourceType::Unknown) {
      bool is_yc_stage = (stage_name.find("YC") != std::string::npos);
      SourceType stage_type =
          is_yc_stage ? SourceType::YC : SourceType::Composite;
      if (stage_type != project.source_format_) {
        std::string expected_name =
            (project.source_format_ == SourceType::YC) ? "YC" : "Composite";
        std::string stage_type_name = is_yc_stage ? "YC" : "Composite";
        throw std::runtime_error(
            "Cannot add " + stage_type_name + " source stage '" + stage_name +
            "' to a project configured for " + expected_name + " sources.");
      }
    }

    // Check video_format if set
    if (project.video_format_ != VideoSystem::Unknown) {
      bool is_ntsc_stage = (stage_name.find("NTSC") != std::string::npos);
      bool is_pal_stage = (stage_name.find("PAL") != std::string::npos);
      if (is_ntsc_stage && project.video_format_ != VideoSystem::NTSC) {
        throw std::runtime_error("Cannot add NTSC source stage '" + stage_name +
                                 "' to a PAL project.");
      }
      if (is_pal_stage && !(project.video_format_ == VideoSystem::PAL ||
                            project.video_format_ == VideoSystem::PAL_M)) {
        throw std::runtime_error("Cannot add PAL source stage '" + stage_name +
                                 "' to an NTSC project.");
      }
    }
  }

  // Generate unique node ID
  NodeID node_id = generate_unique_node_id(project);

  // Create node
  ProjectDAGNode node;
  node.node_id = node_id;
  node.stage_name = stage_name;
  node.node_type = type_info->type;
  node.display_name = type_info->display_name;
  node.user_label = type_info->display_name;  // Initialize user label
  node.x_position = x_position;
  node.y_position = y_position;

  project.nodes_.push_back(node);
  project.is_modified_ = true;
  return node_id;
}

void remove_node(Project& project, NodeID node_id) {
  // Find the node
  auto node_it = std::find_if(
      project.nodes_.begin(), project.nodes_.end(),
      [&node_id](const ProjectDAGNode& n) { return n.node_id == node_id; });

  if (node_it == project.nodes_.end()) {
    throw std::runtime_error("Node not found: " + node_id.to_string());
  }

  // Check if node can be removed
  std::string reason;
  if (!can_remove_node(project, node_id, &reason)) {
    throw std::runtime_error(reason);
  }

  // Remove all edges connected to this node
  project.edges_.erase(
      std::remove_if(project.edges_.begin(), project.edges_.end(),
                     [&node_id](const ProjectDAGEdge& e) {
                       return e.source_node_id == node_id ||
                              e.target_node_id == node_id;
                     }),
      project.edges_.end());

  // Remove the node
  project.nodes_.erase(node_it);

  project.is_modified_ = true;
}

bool can_remove_node(const Project& project, NodeID node_id,
                     std::string* reason) {
  // Find the node
  auto node_it = std::find_if(
      project.nodes_.begin(), project.nodes_.end(),
      [&node_id](const ProjectDAGNode& n) { return n.node_id == node_id; });

  if (node_it == project.nodes_.end()) {
    if (reason) *reason = "Node not found";
    return false;
  }

  // Check if node has any connections
  for (const auto& edge : project.edges_) {
    if (edge.source_node_id == node_id || edge.target_node_id == node_id) {
      if (reason) {
        *reason =
            "Cannot delete node with connections - disconnect all edges first";
      }
      return false;
    }
  }

  // Node can be removed
  if (reason) *reason = "";
  return true;
}

// Returns the preset string from a CVBS .meta SQLite sidecar, or empty string
// if the file is not accessible or does not contain the expected data.
static std::string read_cvbs_meta_preset(const std::string& meta_path) {
  if (!std::filesystem::exists(meta_path)) return {};
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(meta_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    if (db) sqlite3_close(db);
    return {};
  }
  sqlite3_stmt* stmt = nullptr;
  std::string preset;
  if (sqlite3_prepare_v2(db, "SELECT preset FROM cvbs_file LIMIT 1", -1, &stmt,
                         nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char* val =
          reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      if (val) preset = val;
    }
    sqlite3_finalize(stmt);
  }
  sqlite3_close(db);
  return preset;
}

// Returns the system string from a TBC .tbc.db SQLite sidecar, or empty
// string if the file is not accessible or does not contain the expected data.
static std::string read_tbc_video_system(const std::string& db_path) {
  if (!std::filesystem::exists(db_path)) return {};
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    if (db) sqlite3_close(db);
    return {};
  }
  sqlite3_stmt* stmt = nullptr;
  std::string system;
  if (sqlite3_prepare_v2(db, "SELECT system FROM capture LIMIT 1", -1, &stmt,
                         nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char* val =
          reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      if (val) system = val;
    }
    sqlite3_finalize(stmt);
  }
  sqlite3_close(db);
  return system;
}

void set_node_parameters(
    Project& project, NodeID node_id,
    const std::map<std::string, ParameterValue>& parameters) {
  // Find the node
  auto node_it = std::find_if(
      project.nodes_.begin(), project.nodes_.end(),
      [&node_id](const ProjectDAGNode& n) { return n.node_id == node_id; });

  if (node_it == project.nodes_.end()) {
    throw std::runtime_error("Node not found: " + node_id.to_string());
  }

  // For source nodes, ensure the signal type implied by the incoming parameters
  // is consistent with all other configured source nodes. All sources in a
  // project must use the same signal type (all composite or all Y/C).
  if (node_it->node_type == NodeType::SOURCE) {
    const SourceType incoming_type = [&]() -> SourceType {
      auto has_str = [&](const std::string& key) {
        const auto it = parameters.find(key);
        return it != parameters.end() &&
               std::holds_alternative<std::string>(it->second) &&
               !std::get<std::string>(it->second).empty();
      };
      if (is_dual_mode_source(node_it->stage_name)) {
        if (has_str("y_path") && has_str("c_path")) return SourceType::YC;
        if (has_str("input_path")) return SourceType::Composite;
        return SourceType::Unknown;
      }
      if (node_it->stage_name.find("YC") != std::string::npos ||
          node_it->stage_name.find("Yc") != std::string::npos ||
          node_it->stage_name.find("yc") != std::string::npos) {
        return SourceType::YC;
      }
      return SourceType::Composite;
    }();

    if (incoming_type != SourceType::Unknown) {
      for (const auto& other : project.nodes_) {
        if (other.node_type != NodeType::SOURCE) continue;
        if (other.node_id == node_id) continue;
        const SourceType other_type = get_node_signal_type(other);
        if (other_type != SourceType::Unknown && other_type != incoming_type) {
          const std::string type_name =
              (incoming_type == SourceType::YC) ? "Y/C" : "composite";
          const std::string conflict_name =
              (other_type == SourceType::YC) ? "Y/C" : "composite";
          throw std::runtime_error(
              "Cannot configure '" + node_it->display_name + "' as a " +
              type_name + " source: source '" + other.display_name +
              "' is already configured as " + conflict_name +
              ". All sources in a project must use the same signal type.");
        }
      }
    }
  }

  // For source nodes with a stage-name-determined video format (CVBS sources),
  // validate that the format is consistent with all other configured source
  // nodes. PAL-M is distinct from PAL and NTSC and cannot be mixed.
  if (node_it->node_type == NodeType::SOURCE) {
    const VideoSystem incoming_fmt = get_node_video_format(*node_it);
    if (incoming_fmt != VideoSystem::Unknown) {
      for (const auto& other : project.nodes_) {
        if (other.node_type != NodeType::SOURCE) continue;
        if (other.node_id == node_id) continue;
        const VideoSystem other_fmt = get_node_video_format(other);
        if (other_fmt != VideoSystem::Unknown && other_fmt != incoming_fmt) {
          throw std::runtime_error(
              "Cannot configure '" + node_it->display_name + "' as a " +
              video_system_to_string(incoming_fmt) + " source: source '" +
              other.display_name + "' is already configured as " +
              video_system_to_string(other_fmt) +
              ". All sources in a project must use the same video format.");
        }
      }
    }
  }

  // CVBS source: validate that the source file's format matches the stage's
  // fixed system. Throws if the .meta sidecar is accessible and reports a
  // format mismatch.
  static const std::array<std::pair<std::string, std::string>, 3>
      kCvbsStageExpectedSystem = {{{"PAL_CVBS_Source", "PAL"},
                                   {"NTSC_CVBS_Source", "NTSC"},
                                   {"PAL_M_CVBS_Source", "PAL_M"}}};
  for (const auto& [stage, expected_system] : kCvbsStageExpectedSystem) {
    if (node_it->stage_name == stage) {
      auto it = parameters.find("input_path");
      if (it != parameters.end() &&
          std::holds_alternative<std::string>(it->second)) {
        const std::string& input_path = std::get<std::string>(it->second);
        if (!input_path.empty()) {
          namespace fs = std::filesystem;
          const fs::path p(input_path);
          const std::string meta_path =
              (p.parent_path() / p.stem()).string() + ".meta";
          const std::string preset = read_cvbs_meta_preset(meta_path);
          if (!preset.empty() && preset != expected_system) {
            throw std::runtime_error("The selected " + preset +
                                     " source file cannot be used with a " +
                                     expected_system + " source stage");
          }
        }
      }
      break;
    }
  }

  // tbc_source: validate that the source file's video system matches the
  // project's video system. Throws if the .tbc.db sidecar is accessible and
  // reports a system mismatch.
  if (node_it->stage_name == "tbc_source") {
    const VideoSystem project_system = project.get_video_format();

    // Determine the path to check: composite uses input_path, YC uses y_path.
    auto get_str_param = [&](const std::string& key) -> std::string {
      auto pit = parameters.find(key);
      if (pit != parameters.end() &&
          std::holds_alternative<std::string>(pit->second)) {
        return std::get<std::string>(pit->second);
      }
      return {};
    };

    std::string tbc_path = get_str_param("input_path");
    if (tbc_path.empty()) {
      tbc_path = get_str_param("y_path");
    }

    // Prefer an explicit db_path parameter if provided.
    std::string db_path = get_str_param("db_path");

    // For YC sources the metadata lives next to the base .tbc file, not the
    // .tbcy/.tbcc file.  Strip the Y/C extension and restore ".tbc" so that
    // derived paths resolve to e.g. "foo.tbc.db" / "foo.tbc.json".
    std::string meta_base = tbc_path;
    if (meta_base.size() > 5) {
      std::string ext = meta_base.substr(meta_base.size() - 5);
      for (auto& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (ext == ".tbcy" || ext == ".tbcc") {
        meta_base = meta_base.substr(0, meta_base.size() - 5) + ".tbc";
      }
    }

    if (db_path.empty() && !tbc_path.empty()) {
      db_path = meta_base + ".db";
    }

    if (!tbc_path.empty()) {
      // Existence check: metadata file must be present (legacy: .json).
      std::string legacy_json = meta_base + ".json";
      bool metadata_exists =
          (!db_path.empty() && std::filesystem::exists(db_path)) ||
          std::filesystem::exists(legacy_json);
      if (!metadata_exists) {
        throw std::runtime_error(
            "Failed to validate source file: metadata file not found "
            "(expected " +
            db_path + " or " + legacy_json + ")");
      }

      // Format check: if the .tbc.db is accessible and the project system is
      // known, verify the source system matches.
      if (!db_path.empty() && project_system != VideoSystem::Unknown) {
        const std::string file_system_str = read_tbc_video_system(db_path);
        if (!file_system_str.empty()) {
          // DB uses underscore (PAL_M); video_system_from_string handles this.
          const VideoSystem file_system =
              video_system_from_string(file_system_str);
          if (file_system != VideoSystem::Unknown &&
              file_system != project_system) {
            throw std::runtime_error(
                "The selected " + video_system_to_string(file_system) +
                " source file cannot be used with a " +
                video_system_to_string(project_system) + " project");
          }
          // Cross-check against other configured source nodes. PAL-M is
          // distinct from PAL and NTSC and cannot be mixed.
          if (file_system != VideoSystem::Unknown) {
            for (const auto& other : project.nodes_) {
              if (other.node_type != NodeType::SOURCE) continue;
              if (other.node_id == node_id) continue;
              const VideoSystem other_fmt = get_node_video_format(other);
              if (other_fmt != VideoSystem::Unknown &&
                  other_fmt != file_system) {
                throw std::runtime_error(
                    "Cannot use this " + video_system_to_string(file_system) +
                    " TBC source: source '" + other.display_name +
                    "' is already configured as " +
                    video_system_to_string(other_fmt) +
                    ". All sources in a project must use the same video "
                    "format.");
              }
            }
          }
        }
      }
    }
  }

  node_it->parameters = parameters;

  // Source stages handle their own caching via set_parameters()

  project.is_modified_ = true;
}

void set_node_position(Project& project, NodeID node_id, double x_position,
                       double y_position) {
  // Find the node
  auto node_it = std::find_if(
      project.nodes_.begin(), project.nodes_.end(),
      [&node_id](const ProjectDAGNode& n) { return n.node_id == node_id; });

  if (node_it == project.nodes_.end()) {
    throw std::runtime_error("Node not found: " + node_id.to_string());
  }

  node_it->x_position = x_position;
  node_it->y_position = y_position;
  project.is_modified_ = true;
}

void set_node_label(Project& project, NodeID node_id,
                    const std::string& label) {
  // Find the node
  auto node_it = std::find_if(
      project.nodes_.begin(), project.nodes_.end(),
      [&node_id](const ProjectDAGNode& n) { return n.node_id == node_id; });

  if (node_it == project.nodes_.end()) {
    throw std::runtime_error("Node not found: " + node_id.to_string());
  }

  node_it->user_label = label;
  project.is_modified_ = true;
}

void add_edge(Project& project, NodeID source_node_id, NodeID target_node_id) {
  // Validate that project has been initialized
  if (project.name_.empty()) {
    throw std::runtime_error(
        "Cannot add edge to uninitialized project. Create or load a project "
        "first.");
  }

  // Find source and target nodes_
  auto source_it = std::find_if(project.nodes_.begin(), project.nodes_.end(),
                                [&source_node_id](const ProjectDAGNode& n) {
                                  return n.node_id == source_node_id;
                                });
  auto target_it = std::find_if(project.nodes_.begin(), project.nodes_.end(),
                                [&target_node_id](const ProjectDAGNode& n) {
                                  return n.node_id == target_node_id;
                                });

  if (source_it == project.nodes_.end()) {
    throw std::runtime_error("Source node not found: " +
                             source_node_id.to_string());
  }
  if (target_it == project.nodes_.end()) {
    throw std::runtime_error("Target node not found: " +
                             target_node_id.to_string());
  }

  // Validate connection
  if (!is_connection_valid(source_it->stage_name, target_it->stage_name)) {
    throw std::runtime_error("Invalid connection between " +
                             source_it->stage_name + " and " +
                             target_it->stage_name);
  }

  // Check if edge already exists
  for (const auto& edge : project.edges_) {
    if (edge.source_node_id == source_node_id &&
        edge.target_node_id == target_node_id) {
      throw std::runtime_error("Edge already exists");
    }
  }

  // Count existing connections to check limits
  uint32_t source_output_count = 0;
  uint32_t target_input_count = 0;
  for (const auto& edge : project.edges_) {
    if (edge.source_node_id == source_node_id) source_output_count++;
    if (edge.target_node_id == target_node_id) target_input_count++;
  }

  // Get node type info to check limits
  const NodeTypeInfo* source_info = get_node_type_info(source_it->stage_name);
  const NodeTypeInfo* target_info = get_node_type_info(target_it->stage_name);

  // MANY output stages (min_outputs > 1) cannot fan-out
  // They can only have ONE outgoing connection
  if (source_info && source_info->min_outputs > 1 && source_output_count > 0) {
    throw std::runtime_error(
        "MANY output stages cannot fan-out (already has outgoing connection)");
  }

  if (source_info && source_output_count >= source_info->max_outputs) {
    throw std::runtime_error("Source node has reached maximum outputs");
  }
  if (target_info && target_input_count >= target_info->max_inputs) {
    throw std::runtime_error("Target node has reached maximum inputs");
  }

  // Add edge
  ProjectDAGEdge edge;
  edge.source_node_id = source_node_id;
  edge.target_node_id = target_node_id;
  project.edges_.push_back(edge);
  project.is_modified_ = true;
}

void remove_edge(Project& project, NodeID source_node_id,
                 NodeID target_node_id) {
  // Find and remove the edge
  auto edge_it =
      std::find_if(project.edges_.begin(), project.edges_.end(),
                   [&source_node_id, &target_node_id](const ProjectDAGEdge& e) {
                     return e.source_node_id == source_node_id &&
                            e.target_node_id == target_node_id;
                   });

  if (edge_it == project.edges_.end()) {
    throw std::runtime_error("Edge not found");
  }

  project.edges_.erase(edge_it);
  project.is_modified_ = true;
}

void clear_project(Project& project) {
  project.name_.clear();
  project.version_.clear();
  project.nodes_.clear();
  project.edges_.clear();
  project.clear_modified_flag();
}

void set_project_name(Project& project, const std::string& name) {
  if (name.empty()) {
    throw std::invalid_argument("Project name cannot be empty");
  }
  project.name_ = name;
  project.is_modified_ = true;
}

void set_project_description(Project& project, const std::string& description) {
  project.description_ = description;
  project.is_modified_ = true;
}

void set_video_format(Project& project, VideoSystem video_format) {
  project.video_format_ = video_format;
  project.is_modified_ = true;
}

void set_source_format(Project& project, SourceType source_format) {
  project.source_format_ = source_format;
  project.is_modified_ = true;
}

void set_amplitude_unit(Project& project, AmplitudeDisplayUnit unit) {
  project.amplitude_unit_ = unit;
  project.is_modified_ = true;
}

bool can_trigger_node(const Project& project, NodeID node_id,
                      std::string* reason) {
  // Find the node
  auto it = std::find_if(
      project.nodes_.begin(), project.nodes_.end(),
      [&node_id](const ProjectDAGNode& n) { return n.node_id == node_id; });

  if (it == project.nodes_.end()) {
    if (reason) *reason = "Node not found";
    return false;
  }

  // Check if stage is triggerable
  try {
    auto stage = StageRegistry::instance().create_stage(it->stage_name);
    if (!stage) {
      if (reason) *reason = "Failed to create stage";
      return false;
    }

    auto* trigger_stage = dynamic_cast<TriggerableStage*>(stage.get());
    if (!trigger_stage) {
      if (reason) *reason = "Stage is not triggerable";
      return false;
    }

    if (reason) *reason = "";
    return true;
  } catch (const std::exception& e) {
    if (reason) *reason = std::string("Error: ") + e.what();
    return false;
  }
}

bool trigger_node(Project& project, NodeID node_id, std::string& status_out,
                  TriggerProgressCallback progress_callback) {
  // Find the node
  auto it = std::find_if(
      project.nodes_.begin(), project.nodes_.end(),
      [&node_id](const ProjectDAGNode& n) { return n.node_id == node_id; });

  if (it == project.nodes_.end()) {
    status_out = "Node not found";
    throw std::runtime_error("Node '" + node_id.to_string() + "' not found");
  }

  // Get stage instance
  auto stage = StageRegistry::instance().create_stage(it->stage_name);
  if (!stage) {
    status_out = "Failed to create stage";
    throw std::runtime_error("Failed to create stage '" + it->stage_name + "'");
  }

  // Check if triggerable
  auto* trigger_stage = dynamic_cast<TriggerableStage*>(stage.get());
  if (!trigger_stage) {
    status_out = "Stage is not triggerable";
    throw std::runtime_error("Stage is not triggerable");
  }

  // Set progress callback if provided
  if (progress_callback) {
    trigger_stage->set_progress_callback(progress_callback);
  }

  // Build DAG
  auto dag = project_to_dag(project);

  // CRITICAL: Keep executor alive during trigger to prevent dangling pointers
  // Artifacts from execute_to_node may contain representations (like
  // VideoFrameRepresentationWrapper) that hold raw pointers to stages owned
  // by the executor/DAG. These stages must outlive the trigger operation.
  auto executor = std::make_shared<DAGExecutor>();

  // Get inputs by executing to predecessor nodes
  std::vector<ArtifactPtr> inputs;
  for (const auto& edge : project.edges_) {
    if (edge.target_node_id == node_id) {
      auto node_outputs = executor->execute_to_node(*dag, edge.source_node_id);
      if (node_outputs.find(edge.source_node_id) != node_outputs.end()) {
        auto& outputs = node_outputs[edge.source_node_id];
        // For now, assume single output per stage (most common case)
        if (!outputs.empty()) {
          inputs.push_back(outputs[0]);
        }
      }
    }
  }

  if (inputs.empty()) {
    status_out = "No inputs available";
    throw std::runtime_error("No inputs for node '" + node_id.to_string() +
                             "'");
  }

  // Trigger (DAG and executor stay alive, keeping stage instances valid)
  ObservationContext observation_context;
  bool success =
      trigger_stage->trigger(inputs, it->parameters, observation_context);
  status_out = trigger_stage->get_trigger_status();

  // DAG and executor destroyed here AFTER trigger completes, ensuring stages
  // outlive artifacts
  return success;
}

std::future<std::pair<bool, std::string>> trigger_node_async(
    Project& project, NodeID node_id,
    TriggerProgressCallback progress_callback) {
  // Build DAG and get inputs BEFORE launching async task
  // This ensures the DAG's lifetime is managed by the async task
  auto dag = project_to_dag(project);
  auto executor = std::make_shared<DAGExecutor>();

  // Get inputs by executing to predecessor nodes
  std::vector<ArtifactPtr> inputs;
  const auto& edges = project.get_edges();
  for (const auto& edge : edges) {
    if (edge.target_node_id == node_id) {
      auto node_outputs = executor->execute_to_node(*dag, edge.source_node_id);
      if (node_outputs.find(edge.source_node_id) != node_outputs.end()) {
        auto& outputs = node_outputs[edge.source_node_id];
        if (!outputs.empty()) {
          inputs.push_back(outputs[0]);
        }
      }
    }
  }

  // Launch async task, capturing DAG and inputs to keep stages alive
  return std::async(
      std::launch::async,
      [dag, node_id, inputs,
       progress_callback]() -> std::pair<bool, std::string> {
        try {
          // Find the target node in the DAG
          const DAGNode* target_node = nullptr;
          for (const auto& node : dag->nodes()) {
            if (node.node_id == node_id) {
              target_node = &node;
              break;
            }
          }

          if (!target_node) {
            return {false, "Node not found in DAG"};
          }

          auto trigger_stage =
              dynamic_cast<TriggerableStage*>(target_node->stage.get());
          if (!trigger_stage) {
            return {false, "Stage is not triggerable"};
          }

          if (progress_callback) {
            trigger_stage->set_progress_callback(progress_callback);
          }

          ObservationContext observation_context;
          bool success = trigger_stage->trigger(inputs, target_node->parameters,
                                                observation_context);
          std::string status = trigger_stage->get_trigger_status();

          // DAG will be destroyed here, keeping stages alive throughout trigger
          return {success, status};

        } catch (const std::exception& e) {
          return {false, std::string("Exception: ") + e.what()};
        }
      });
}

std::string find_source_file_for_node(const Project& project, NodeID node_id) {
  // Find node
  auto node_it = std::find_if(
      project.nodes_.begin(), project.nodes_.end(),
      [&node_id](const ProjectDAGNode& n) { return n.node_id == node_id; });

  if (node_it == project.nodes_.end()) {
    return "";
  }

  // Check if this node has input_path
  auto param_it = node_it->parameters.find("input_path");
  if (param_it != node_it->parameters.end()) {
    if (auto* path = std::get_if<std::string>(&param_it->second)) {
      return *path;
    }
  }

  // Trace back through DAG
  std::queue<NodeID> to_visit;
  std::set<NodeID> visited;

  for (const auto& edge : project.edges_) {
    if (edge.target_node_id == node_id) {
      to_visit.push(edge.source_node_id);
    }
  }

  while (!to_visit.empty()) {
    NodeID current_id = to_visit.front();
    to_visit.pop();

    if (visited.count(current_id)) continue;
    visited.insert(current_id);

    auto current_it = std::find_if(project.nodes_.begin(), project.nodes_.end(),
                                   [&current_id](const ProjectDAGNode& n) {
                                     return n.node_id == current_id;
                                   });

    if (current_it != project.nodes_.end()) {
      auto path_param = current_it->parameters.find("input_path");
      if (path_param != current_it->parameters.end()) {
        if (auto* path = std::get_if<std::string>(&path_param->second)) {
          return *path;
        }
      }

      for (const auto& edge : project.edges_) {
        if (edge.target_node_id == current_id) {
          to_visit.push(edge.source_node_id);
        }
      }
    }
  }

  return "";
}

namespace {

// Whether |stage| offers a batch-analysis tool, i.e. a run of it produces
// something to look at in the host as well as (or instead of) a file. A
// catalogue browser is such a tool — the browsable result *is* the output —
// so a sink offering one is not required to name an output file either.
bool stage_has_batch_analysis_tool(const DAGStage& stage) {
  const auto* provider = dynamic_cast<const StageToolProvider*>(&stage);
  if (provider == nullptr) {
    return false;
  }
  for (const auto& tool : provider->get_stage_tools()) {
    if (tool.kind == StageToolKind::BatchAnalysis ||
        tool.kind == StageToolKind::CatalogueBrowser) {
      return true;
    }
  }
  return false;
}

}  // namespace

NodeCapabilities get_node_capabilities(const Project& project, NodeID node_id) {
  NodeCapabilities caps;
  caps.node_id = node_id;

  // Find the node
  auto it = std::find_if(
      project.nodes_.begin(), project.nodes_.end(),
      [&node_id](const ProjectDAGNode& n) { return n.node_id == node_id; });

  if (it == project.nodes_.end()) {
    caps.remove_reason = "Node not found";
    caps.trigger_reason = "Node not found";
    return caps;
  }

  caps.stage_name = it->stage_name;
  caps.node_label = it->user_label.empty() ? it->display_name : it->user_label;

  // Check can_remove - cannot remove if node has connections
  bool has_connections = false;
  for (const auto& edge : project.edges_) {
    if (edge.source_node_id == node_id || edge.target_node_id == node_id) {
      has_connections = true;
      break;
    }
  }
  caps.can_remove = !has_connections;
  if (has_connections) {
    caps.remove_reason = "Cannot remove connected node";
  }

  // Check can_trigger - must be TriggerableStage
  try {
    auto stage = StageRegistry::instance().create_stage(it->stage_name);
    if (stage) {
      auto* trigger_stage = dynamic_cast<TriggerableStage*>(stage.get());
      caps.can_trigger = (trigger_stage != nullptr);
      if (!caps.can_trigger) {
        caps.trigger_reason = "Stage is not triggerable";
      } else {
        // For sink stages, check if output filename is set — unless the stage
        // offers a batch-analysis tool, in which case a run with no output
        // path still produces something the user asked for (the teletext sink
        // decodes and catalogues the pages without writing a packet stream).
        auto node_type = stage->get_node_type_info().type;
        if (node_type == NodeType::SINK &&
            !stage_has_batch_analysis_tool(*stage)) {
          // All sink stages use "output_path" parameter
          auto param_it = it->parameters.find("output_path");
          bool has_output = false;

          if (param_it != it->parameters.end() &&
              std::holds_alternative<std::string>(param_it->second)) {
            std::string path = std::get<std::string>(param_it->second);
            has_output = !path.empty();
          }

          if (!has_output) {
            caps.can_trigger = false;
            caps.trigger_reason = "No output filename specified";
          }
        }
      }

    } else {
      caps.trigger_reason = "Failed to create stage";
    }
  } catch (const std::exception& e) {
    caps.trigger_reason = std::string("Error: ") + e.what();
  }

  return caps;
}

}  // namespace project_io
}  // namespace orc
