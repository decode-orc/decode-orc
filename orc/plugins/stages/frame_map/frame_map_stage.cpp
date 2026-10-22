/*
 * File:        frame_map_stage.cpp
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     Frame mapping/reordering and sequence-correction stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_map_stage.h"

#include <orc/stage/error_types.h>
#include <orc/stage/observation/colour_frame_phase_query.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <sstream>
#include <variant>

namespace orc {

// ============================================================================
// FrameMappedRepresentation
// ============================================================================

static constexpr FrameID kPaddingFrameID{UINT64_MAX};

FrameMappedRepresentation::FrameMappedRepresentation(
    std::shared_ptr<const VideoFrameRepresentation> source,
    std::vector<FrameID> frame_mapping,
    std::vector<PaddingDescriptor> padding_descriptors, const std::string& tag)
    : VideoFrameRepresentationWrapper(std::move(source)),
      Artifact(ArtifactID("frame_map_" + tag), Provenance{}),
      frame_mapping_(std::move(frame_mapping)),
      padding_descriptors_(std::move(padding_descriptors)) {}

std::optional<size_t> FrameMappedRepresentation::resolve_index(
    FrameID id) const {
  if (id >= static_cast<FrameID>(frame_mapping_.size())) {
    return std::nullopt;
  }
  return static_cast<size_t>(id);
}

bool FrameMappedRepresentation::is_padding(size_t index) const {
  return frame_mapping_[index] == kPaddingFrameID;
}

const FrameMappedRepresentation::PaddingDescriptor*
FrameMappedRepresentation::find_padding(FrameID output_id) const {
  for (const auto& pd : padding_descriptors_) {
    if (pd.output_id == output_id) return &pd;
  }
  return nullptr;
}

void FrameMappedRepresentation::ensure_black_frame() const {
  if (!black_frame_.empty()) return;
  if (!source_) return;
  auto params = source_->get_video_parameters();
  if (!params) return;
  // PAL lines 312 and 624 carry extra samples, so a frame is not
  // height x nominal width.
  size_t total = (params->frame_width_nominal > 0 && params->frame_height > 0)
                     ? frame_line_sample_offset(
                           params->system,
                           static_cast<size_t>(params->frame_width_nominal),
                           static_cast<size_t>(params->frame_height))
                     : 0;
  if (total == 0) {
    // Fallback: use first real frame size
    auto range = source_->frame_range();
    for (FrameID fid = range.first; fid <= range.last; ++fid) {
      auto desc = source_->get_frame_descriptor(fid);
      if (desc) {
        total = desc->samples_total;
        break;
      }
    }
  }
  black_frame_.assign(total,
                      static_cast<sample_type>(
                          source_->get_video_parameters()->blanking_level));
}

FrameIDRange FrameMappedRepresentation::frame_range() const {
  if (frame_mapping_.empty()) return FrameIDRange{FrameID{0}, FrameID{0}};
  // FrameIDRange.last is inclusive; the last valid index is size() - 1.
  return FrameIDRange{FrameID{0}, FrameID{frame_mapping_.size() - 1}};
}

bool FrameMappedRepresentation::has_frame(FrameID id) const {
  auto idx = resolve_index(id);
  if (!idx) return false;
  if (is_padding(*idx)) return true;
  return source_ && source_->has_frame(frame_mapping_[*idx]);
}

std::optional<FrameDescriptor> FrameMappedRepresentation::get_frame_descriptor(
    FrameID id) const {
  auto idx = resolve_index(id);
  if (!idx) return std::nullopt;

  if (is_padding(*idx)) {
    const auto* pd = find_padding(id);
    if (!pd) {
      // Range-spec PAD frames (from "PAD_N" in the mapping spec) have no
      // PaddingDescriptor because apply_pad_gaps is not called for them.
      // Returning nullopt here would cause the stacker to treat the frame as
      // present-but-unknown and search ±4 frames for a CFI match — the PAL
      // 4-frame cycle guarantees a hit 4 positions back, which is a real frame
      // from a different disc picture. Synthesise a minimal is_padding_frame
      // descriptor so callers correctly exclude this source.
      FrameDescriptor desc;
      desc.frame_id = id;
      desc.is_padding_frame = true;
      if (source_) {
        if (const auto params = source_->get_video_parameters()) {
          desc.system = params->system;
          desc.height = static_cast<size_t>(params->frame_height);
          desc.samples_per_line_nominal =
              static_cast<size_t>(params->frame_width_nominal);
          desc.samples_total = frame_line_sample_offset(
              desc.system, desc.samples_per_line_nominal, desc.height);
        }
      }
      return desc;
    }
    FrameDescriptor desc;
    desc.frame_id = id;
    desc.system = pd->system;
    desc.height = pd->height;
    desc.samples_total = pd->samples_total;
    desc.samples_per_line_nominal = pd->samples_per_line_nominal;
    desc.is_padding_frame = true;
    return desc;
  }

  if (!source_) return std::nullopt;
  auto desc = source_->get_frame_descriptor(frame_mapping_[*idx]);
  if (desc) desc->frame_id = id;
  return desc;
}

const VideoFrameRepresentation::sample_type*
FrameMappedRepresentation::get_frame(FrameID id) const {
  auto idx = resolve_index(id);
  if (!idx) return nullptr;
  if (is_padding(*idx)) {
    ensure_black_frame();
    return black_frame_.empty() ? nullptr : black_frame_.data();
  }
  return source_ ? source_->get_frame(frame_mapping_[*idx]) : nullptr;
}

const VideoFrameRepresentation::sample_type*
FrameMappedRepresentation::get_line(FrameID id, size_t line) const {
  auto idx = resolve_index(id);
  if (!idx) return nullptr;
  if (is_padding(*idx)) {
    ensure_black_frame();
    if (black_frame_.empty() || !source_) return nullptr;
    const auto params = source_->get_video_parameters();
    if (!params || line >= static_cast<size_t>(params->frame_height)) {
      return nullptr;
    }
    return black_frame_.data() +
           frame_line_sample_offset(
               params->system, static_cast<size_t>(params->frame_width_nominal),
               line);
  }
  return source_ ? source_->get_line(frame_mapping_[*idx], line) : nullptr;
}

std::vector<VideoFrameRepresentation::sample_type>
FrameMappedRepresentation::get_frame_copy(FrameID id) const {
  auto idx = resolve_index(id);
  if (!idx) return {};
  if (is_padding(*idx)) {
    ensure_black_frame();
    return black_frame_;
  }
  return source_ ? source_->get_frame_copy(frame_mapping_[*idx])
                 : std::vector<sample_type>{};
}

const VideoFrameRepresentation::sample_type*
FrameMappedRepresentation::get_frame_luma(FrameID id) const {
  auto idx = resolve_index(id);
  if (!idx) return nullptr;
  if (is_padding(*idx)) return nullptr;
  return source_ ? source_->get_frame_luma(frame_mapping_[*idx]) : nullptr;
}

const VideoFrameRepresentation::sample_type*
FrameMappedRepresentation::get_frame_chroma(FrameID id) const {
  auto idx = resolve_index(id);
  if (!idx) return nullptr;
  if (is_padding(*idx)) return nullptr;
  return source_ ? source_->get_frame_chroma(frame_mapping_[*idx]) : nullptr;
}

const VideoFrameRepresentation::sample_type*
FrameMappedRepresentation::get_line_luma(FrameID id, size_t line) const {
  auto idx = resolve_index(id);
  if (!idx) return nullptr;
  if (is_padding(*idx)) return nullptr;
  return source_ ? source_->get_line_luma(frame_mapping_[*idx], line) : nullptr;
}

const VideoFrameRepresentation::sample_type*
FrameMappedRepresentation::get_line_chroma(FrameID id, size_t line) const {
  auto idx = resolve_index(id);
  if (!idx) return nullptr;
  if (is_padding(*idx)) return nullptr;
  return source_ ? source_->get_line_chroma(frame_mapping_[*idx], line)
                 : nullptr;
}

std::vector<DropoutRun> FrameMappedRepresentation::get_dropout_hints(
    FrameID id) const {
  auto idx = resolve_index(id);
  if (!idx) return {};
  if (is_padding(*idx)) return {};
  if (!source_) return {};
  // Rewrite frame IDs so the runs describe this representation's frame, not
  // the source frame they were mapped from (consumers such as dropout_map
  // may rely on the frame_id field).
  auto runs = source_->get_dropout_hints(frame_mapping_[*idx]);
  for (auto& run : runs) {
    run.frame_id = id;
  }
  return runs;
}

std::vector<int32_t> FrameMappedRepresentation::get_audio_samples(
    size_t pair, FrameID id) const {
  auto idx = resolve_index(id);
  if (!idx || !source_) return {};
  if (pair >= source_->audio_channel_pair_count()) return {};

  const auto params = source_->get_video_parameters();
  const VideoSystem system = params ? params->system : VideoSystem::Unknown;
  // Every output frame must serve exactly audio_pairs_in_frame(id) stereo
  // pairs regardless of the mapped source frame's native count.
  const size_t out_pairs = audio_pairs_in_frame(id, system);

  if (is_padding(*idx)) {
    // Padding frames carry cadence-sized silence.
    return std::vector<int32_t>(out_pairs * 2, 0);
  }

  auto samples = source_->get_audio_samples(pair, frame_mapping_[*idx]);
  if (samples.empty() || out_pairs == 0) return samples;
  // SMPTE 272M-1994 §14.3: NTSC/PAL-M frames carry 1602 or 1601 stereo pairs
  // by position in the five-frame audio sequence. A mapping that breaks the
  // sequence phase changes the required count by one pair — truncate one
  // trailing pair or append one trailing silence pair. Phase-preserving
  // mappings and all PAL mappings leave the window untouched.
  samples.resize(out_pairs * 2, 0);
  return samples;
}

uint32_t FrameMappedRepresentation::get_efm_sample_count(FrameID id) const {
  auto idx = resolve_index(id);
  if (!idx || is_padding(*idx)) return 0;
  return source_ ? source_->get_efm_sample_count(frame_mapping_[*idx]) : 0;
}

std::vector<uint8_t> FrameMappedRepresentation::get_efm_samples(
    FrameID id) const {
  auto idx = resolve_index(id);
  if (!idx || is_padding(*idx)) return {};
  return source_ ? source_->get_efm_samples(frame_mapping_[*idx])
                 : std::vector<uint8_t>{};
}

uint32_t FrameMappedRepresentation::get_ac3_symbol_count(FrameID id) const {
  auto idx = resolve_index(id);
  if (!idx || is_padding(*idx)) return 0;
  return source_ ? source_->get_ac3_symbol_count(frame_mapping_[*idx]) : 0;
}

std::vector<uint8_t> FrameMappedRepresentation::get_ac3_symbols(
    FrameID id) const {
  auto idx = resolve_index(id);
  if (!idx || is_padding(*idx)) return {};
  return source_ ? source_->get_ac3_symbols(frame_mapping_[*idx])
                 : std::vector<uint8_t>{};
}

// ============================================================================
// FrameMapStage
// ============================================================================

FrameMapStage::FrameMapStage() {
  set_configuration_status(orc::ConfigurationStatus::Red);
}

std::vector<std::pair<uint64_t, uint64_t>> FrameMapStage::parse_ranges(
    const std::string& spec) {
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  if (spec.empty()) return ranges;

  std::istringstream iss(spec);
  std::string token;
  while (std::getline(iss, token, ',')) {
    // Trim whitespace
    auto start_it = token.find_first_not_of(" \t");
    auto end_it = token.find_last_not_of(" \t");
    if (start_it == std::string::npos) continue;
    token = token.substr(start_it, end_it - start_it + 1);

    // PAD_N directive
    if (token.size() > 4 && token.substr(0, 4) == "PAD_") {
      try {
        uint64_t n = std::stoull(token.substr(4));
        ranges.emplace_back(UINT64_MAX, n);
        continue;
      } catch (...) {
        ORC_LOG_ERROR("FrameMapStage: invalid PAD directive '{}'", token);
        return {};
      }
    }

    size_t dash = token.find('-');
    if (dash == std::string::npos) {
      // Single frame
      try {
        uint64_t id = std::stoull(token);
        ranges.emplace_back(id, id);
      } catch (...) {
        ORC_LOG_ERROR("FrameMapStage: invalid frame id '{}'", token);
        return {};
      }
    } else {
      // Range start-end
      auto lhs = token.substr(0, dash);
      auto rhs = token.substr(dash + 1);
      try {
        uint64_t s = std::stoull(lhs);
        uint64_t e = std::stoull(rhs);
        if (s > e) {
          ORC_LOG_ERROR("FrameMapStage: range start > end in '{}'", token);
          return {};
        }
        ranges.emplace_back(s, e);
      } catch (...) {
        ORC_LOG_ERROR("FrameMapStage: invalid range '{}'", token);
        return {};
      }
    }
  }
  return ranges;
}

std::vector<FrameID> FrameMapStage::build_frame_mapping(
    const std::vector<std::pair<uint64_t, uint64_t>>& ranges,
    const VideoFrameRepresentation& source) {
  std::vector<FrameID> mapping;
  FrameIDRange src_range = source.frame_range();
  uint64_t src_end = src_range.last;

  // Frames past the end of the source are counted and reported once. A spec
  // that overshoots by a million frames is one fact about the spec, not a
  // million facts, and this runs on every execute() of the node.
  uint64_t skipped = 0;
  uint64_t first_skipped = 0;

  for (const auto& [s, e] : ranges) {
    if (s == UINT64_MAX) {
      // PAD_N: insert e padding slots
      for (uint64_t i = 0; i < e; ++i) {
        mapping.push_back(FrameID{UINT64_MAX});
      }
      continue;
    }
    // Nothing past the source's last frame can map, so the walk stops there
    // rather than counting its way to the end of a spec that overshoots.
    const uint64_t last = std::min(e, src_end);
    if (e > src_end) {
      const uint64_t range_first_skipped = std::max(s, src_end + 1);
      if (skipped == 0) {
        first_skipped = range_first_skipped;
      }
      skipped += e - range_first_skipped + 1;
    }
    for (uint64_t id = s; id <= last; ++id) {
      FrameID fid{id};
      if (source.has_frame(fid)) {
        mapping.push_back(fid);
      }
    }
  }

  if (skipped > 0) {
    ORC_LOG_WARN(
        "FrameMapStage: {} frame(s) of the range specification are past the "
        "end of the source (first {}, source ends at {}); skipped",
        skipped, first_skipped, src_end);
  }
  return mapping;
}

size_t FrameMapStage::apply_remove_duplicates(
    std::vector<FrameID>& mapping, const VideoFrameRepresentation& source) {
  size_t removed = 0;
  size_t i = 1;
  while (i < mapping.size()) {
    FrameID prev = mapping[i - 1];
    FrameID cur = mapping[i];

    // Skip padding frames in comparison
    if (prev == FrameID{UINT64_MAX} || cur == FrameID{UINT64_MAX}) {
      ++i;
      continue;
    }

    // Phase is measured from the burst signal (see colour_frame_phase_query.h),
    // not read from source-baked metadata, so duplicate detection works for
    // TBC and CVBS sources alike.
    const int prev_cfi =
        orc::observation::measure_colour_frame_index(source, prev);
    const int cur_cfi =
        orc::observation::measure_colour_frame_index(source, cur);

    if (prev_cfi >= 0 && cur_cfi >= 0 && prev_cfi == cur_cfi) {
      ORC_LOG_DEBUG(
          "FrameMapStage: removing duplicate frame {} (colour_frame_index {})",
          cur, cur_cfi);
      mapping.erase(mapping.begin() + static_cast<std::ptrdiff_t>(i));
      ++removed;
      // Don't advance i — re-compare the new pair at position i
    } else {
      ++i;
    }
  }
  return removed;
}

int FrameMapStage::next_colour_index(int current, VideoSystem sys) {
  if (current < 0) return -1;
  switch (sys) {
    case VideoSystem::PAL:
    case VideoSystem::PAL_M:
      // EBU Tech. 3280-E §1.1.1 / ITU-R BT.1700-1: 4-frame sequence 1-2-3-4
      return (current % 4) + 1;
    case VideoSystem::NTSC:
      // SMPTE 244M-2003 §3.2: 2-frame A/B sequence 0-1
      return 1 - current;
    default:
      return -1;
  }
}

size_t FrameMapStage::apply_pad_gaps(
    std::vector<FrameID>& mapping,
    std::vector<FrameMappedRepresentation::PaddingDescriptor>& pads,
    const VideoFrameRepresentation& source, std::string& gap_positions_out) {
  size_t total_inserted = 0;
  std::ostringstream gap_ss;
  bool first_gap = true;

  // Gather geometry from first real frame
  VideoSystem sys = VideoSystem::Unknown;
  size_t height = 0;
  size_t samples_total = 0;
  size_t samples_per_line_nominal = 0;
  {
    for (const auto& fid : mapping) {
      if (fid == FrameID{UINT64_MAX}) continue;
      auto desc = source.get_frame_descriptor(fid);
      if (desc) {
        sys = desc->system;
        height = desc->height;
        samples_total = desc->samples_total;
        samples_per_line_nominal = desc->samples_per_line_nominal;
        break;
      }
    }
  }

  if (sys == VideoSystem::Unknown) return 0;

  size_t i = 1;
  while (i < mapping.size()) {
    FrameID prev_fid = mapping[i - 1];
    FrameID cur_fid = mapping[i];

    if (prev_fid == FrameID{UINT64_MAX} || cur_fid == FrameID{UINT64_MAX}) {
      ++i;
      continue;
    }

    // Phase measured from the burst signal, not source-baked metadata.
    int prev_idx =
        orc::observation::measure_colour_frame_index(source, prev_fid);
    int cur_idx = orc::observation::measure_colour_frame_index(source, cur_fid);

    if (prev_idx < 0 || cur_idx < 0) {
      ++i;
      continue;
    }

    int expected = next_colour_index(prev_idx, sys);
    if (expected == cur_idx) {
      ++i;
      continue;
    }

    // Gap detected — how many frames are missing?
    size_t insert_pos = i;
    size_t gap_count = 0;
    int fill_idx = expected;
    while (fill_idx != cur_idx && gap_count < 8) {
      FrameMappedRepresentation::PaddingDescriptor pd;
      // The output FrameID will be determined after insertion; we use a
      // placeholder here and fix it up after the insert loop
      pd.output_id = FrameID{UINT64_MAX};  // filled below
      pd.system = sys;
      pd.height = height;
      pd.samples_total = samples_total;
      pd.samples_per_line_nominal = samples_per_line_nominal;

      mapping.insert(mapping.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                     FrameID{UINT64_MAX});
      pads.push_back(pd);

      ++insert_pos;
      ++gap_count;
      ++total_inserted;

      fill_idx = next_colour_index(fill_idx, sys);
      if (fill_idx == cur_idx) break;
    }

    if (gap_count > 0) {
      if (!first_gap) gap_ss << ",";
      gap_ss << i;
      first_gap = false;
      ORC_LOG_DEBUG(
          "FrameMapStage: inserted {} padding frame(s) at position {} "
          "(colour index gap {} → {})",
          gap_count, i, prev_idx, cur_idx);
    }

    i += gap_count + 1;
  }

  // Fix up output_id fields for all inserted padding descriptors
  // Re-walk mapping to assign correct output FrameIDs to padding descriptors
  size_t pad_assign_idx = 0;
  for (size_t pos = 0; pos < mapping.size(); ++pos) {
    if (mapping[pos] == FrameID{UINT64_MAX} && pad_assign_idx < pads.size()) {
      // Only assign if this pad doesn't already have a valid output_id
      if (pads[pad_assign_idx].output_id == FrameID{UINT64_MAX}) {
        pads[pad_assign_idx].output_id = FrameID{pos};
        ++pad_assign_idx;
      }
    }
  }

  gap_positions_out = gap_ss.str();
  return total_inserted;
}

std::optional<FrameMapStage::RunConfig> FrameMapStage::config_for(
    const std::map<std::string, ParameterValue>& parameters) const {
  RunConfig config;
  config.range_spec = range_spec_;
  config.ranges = cached_ranges_;
  config.remove_duplicates = remove_duplicates_;
  config.pad_gaps = pad_gaps_;

  const auto ranges_it = parameters.find("ranges");
  if (ranges_it != parameters.end()) {
    if (const auto* v = std::get_if<std::string>(&ranges_it->second)) {
      config.range_spec = *v;
      config.ranges = config.range_spec.empty()
                          ? std::vector<std::pair<uint64_t, uint64_t>>{}
                          : parse_ranges(config.range_spec);
      if (!config.range_spec.empty() && config.ranges.empty()) {
        ORC_LOG_ERROR("FrameMapStage: invalid range spec '{}'",
                      config.range_spec);
        return std::nullopt;
      }
    }
  }
  const auto dedup_it = parameters.find("remove_duplicates");
  if (dedup_it != parameters.end()) {
    if (const auto* v = std::get_if<bool>(&dedup_it->second)) {
      config.remove_duplicates = *v;
    }
  }
  const auto pad_it = parameters.find("pad_gaps");
  if (pad_it != parameters.end()) {
    if (const auto* v = std::get_if<bool>(&pad_it->second)) {
      config.pad_gaps = *v;
    }
  }
  // pad_strategy has one accepted value, so it cannot change the run.
  return config;
}

std::vector<ArtifactPtr> FrameMapStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  if (inputs.empty()) {
    throw DAGExecutionError("FrameMapStage requires one input");
  }

  auto source =
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  if (!source) {
    throw DAGExecutionError(
        "FrameMapStage: input must be a VideoFrameRepresentation");
  }

  // Resolve the run's parameters onto the stack. The stage's own members are
  // left alone: execute() is not the configuration path, and writing them here
  // is what made two workers sharing this instance corrupt each other.
  const auto config_opt = config_for(parameters);
  if (!config_opt) {
    throw DAGExecutionError("FrameMapStage: invalid range specification");
  }
  const RunConfig& config = *config_opt;

  // Pass-through when no ranges and no processing requested
  if (config.range_spec.empty() && !config.remove_duplicates &&
      !config.pad_gaps) {
    cached_output_ = source;
    return {inputs[0]};
  }

  // Build initial frame mapping from range specification
  std::vector<FrameID> mapping;
  if (!config.range_spec.empty() && !config.ranges.empty()) {
    mapping = build_frame_mapping(config.ranges, *source);
    if (mapping.empty()) {
      ORC_LOG_WARN("FrameMapStage: range spec produced empty mapping");
      cached_output_ = source;
      return {inputs[0]};
    }
  } else {
    // No range spec — use identity mapping over source range
    FrameIDRange rng = source->frame_range();
    for (FrameID fid = rng.first; fid <= rng.last; ++fid) {
      if (source->has_frame(fid)) {
        mapping.push_back(fid);
      }
    }
  }

  // Duplicate frame removal
  size_t removed_count = 0;
  if (config.remove_duplicates) {
    removed_count = apply_remove_duplicates(mapping, *source);
    ORC_LOG_DEBUG("FrameMapStage: removed {} duplicate frame(s)",
                  removed_count);
  }

  // Gap padding
  std::vector<FrameMappedRepresentation::PaddingDescriptor> pads;
  size_t padded_count = 0;
  std::string gap_positions;
  if (config.pad_gaps) {
    padded_count = apply_pad_gaps(mapping, pads, *source, gap_positions);
    ORC_LOG_DEBUG("FrameMapStage: inserted {} padding frame(s) into {} gap(s)",
                  padded_count, gap_positions.empty() ? 0 : 1);
  }

  // Emit aggregate observations on field 0 as a convention
  // (ObservationContext is still FieldID-keyed; use FieldID(0) for aggregates)
  if (removed_count > 0) {
    observation_context.set(FieldID(0), "frame_map", "frames_removed",
                            static_cast<int64_t>(removed_count));
  }
  if (padded_count > 0) {
    observation_context.set(FieldID(0), "frame_map", "frames_padded",
                            static_cast<int64_t>(padded_count));
    if (!gap_positions.empty()) {
      observation_context.set(FieldID(0), "frame_map", "gap_positions",
                              gap_positions);
    }
  }

  std::string tag = config.range_spec + "_rd" +
                    std::to_string(config.remove_duplicates) + "_pg" +
                    std::to_string(config.pad_gaps);

  auto result = std::make_shared<FrameMappedRepresentation>(
      source, std::move(mapping), std::move(pads), tag);

  cached_output_ = result;
  return {result};
}

std::vector<ParameterDescriptor> FrameMapStage::get_parameter_descriptors(
    VideoSystem /*project_format*/, SourceType /*source_type*/) const {
  return {
      ParameterDescriptor{
          "ranges", "Frame Ranges",
          "Comma-separated frame ranges, 1-based as shown in the preview "
          "(e.g., '1-11,21-31,12-20'). Stored 0-based in the project file. "
          "Empty = pass-through.",
          ParameterType::STRING,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue{std::string("")},
                               {},
                               false,
                               std::nullopt}},
      ParameterDescriptor{
          "remove_duplicates", "Remove Duplicates",
          "Remove consecutive frames with identical colour_frame_index",
          ParameterType::BOOL,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue{false},
                               {},
                               false,
                               std::nullopt}},
      ParameterDescriptor{
          "pad_gaps", "Pad Gaps",
          "Fill colour-frame-index sequence gaps with synthetic padding frames",
          ParameterType::BOOL,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue{false},
                               {},
                               false,
                               std::nullopt}},
      ParameterDescriptor{
          "pad_strategy", "Padding Strategy",
          "How to fill gaps: 'black' (blank padding frames)",
          ParameterType::STRING,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue{std::string("black")},
                               {"black"},
                               false,
                               std::nullopt}},
  };
}

