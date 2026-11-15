/*
 * File:        node_type.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Node type registry
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace orc {

// Forward declarations to avoid circular dependencies
enum class VideoSystem;

/**
 * @brief Video format compatibility for stages
 *
 * Defines which video formats (NTSC/PAL/PAL-M/SECAM) a stage supports.
 */
enum class VideoFormatCompatibility {
  ALL,         // Works with any format (NTSC, PAL, PAL-M, SECAM)
  NTSC_ONLY,   // Only works with NTSC
  PAL_ONLY,    // Only works with PAL (625-line PAL colour; not SECAM)
  PAL_M_ONLY,  // Only works with PAL-M (525-line Brazilian PAL variant)
  SECAM_ONLY,  // Only works with SECAM (appended in ABI 21: values unchanged)
  NOT_SECAM,   // Works with NTSC, PAL and PAL-M, not SECAM: LaserDisc stages,
               // since no LaserDisc carries SECAM (appended in ABI 21)
};

/**
 * @brief Visual configuration status reported by a stage
 *
 * Stages set this in their constructor and whenever set_parameters() is called
 * to tell the GUI how to colour the status dot on the node.
 */
enum class ConfigurationStatus {
  Green = 0,   ///< Fully configured or requires no parameters
  Yellow = 1,  ///< Partially configured; some parameters are missing or empty
  Red = 2,     ///< Unconfigured; critical required parameters are absent
};

/**
 * @brief Node connectivity pattern
 *
 * Defines the input/output structure of a DAG node type.
 * This determines how nodes can be connected and how the GUI renders them.
 */
enum class NodeType {
  /// Source node - no inputs, produces outputs (e.g., video source nodes)
  SOURCE,

  /// Sink node - consumes inputs, no outputs (e.g., export to file, display
  /// output)
  SINK,

  /// Transform node - one input, one output (most common processing stages)
  TRANSFORM,

  /// Merger node - multiple inputs, one output (stacking, blending)
  MERGER,

  /// Complex node - multiple inputs, multiple outputs (advanced processing)
  COMPLEX,

  /// Analysis sink - consumes inputs, triggers batch analysis, produces no
  /// frame outputs
  ANALYSIS_SINK
};

/**
 * @brief Stage menu category
 *
 * The Add Stage menu group a stage belongs to. This is not declared by
 * plugins; it is derived from the stage's NodeType via stage_category_for()
 * so a stage can never appear under the wrong (or an invented) category.
 */
enum class StageCategory { SOURCE, TRANSFORM, ANALYSIS, SINK };

/**
 * @brief Derive the menu category from a node's connectivity type
 *
 * SOURCE maps to Source, SINK to Sink, ANALYSIS_SINK to Analysis, and all
 * frame-processing types (TRANSFORM, MERGER, COMPLEX) to Transform.
 */
constexpr StageCategory stage_category_for(NodeType type) {
  switch (type) {
    case NodeType::SOURCE:
      return StageCategory::SOURCE;
    case NodeType::SINK:
      return StageCategory::SINK;
    case NodeType::ANALYSIS_SINK:
      return StageCategory::ANALYSIS;
    case NodeType::TRANSFORM:
    case NodeType::MERGER:
    case NodeType::COMPLEX:
      break;
  }
  return StageCategory::TRANSFORM;
}

/**
 * @brief Human-readable label for a stage category
 */
constexpr const char* stage_category_label(StageCategory category) {
  switch (category) {
    case StageCategory::SOURCE:
      return "Source";
    case StageCategory::TRANSFORM:
      return "Transform";
    case StageCategory::ANALYSIS:
      return "Analysis";
    case StageCategory::SINK:
      break;
  }
  return "Sink";
}

/**
 * @brief Metadata about a node type
 *
 * Describes the characteristics of a processing stage for GUI rendering
 * and DAG validation.
 */
struct NodeTypeInfo {
  NodeType type;           // Connectivity pattern
  std::string stage_name;  // Type identifier (e.g., "START", "DropoutCorrect")
  std::string display_name;  // Human-readable name (e.g., "Source", "Dropout
                             // Correction")
  std::string description;   // Detailed description for tooltips
  uint32_t min_inputs;       // Minimum number of inputs (0 for SOURCE)
  uint32_t max_inputs;   // Maximum number of inputs (0 for SOURCE, UINT32_MAX
                         // for unlimited)
  uint32_t min_outputs;  // Minimum number of outputs (0 for SINK)
  uint32_t max_outputs;  // Maximum number of outputs (0 for SINK, UINT32_MAX
                         // for unlimited)
  VideoFormatCompatibility compatible_formats;  // Video format compatibility

  NodeTypeInfo(NodeType type, std::string stage_name, std::string display_name,
               std::string description, uint32_t min_inputs,
               uint32_t max_inputs, uint32_t min_outputs, uint32_t max_outputs,
               VideoFormatCompatibility compatible_formats)
      : type(type),
        stage_name(std::move(stage_name)),
        display_name(std::move(display_name)),
        description(std::move(description)),
        min_inputs(min_inputs),
        max_inputs(max_inputs),
        min_outputs(min_outputs),
        max_outputs(max_outputs),
        compatible_formats(compatible_formats) {}

  /// Menu category derived from the connectivity type; never plugin-declared.
  StageCategory category() const { return stage_category_for(type); }

  NodeTypeInfo() = delete;
};

/**
 * @brief Get node type information by stage name
 *
 * @param stage_name Stage identifier (e.g., "START", "Passthrough")
 * @return Node type info, or nullptr if stage_name not recognized
 */
const NodeTypeInfo* get_node_type_info(const std::string& stage_name);

/**
 * @brief Get list of all available node types
 *
 * @return Vector of all registered node type info
 */
const std::vector<NodeTypeInfo>& get_all_node_types();

/**
 * @brief Check if a connection is valid between two node types
 *
 * @param source_stage Source node stage name
 * @param target_stage Target node stage name
 * @return true if connection is allowed
 */
bool is_connection_valid(const std::string& source_stage,
                         const std::string& target_stage);

/**
 * @brief Check if a stage is compatible with a video format
 *
 * @param stage_name Stage identifier
 * @param format Video format to check
 * @return true if stage is compatible with the format
 */
bool is_stage_compatible_with_format(const std::string& stage_name,
                                     VideoSystem format);

}  // namespace orc
