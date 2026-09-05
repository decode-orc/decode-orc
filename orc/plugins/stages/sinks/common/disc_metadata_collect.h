/*
 * File:        disc_metadata_collect.h
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     Read LaserDisc VBI observations into DiscMetadataFrame records
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_PLUGIN_VIDEO_SINK_DISC_METADATA_COLLECT_H
#define ORC_STAGE_PLUGIN_VIDEO_SINK_DISC_METADATA_COLLECT_H

#include <orc/stage/observation/observation_context_interface.h>

#include <cstdint>
#include <vector>

#include "disc_metadata_document.h"

namespace orc {

/**
 * @brief Read per-field VBI observations into per-frame records
 *
 * File frame n corresponds to fields start_field_index + 2n and + 2n + 1
 * (BiphaseObserver writes to FieldID(frame_id * 2 + field_index), and the
 * backend receives start_field_index = frame_range.first * 2).
 *
 * LaserDisc VBI is routinely split across the two fields of a frame, so
 * frame-level values take whichever field carries them; the programme status
 * word is kept per-field so the disc-level vote can use both.
 *
 * The context must have been populated by a biphase observer pass over the
 * sink's *own input* frames. Reading observations produced further upstream
 * is not safe: frame_map removes and pads frames without remapping the
 * observation context, so an output field index would address a different
 * source field.
 */
std::vector<DiscMetadataFrame> collect_disc_metadata_frames(
    const IObservationContext& context, uint64_t start_field_index,
    uint64_t frame_count);

}  // namespace orc

#endif  // ORC_STAGE_PLUGIN_VIDEO_SINK_DISC_METADATA_COLLECT_H
