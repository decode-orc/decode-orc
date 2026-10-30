/*
 * File:        dropout_correct_stage.cpp
 * Module:      orc-core
 * Purpose:     Dropout correction stage implementation (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dropout_correct_stage.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/error_types.h>
#include <orc/support/composite_split.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <cmath>

namespace orc {

// ============================================================================
// Helper: determine first-field line count for a VideoSystem
// ============================================================================

namespace {

size_t field1_lines_for_system(VideoSystem sys) {
  switch (sys) {
    case VideoSystem::PAL:
      return static_cast<size_t>(kPalField1Lines);
    case VideoSystem::NTSC:
      return static_cast<size_t>(kNtscField1Lines);
    case VideoSystem::PAL_M:
      return static_cast<size_t>(kPalMField1Lines);
    default:
      return static_cast<size_t>(kNtscField1Lines);
  }
}

bool is_pal(VideoSystem sys) { return sys == VideoSystem::PAL; }

// PAL_M has 525-line geometry but PAL's colour: fsc is 227.25 fH, so the
// subcarrier steps 90 degrees a line and V switches every line, and only
// lines 4 apart are in phase, as on PAL.
bool has_pal_line_phase(VideoSystem sys) {
  return sys == VideoSystem::PAL || sys == VideoSystem::PAL_M;
}

// Lines inside the nominal active range that carry no picture where a full
// picture line would, so borrowing from them pastes black (or data) into the
// picture:
//   PAL lines 23 and 623 (1-based) are half-lines; LaserDisc mastering often
//   blanks line 23 entirely.  Field 2 starts at line 314.
//   525-line lines 21 and 284 carry the line-21 data service (closed captions;
//   blank after the run-in when idle), and line 263 is a half-line.  Field 2
//   starts at line 264.
bool is_non_picture_line(VideoSystem sys, size_t field1_lines,
                         uint32_t flat_line) {
  if (is_pal(sys)) {
    return flat_line == 22 || flat_line == field1_lines + (623 - 314);
  }
  return flat_line == 20 || flat_line == 262 ||
         flat_line == field1_lines + (284 - 264);
}

// The interpolate method reaches at most this many field lines either way
// for a clean line, as far as line copy's phase-matched PAL candidate.
constexpr int32_t kInterpolateReach = 4;

// A burst below this many IRE is taken as none: a monochrome line.
constexpr double kMinBurstIre = 5.0;

// Luma difference below the chroma band, in IRE, at which a neighbour's
// comb with its partner gives way to its own split (split_combed()). Chosen
// on the videosynth concealment cases: 3 to 20 IRE give the same errors
// within a few per cent; with no limit, chroma edges blur across the comb.
constexpr double kCombMaxLumaDifferenceIre = 8.0;

// Whether |line| has a dropout overlapping samples [first, last].
bool line_has_dropout(const std::vector<LineDropout>& frame_dropouts,
                      uint32_t line, int64_t first, int64_t last) {
  return std::any_of(frame_dropouts.begin(), frame_dropouts.end(),
                     [&](const LineDropout& d) {
                       return d.line == line && d.start_sample <= last &&
                              first <= d.end_sample;
                     });
}

}  // namespace

std::optional<DropoutCorrectionMethod> parse_dropout_correction_method(
    const std::string& value) {
  if (value == kMethodLineCopy) return DropoutCorrectionMethod::LineCopy;
  if (value == kMethodInterpolate) return DropoutCorrectionMethod::Interpolate;
  return std::nullopt;
}

// ============================================================================
// DropoutCorrectStage::runs_to_line_dropouts
// ============================================================================

std::vector<LineDropout> DropoutCorrectStage::runs_to_line_dropouts(
    const std::vector<DropoutRun>& runs, VideoSystem system,
    size_t nominal_spl) {
  if (nominal_spl == 0) {
    return {};
  }
  std::vector<LineDropout> result;
  result.reserve(runs.size());
  for (const auto& run : runs) {
    uint64_t remaining = run.sample_count;
    uint64_t offset = run.sample_start;
    while (remaining > 0) {
      auto [flat_line, start_in_line] =
          frame_flat_offset_to_line_sample(system, nominal_spl, offset);
      const uint32_t line = static_cast<uint32_t>(flat_line);
      const uint32_t start = static_cast<uint32_t>(start_in_line);
      const size_t line_len =
          frame_line_sample_count(system, nominal_spl, flat_line);
      const uint32_t avail = static_cast<uint32_t>(line_len) - start;
      const uint32_t count = static_cast<uint32_t>(
          std::min(remaining, static_cast<uint64_t>(avail)));
      result.push_back({line, start, static_cast<uint32_t>(start + count - 1)});
      offset += count;
      remaining -= count;
    }
  }
  return result;
}

// ============================================================================
// execute()
// ============================================================================

std::vector<ArtifactPtr> DropoutCorrectStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  (void)observation_context;
  if (inputs.empty()) {
    throw DAGExecutionError("DropoutCorrectStage requires one input");
  }

  auto source =
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  if (!source) {
    throw DAGExecutionError(
        "DropoutCorrectStage input must be VideoFrameRepresentation");
  }

  if (!parameters.empty()) {
    set_parameters(parameters);
  }

  auto corrected = std::make_shared<CorrectedVideoFrameRepresentation>(
      source, this, config_.highlight_corrections);

  cached_output_ = corrected;

  std::vector<ArtifactPtr> outputs;
  outputs.push_back(corrected);
  return outputs;
}

// ============================================================================
// CorrectedVideoFrameRepresentation
// ============================================================================

CorrectedVideoFrameRepresentation::CorrectedVideoFrameRepresentation(
    std::shared_ptr<const VideoFrameRepresentation> source,
    DropoutCorrectStage* stage, bool highlight_corrections)
    : VideoFrameRepresentationWrapper(std::move(source)),
      Artifact(ArtifactID("corrected_frame"), Provenance{}),
      stage_(stage),
      highlight_corrections_(highlight_corrections),
      corrected_frames_(MAX_CACHED_FRAMES),
      corrected_luma_frames_(MAX_CACHED_FRAMES),
      corrected_chroma_frames_(MAX_CACHED_FRAMES) {}

void CorrectedVideoFrameRepresentation::ensure_frame_corrected(
    FrameID frame_id) const {
  if (source_->has_separate_channels()) {
    if (corrected_luma_frames_.contains(frame_id) &&
        corrected_chroma_frames_.contains(frame_id)) {
      return;
    }
  } else {
    if (corrected_frames_.contains(frame_id)) {
      return;
    }
  }
  stage_->correct_single_frame(
      const_cast<CorrectedVideoFrameRepresentation*>(this), source_, frame_id);
}

const int16_t* CorrectedVideoFrameRepresentation::get_frame(FrameID id) const {
  ensure_frame_corrected(id);
  const auto* cached = corrected_frames_.get_ptr(id);
  if (cached && !cached->empty()) {
    return cached->data();
  }
  return source_->get_frame(id);
}

const int16_t* CorrectedVideoFrameRepresentation::get_line(FrameID id,
                                                           size_t line) const {
  ensure_frame_corrected(id);
  const auto* cached = corrected_frames_.get_ptr(id);
  if (cached && !cached->empty()) {
    auto desc = source_->get_frame_descriptor(id);
    if (desc && line < desc->height) {
      return &(*cached)[frame_line_sample_offset(
          desc->system, desc->samples_per_line_nominal, line)];
    }
  }
  return source_->get_line(id, line);
}

std::vector<int16_t> CorrectedVideoFrameRepresentation::get_frame_copy(
    FrameID id) const {
  ensure_frame_corrected(id);
  const auto* cached = corrected_frames_.get_ptr(id);
  if (cached && !cached->empty()) {
    return *cached;
  }
  return source_->get_frame_copy(id);
}

const int16_t* CorrectedVideoFrameRepresentation::get_frame_luma(
    FrameID id) const {
  if (!source_ || !source_->has_separate_channels()) {
    return VideoFrameRepresentationWrapper::get_frame_luma(id);
  }
  ensure_frame_corrected(id);
  const auto* cached = corrected_luma_frames_.get_ptr(id);
  if (cached && !cached->empty()) {
    return cached->data();
  }
  return source_->get_frame_luma(id);
}

const int16_t* CorrectedVideoFrameRepresentation::get_line_luma(
    FrameID id, size_t line) const {
  if (!source_ || !source_->has_separate_channels()) {
    return VideoFrameRepresentationWrapper::get_line_luma(id, line);
  }
  ensure_frame_corrected(id);
  const auto* cached = corrected_luma_frames_.get_ptr(id);
  if (cached && !cached->empty()) {
    auto desc = source_->get_frame_descriptor(id);
    if (desc && line < desc->height) {
      return &(*cached)[frame_line_sample_offset(
          desc->system, desc->samples_per_line_nominal, line)];
    }
  }
  return source_->get_line_luma(id, line);
}

const int16_t* CorrectedVideoFrameRepresentation::get_frame_chroma(
    FrameID id) const {
  if (!source_ || !source_->has_separate_channels()) {
    return VideoFrameRepresentationWrapper::get_frame_chroma(id);
  }
  ensure_frame_corrected(id);
  const auto* cached = corrected_chroma_frames_.get_ptr(id);
  if (cached && !cached->empty()) {
    return cached->data();
  }
  return source_->get_frame_chroma(id);
}

const int16_t* CorrectedVideoFrameRepresentation::get_line_chroma(
    FrameID id, size_t line) const {
  if (!source_ || !source_->has_separate_channels()) {
    return VideoFrameRepresentationWrapper::get_line_chroma(id, line);
  }
  ensure_frame_corrected(id);
  const auto* cached = corrected_chroma_frames_.get_ptr(id);
  if (cached && !cached->empty()) {
    auto desc = source_->get_frame_descriptor(id);
    if (desc && line < desc->height) {
      return &(*cached)[frame_line_sample_offset(
          desc->system, desc->samples_per_line_nominal, line)];
    }
  }
  return source_->get_line_chroma(id, line);
}

// ============================================================================
// Dropout classification helpers
// ============================================================================

DropoutCorrectStage::DropoutLocation DropoutCorrectStage::classify_dropout(
    const LineDropout& dropout, const FrameDescriptor& /*desc*/,
    const std::optional<SourceParameters>& params) const {
  // Colour burst end derived from video system constant; active_video_end from
  // the canonical SourceParameters field.
  uint32_t cb_end = static_cast<uint32_t>(kNtscColourBurstEnd);  // safe default
  uint32_t av_end = 800;

  if (params) {
    cb_end = static_cast<uint32_t>(colour_burst_range(params->system).second);
    if (params->active_video_end >= 0) {
      av_end = static_cast<uint32_t>(params->active_video_end);
    }
  }

  if (dropout.start_sample <= cb_end) {
    return DropoutLocation::COLOUR_BURST;
  }
  if (dropout.start_sample > cb_end && dropout.start_sample <= av_end) {
    return DropoutLocation::VISIBLE_LINE;
  }
  return DropoutLocation::UNKNOWN;
}

