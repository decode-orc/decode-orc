/*
 * File:        edge_ring_calibration_tool.h
 * Module:      orc-stage-plugin-edge-ring-correct
 * Purpose:     Edge Ring Calibration, the stage's analysis tool: sets the
 *              ring track from the stage's input
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/tooling/stage_analysis_tool.h>

#include "edge_ring_calibration.h"

namespace orc::edge_ring_calibration_tool {

/// The tool's id, which projects store in their tool_refresh blocks.
inline constexpr char kToolId[] = "edge_ring_calibration";
inline constexpr char kContractId[] =
    "decode-orc.stage-tools.edge-ring-calibration.v1";

/// Calibrate on the node's input (context.inputs()[0]).
StageAnalysisReport run(IToolJobContext& context,
                        const EdgeRingCalibrationOptions& options = {});

/// The report for a finished calibration (exposed for tests).
StageAnalysisReport report(const EdgeRingCalibration& calibration);

}  // namespace orc::edge_ring_calibration_tool
