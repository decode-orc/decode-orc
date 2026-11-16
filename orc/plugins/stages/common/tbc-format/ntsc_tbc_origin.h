/*
 * File:        ntsc_tbc_origin.h
 * Module:      orc-plugin-common
 * Purpose:     Moves NTSC frames between ld-decode's .tbc line origin and the
 *              CVBS specification's stored line origin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/cvbs_signal_constants.h>

#include <array>
#include <cstddef>
#include <cstdint>

// Header only, shared by tbc_source (ld-decode .tbc -> pipeline) and TBC Sink
// (pipeline -> ld-decode .tbc). Not part of the SDK contract.
//
// The pipeline works in the CVBS File Format Specification's NTSC origin
// (v1.8.1, video-standard-presets.md, NTSC "Stored line alignment"): sample 0
// of each stored line is the first sampling instant at or after 0H, so 0H falls
// 57/90 of a sample before it (SMPTE 244M-2003 §3.1-§3.2: 0H lies 0.367 sample
// after digital sample 784, and digital sample 785 is stored sample 0).
//
// ld-decode's NTSC .tbc stores 0H 1.4-1.9 samples after its sample 0 (measured
// on ve-snw-cut, ggv-ntsc-mb-v2800 and ntsc-basic). Both grids sample the same
// I/Q phases, so the two origins are a whole number of samples apart: two,
// which is 180 degrees of subcarrier. Stored sample i of the specification's
// frame is sample i + 2 of ld-decode's.
//
// NTSC lines are all 910 samples, so the shift moves the whole frame-flat
// stream and no line is resampled. The two samples that leave the front of an
// ld-decode frame (the front porch before frame line 0's 0H) are placed at the
// end of the specification's frame, where its last line's front porch ends.
// That is a rotation, so the move and its inverse round-trip bit for bit. Both
// positions are horizontal blanking, outside every active window.
//
// PAL_M is not moved: the specification's origin is taken as written for it
// (vhs-decode's PAL_M line origin is not a constant offset).
namespace orc::ntsc_tbc_origin {

// Whole samples from ld-decode's NTSC line origin to the specification's.
constexpr size_t kOriginShiftSamples = 2;

constexpr size_t kFrameSamples = static_cast<size_t>(kNtscFrameSamples);

// The specification-origin frame index of ld-decode frame sample |tbc_index|.
constexpr size_t spec_index_from_tbc(size_t tbc_index) {
  return (tbc_index + kFrameSamples - kOriginShiftSamples) % kFrameSamples;
}

// The ld-decode frame index of specification-origin frame sample |spec_index|.
constexpr size_t tbc_index_from_spec(size_t spec_index) {
  return (spec_index + kOriginShiftSamples) % kFrameSamples;
}

// A sample position stored in ld-decode NTSC metadata (active video, colour
// burst), given as a specification-origin position on the same line.
constexpr int32_t tbc_line_position_from_spec(int32_t spec_position) {
  return spec_position + static_cast<int32_t>(kOriginShiftSamples);
}

// A run of frame-flat samples [start, start + count).
struct FrameRun {
  uint64_t start = 0;
  uint64_t count = 0;
};

// A run moved by |shift| samples (mod the frame), split where it wraps past
// the frame's end. The second part is empty (count 0) when it does not wrap.
// |count| is clamped to one frame.
inline std::array<FrameRun, 2> rotate_run(uint64_t start, uint64_t count,
                                          uint64_t shift) {
  constexpr uint64_t n = kFrameSamples;
  if (count > n) count = n;
  const uint64_t moved = (start % n + shift % n) % n;
  if (moved + count <= n) return {FrameRun{moved, count}, FrameRun{}};
  const uint64_t head = n - moved;
  return {FrameRun{moved, head}, FrameRun{0, count - head}};
}

// An ld-decode frame-flat run in specification-origin coordinates.
inline std::array<FrameRun, 2> spec_run_from_tbc(uint64_t start,
                                                 uint64_t count) {
  return rotate_run(start, count, kFrameSamples - kOriginShiftSamples);
}

// A specification-origin frame-flat run in ld-decode coordinates.
inline std::array<FrameRun, 2> tbc_run_from_spec(uint64_t start,
                                                 uint64_t count) {
  return rotate_run(start, count, kOriginShiftSamples);
}

}  // namespace orc::ntsc_tbc_origin
