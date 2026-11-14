/*
 * File:        frame_map_range_contract.h
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     What the Frame Map Range Finder and its window share: the
 *              tool's contract id and the names of its two parameters
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_FRAME_MAP_FRAME_MAP_RANGE_CONTRACT_H
#define ORC_STAGE_FRAME_MAP_FRAME_MAP_RANGE_CONTRACT_H

// Included by the core view binary as well as the stage, so nothing here may
// depend on the stage contract headers.

namespace orc::frame_map_range_contract {

constexpr char kContractId[] = "decode-orc.stage-tools.frame-map-range.v1";

/// The first and last frame's address: a picture number ("12345") or a CLV
/// time code ("h:m:s.p"), as text.
constexpr char kStartAddress[] = "startAddress";
constexpr char kEndAddress[] = "endAddress";

}  // namespace orc::frame_map_range_contract

#endif  // ORC_STAGE_FRAME_MAP_FRAME_MAP_RANGE_CONTRACT_H
