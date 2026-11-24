/*
 * File:        channel_calibration_tool.h
 * Module:      orc-stage-plugin-channel-equalise
 * Purpose:     Channel Calibration and Channel Tracking, the stage's analysis
 *              tools: set the edge smear from the VITS on the stage's input,
 *              for the whole source or frame by frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/tooling/stage_analysis_tool.h>

#include "channel_calibration.h"

namespace orc::channel_calibration_tool {

/// The tool's id, which projects store in their tool_refresh blocks.
inline constexpr char kToolId[] = "channel_calibration";
inline constexpr char kContractId[] =
    "decode-orc.stage-tools.channel-calibration.v1";

/// The tracking tool's ids.
inline constexpr char kTrackingToolId[] = "channel_tracking";
inline constexpr char kTrackingContractId[] =
    "decode-orc.stage-tools.channel-tracking.v1";

/// The report of @p calibration: status, items, statistics and summary. Only
/// a Smear or NoSmear verdict leaves parameter changes to apply.
StageAnalysisReport describe(const ChannelCalibration& calibration);

/// Measure the smear on the node's input (context.inputs()[0]).
StageAnalysisReport run(IToolJobContext& context);

/// The report of @p track: one item per run of blocks with the same verdict,
/// and smear_track to apply (empty when no block has a smear, which passes
/// every frame through).
StageAnalysisReport describe_track(const ChannelTrack& track);

/// The tracking tool's parameter: whether tails confined near the edge are
/// corrected too (ChannelTrackOptions::near_edge).
inline constexpr char kNearEdgeParameter[] = "near_edge";

/// Track the smear along the node's input (context.inputs()[0]).
StageAnalysisReport run_tracking(IToolJobContext& context,
                                 const ChannelTrackOptions& options = {});

}  // namespace orc::channel_calibration_tool