std::vector<LineDropout> DropoutCorrectStage::split_dropout_regions(
    const std::vector<LineDropout>& dropouts, const FrameDescriptor& desc,
    const std::optional<SourceParameters>& params) const {
  std::vector<LineDropout> result;

  // Colour burst end derived from video system constant; active_video_end from
  // the canonical SourceParameters field.
  uint32_t cb_end = static_cast<uint32_t>(kNtscColourBurstEnd);  // safe default
  uint32_t av_end = 800;

  if (params) {
    cb_end = static_cast<uint32_t>(colour_burst_range(params->system).second);
    if (params->active_video_end >= 0) {
      av_end = static_cast<uint32_t>(params->active_video_end);
    }
  }

  for (const auto& d : dropouts) {
    if (d.line >= desc.height) {
      ORC_LOG_DEBUG(
          "split_dropout_regions: skipping dropout on line {} "
          "(frame height {})",
          d.line, desc.height);
      continue;
    }
    const auto loc = classify_dropout(d, desc, params);
    if (loc == DropoutLocation::COLOUR_BURST && d.end_sample > cb_end) {
      result.push_back({d.line, d.start_sample, cb_end});
      result.push_back({d.line, cb_end + 1, d.end_sample});
    } else if (loc == DropoutLocation::VISIBLE_LINE && d.end_sample > av_end) {
      LineDropout truncated = d;
      truncated.end_sample = av_end;
      result.push_back(truncated);
    } else {
      result.push_back(d);
    }
  }
  return result;
}

