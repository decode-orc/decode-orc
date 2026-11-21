/*
 * File:        fm_beat_calibration_tool.h
 * Module:      orc-stage-plugin-fm-beat-cancel
 * Purpose:     FM Beat Calibration, the stage's analysis tool: sets the
 *              carrier from the beat on the stage's input
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/tooling/stage_analysis_tool.h>

namespace orc::fm_beat_calibration_tool {

/// The tool's id, which projects store in their tool_refresh blocks.
inline constexpr char kToolId[] = "fm_beat_calibration";
inline constexpr char kContractId[] =
    "decode-orc.stage-tools.fm-beat-calibration.v1";

/// Measure the carrier on the node's input (context.inputs()[0]).
StageAnalysisReport run(IToolJobContext& context);

}  // namespace orc::fm_beat_calibration_tool
