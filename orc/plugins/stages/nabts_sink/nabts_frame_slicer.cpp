/*
 * File:        nabts_frame_slicer.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Per-frame NABTS line recovery implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_frame_slicer.h"

#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace orc {

namespace {

// The slicer options the stage's tuning parameters map onto. Every slicer this
// builds is a System C one, so the system is fixed here rather than carried
// through the options.
TeletextSlicerOptions to_slicer_options(
    const NabtsFrameSlicerOptions& options) {
  TeletextSlicerOptions slicer_options;
  slicer_options.detector = options.detector;
  // See NabtsFrameSlicerOptions: CEA-516 §3.3 leaves byte parity conditional
  // on the data group type, so it is never repaired here.
  slicer_options.parity_repair = false;
  return slicer_options;
}

// The slicer for entry |index| of kNabtsVideoSystems, from that system's own
// profile — so the sample rate comes from one place rather than being asserted
// again here.
TeletextSlicer make_slicer(size_t index,
                           const NabtsFrameSlicerOptions& options) {
  const auto profile = NabtsFrameSlicer::profile_for(kNabtsVideoSystems[index]);
  return TeletextSlicer(profile.sample_rate, TeletextSystem::kNabts525,
                        to_slicer_options(options));
}

// Lines of |profile|'s window a field reads: those at or after line 0, inside
// the frame, and not masked off for this frame. slice_field() reads exactly
// these, so a field's results never outnumber them.
size_t lines_in_window(const NabtsFrameSlicer::SystemProfile& profile,
                       size_t field_idx, size_t line_offset,
                       size_t frame_height, uint64_t frame_index,
                       bool mask_lines, const NabtsScanSnapshot& snapshot) {
  size_t count = 0;
  for (int32_t field_line = std::max(profile.first_field_line, 0);
       field_line <= profile.last_field_line; ++field_line) {
    if (mask_lines &&
        !snapshot.should_probe(frame_index, field_idx, field_line)) {
      continue;
    }
    if (line_offset + static_cast<size_t>(field_line) < frame_height) {
      ++count;
    }
  }
  return count;
}

}  // namespace

NabtsFrameSlicer::NabtsFrameSlicer(NabtsFrameSlicerOptions options)
    : options_(options),
      slicers_{{make_slicer(0, options), make_slicer(1, options)}} {
  static_assert(kNabtsVideoSystems.size() == 2,
                "add a make_slicer() row when a television system is added");
}

NabtsFrameSlicer::SystemProfile NabtsFrameSlicer::profile_for(
    VideoSystem system) {
  switch (system) {
    case VideoSystem::NTSC:
      // NTSC and PAL_M share the 525-line structure and therefore the service;
      // only the 4FSC sample rate differs.
      return SystemProfile{true,
                           kNabtsFirstFieldLine,
                           kNabtsLastFieldLine,
                           kNtscSampleRate,
                           static_cast<int16_t>(kNtscBlack),
                           static_cast<int16_t>(kNtscWhite),
                           0};
    case VideoSystem::PAL_M:
      return SystemProfile{true,
                           kNabtsFirstFieldLine,
                           kNabtsLastFieldLine,
                           kPalMSampleRate,
                           static_cast<int16_t>(kNtscBlack),
                           static_cast<int16_t>(kNtscWhite),
                           1};
    // PAL is 625 lines, and CEA-516 §1.1.1 specifies NABTS on the 525-line
    // signal, so there is no System C service there to recover; an unknown
    // system is not claimed to carry one either.
    case VideoSystem::PAL:
    case VideoSystem::Unknown:
      break;
  }
  SystemProfile none;
  none.carries_nabts = false;
  return none;
}

NabtsFrameSlicer::SystemProfile NabtsFrameSlicer::effective_profile(
    VideoSystem system) const {
  SystemProfile profile = profile_for(system);
  if (options_.first_field_line.has_value()) {
    profile.first_field_line = *options_.first_field_line;
  }
  if (options_.last_field_line.has_value()) {
    profile.last_field_line = *options_.last_field_line;
  }
  return profile;
}

size_t NabtsFrameSlicer::lines_to_read(
    const VideoFrameRepresentation& representation, size_t field_idx,
    uint64_t frame_index, const NabtsScanSnapshot& snapshot) const {
  const auto vp_opt = representation.get_video_parameters();
  if (!vp_opt.has_value()) {
    return 0;
  }
  const SystemProfile profile = effective_profile(vp_opt->system);
  if (!profile.carries_nabts ||
      profile.last_field_line < profile.first_field_line) {
    return 0;
  }
  const size_t line_offset =
      (field_idx == 0) ? 0 : field1_lines(vp_opt->system);
  return lines_in_window(profile, field_idx, line_offset,
                         static_cast<size_t>(vp_opt->frame_height), frame_index,
                         !snapshot.reads_full_window(frame_index), snapshot);
}

void NabtsFrameSlicer::slice_field(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    size_t field_idx, uint64_t frame_index, const NabtsScanSnapshot& snapshot,
    NabtsFieldScan& out) const {
  out.clear();

  const auto vp_opt = representation.get_video_parameters();
  if (!vp_opt.has_value()) {
    return;
  }
  const auto& vp = vp_opt.value();

  const SystemProfile profile = effective_profile(vp.system);
  if (!profile.carries_nabts ||
      profile.last_field_line < profile.first_field_line) {
    return;
  }
  // The nominal slicers are built once; a learned bit clock needs its own.
  TeletextSlicerOptions slicer_options = to_slicer_options(options_);
  slicer_options.bit_clock = snapshot.bit_clock;
  std::optional<TeletextSlicer> clocked;
  if (snapshot.bit_clock != 1.0) {
    clocked.emplace(profile.sample_rate, TeletextSystem::kNabts525,
                    slicer_options);
  }
  const TeletextSlicer& slicer =
      clocked ? *clocked : slicers_[profile.slicer_index];

  // Levels from the source, falling back to the system's own (see
  // SystemProfile) when it states none.
  const int16_t black_level = static_cast<int16_t>(
      vp.black_level >= 0 ? vp.black_level : profile.default_black);
  const int16_t white_level = static_cast<int16_t>(
      vp.white_level >= 0 ? vp.white_level : profile.default_white);

  const size_t f1_lines = field1_lines(vp.system);
  const size_t line_width = static_cast<size_t>(vp.frame_width_nominal);
  const size_t frame_height = static_cast<size_t>(vp.frame_height);
  const size_t line_offset = (field_idx == 0) ? 0 : f1_lines;

  // Read once for the whole field: the hint is the same for every line of it,
  // and whether the mask applies to this frame at all is a property of the
  // frame rather than of the line.
  const TeletextPhaseHint phase_hint = snapshot.acquisition_hint();
  const bool mask_lines = !snapshot.reads_full_window(frame_index);

  // The lines it reads rather than the whole window; nothing at all where the
  // caller sized it already (lines_to_read()).
  out.lines.reserve(lines_in_window(profile, field_idx, line_offset,
                                    frame_height, frame_index, mask_lines,
                                    snapshot));

  for (int32_t field_line = profile.first_field_line;
       field_line <= profile.last_field_line; ++field_line) {
    if (field_line < 0) {
      continue;
    }
    if (mask_lines &&
        !snapshot.should_probe(frame_index, field_idx, field_line)) {
      ++out.lines_skipped;
      continue;
    }
    const size_t flat_line = line_offset + static_cast<size_t>(field_line);
    if (flat_line >= frame_height) {
      continue;
    }

    // Luma-aware fetch: YC sources carry the data burst in the luma channel;
    // composite sources go through get_line_samples() so per-line reads use the
    // source stage's field buffering.
    const int16_t* line_data = nullptr;
    size_t sample_count = 0;
    std::vector<int16_t> line_copy;
    if (representation.has_separate_channels()) {
      line_data = representation.get_line_luma(frame_id, flat_line);
      sample_count = line_width;
    } else {
      line_copy = representation.get_line_samples(frame_id, flat_line);
      line_data = line_copy.data();
      sample_count = line_copy.size();
    }
    if (line_data == nullptr || sample_count == 0) {
      continue;
    }

    NabtsFrameLineResult entry;
    entry.field_line = field_line;
    entry.flat_line = flat_line;
    entry.sliced = slicer.slice(line_data, sample_count, black_level,
                                white_level, phase_hint);
    if (snapshot.calibrate_clock && entry.sliced.valid) {
      entry.clock_scores = TeletextClockCalibrator::score(
          line_data, sample_count, black_level, white_level,
          profile.sample_rate, TeletextSystem::kNabts525, slicer_options,
          entry.sliced);
    }
    out.lines.push_back(entry);
  }
}

}  // namespace orc