// ============================================================================
// find_replacement_line
// ============================================================================

DropoutCorrectStage::ReplacementLine DropoutCorrectStage::find_replacement_line(
    const VideoFrameRepresentation& source, FrameID frame_id, uint32_t line,
    const LineDropout& dropout, bool intrafield,
    bool match_chroma_phase_override, size_t field1_lines,
    const std::vector<LineDropout>& frame_dropouts, Channel channel) const {
  ReplacementLine best;

  const auto desc_opt = source.get_frame_descriptor(frame_id);
  if (!desc_opt) {
    return best;
  }
  const auto& desc = *desc_opt;

  const int32_t height = static_cast<int32_t>(desc.height);
  const size_t spl = desc.samples_per_line_nominal;

  const bool is_field1 = (line < field1_lines);

  uint32_t step = 1;
  int32_t other_field_off = 0;

  if (match_chroma_phase_override) {
    auto vp = source.get_video_parameters();
    if (vp) {
      // Step values are the field-sequential line offsets that keep the colour
      // subcarrier (and PAL V-switch) in phase.  Verified empirically by burst
      // correlation against neighbouring lines: for NTSC only even offsets stay
      // in phase (odd offsets invert), and for PAL and PAL_M only offsets that
      // are a multiple of 4 stay in phase for every line.  Smaller offsets
      // invert the burst and decode to the wrong hue.  PAL_M's 263-line first
      // field leaves the other-field offsets the same as PAL's.
      if (has_pal_line_phase(vp->system)) {
        step = 4;
        other_field_off = is_field1 ? -3 : -1;
      } else {
        step = 2;
        other_field_off = -1;
      }
    }
  }

  // Active picture line range as a FIELD-line range.  first/last_active_frame_
  // line are interlaced frame numbers (2x the field line); the replacement
  // search works in field-sequential flat lines, so the bounds must be applied
  // per field.  Restricting with the raw frame numbers would let the search
  // descend into a field's VBI/blanking lines, which carry no subcarrier and
  // decode to a black line with no chroma.
  // last_active_frame_line is exclusive (PAL 44 + 576 = 620).  Field 1 holds
  // the even frame lines and field 2 the odd ones, so each field's exclusive
  // end is the count of its frame lines below that bound; treating the bound as
  // inclusive reaches one blanked line past the picture in each field.
  int32_t active_field_first = 0;
  int32_t active_field1_end = height;  // permissive fallback (whole field)
  int32_t active_field2_end = height;
  const auto vp = source.get_video_parameters();
  if (vp && vp->first_active_frame_line >= 0 &&
      vp->last_active_frame_line >= 0) {
    active_field_first = vp->first_active_frame_line / 2;
    active_field1_end = (vp->last_active_frame_line + 1) / 2;
    active_field2_end = vp->last_active_frame_line / 2;
  }

  auto get_line_fn = [&source, frame_id, channel](size_t ln) -> const int16_t* {
    switch (channel) {
      case Channel::LUMA:
        return source.get_line_luma(frame_id, ln);
      case Channel::CHROMA:
        return source.get_line_chroma(frame_id, ln);
      default:
        return source.get_line(frame_id, ln);
    }
  };

  // Reject a candidate replacement line whose own dropouts overlap the sample
  // range being corrected — copying corrupted samples would not repair the
  // dropout — or that carries no picture there (see is_non_picture_line).
  // frame_dropouts holds every line-dropout in this frame
  // (linear scan; dropout counts per frame are small in practice).
  auto has_overlap = [&frame_dropouts, &dropout, &desc,
                      field1_lines](uint32_t chk_line) -> bool {
    if (is_non_picture_line(desc.system, field1_lines, chk_line)) {
      return true;
    }
    for (const auto& d : frame_dropouts) {
      if (d.line != chk_line) {
        continue;
      }
      if (d.start_sample <= dropout.end_sample &&
          dropout.start_sample <= d.end_sample) {
        return true;
      }
    }
    return false;
  };

  std::vector<ReplacementLine> candidates;

  if (intrafield) {
    const int32_t field_start =
        is_field1 ? 0 : static_cast<int32_t>(field1_lines);
    const int32_t field_end =
        is_field1 ? static_cast<int32_t>(field1_lines) : height;
    // Active picture bounds for the target field, in flat coordinates.
    const int32_t active_lo = field_start + active_field_first;
    const int32_t active_hi =
        field_start +
        std::min(is_field1 ? active_field1_end : active_field2_end,
                 field_end - field_start);

    // Search upward
    int32_t sl = static_cast<int32_t>(line) - static_cast<int32_t>(step);
    while (sl >= active_lo) {
      const uint32_t cl = static_cast<uint32_t>(sl);
      if (!has_overlap(cl)) {
        const int16_t* data = get_line_fn(cl);
        if (data) {
          ReplacementLine c;
          c.found = true;
          c.source_frame = frame_id;
          c.source_line = cl;
          c.quality = calculate_line_quality(data, spl, dropout);
          c.distance =
              static_cast<uint32_t>(std::abs(static_cast<int32_t>(line) - sl));
          c.cached_data = data;
          candidates.push_back(c);
          break;
        }
      }
      sl -= static_cast<int32_t>(step);
    }

    // Search downward
    sl = static_cast<int32_t>(line) + static_cast<int32_t>(step);
    while (sl < active_hi) {
      const uint32_t cl = static_cast<uint32_t>(sl);
      if (!has_overlap(cl)) {
        const int16_t* data = get_line_fn(cl);
        if (data) {
          ReplacementLine c;
          c.found = true;
          c.source_frame = frame_id;
          c.source_line = cl;
          c.quality = calculate_line_quality(data, spl, dropout);
          c.distance =
              static_cast<uint32_t>(std::abs(static_cast<int32_t>(line) - sl));
          c.cached_data = data;
          candidates.push_back(c);
          break;
        }
      }
      sl += static_cast<int32_t>(step);
    }
  } else {
    const int32_t field_offset = is_field1
                                     ? static_cast<int32_t>(field1_lines)
                                     : -static_cast<int32_t>(field1_lines);
    const int32_t start_line =
        static_cast<int32_t>(line) + field_offset + other_field_off;

    const int32_t other_field_start =
        is_field1 ? static_cast<int32_t>(field1_lines) : 0;
    const int32_t other_field_end =
        is_field1 ? height : static_cast<int32_t>(field1_lines);
    // Active picture bounds for the other field, in flat coordinates.
    const int32_t active_lo = other_field_start + active_field_first;
    const int32_t active_hi =
        other_field_start +
        std::min(is_field1 ? active_field2_end : active_field1_end,
                 other_field_end - other_field_start);

    if (start_line < other_field_start || start_line >= other_field_end) {
      return best;
    }

    // Search up in other field
    int32_t sl = start_line;
    while (sl >= active_lo) {
      const uint32_t cl = static_cast<uint32_t>(sl);
      if (!has_overlap(cl)) {
        const int16_t* data = get_line_fn(cl);
        if (data) {
          ReplacementLine c;
          c.found = true;
          c.source_frame = frame_id;
          c.source_line = cl;
          c.quality = calculate_line_quality(data, spl, dropout);
          c.distance =
              static_cast<uint32_t>(std::abs(static_cast<int32_t>(line) - sl));
          c.cached_data = data;
          candidates.push_back(c);
          break;
        }
      }
      sl -= static_cast<int32_t>(step);
    }

    // Search down in other field
    sl = start_line + static_cast<int32_t>(step);
    while (sl < active_hi) {
      const uint32_t cl = static_cast<uint32_t>(sl);
      if (!has_overlap(cl)) {
        const int16_t* data = get_line_fn(cl);
        if (data) {
          ReplacementLine c;
          c.found = true;
          c.source_frame = frame_id;
          c.source_line = cl;
          c.quality = calculate_line_quality(data, spl, dropout);
          c.distance =
              static_cast<uint32_t>(std::abs(static_cast<int32_t>(line) - sl));
          c.cached_data = data;
          candidates.push_back(c);
          break;
        }
      }
      sl += static_cast<int32_t>(step);
    }
  }

  for (const auto& c : candidates) {
    if (!best.found || c.distance < best.distance ||
        (c.distance == best.distance && c.quality > best.quality)) {
      best = c;
    }
  }
  return best;
}

