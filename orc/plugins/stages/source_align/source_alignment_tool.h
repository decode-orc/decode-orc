/*
 * File:        source_alignment_tool.h
 * Module:      orc-stage-plugin-source-align
 * Purpose:     The Source Align's alignment analysis: place each input by
 *              its picture numbers, as the stage's alignment map
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ORC_STAGE_SOURCE_ALIGN_SOURCE_ALIGNMENT_TOOL_H
#define ORC_STAGE_SOURCE_ALIGN_SOURCE_ALIGNMENT_TOOL_H

#include <orc/stage/params/parameter_types.h>
#include <orc/stage/tooling/stage_analysis_tool.h>
#include <orc/stage/tooling/stage_tool_jobs.h>

#include <map>
#include <string>
#include <vector>

namespace orc::source_alignment_tool {

/// The id `orc-cli tools run` and stored `tool_refresh` blocks name it by.
constexpr char kToolId[] = "source_alignment";
constexpr char kContractId[] = "decode-orc.stage-tools.source-alignment.v1";

/// Its parameters, with their defaults.
std::vector<ParameterDescriptor> parameters();

/**
 * @brief Align the captures the node's inputs carry
 *
 * Reads each input's picture numbers (CAV) or time codes (CLV) and places
 * the inputs by the alignmentMode parameter, then compares the aligned
 * captures (noise, pressing groups, content sitting off its picture
 * numbers). On success the report sets `alignmentMap` and `alignmentMode`.
 */
StageAnalysisReport run(const std::map<std::string, ParameterValue>& params,
                        IToolJobContext& context);

}  // namespace orc::source_alignment_tool

#endif  // ORC_STAGE_SOURCE_ALIGN_SOURCE_ALIGNMENT_TOOL_H
