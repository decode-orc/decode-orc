/*
 * File:        frame_descriptor.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Per-frame metadata descriptor for CVBS_U10_4FSC frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>
#include <orc/stage/frame_id.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace orc {

// Metadata describing a single decoded frame in the CVBS_U10_4FSC domain.
//
// Signal-measured facts are deliberately NOT part of this descriptor:
// - Colour-sequence phase is a property of the burst signal, not of
//   source-side metadata, so stages that need it measure it uniformly (for
//   TBC and CVBS sources) via the host "colour_frame_phase" observer.  See
//   orc/stage/observation/colour_frame_phase_query.h.
// - VBI picture numbers and VITC/LTC timecodes are likewise measured from the
//   signal by the host "biphase" observer (interpreted "vbi.*" keys), not
//   baked in by source stages.
struct FrameDescriptor {
  FrameID frame_id = 0;
  VideoSystem system = VideoSystem::Unknown;

  // Total frame height in lines (625 PAL, 525 NTSC/PAL_M).
  size_t height = 0;

  // Total flat-buffer sample count for this frame.
  size_t samples_total = 0;

  // Nominal samples per line (1135 PAL, 910 NTSC, 909 PAL_M).
  // PAL frames may have up to 4 lines with one extra sample; this field
  // holds the base/nominal value.
  size_t samples_per_line_nominal = 0;

  // NTSC-J non-standard black level in the CVBS_U10_4FSC 10-bit domain.
  // Present only when the source metadata carries an explicit black-level
  // override (e.g., a Japanese NTSC disc mastered at 0 IRE).
  std::optional<int32_t> black_level_override;

  // True for frames with no real signal content: synthetic frames inserted to
  // fill sequence gaps (frame_map, source_align) and source frames whose TBC
  // metadata marks both fields as padding. The host observer pass skips such
  // frames and records "padded, no data" observations instead (issue #77).
  bool is_padding_frame = false;

  // How far the frame's picture is held, as a stage upstream decided it from
  // the picture itself (the stacker, from the repeats it stacks with
  // still_frame_stacking): the same picture is shown from frame_id -
  // held_frames_before to frame_id + held_frames_after. Zero when no stage
  // found a repeat on that side, or none looked; held_checked says which. A
  // consumer can use it to choose temporal processing only where every frame
  // it reaches shows the same picture (the video sink's "held:" decoder
  // schedule entry and its still_only noise reduction).
  uint32_t held_frames_before = 0;
  uint32_t held_frames_after = 0;
  bool held_checked = false;
};

}  // namespace orc