// ============================================================================
// calculate_line_quality
// ============================================================================

double DropoutCorrectStage::calculate_line_quality(
    const int16_t* line_data, size_t width, const LineDropout& dropout) const {
  if (dropout.start_sample >= dropout.end_sample ||
      dropout.end_sample >= width) {
    return 0.0;
  }
  double sum = 0.0;
  const uint32_t count = dropout.end_sample - dropout.start_sample;
  for (uint32_t i = dropout.start_sample; i < dropout.end_sample; ++i) {
    sum += line_data[i];
  }
  const double mean = sum / count;
  double mad = 0.0;
  for (uint32_t i = dropout.start_sample; i < dropout.end_sample; ++i) {
    mad += std::abs(static_cast<double>(line_data[i]) - mean);
  }
  return 1.0 / (mad / count + 1.0);
}

// ============================================================================
// interpolate_dropout
// ============================================================================

bool DropoutCorrectStage::interpolate_dropout(
    const VideoFrameRepresentation& source, FrameID frame_id,
    const FrameDescriptor& desc, const LineDropout& dropout,
    const std::vector<LineDropout>& frame_dropouts, size_t field1_lines,
    Channel channel, std::optional<int16_t> highlight,
    int16_t* out_line) const {
  namespace split = composite_split;
  const auto vp = source.get_video_parameters();
  const auto system = desc.system;
  const size_t spl = desc.samples_per_line_nominal;
  const auto line = static_cast<int32_t>(dropout.line);
  const auto line_length =
      static_cast<int32_t>(frame_line_sample_count(system, spl, dropout.line));
  const auto first = static_cast<int32_t>(dropout.start_sample);
  const int32_t last =
      std::min(static_cast<int32_t>(dropout.end_sample), line_length - 1);
  if (first > last) return false;

  auto read_line = [&source, frame_id, channel](uint32_t ln) {
    switch (channel) {
      case Channel::LUMA:
        return source.get_line_luma(frame_id, ln);
      case Channel::CHROMA:
        return source.get_line_chroma(frame_id, ln);
      default:
        return source.get_line(frame_id, ln);
    }
  };

  // The field's active lines, in flat lines (see find_replacement_line).
  const bool is_field1 = static_cast<size_t>(line) < field1_lines;
  const int32_t field_start =
      is_field1 ? 0 : static_cast<int32_t>(field1_lines);
  const int32_t field_end = is_field1 ? static_cast<int32_t>(field1_lines)
                                      : static_cast<int32_t>(desc.height);
  int32_t active_lo = field_start;
  int32_t active_hi = field_end;
  if (vp && vp->first_active_frame_line >= 0 &&
      vp->last_active_frame_line >= 0) {
    active_lo = field_start + (vp->first_active_frame_line / 2);
    active_hi =
        std::min(field_end,
                 field_start + (is_field1 ? (vp->last_active_frame_line + 1) / 2
                                          : vp->last_active_frame_line / 2));
  }

  // Luma carries no subcarrier and is interpolated as it is. Composite and
  // chroma are split, and their chroma moved to the damaged line's phase.
  const bool modulated = channel != Channel::LUMA;
  std::optional<split::ChromaSplitter> splitter;
  int32_t burst_start = 0;
  int32_t burst_end = 0;
  const double ire_codes = vp && vp->white_level > vp->blanking_level
                               ? (vp->white_level - vp->blanking_level) / 100.0
                               : 5.88;
  const double min_burst = kMinBurstIre * ire_codes;
  if (modulated) {
    splitter.emplace(system);
    std::tie(burst_start, burst_end) = colour_burst_range(system);
  }
  const int32_t context = modulated ? split::kChromaFilterRadius : 0;

  // The burst of a flat line whose burst is clean, or nullopt.
  auto burst_of = [&](int32_t ln) -> std::optional<std::complex<double>> {
    if (ln < field_start || ln >= field_end) return std::nullopt;
    const auto uln = static_cast<uint32_t>(ln);
    if (line_has_dropout(frame_dropouts, uln, burst_start, burst_end - 1)) {
      return std::nullopt;
    }
    const int16_t* data = read_line(uln);
    if (!data) return std::nullopt;
    return split::burst_reference(data, burst_start, burst_end, min_burst);
  };

  // The damaged line's own burst, or, where a dropout covers it, that of the
  // nearest line whose subcarrier is in phase with it (as line copy's
  // candidates are). None anywhere: a monochrome line, whose samples are
  // interpolated as they are.
  std::optional<std::complex<double>> target_burst;
  if (modulated) {
    const bool burst_damaged = line_has_dropout(frame_dropouts, dropout.line,
                                                burst_start, burst_end - 1);
    if (!burst_damaged) {
      target_burst = burst_of(line);
    } else {
      const int32_t step = (system == VideoSystem::NTSC) ? 2 : 4;
      target_burst = burst_of(line - step);
      if (!target_burst) target_burst = burst_of(line + step);
      if (!target_burst) return false;
    }
  }
  const bool colour = modulated && target_burst.has_value();

  struct Neighbour {
    int32_t distance = 0;
    const int16_t* data = nullptr;
    std::optional<std::complex<double>> burst;
    // The composite comb partner: the line beyond, away from the dropout,
    // whose chroma is this line's inverted (split_combed()); null where it
    // is damaged or outside the picture.
    const int16_t* partner = nullptr;
  };
  // Whether flat line |ln| is clean over the span and its filter's reach.
  auto usable = [&](int32_t ln) {
    if (ln < active_lo || ln >= active_hi) return false;
    const auto uln = static_cast<uint32_t>(ln);
    return !is_non_picture_line(system, field1_lines, uln) &&
           !line_has_dropout(frame_dropouts, uln, first - context,
                             last + context);
  };
  auto find_neighbour = [&](int32_t direction) -> std::optional<Neighbour> {
    for (int32_t k = 1; k <= kInterpolateReach; ++k) {
      const int32_t ln = line + (direction * k);
      if (ln < active_lo || ln >= active_hi) return std::nullopt;
      if (!usable(ln)) continue;
      Neighbour n{k, read_line(static_cast<uint32_t>(ln)), std::nullopt};
      if (!n.data) continue;
      if (colour) {
        n.burst = burst_of(ln);
        if (!n.burst) continue;
        const int32_t partner = ln + (direction * split::comb_distance(system));
        if (channel == Channel::COMPOSITE && usable(partner)) {
          n.partner = read_line(static_cast<uint32_t>(partner));
        }
      }
      return n;
    }
    return std::nullopt;
  };
  const auto above = find_neighbour(-1);
  const auto below = find_neighbour(1);
  if (!above && !below) return false;

  // Each side weighted by the other's distance: a straight line between them.
  const double total =
      (above ? above->distance : 0) + (below ? below->distance : 0);
  const double w_above = above ? (below ? below->distance / total : 1.0) : 0.0;
  const double w_below = 1.0 - w_above;

  std::vector<double> value(static_cast<size_t>(last - first + 1), 0.0);
  if (!colour) {
    for (int32_t s = first; s <= last; ++s) {
      double v = 0.0;
      if (above) v += w_above * above->data[s];
      if (below) v += w_below * below->data[s];
      value[static_cast<size_t>(s - first)] = v;
    }
  } else {
    std::vector<std::complex<double>> relative(value.size(), {0.0, 0.0});
    auto add = [&](const Neighbour& n, double weight) {
      const auto span =
          n.partner ? splitter->split_combed(
                          n.data, n.partner, line_length, first, last + 1,
                          kCombMaxLumaDifferenceIre * ire_codes)
                    : splitter->split(n.data, line_length, first, last + 1);
      const bool differs = split::v_switch_differs(system, n.distance);
      for (size_t i = 0; i < value.size(); ++i) {
        value[i] += weight * span.luma[i];
        relative[i] +=
            weight *
            split::across_v_switch(
                split::to_burst_relative(span.chroma[i], *n.burst), differs);
      }
    };
    if (above) add(*above, w_above);
    if (below) add(*below, w_below);
    for (int32_t s = first; s <= last; ++s) {
      const auto i = static_cast<size_t>(s - first);
      value[i] += split::remodulate(
          split::from_burst_relative(relative[i], *target_burst), s);
    }
  }

  for (int32_t s = first; s <= last; ++s) {
    out_line[s] = highlight
                      ? *highlight
                      : static_cast<int16_t>(std::clamp(
                            std::lround(value[static_cast<size_t>(s - first)]),
                            0L, 1023L));
  }
  return true;
}

