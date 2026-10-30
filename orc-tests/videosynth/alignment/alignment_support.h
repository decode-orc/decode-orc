/*
 * File:        alignment_support.h
 * Module:      orc-videosynth-tests
 * Purpose:     Picture numbers, the Disc Mapper and Frame Map for the
 *              videosynth suite's alignment and stacking cases
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <disc_mapper_analyzer.h>
#include <orc/stage/artifact.h>
#include <orc/stage/common_types.h>
#include <orc/stage/video_frame_representation.h>
#include <source_alignment_offsets.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "frame_map_stage.h"

namespace orc_videosynth {

// The picture number of each frame of |vfr|, read from the VBI of either
// field; nullopt where none decodes.
std::vector<std::optional<int32_t>> read_pictures(
    const orc::VideoFrameRepresentation& vfr);

// The readings Source Align's offsets are computed from.
std::vector<orc::PictureNumberReading> readings_of(
    const std::vector<std::optional<int32_t>>& pictures);

// The Disc Mapper's decision for |vfr|, from the observers it reads.
orc::FieldMappingDecision map_disc(const orc::VideoFrameRepresentation& vfr);

// |capture| remapped by Frame Map to |ranges|; nullptr, with the test
// failed, when it cannot be. |stage| is kept by the caller, as the output
// reads through it.
orc::ArtifactPtr apply_frame_map(const orc::ArtifactPtr& capture,
                                 orc::FrameMapStage& stage,
                                 const std::string& ranges);

}  // namespace orc_videosynth
