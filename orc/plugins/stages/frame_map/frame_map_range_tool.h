/*
 * File:        frame_map_range_tool.h
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     The Frame Map's Range Finder: the frames between two picture
 *              numbers or CLV time codes, as the Frame Map's ranges
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_FRAME_MAP_FRAME_MAP_RANGE_TOOL_H
#define ORC_STAGE_FRAME_MAP_FRAME_MAP_RANGE_TOOL_H

#include <orc/stage/params/parameter_types.h>
#include <orc/stage/tooling/stage_analysis_tool.h>
#include <orc/stage/tooling/stage_tool_jobs.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "frame_map_range_contract.h"

namespace orc::frame_map_range_tool {

constexpr char kToolId[] = "frame_map_range";
/// The window orc-gui opens is the core view's (start and end as picture
/// numbers and as time codes, each following the other).
constexpr const char* kContractId = frame_map_range_contract::kContractId;

/// Its parameters, with their defaults.
std::vector<ParameterDescriptor> parameters();

/// A start or end address as the user typed it.
struct ParsedAddress {
  bool ok = false;
  bool is_timecode = false;
  /// The picture number it names; a time code 0:0:0.0 is picture 1.
  int32_t picture_number = 0;
  std::string normalized;
  /// Why it is not an address, when !ok.
  std::string error;
};

/**
 * @brief Read an address: a picture number ("12345") or a CLV time code
 *        ("h:m:s.p", "h:m:s;p" or "h:m:s:p")
 *
 * @param is_pal A time code counts 25 pictures a second for PAL, else 30
 */
ParsedAddress parse_address(const std::string& input, bool is_pal);

/**
 * @brief Find the frames from the start address to the end address
 *
 * Decodes the picture numbers of as few frames as a binary chop needs,
 * falling back to a scan where the numbers are not in order. On success the
 * report sets `ranges` to the frame range found.
 */
StageAnalysisReport run(const std::map<std::string, ParameterValue>& params,
                        IToolJobContext& context);

}  // namespace orc::frame_map_range_tool

#endif  // ORC_STAGE_FRAME_MAP_FRAME_MAP_RANGE_TOOL_H