// ============================================================================
// correct_single_frame
// ============================================================================

void DropoutCorrectStage::correct_single_frame(
    CorrectedVideoFrameRepresentation* corrected,
    std::shared_ptr<const VideoFrameRepresentation> source,
    FrameID frame_id) const {
  ORC_LOG_DEBUG("DropoutCorrectStage::correct_single_frame - frame {}",
                frame_id);

  const auto desc_opt = source->get_frame_descriptor(frame_id);
  if (!desc_opt) {
    ORC_LOG_DEBUG("DropoutCorrectStage: no descriptor for frame {}", frame_id);
    return;
  }
  const auto& desc = *desc_opt;

  const size_t spl = desc.samples_per_line_nominal;
  const size_t height = desc.height;
  const size_t field1_lines = field1_lines_for_system(desc.system);

  auto runs = source->get_dropout_hints(frame_id);
  auto dropouts = runs_to_line_dropouts(runs, desc.system, spl);

  ORC_LOG_DEBUG(
      "DropoutCorrectStage: frame {} has {} dropout hints ({} line-dropouts)",
      frame_id, runs.size(), dropouts.size());

  // put_if_absent throughout: overlapping decode windows (e.g. Transform 3D
  // look-around) make concurrent duplicate corrections of the same frame
  // routine; replacing a cached entry would free the buffer another thread's
  // get_frame() pointer still refers to.
  if (dropouts.empty()) {
    if (source->has_separate_channels()) {
      corrected->corrected_luma_frames_.put_if_absent(frame_id,
                                                      std::vector<int16_t>{});
      corrected->corrected_chroma_frames_.put_if_absent(frame_id,
                                                        std::vector<int16_t>{});
    } else {
      corrected->corrected_frames_.put_if_absent(frame_id,
                                                 std::vector<int16_t>{});
    }
    return;
  }

  const auto video_params = source->get_video_parameters();

  const int16_t highlight_val =
      video_params ? static_cast<int16_t>(video_params->white_level)
                   : int16_t{1023};

  if (config_.overcorrect_extension > 0) {
    for (auto& d : dropouts) {
      if (d.start_sample > config_.overcorrect_extension) {
        d.start_sample -= config_.overcorrect_extension;
      } else {
        d.start_sample = 0;
      }
      if (d.end_sample + config_.overcorrect_extension <
          static_cast<uint32_t>(spl)) {
        d.end_sample += config_.overcorrect_extension;
      } else {
        d.end_sample = static_cast<uint32_t>(spl - 1);
      }
    }
  }

  const auto split_dropouts =
      split_dropout_regions(dropouts, desc, video_params);

  // interpolate acts on the picture; a dropout in the burst, or past the
  // active line, is copied from a line as before.
  auto interpolating = [&](const LineDropout& d) {
    return config_.method == DropoutCorrectionMethod::Interpolate &&
           classify_dropout(d, desc, video_params) ==
               DropoutLocation::VISIBLE_LINE;
  };
  const std::optional<int16_t> highlight =
      corrected->highlight_corrections_ ? std::optional<int16_t>(highlight_val)
                                        : std::nullopt;

  // -----------------------------------------------------------------------
  // YC source path
  // -----------------------------------------------------------------------
  if (source->has_separate_channels()) {
    // Copy the exact source luma/chroma frame buffers so the corrected buffers
    // keep the source sample layout (see composite path note); consumers read
    // them whole via get_frame_luma()/get_frame_chroma().
    const size_t frame_samples = desc.samples_total;
    const int16_t* src_luma = source->get_frame_luma(frame_id);
    const int16_t* src_chroma = source->get_frame_chroma(frame_id);
    std::vector<int16_t> luma_data =
        src_luma ? std::vector<int16_t>(src_luma, src_luma + frame_samples)
                 : std::vector<int16_t>(frame_samples, int16_t{0});
    std::vector<int16_t> chroma_data =
        src_chroma
            ? std::vector<int16_t>(src_chroma, src_chroma + frame_samples)
            : std::vector<int16_t>(frame_samples, int16_t{0});

    size_t corrections = 0;
    for (const auto& d : split_dropouts) {
      if (d.line >= height) {
        continue;
      }
      const size_t line_base =
          frame_line_sample_offset(desc.system, spl, d.line);
      const size_t line_len = frame_line_sample_count(desc.system, spl, d.line);
      int16_t* ly_ptr = &luma_data[line_base];
      int16_t* lc_ptr = &chroma_data[line_base];

      // interpolate fills each channel it can; the rest is copied below.
      bool luma_done = false;
      bool chroma_done = false;
      if (interpolating(d)) {
        luma_done =
            interpolate_dropout(*source, frame_id, desc, d, dropouts,
                                field1_lines, Channel::LUMA, highlight, ly_ptr);
        chroma_done = interpolate_dropout(*source, frame_id, desc, d, dropouts,
                                          field1_lines, Channel::CHROMA,
                                          highlight, lc_ptr);
        if (luma_done && chroma_done) {
          corrections++;
          continue;
        }
      }

      // Luma carries no subcarrier, so it may borrow the spatially nearest
      // line, falling back to the other field when no intrafield line is found.
      ReplacementLine luma_repl;
      if (!luma_done) {
        luma_repl = find_replacement_line(
            *source, frame_id, d.line, d, /*intrafield=*/true,
            /*match_chroma_phase_override=*/false, field1_lines, dropouts,
            Channel::LUMA);
      }
      if (!luma_done && !luma_repl.found) {
        luma_repl = find_replacement_line(
            *source, frame_id, d.line, d, /*intrafield=*/false,
            /*match_chroma_phase_override=*/false, field1_lines, dropouts,
            Channel::LUMA);
      }

      // Chroma must preserve subcarrier phase, so it is restricted to a
      // phase-matched intrafield line; an interfield line would invert the
      // colour and is never used.
      ReplacementLine chroma_repl;
      if (!chroma_done) {
        chroma_repl = find_replacement_line(
            *source, frame_id, d.line, d, /*intrafield=*/true,
            config_.match_chroma_phase, field1_lines, dropouts,
            Channel::CHROMA);
      }

      if (!luma_done && !chroma_done && !luma_repl.found &&
          !chroma_repl.found) {
        continue;
      }

      const int16_t* ry = luma_repl.found ? source->get_line_luma(
                                                frame_id, luma_repl.source_line)
                                          : nullptr;
      const int16_t* rc =
          chroma_repl.found
              ? source->get_line_chroma(frame_id, chroma_repl.source_line)
              : nullptr;
      for (uint32_t s = d.start_sample; s <= d.end_sample && s < line_len;
           ++s) {
        if (luma_repl.found) {
          ly_ptr[s] = corrected->highlight_corrections_ ? highlight_val
                      : ry                              ? ry[s]
                                                        : int16_t{0};
        }
        if (chroma_repl.found) {
          lc_ptr[s] = corrected->highlight_corrections_ ? highlight_val
                      : rc                              ? rc[s]
                                                        : int16_t{0};
        }
      }
      corrections++;
    }

    corrected->corrected_luma_frames_.put_if_absent(frame_id,
                                                    std::move(luma_data));
    corrected->corrected_chroma_frames_.put_if_absent(frame_id,
                                                      std::move(chroma_data));
    ORC_LOG_DEBUG("DropoutCorrectStage: YC frame {} done - {} corrections",
                  frame_id, corrections);
    return;
  }

  // -----------------------------------------------------------------------
  // Composite source path
  // -----------------------------------------------------------------------
  // Start from an exact copy of the source frame so the corrected buffer keeps
  // the source sample layout (PAL frames are non-uniform: lines 312 and 624
  // carry two extra samples).  Consumers that read the whole frame via
  // get_frame() — e.g. the chroma decoder — depend on this layout.
  std::vector<int16_t> frame_data = source->get_frame_copy(frame_id);

  size_t corrections = 0;

  for (const auto& d : split_dropouts) {
    if (d.line >= height) {
      continue;
    }
    const size_t line_base = frame_line_sample_offset(desc.system, spl, d.line);
    const size_t line_len = frame_line_sample_count(desc.system, spl, d.line);
    int16_t* line_ptr = &frame_data[line_base];

    if (interpolating(d) &&
        interpolate_dropout(*source, frame_id, desc, d, dropouts, field1_lines,
                            Channel::COMPOSITE, highlight, line_ptr)) {
      corrections++;
      continue;
    }

    // Composite samples carry luma and chroma interleaved on the subcarrier, so
    // the whole region must be copied from a single chroma-phase-matched line.
    // Replacement is restricted to an intrafield (same-field) line: an
    // interfield line does not preserve subcarrier phase and would invert the
    // colour, so it is never used here.
    auto repl = find_replacement_line(
        *source, frame_id, d.line, d, /*intrafield=*/true,
        config_.match_chroma_phase, field1_lines, dropouts, Channel::COMPOSITE);
    if (repl.found) {
      const int16_t* rep = source->get_line(frame_id, repl.source_line);
      for (uint32_t s = d.start_sample; s <= d.end_sample && s < line_len;
           ++s) {
        line_ptr[s] = corrected->highlight_corrections_ ? highlight_val
                      : rep                             ? rep[s]
                                                        : int16_t{0};
      }
      corrections++;
    }
  }

  corrected->corrected_frames_.put_if_absent(frame_id, std::move(frame_data));
  ORC_LOG_DEBUG("DropoutCorrectStage: composite frame {} done - {} corrections",
                frame_id, corrections);
}

