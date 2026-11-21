/*
 * File:        remux_dts.h
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     Rebuilds the decode timestamps a Matroska stream loses for its
 *              leading packets, for the video sink's metadata remux
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_STAGE_PLUGIN_VIDEO_SINK_REMUX_DTS_H
#define ORC_STAGE_PLUGIN_VIDEO_SINK_REMUX_DTS_H

#include <cstdint>
#include <vector>

namespace orc {

// Matroska stores presentation times only. When the video sink remuxes its
// own Matroska output to add chapters and disc metadata, a stream that has
// B-frames (the lossy emulation format, mkv-h264) comes back with the decode
// time unset on its first few packets: the encoder gave them negative decode
// times, which the file cannot record, and the demuxer cannot know them until
// a later packet. Passing such packets on leaves the muxer to guess, which
// FFmpeg deprecates.
//
// Given the durations of the packets that precede the first packet with a
// known decode time, in decode order, and that decode time, returns the
// decode times of those packets: each one a packet duration before the next.
// A duration of zero or less is replaced by fallback_duration. All values are
// in the stream's time base. The function is pure, so it can be unit tested.
inline std::vector<int64_t> backfill_leading_dts(
    const std::vector<int64_t>& durations, int64_t first_known_dts,
    int64_t fallback_duration) {
  std::vector<int64_t> dts(durations.size());
  int64_t next = first_known_dts;
  for (size_t i = durations.size(); i-- > 0;) {
    const int64_t duration =
        durations[i] > 0 ? durations[i] : fallback_duration;
    next -= duration;
    dts[i] = next;
  }
  return dts;
}

}  // namespace orc

#endif  // ORC_STAGE_PLUGIN_VIDEO_SINK_REMUX_DTS_H
