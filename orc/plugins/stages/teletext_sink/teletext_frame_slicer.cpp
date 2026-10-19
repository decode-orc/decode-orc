/*
 * File:        teletext_frame_slicer.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Per-frame teletext line recovery implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_frame_slicer.h"

#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace orc {

namespace {

// The slicer options the stage's tuning parameters map onto. The system is set
// per slicer by the constructor below.
TeletextSlicerOptions to_slicer_options(
    const TeletextFrameSlicerOptions& options) {
  TeletextSlicerOptions slicer_options;
  slicer_options.detector = options.detector;
  slicer_options.parity_repair = options.parity_repair;
  return slicer_options;
}

// The slicer for entry |index| of kTeletextVideoSystems, from that system's
// own profile — so the sample rate and the teletext system are paired in one
// place rather than asserted again here.
TeletextSlicer make_slicer(size_t index,
                           const TeletextFrameSlicerOptions& options) {
  const auto profile =
      TeletextFrameSlicer::profile_for(kTeletextVideoSystems[index]);
  return TeletextSlicer(profile.sample_rate, profile.teletext_system,
                        to_slicer_options(options));
}

}  // namespace

TeletextFrameSlicer::TeletextFrameSlicer(TeletextFrameSlicerOptions options)
    : options_(options),
      slicers_{{make_slicer(0, options), make_slicer(1, options),
                make_slicer(2, options)}} {
  static_assert(kTeletextVideoSystems.size() == 3,
                "add a make_slicer() row when a television system is added");
}

TeletextFrameSlicer::SystemProfile TeletextFrameSlicer::profile_for(
    VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
      // 625 lines: ETSI EN 300 706.
      return SystemProfile{true,
                           TeletextSystem::kWst625,
                           kTeletextFirstFieldLine625,
                           kTeletextLastFieldLine625,
                           kPalSampleRate,
                           static_cast<int16_t>(kPalBlack),
                           static_cast<int16_t>(kPalWhite),
                           0};
    case VideoSystem::NTSC:
      // 525 lines: ITU-R BT.653 Table 1b. NTSC and PAL_M share the line
      // structure and therefore the service; only the 4FSC sample rate
      // differs.
      return SystemProfile{true,
                           TeletextSystem::kWst525,
                           kTeletextFirstFieldLine525,
                           kTeletextLastFieldLine525,
                           kNtscSampleRate,
                           static_cast<int16_t>(kNtscBlack),
                           static_cast<int16_t>(kNtscWhite),
                           1};
    case VideoSystem::PAL_M:
      return SystemProfile{true,
                           TeletextSystem::kWst525,
                           kTeletextFirstFieldLine525,
                           kTeletextLastFieldLine525,
                           kPalMSampleRate,
                           static_cast<int16_t>(kNtscBlack),
                           static_cast<int16_t>(kNtscWhite),
                           2};
    case VideoSystem::Unknown:
      break;
  }
  SystemProfile none;
  none.carries_teletext = false;
  return none;
}

TeletextFrameSlicer::SystemProfile TeletextFrameSlicer::effective_profile(
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

void TeletextFrameSlicer::slice_field(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    size_t field_idx, uint64_t frame_index,
    const TeletextScanSnapshot& snapshot, TeletextFieldScan& out) const {
  out.clear();

  const auto vp_opt = representation.get_video_parameters();
  if (!vp_opt.has_value()) {
    return;
  }
  const auto& vp = vp_opt.value();

  const SystemProfile profile = effective_profile(vp.system);
  if (!profile.carries_teletext ||
      profile.last_field_line < profile.first_field_line) {
    return;
  }
  // The nominal slicers are built once; a learned bit clock needs its own.
  TeletextSlicerOptions slicer_options = to_slicer_options(options_);
  slicer_options.bit_clock = snapshot.bit_clock;
  std::optional<TeletextSlicer> clocked;
  if (snapshot.bit_clock != 1.0) {
    clocked.emplace(profile.sample_rate, profile.teletext_system,
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

  out.lines.reserve(
      static_cast<size_t>(profile.last_field_line - profile.first_field_line) +
      1);

  // Read once for the whole field: the hint is the same for every line of it,
  // and whether the mask applies to this frame at all is a property of the
  // frame rather than of the line.
  const TeletextPhaseHint phase_hint = snapshot.acquisition_hint();
  const bool mask_lines = !snapshot.reads_full_window(frame_index);

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

    TeletextFrameLineResult entry;
    entry.field_line = field_line;
    entry.flat_line = flat_line;
    entry.sliced = slicer.slice(line_data, sample_count, black_level,
                                white_level, phase_hint);
    if (snapshot.calibrate_clock && entry.sliced.valid) {
      entry.clock_scores = TeletextClockCalibrator::score(
          line_data, sample_count, black_level, white_level,
          profile.sample_rate, profile.teletext_system, slicer_options,
          entry.sliced);
    }
    out.lines.push_back(entry);
  }
}

}  // namespace orc