// ============================================================================
// Parameters
// ============================================================================

std::vector<ParameterDescriptor> DropoutCorrectStage::get_parameter_descriptors(
    VideoSystem, SourceType) const {
  return {
      ParameterDescriptor{
          "method", "Method",
          "How a dropout in the picture is filled: copied "
          "from one nearby line (line_copy), or interpolated "
          "from the nearest clean lines above and below with "
          "luma and chroma separated (interpolate).",
          ParameterType::STRING,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue(std::string(kMethodInterpolate)),
                               {kMethodLineCopy, kMethodInterpolate},
                               false,
                               std::nullopt}},
      ParameterDescriptor{"overcorrect_extension", "Overcorrect (samples)",
                          "Extend dropout regions by N samples on each side "
                          "(0 = disabled, 24 = typical for damaged sources).",
                          ParameterType::UINT32,
                          ParameterConstraints{ParameterValue(0U),
                                               ParameterValue(48U),
                                               ParameterValue(0U),
                                               {},
                                               false,
                                               std::nullopt}},
      ParameterDescriptor{
          "match_chroma_phase", "Match Chroma Phase",
          "Prefer replacement lines with matching chroma phase.",
          ParameterType::BOOL,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue(true),
                               {},
                               false,
                               std::nullopt}},
      ParameterDescriptor{
          "highlight_corrections", "Highlight Corrections",
          "Fill corrections with white level instead of replacement data "
          "(for debugging).",
          ParameterType::BOOL,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue(false),
                               {},
                               false,
                               std::nullopt}},
  };
}

