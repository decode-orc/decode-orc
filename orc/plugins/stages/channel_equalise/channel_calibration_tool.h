/*
 * File:        channel_calibration_tool.h
 * Module:      orc-stage-plugin-channel-equalise
 * Purpose:     Channel Calibration, the stage's analysis tool: sets the edge
 *              smear from the VITS on the stage's input
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

/// The report of @p calibration: status, items, statistics and summary. Only
/// a Smear or NoSmear verdict leaves parameter changes to apply.
StageAnalysisReport describe(const ChannelCalibration& calibration);

/// Measure the smear on the node's input (context.inputs()[0]).
StageAnalysisReport run(IToolJobContext& context);

}  // namespace orc::channel_calibration_tool
