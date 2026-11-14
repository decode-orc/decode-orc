/*
 * File:        vbi_frame_builder.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Places VBI line records on an otherwise blank CVBS frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_frame_builder.h"

#include <algorithm>
#include <utility>

#include "vbi_line_mapping.h"
#include "vbi_teletext_service.h"

namespace orc {

namespace {

const std::vector<uint32_t>& empty_frame_lines() {
  static const std::vector<uint32_t> empty;
  return empty;
}

}  // namespace

VBIFrameBuilder::VBIFrameBuilder(
    VBISourceFormat format, VBIOutputFrame output_frame,
    VBIDataPlacement placement, VBIRecordResampler resampler,
    VBILevelMapper level_mapper,
    std::array<std::vector<uint32_t>, 2> frame_lines)
    : format_(format),
      output_frame_(output_frame),
      placement_(placement),
      resampler_(std::move(resampler)),
      level_mapper_(std::move(level_mapper)),
      frame_lines_(std::move(frame_lines)) {
  // Slots follow the reader's record order: the first stored field's records,
  // then the second's.
  for (const std::vector<uint32_t>& field_lines : frame_lines_) {
    slot_lines_.insert(slot_lines_.end(), field_lines.begin(),
                       field_lines.end());
  }

  line_slots_.assign(output_frame_.lines_per_frame, kNoSlot);
  slot_line_stride_ = output_frame_.samples_per_line_nominal;
  for (size_t slot = 0; slot < slot_lines_.size(); ++slot) {
    const uint32_t frame_line = slot_lines_[slot];
    if (frame_line < line_slots_.size()) {
      line_slots_[frame_line] = static_cast<int32_t>(slot);
    }
    slot_line_stride_ =
        std::max(slot_line_stride_, output_frame_.line_length(frame_line));
  }
}

const std::vector<uint32_t>& VBIFrameBuilder::frame_lines(
    uint32_t stored_field_index) const {
  if (stored_field_index >= frame_lines_.size()) {
    return empty_frame_lines();
  }
  return frame_lines_[stored_field_index];
}

void VBIFrameBuilder::build_blank_frame(
    std::vector<int16_t>& out_samples) const {
  out_samples.assign(output_frame_.samples_per_frame,
                     static_cast<int16_t>(output_frame_.levels.blanking));
}

bool VBIFrameBuilder::build_frame(const std::vector<VBILineRecord>& records,
                                  std::vector<int16_t>& out_samples,
                                  uint32_t& out_data_lines,
                                  std::string& error_message) const {
  out_data_lines = 0;

  VBIPlacedFrame placed;
  if (!place_frame(records, placed, error_message)) {
    return false;
  }
  render_frame(placed, out_samples);
  out_data_lines = placed.data_lines;
  return true;
}

void VBIFrameBuilder::place_blank_frame(VBIPlacedFrame& out_placed) const {
  out_placed.line_stride = slot_line_stride_;
  out_placed.slot_placed.assign(slot_lines_.size(), 0);
  out_placed.slot_resolved.assign(slot_lines_.size(), 1);
  out_placed.samples.assign(
      slot_lines_.size() * slot_line_stride_,
      static_cast<int16_t>(output_frame_.levels.blanking));
  out_placed.data_lines = 0;
}

bool VBIFrameBuilder::place_frame(const std::vector<VBILineRecord>& records,
                                  VBIPlacedFrame& out_placed,
                                  std::string& error_message) const {
  return place_slots(records, nullptr, out_placed, error_message);
}

bool VBIFrameBuilder::place_frame(const std::vector<VBILineRecord>& records,
                                  const std::vector<uint8_t>& wanted,
                                  VBIPlacedFrame& out_placed,
                                  std::string& error_message) const {
  return place_slots(records, &wanted, out_placed, error_message);
}

bool VBIFrameBuilder::place_slots(const std::vector<VBILineRecord>& records,
                                  const std::vector<uint8_t>* wanted,
                                  VBIPlacedFrame& out_placed,
                                  std::string& error_message) const {
  if (output_frame_.samples_per_frame == 0 || resampler_.output_count() == 0) {
    error_message =
        "The frame builder has no output lattice, so no frame can be built.";
    return false;
  }

  place_blank_frame(out_placed);

  std::vector<VBILineLevels> levels;
  level_mapper_.map_frame(records, levels);

  // Slots per stored field: the records of the format's field range.
  const size_t field_slots = frame_lines_[0].size();

  // The records placed, and the slot each goes to.
  std::vector<size_t> placed;
  std::vector<size_t> slots;
  std::vector<const std::vector<double>*> sources;
  for (size_t index = 0; index < records.size(); ++index) {
    const VBILineRecord& record = records[index];
    if (record.samples.empty() || !levels[index].usable) {
      // No level reference could be established, so there is nothing to
      // normalise the record against.  The line stays as blanking rather than
      // carrying an arbitrarily scaled waveform.
      continue;
    }

    const std::vector<uint32_t>& lines = frame_lines(record.field_index);
    if (record.record_index < format_.field_range.start ||
        record.record_index - format_.field_range.start >= lines.size()) {
      error_message = "Stored field " + std::to_string(record.field_index) +
                      " record " + std::to_string(record.record_index) +
                      " is outside the configured field range, so there is no "
                      "frame line for it.";
      return false;
    }
    const size_t slot = static_cast<size_t>(record.field_index) * field_slots +
                        (record.record_index - format_.field_range.start);
    if (wanted != nullptr && (slot >= wanted->size() || (*wanted)[slot] == 0)) {
      // Not asked for: left unresolved, and not resampled.
      out_placed.slot_resolved[slot] = 0;
      continue;
    }
    placed.push_back(index);
    slots.push_back(slot);
    sources.push_back(&record.samples);
  }

  // The frame's records are resampled together, which is several times
  // quicker than one at a time and the same to the bit (vbi_resampler.h).  The
  // thread placing frames keeps the buffer from one to the next.
  thread_local std::vector<double> resampled;
  resampler_.resample_records(sources, resampled);

  const size_t count = resampler_.output_count();
  for (size_t at = 0; at < placed.size(); ++at) {
    const VBISampleMap map =
        make_vbi_sample_map(levels[placed[at]], output_frame_.levels);
    const double* samples = resampled.data() + at * count;
    int16_t* line = out_placed.samples.data() + slots[at] * slot_line_stride_ +
                    placement_.output_begin;
    for (size_t sample = 0; sample < count; ++sample) {
      line[sample] = clamp_vbi_output_sample(map.apply(samples[sample]));
    }

    out_placed.slot_placed[slots[at]] = 1;
    ++out_placed.data_lines;
  }

  return true;
}

void VBIFrameBuilder::render_frame(const VBIPlacedFrame& placed,
                                   std::vector<int16_t>& out_samples) const {
  build_blank_frame(out_samples);

  // Only the data window of a placed line differs from blanking, so that is
  // all that is copied; the rest of its frame line is blanking already.
  const size_t begin = placement_.output_begin;
  const size_t count = resampler_.output_count();
  const size_t slots = std::min(placed.slot_placed.size(), slot_lines_.size());
  for (size_t slot = 0; slot < slots; ++slot) {
    if (placed.slot_placed[slot] == 0) {
      continue;
    }
    const int16_t* source = placed.line(slot) + begin;
    int16_t* destination = out_samples.data() +
                           output_frame_.line_offset(slot_lines_[slot]) + begin;
    std::copy(source, source + count, destination);
  }
}

bool make_vbi_frame_builder(const VBISourceFormat& format,
                            const VBILevelMapperConfig& level_config,
                            const VBIResolvedTiming& timing,
                            VBIFrameBuilder& out_builder,
                            std::string& error_message) {
  out_builder = VBIFrameBuilder();

  const double capture_offset_samples = timing.capture_offset_samples;

  VBIOutputFrame output_frame;
  if (!make_vbi_output_frame(format.tv_system, format.tt_system, output_frame,
                             error_message)) {
    return false;
  }

  VBITeletextService service;
  if (!vbi_teletext_service(format.tv_system, format.tt_system, service,
                            error_message)) {
    return false;
  }

  // A measured anchor replaces the tabulated one for everything downstream:
  // the data window is cut from it, and so are the windows the level mapper
  // reads its logic levels from.  Both have to follow the run-in to where it
  // actually is, or the window is cut around the wrong part of the line.
  if (timing.service_anchor_ns.has_value()) {
    service.t_offset_ns = *timing.service_anchor_ns;
  }

  VBITeletextLineMap line_map;
  if (!make_vbi_teletext_line_map(format.tv_system, format.tt_system, line_map,
                                  error_message)) {
    return false;
  }

  VBIDataPlacement placement;
  if (!make_vbi_data_placement(format, timing.written_service.value_or(service),
                               output_frame, capture_offset_samples, placement,
                               error_message)) {
    return false;
  }
  if (placement.output_count() == 0) {
    error_message =
        "No part of a line record falls inside the data region of its output "
        "line, so the capture offset or the sampling rate must be wrong.";
    return false;
  }

  // Resolve every record's frame line once, and reject a configuration that
  // puts two records of a frame on the same line: that is a wrong field range,
  // and truncating or overwriting silently would hide it (design §5.1).
  std::array<std::vector<uint32_t>, 2> frame_lines;
  std::vector<uint32_t> taken;
  for (uint32_t field = 0; field < 2u; ++field) {
    for (uint32_t record = format.field_range.start;
         record <= format.field_range.end + format.caption_records; ++record) {
      uint32_t frame_line = 0;
      if (!map_vbi_record_to_frame_line(format, line_map, field, record,
                                        frame_line, error_message)) {
        return false;
      }
      if (std::find(taken.begin(), taken.end(), frame_line) != taken.end()) {
        error_message = "Stored field " + std::to_string(field) + " record " +
                        std::to_string(record) + " maps to frame line " +
                        std::to_string(frame_line) +
                        ", which another record of the same frame already "
                        "occupies.";
        return false;
      }
      if (static_cast<uint64_t>(frame_line) >= output_frame.lines_per_frame) {
        error_message = "Frame line " + std::to_string(frame_line) +
                        " is outside the " +
                        std::to_string(output_frame.lines_per_frame) +
                        " line output frame.";
        return false;
      }
      taken.push_back(frame_line);
      frame_lines[field].push_back(frame_line);
    }
  }

  // A time-base corrected capture arrives with its levels already fixed, so the
  // level policy is not the caller's to choose: there is nothing to estimate
  // and the estimate would be wrong if it were made (vbi_level_mapper.h).
  const VBILevelMapperConfig levels =
      (format.family == VBISourceFamily::kTBCDerived)
          ? vbi_absolute_level_config(output_frame.levels)
          : level_config;

  VBIRecordResampler resampler(placement, format.valid_samples);
  VBILevelMapper level_mapper(format, service, levels);

  out_builder =
      VBIFrameBuilder(format, output_frame, placement, std::move(resampler),
                      std::move(level_mapper), std::move(frame_lines));
  return true;
}

}  // namespace orc