std::map<std::string, ParameterValue> DropoutCorrectStage::get_parameters()
    const {
  return {
      {"method", ParameterValue{std::string(
                     config_.method == DropoutCorrectionMethod::Interpolate
                         ? kMethodInterpolate
                         : kMethodLineCopy)}},
      {"overcorrect_extension",
       ParameterValue{static_cast<uint32_t>(config_.overcorrect_extension)}},
      {"match_chroma_phase", ParameterValue{config_.match_chroma_phase}},
      {"highlight_corrections", ParameterValue{config_.highlight_corrections}},
  };
}

bool DropoutCorrectStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key == "method") {
      const auto* v = std::get_if<std::string>(&value);
      const auto method =
          v ? parse_dropout_correction_method(*v) : std::nullopt;
      if (!method) {
        return false;
      }
      config_.method = *method;
    } else if (key == "overcorrect_extension") {
      if (const auto* v = std::get_if<uint32_t>(&value)) {
        if (*v > 48U) {
          return false;
        }
        config_.overcorrect_extension = *v;
      } else {
        return false;
      }
    } else if (key == "match_chroma_phase") {
      if (const auto* v = std::get_if<bool>(&value)) {
        config_.match_chroma_phase = *v;
      } else {
        return false;
      }
    } else if (key == "highlight_corrections") {
      if (const auto* v = std::get_if<bool>(&value)) {
        config_.highlight_corrections = *v;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

// ============================================================================
// Preview
// ============================================================================

StagePreviewCapability DropoutCorrectStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

}  // namespace orc
