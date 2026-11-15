/*
 * File:        frame_line_util.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Per-line sample count and offset helpers for 4FSC CVBS flat
 * frame buffers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <cstddef>
#include <cstdint>

// Free functions for per-line access within a 4FSC CVBS flat frame buffer.
// These are the appropriate primitives for stages that need line-level data;
// VideoFrameRepresentation is a frame-based interface and does not carry
// per-line geometry knowledge.
//
// EBU Tech. 3280-E §1.3.1 (PAL); SMPTE 244M-2003 §4.1 (NTSC / PAL_M).

namespace orc {

// Returns the number of samples in frame-flat line |line| (0-based).
// For PAL and SECAM (the 625-line 4fsc lattice; CVBS File Format Specification
// v1.8.1, Preset SECAM), lines listed in kPalExtraSampleLines carry additional
// samples; each
// occurrence of a line index in that array adds 1 to its count (so lines 312
// and 624, which appear twice, carry spl_nominal + 2 = 1137 samples).
// All other PAL lines and all NTSC/PAL_M lines carry |spl_nominal| samples.
inline size_t frame_line_sample_count(VideoSystem system, size_t spl_nominal,
                                      size_t line) {
  if (is_625_line(system)) {
    const auto l = static_cast<int32_t>(line);
    size_t extra = 0;
    for (int32_t e : kPalExtraSampleLines) {
      if (e == l) ++extra;
    }
    return spl_nominal + extra;
  }
  return spl_nominal;
}

// Returns the 0-based sample offset of the start of frame-flat line |line|
// within a flat 4FSC CVBS frame buffer.
// O(4) for PAL and SECAM (iterates kPalExtraSampleLines); O(1) for NTSC and
// PAL_M.
inline size_t frame_line_sample_offset(VideoSystem system, size_t spl_nominal,
                                       size_t line) {
  if (!is_625_line(system)) return line * spl_nominal;
  // Base: every line contributes spl_nominal (1135) samples.
  size_t offset = line * spl_nominal;
  // Each non-orthogonal line before |line| contributes one extra sample.
  for (int32_t extra : kPalExtraSampleLines) {
    if (extra < static_cast<int32_t>(line)) ++offset;
  }
  return offset;
}

// Converts a frame-flat sample offset to (flat_line, sample_within_line).
// O(1) for NTSC and PAL_M; O(4) for PAL and SECAM (corrects at most 4
// extra-sample lines that make simple division inexact).
inline std::pair<size_t, size_t> frame_flat_offset_to_line_sample(
    VideoSystem system, size_t spl_nominal, uint64_t flat_offset) {
  if (!is_625_line(system)) {
    return {static_cast<size_t>(flat_offset / spl_nominal),
            static_cast<size_t>(flat_offset % spl_nominal)};
  }
  // PAL: integer division by spl_nominal gives a good first estimate; correct
  // one step at a time until we find the line that brackets flat_offset. For a
  // valid in-range offset this converges in a handful of steps (PAL has at most
  // 4 extra-sample lines). Cap the search so a pathological (e.g. overflowed)
  // offset falls back to the nominal estimate instead of spinning forever —
  // callers still make progress and skip the out-of-range region (issue #209).
  size_t est = static_cast<size_t>(flat_offset / spl_nominal);
  constexpr size_t kMaxLineSearchSteps = 4096;
  for (size_t step = 0; step < kMaxLineSearchSteps; ++step) {
    size_t line_start =
        frame_line_sample_offset(VideoSystem::PAL, spl_nominal, est);
    size_t line_len =
        frame_line_sample_count(VideoSystem::PAL, spl_nominal, est);
    if (flat_offset < line_start) {
      --est;
    } else if (flat_offset >= line_start + line_len) {
      ++est;
    } else {
      return {est, static_cast<size_t>(flat_offset - line_start)};
    }
  }
  // Fallback: nominal mapping. sample_in_line (0) stays < line_len so the
  // caller's loop still advances and terminates.
  return {static_cast<size_t>(flat_offset / spl_nominal), 0};
}

}  // namespace orc
