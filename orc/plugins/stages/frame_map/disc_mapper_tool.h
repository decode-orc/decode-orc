/*
 * File:        disc_mapper_tool.h
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     The Frame Map's Disc Mapper: map a whole side's frames by
 *              their picture numbers, as the Frame Map's ranges
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_FRAME_MAP_DISC_MAPPER_TOOL_H
#define ORC_STAGE_FRAME_MAP_DISC_MAPPER_TOOL_H

#include <orc/stage/common_types.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/tooling/stage_analysis_tool.h>
#include <orc/stage/tooling/stage_tool_jobs.h>
#include <orc/support/observation_pass.h>

#include <map>
#include <string>
#include <vector>

#include "disc_mapper_analyzer.h"

namespace orc::disc_mapper_tool {

/// The id `orc-cli tools run` and stored `tool_refresh` blocks name it by.
constexpr char kToolId[] = "field_mapping";
constexpr char kContractId[] = "decode-orc.stage-tools.disc-mapper.v1";

/// Its parameters, with their defaults.
std::vector<ParameterDescriptor> parameters();

/**
 * @brief What the disc says of itself over the whole side
 *
 * The disc-level vote of the disc metadata document the video sink embeds
 * (disc-metadata/disc_metadata_document.h), read from the biphase observations
 * of |frames| in |observations|, with the lowest and highest picture number
 * |decision| mapped (CAV only). Values never recovered are left out. The keys
 * are listed in docs/technical/disc-mapping.md.
 */
std::map<std::string, StageAnalysisValue> disc_identity_statistics(
    const IObservationContext& observations, FrameIDRange frames,
    VideoSystem system, const FieldMappingDecision& decision);

/**
 * @brief Map the side the node's input carries
 *
 * Sweeps the disc-analysis observers over every frame (several readers, each
 * on a private copy of the inputs, where the host can make them), then maps
 * the frames by picture number. On success the report sets `ranges` to the
 * mapping, unless no frame could be mapped.
 *
 * @param options How the sweep divides its work; the default is the one the
 *        tool runs with
 */
StageAnalysisReport run(const std::map<std::string, ParameterValue>& params,
                        IToolJobContext& context,
                        const ObservationPassOptions& options = {});

}  // namespace orc::disc_mapper_tool

#endif  // ORC_STAGE_FRAME_MAP_DISC_MAPPER_TOOL_H
