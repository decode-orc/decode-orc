/*
 * File:        node_type.cpp
 * Module:      orc-core
 * Purpose:     Node type registry
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <orc/stage/node_type.h>
#include <orc/support/logging.h>

#include <limits>
#include <unordered_map>
#include <vector>

#include "include/stage_registry.h"

namespace orc {

const NodeTypeInfo* get_node_type_info(const std::string& stage_name) {
  // Create a temporary instance of the stage to query its metadata
  auto& registry = StageRegistry::instance();

  if (!registry.has_stage(stage_name)) {
    auto stages = registry.get_registered_stages();
    std::string available_stages;
    for (size_t i = 0; i < stages.size(); ++i) {
      available_stages += stages[i];
      if (i < stages.size() - 1) available_stages += ", ";
    }
    ORC_LOG_ERROR("Stage '{}' is not registered. Available: {}", stage_name,
                  available_stages);
    return nullptr;
  }

  try {
    auto stage = registry.create_stage(stage_name);
    static thread_local std::unordered_map<std::string, NodeTypeInfo> cache;

    // Cache the result to avoid repeated stage creation
    auto it = cache.find(stage_name);
    if (it == cache.end()) {
      const auto inserted =
          cache.emplace(stage_name, stage->get_node_type_info());
      it = inserted.first;
    }

    return &it->second;
  } catch (...) {
    return nullptr;
  }
}

const std::vector<NodeTypeInfo>& get_all_node_types() {
  static std::vector<NodeTypeInfo> all_types;
  static bool initialized = false;

  if (!initialized) {
    auto& registry = StageRegistry::instance();
    auto stage_names = registry.get_registered_stages();

    for (const auto& name : stage_names) {
      const NodeTypeInfo* info = get_node_type_info(name);
      if (info) {
        all_types.push_back(*info);
      }
    }

    initialized = true;
  }

  return all_types;
}

bool is_connection_valid(const std::string& source_stage,
                         const std::string& target_stage) {
  const NodeTypeInfo* source_info = get_node_type_info(source_stage);
  const NodeTypeInfo* target_info = get_node_type_info(target_stage);

  // Unknown stages are not allowed
  if (!source_info || !target_info) {
    return false;
  }

  // Source must have outputs
  if (source_info->max_outputs == 0) {
    return false;
  }

  // Target must have inputs
  if (target_info->max_inputs == 0) {
    return false;
  }

  // Cardinality matching rules:
  // - ONE output stages (min_outputs == 1) support fan-out and can connect to:
  //   * ONE input stages (max_inputs == 1) - single connection
  //   * MANY input stages (max_inputs > 1) - multiple ONE sources to one MANY
  //   target
  // - MANY output stages (min_outputs > 1) can ONLY connect to MANY input
  // stages (max_inputs > 1)
  //   They do NOT support fan-out (enforced in add_edge)
  // - FORBIDDEN: MANY output → ONE input (would violate ONE input's
  // single-connection semantics)

  bool source_is_many = (source_info->min_outputs > 1);
  bool target_is_one = (target_info->max_inputs == 1);

  // Reject MANY outputs connecting to ONE inputs
  if (source_is_many && target_is_one) {
    return false;
  }

  return true;
}

bool is_stage_compatible_with_format(const std::string& stage_name,
                                     VideoSystem format) {
  const NodeTypeInfo* info = get_node_type_info(stage_name);
  if (!info) {
    return false;  // Unknown stage
  }

  // ALL is compatible with everything
  if (info->compatible_formats == VideoFormatCompatibility::ALL) {
    return true;
  }

  // NTSC_ONLY is compatible with NTSC only
  if (info->compatible_formats == VideoFormatCompatibility::NTSC_ONLY) {
    return format == VideoSystem::NTSC;
  }

  // PAL_ONLY is compatible with PAL only: it means PAL colour, so a SECAM
  // project (also 625-line) does not offer it
  if (info->compatible_formats == VideoFormatCompatibility::PAL_ONLY) {
    return format == VideoSystem::PAL;
  }

  // PAL_M_ONLY is compatible with PAL-M only
  if (info->compatible_formats == VideoFormatCompatibility::PAL_M_ONLY) {
    return format == VideoSystem::PAL_M;
  }

  // SECAM_ONLY is compatible with SECAM only
  if (info->compatible_formats == VideoFormatCompatibility::SECAM_ONLY) {
    return format == VideoSystem::SECAM;
  }

  // NOT_SECAM is compatible with everything but SECAM
  if (info->compatible_formats == VideoFormatCompatibility::NOT_SECAM) {
    return format != VideoSystem::SECAM;
  }

  // Unknown format or Unknown compatibility - allow it
  return true;
}

}  // namespace orc
