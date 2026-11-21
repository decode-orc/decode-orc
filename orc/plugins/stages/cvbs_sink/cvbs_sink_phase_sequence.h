/*
 * File:        cvbs_sink_phase_sequence.h
 * Module:      orc-core
 * Purpose:     Colour-sequence measurement for the CVBS sink's
 *              signal_state_preset and sequence_continuous decisions
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_CVBS_SINK_PHASE_SEQUENCE_H
#define ORC_CORE_CVBS_SINK_PHASE_SEQUENCE_H

#include <orc/stage/common_types.h>

#include <cstdint>
#include <optional>
#include <string>

namespace orc {

// Measures the colour-sequence phase of the frames a CVBS sink writes, so the
// .meta can state what was observed rather than what was assumed.  CVBS file
// format spec v1.6.0 separates the two verdicts this produces:
//
//  - signal_state_preset LOCK axis (phase locked): the burst was measurable
//    at the standard subcarrier-reference-locked phase points in at least one
//    frame.  A file with no measurable burst anywhere (monochrome material,
//    or an unknown video system) cannot support the claim and is UNLOCKED.
//  - sequence_continuous: whether the measured colour sequence ran unbroken
//    through every frame written.  Unknown (NULL) when nothing was
//    measurable; a discontinuity is a property of the content and must not
//    downgrade the preset.
//
// The phase IDs fed in here come from the host "colour_frame_phase" observer
// (see orc/stage/observation/colour_frame_phase_query.h): 1-8 per field for
// PAL/PAL_M (EBU Tech. 3280-E §1.1.1) and 1-4 for NTSC (SMPTE 244M-2003 §3.2),
// both measured from the burst, with -1 where it could not be measured.
//
// SECAM has no burst and no Sc/H convention.  Its ids are 1-4, the four
// fields of the 2-frame sequence (frame 1 = 1/2, frame 2 = 3/4), read from
// whether each line carries D'R or D'B; a break in the D'R/D'B alternation
// between consecutive frames is a discontinuity (CVBS spec v1.8.1, Preset
// SECAM, "Colour frame sequence").  The ids say nothing about the sampling
// phase, and the specification forbids a locked SECAM file, so SECAM is
// always written UNLOCKED; sequence_continuous is checked as for the others.
// A 2-frame cycle cannot see a skip of an even number of frames.
//
// Two things are checked per frame: that its second field follows its first,
// and that its first field follows the previous measured frame's.  A frame
// advances the sequence by two fields, so a frame whose burst is unmeasurable
// (blank leader, lead-in, black frame) is not a discontinuity in itself: the
// expected phase is projected across the gap and re-checked on the far side.
// Penalising those would mark all but the cleanest capture unlocked.
//
// Feed observe() exactly once per frame actually written, in output order.
class ColourPhaseSequenceCheck {
 public:
  explicit ColourPhaseSequenceCheck(VideoSystem system)
      : fields_in_sequence_(fields_in_sequence(system)),
        lock_measurable_(system != VideoSystem::SECAM) {}

  // Record one written frame's per-field phase IDs (-1 where unmeasurable).
  void observe(int32_t field1_phase_id, int32_t field2_phase_id) {
    const uint64_t frame_index = frames_seen_;
    ++frames_seen_;

    if (fields_in_sequence_ == 0) return;  // unknown system: nothing to check
    if (!is_valid(field1_phase_id) || !is_valid(field2_phase_id)) return;

    ++frames_measured_;

    bool broken = (field2_phase_id != advance(field1_phase_id, 1));

    if (last_measured_frame_.has_value()) {
      const uint64_t gap = frame_index - *last_measured_frame_;
      if (field1_phase_id != advance(last_field1_phase_id_, 2 * gap)) {
        broken = true;
      }
    }

    if (broken) {
      ++discontinuities_;
      if (!first_discontinuity_frame_.has_value()) {
        first_discontinuity_frame_ = frame_index;
      }
    }

    // Re-anchor on this frame either way, so one skip costs one discontinuity
    // rather than marking every remaining frame as broken.
    last_measured_frame_ = frame_index;
    last_field1_phase_id_ = field1_phase_id;
  }

  // True when the burst was measurable at the standard phase points in at
  // least one frame.  A file in which no burst could be measured at all is
  // not phase locked: there is nothing to support the claim.  Never for
  // SECAM, whose sequence ids are not a phase measurement.
  bool phase_locked() const { return lock_measurable_ && frames_measured_ > 0; }

  // Tri-state continuity verdict for the .meta sequence_continuous column:
  // true when the measured sequence ran unbroken, false when it broke at
  // least once, nullopt when nothing was measurable (continuity unknown).
  std::optional<bool> sequence_continuous() const {
    if (frames_measured_ == 0) return std::nullopt;
    return discontinuities_ == 0;
  }

  const char* signal_state_preset() const {
    return phase_locked() ? "STANDARD_STABLE_LOCKED"
                          : "STANDARD_STABLE_UNLOCKED";
  }

  uint64_t frames_seen() const { return frames_seen_; }
  uint64_t frames_measured() const { return frames_measured_; }
  uint64_t discontinuities() const { return discontinuities_; }

  // Output-file frame index of the first discontinuity, if any.
  std::optional<uint64_t> first_discontinuity_frame() const {
    return first_discontinuity_frame_;
  }

  // One line for the log and the trigger status, explaining the verdicts.
  std::string summary() const {
    if (fields_in_sequence_ == 0) {
      return "colour phase sequence not checked (unknown video system): "
             "written as STANDARD_STABLE_UNLOCKED, sequence_continuous "
             "unknown";
    }
    if (!lock_measurable_) {
      // SECAM: the D'R/D'B line sequence, never locked.
      if (frames_measured_ == 0) {
        return "SECAM line sequence unmeasurable (no D'R/D'B reference found "
               "in any frame): written as STANDARD_STABLE_UNLOCKED, "
               "sequence_continuous unknown";
      }
      if (discontinuities_ == 0) {
        return "SECAM line sequence continuous over " +
               std::to_string(frames_measured_) +
               " measured frames: written as STANDARD_STABLE_UNLOCKED with "
               "sequence_continuous = TRUE";
      }
      return "SECAM line sequence breaks " + std::to_string(discontinuities_) +
             " time(s), first at output frame " +
             std::to_string(first_discontinuity_frame_.value_or(0)) +
             ": written as STANDARD_STABLE_UNLOCKED with sequence_continuous "
             "= FALSE";
    }
    if (frames_measured_ == 0) {
      return "colour phase sequence unmeasurable (no burst found in any "
             "frame): written as STANDARD_STABLE_UNLOCKED, "
             "sequence_continuous unknown";
    }
    if (discontinuities_ == 0) {
      return "colour phase sequence continuous over " +
             std::to_string(frames_measured_) +
             " measured frames: written as STANDARD_STABLE_LOCKED with "
             "sequence_continuous = TRUE";
    }
    return "colour phase sequence breaks " + std::to_string(discontinuities_) +
           " time(s), first at output frame " +
           std::to_string(first_discontinuity_frame_.value_or(0)) +
           ": written as STANDARD_STABLE_LOCKED with sequence_continuous = "
           "FALSE (run disc mapping to restore a continuous sequence)";
  }

 private:
  static int32_t fields_in_sequence(VideoSystem system) {
    switch (system) {
      case VideoSystem::PAL:
      case VideoSystem::PAL_M:
        return 8;
      case VideoSystem::NTSC:
      case VideoSystem::SECAM:  // 2 frames of 2 fields
        return 4;
      case VideoSystem::Unknown:
        break;
    }
    return 0;
  }

  bool is_valid(int32_t phase_id) const {
    return phase_id >= 1 && phase_id <= fields_in_sequence_;
  }

  int32_t advance(int32_t phase_id, uint64_t fields) const {
    const uint64_t n = static_cast<uint64_t>(fields_in_sequence_);
    return static_cast<int32_t>(
        ((static_cast<uint64_t>(phase_id - 1) + fields) % n) + 1);
  }

  int32_t fields_in_sequence_ = 0;
  bool lock_measurable_ = true;
  uint64_t frames_seen_ = 0;
  uint64_t frames_measured_ = 0;
  uint64_t discontinuities_ = 0;
  std::optional<uint64_t> first_discontinuity_frame_;
  std::optional<uint64_t> last_measured_frame_;
  int32_t last_field1_phase_id_ = 0;
};

}  // namespace orc

#endif  // ORC_CORE_CVBS_SINK_PHASE_SEQUENCE_H
