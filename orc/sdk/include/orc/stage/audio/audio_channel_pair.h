/*
 * File:        audio_channel_pair.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Audio channel-pair model shared by all
 *              VideoFrameRepresentation implementations (descriptors,
 *              pair limits, SMPTE 272M cadence timing)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// SDK TIER: stage/audio — stage contract type crossing the plugin boundary.
// A layout change here bumps the host ABI version.

#include <orc/stage/common_types.h>

#include <cstdint>
#include <string>

namespace orc {

// ============================================================================
// Audio channel-pair model
// ============================================================================
// Every audio channel pair in the pipeline is stereo, interleaved int32_t
// pairs (L, R, L, R, …) carrying 24-bit two's-complement values in the range
// −8388608 … 8388607, sampled at 48000 Hz synchronous (frame-locked) to
// video — the only audio form permitted by the CVBS file format
// specification (SMPTE 272M-1994 compliant). There is no free-running
// regime; producers whose native material is not 48 kHz synchronous resample
// at the point of production. Producers and transforms must saturate to the
// 24-bit range. Pipeline pair p maps to container file _audio_<p>.wav and
// SMPTE 272M channels 2p+1 / 2p+2.

// Maximum number of stereo audio channel pairs a representation may carry —
// the CVBS container limit (8 pairs = 16 SMPTE 272M channels). Enforced at
// every pair-adding stage, not just at the container sink, so a DAG that
// validates also exports.
inline constexpr size_t kMaxAudioChannelPairs = 8;

// SMPTE 272M-1994 §1.2: 48 kHz, clock locked (synchronous) to video.
inline constexpr uint32_t kAudioSampleRateHz = 48000;

// SMPTE 272M-1994 §1.3/§3.10: 24-bit audio (level C operation).
inline constexpr uint32_t kAudioBitDepth = 24;

// Where a channel pair's audio came from.
enum class AudioOrigin : uint8_t {
  ANALOGUE,  // decoder-provided analogue audio (LD analogue, tape linear)
  HIFI,      // tape Hi-Fi stereo
  EFM,       // decoded EFM digital audio
  IMPORTED,  // attached external WAV
  DERIVED,   // produced by a transform (e.g. bilingual split)
  UNKNOWN,
};

struct AudioChannelPairDescriptor {
  std::string name;  // human-readable, e.g. "Analogue", "EFM digital audio"
  AudioOrigin origin = AudioOrigin::UNKNOWN;
};

// ----------------------------------------------------------------------------
// The audio frame sequence (cadence)
// ----------------------------------------------------------------------------
// Per-frame stereo-pair counts follow SMPTE 272M-1994 §3.7/§3.8/§14.3:
//   PAL          — 48000 / 25 = 1920 pairs per frame, constant.
//   NTSC / PAL-M — 48000 ÷ (30000/1001) = 8008/5 pairs per frame: a 5-frame
//                  audio frame sequence totalling 8008 pairs
//                  (SMPTE 272M-1994 §14.3 Table 1: 1602/1601/1602/1601/1602).
// These helpers are the single authority for frame→pair-offset mapping;
// every stage and sink that maps frames to audio positions MUST use them so
// window boundaries agree pipeline-wide.

// Cumulative stereo-pair offset of the START of frame |frame_index|:
//   PAL/SECAM: 1920 × n.   NTSC/PAL-M: round(n × 8008 / 5), computed with exact
//   64-bit integer arithmetic. Cumulative in-sequence offsets are
//   0, 1602, 3203, 4805, 6406 — no drift accumulates over any range.
// Returns 0 for VideoSystem::Unknown (no defined audio layout).
inline constexpr uint64_t audio_pair_offset(uint64_t frame_index,
                                            VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
    case VideoSystem::SECAM:
      // ITU-R BT.1700 Annex 1 Part B (625-line PAL): 25 frames/s.
      // ITU-R BT.1700-1 Annex 1 Part C: SECAM has 625-line PAL timing.
      return frame_index * 1920u;
    case VideoSystem::NTSC:
    case VideoSystem::PAL_M:
      // SMPTE 170M-2004 Section 11.3: 30000/1001 frames/s.
      // offset = round(frame_index × 8008 / 5).
      return (frame_index * 8008u + 2u) / 5u;
    case VideoSystem::Unknown:
      break;
  }
  return 0;
}

// Stereo pairs in frame |frame_index| = offset(n + 1) − offset(n):
//   PAL/SECAM 1920 constant; NTSC/PAL-M 1602 or 1601 by audio frame number
//   (1602 when n mod 5 ∈ {0, 2, 4}, 1601 when n mod 5 ∈ {1, 3}).
// Returns 0 for VideoSystem::Unknown.
inline constexpr uint32_t audio_pairs_in_frame(uint64_t frame_index,
                                               VideoSystem system) {
  return static_cast<uint32_t>(audio_pair_offset(frame_index + 1, system) -
                               audio_pair_offset(frame_index, system));
}

}  // namespace orc