std::map<std::string, ParameterValue> FrameMapStage::get_parameters() const {
  return {{"ranges", ParameterValue{range_spec_}},
          {"remove_duplicates", ParameterValue{remove_duplicates_}},
          {"pad_gaps", ParameterValue{pad_gaps_}},
          {"pad_strategy", ParameterValue{pad_strategy_}}};
}

bool FrameMapStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key == "ranges") {
      if (auto* v = std::get_if<std::string>(&value)) {
        range_spec_ = *v;
        if (!range_spec_.empty()) {
          cached_ranges_ = parse_ranges(range_spec_);
          if (cached_ranges_.empty()) {
            ORC_LOG_ERROR("FrameMapStage: invalid range spec '{}'",
                          range_spec_);
            return false;
          }
        } else {
          cached_ranges_.clear();
        }
      } else {
        return false;
      }
    } else if (key == "remove_duplicates") {
      if (auto* v = std::get_if<bool>(&value)) {
        remove_duplicates_ = *v;
      } else {
        return false;
      }
    } else if (key == "pad_gaps") {
      if (auto* v = std::get_if<bool>(&value)) {
        pad_gaps_ = *v;
      } else {
        return false;
      }
    } else if (key == "pad_strategy") {
      if (auto* v = std::get_if<std::string>(&value)) {
        // "nearest" was a no-op that always rendered black; it is retained as
        // a deprecated alias so existing project files still load.
        if (*v == "nearest") {
          ORC_LOG_WARN(
              "FrameMapStage: pad_strategy 'nearest' is deprecated and treated "
              "as 'black'");
          pad_strategy_ = "black";
        } else if (*v == "black") {
          pad_strategy_ = "black";
        } else {
          ORC_LOG_ERROR("FrameMapStage: unknown pad_strategy '{}'", *v);
          return false;
        }
      } else {
        return false;
      }
    } else {
      ORC_LOG_WARN("FrameMapStage: unknown parameter '{}'", key);
      return false;
    }
  }
  set_configuration_status(range_spec_.empty()
                               ? orc::ConfigurationStatus::Red
                               : orc::ConfigurationStatus::Green);
  return true;
}

StagePreviewCapability FrameMapStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

}  // namespace orc
