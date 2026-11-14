/*
 * File:        frame_corruption_tool.h
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     The Frame Map's Frame Corruption Generator: a frame map with
 *              skips, repeats and gaps, to test the Disc Mapper against
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_FRAME_MAP_FRAME_CORRUPTION_TOOL_H
#define ORC_STAGE_FRAME_MAP_FRAME_CORRUPTION_TOOL_H

#include <orc/stage/params/parameter_types.h>
#include <orc/stage/tooling/stage_analysis_tool.h>
#include <orc/stage/tooling/stage_tool_jobs.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace orc::frame_corruption_tool {

constexpr char kToolId[] = "frame_corruption";
constexpr char kContractId[] = "decode-orc.stage-tools.frame-corruption.v1";

/// The node parameter the seed of the last pattern applied is kept in, so
/// that a later run can make the same pattern again.
constexpr char kSeedParameter[] = "seed";

/// Its parameters, with their defaults.
std::vector<ParameterDescriptor> parameters();

/**
 * @brief Make a corruption pattern over the node's input
 *
 * @param stored_seed The node's seed (0 when none was applied yet); reused
 *        unless the run asks for a new one
 * @param new_seed Gives a new seed when one is needed
 * @return on success, `ranges` (the corrupted frame map) and `seed` as the
 *         changes to apply
 */
StageAnalysisReport run(const std::map<std::string, ParameterValue>& params,
                        IToolJobContext& context, int32_t stored_seed,
                        const std::function<uint32_t()>& new_seed);

}  // namespace orc::frame_corruption_tool

#endif  // ORC_STAGE_FRAME_MAP_FRAME_CORRUPTION_TOOL_H
