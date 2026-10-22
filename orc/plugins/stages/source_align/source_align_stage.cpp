/*
 * File:        source_align_stage.cpp
 * Module:      orc-core
 * Purpose:     Source alignment stage implementation (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "source_align_stage.h"

#include <orc/stage/artifact.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <limits>
#include <sstream>

namespace orc {

// ============================================================================
// AlignedSourceFrameRepresentation
// ============================================================================
// Wraps a VFrameR and remaps frame IDs by adding a fixed offset so that
// output frame_id 0 maps to source frame_id `offset_`.
class AlignedSourceFrameRepresentation : public VideoFrameRepresentationWrapper,
                                         public Artifact {
 public:
  AlignedSourceFrameRepresentation(
      std::shared_ptr<const VideoFrameRepresentation> source, FrameID offset,
      size_t source_index)
      : VideoFrameRepresentationWrapper(std::move(source)),
        Artifact(ArtifactID("aligned_source_" + std::to_string(source_index) +
                            "_offset_" + std::to_string(offset)),
                 Provenance{}),
        offset_(offset) {
    if (source_) {
      if (auto params = source_->get_video_parameters()) {
        system_ = params->system;
      }
    }
  }

  std::string type_name() const override {
    return "aligned_source_frame_representation";
  }

  FrameIDRange frame_range() const override {
    if (!source_) {
      return FrameIDRange{};
    }
    const auto src = source_->frame_range();
    if (offset_ >= src.count()) {
      return FrameIDRange{};
    }
    const size_t new_size = static_cast<size_t>(src.count() - offset_);
    return FrameIDRange{FrameID{0}, FrameID{new_size - 1}};
  }

  size_t frame_count() const override {
    if (!source_) {
      return 0;
    }
    const auto src = source_->frame_range();
    if (offset_ >= src.count()) {
      return 0;
    }
    return static_cast<size_t>(src.count() - offset_);
  }

  bool has_frame(FrameID id) const override {
    return source_ ? source_->has_frame(id + offset_) : false;
  }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (!source_) {
      return std::nullopt;
    }
    auto desc = source_->get_frame_descriptor(id + offset_);
    if (desc) {
      desc->frame_id = id;
    }
    return desc;
  }

  const sample_type* get_frame(FrameID id) const override {
    return source_ ? source_->get_frame(id + offset_) : nullptr;
  }

  const sample_type* get_line(FrameID id, size_t line) const override {
    return source_ ? source_->get_line(id + offset_, line) : nullptr;
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    return source_ ? source_->get_frame_copy(id + offset_)
                   : std::vector<sample_type>{};
  }

  bool has_separate_channels() const override {
    return source_ ? source_->has_separate_channels() : false;
  }

  const sample_type* get_frame_luma(FrameID id) const override {
    return source_ ? source_->get_frame_luma(id + offset_) : nullptr;
  }

  const sample_type* get_frame_chroma(FrameID id) const override {
    return source_ ? source_->get_frame_chroma(id + offset_) : nullptr;
  }

  const sample_type* get_line_luma(FrameID id, size_t line) const override {
    return source_ ? source_->get_line_luma(id + offset_, line) : nullptr;
  }

  const sample_type* get_line_chroma(FrameID id, size_t line) const override {
    return source_ ? source_->get_line_chroma(id + offset_, line) : nullptr;
  }

  std::vector<DropoutRun> get_dropout_hints(FrameID id) const override {
    if (!source_) {
      return {};
    }
    auto runs = source_->get_dropout_hints(id + offset_);
    for (auto& run : runs) {
      run.frame_id = id;
    }
    return runs;
  }

  // Audio channel pairs must follow the shifted frame IDs; pair count and
  // descriptors forward from the source via the wrapper base. Every output
  // frame must serve exactly audio_pairs_in_frame(id) stereo pairs, so an
  // offset that breaks the NTSC/PAL-M five-frame audio sequence phase
  // (SMPTE 272M-1994 §14.3: 1602/1601 pairs by sequence position) truncates
  // one trailing pair or appends one trailing silence pair. Offsets that
  // preserve the phase (multiples of 5) and all PAL offsets are sample-exact.
  std::vector<int32_t> get_audio_samples(size_t pair,
                                         FrameID id) const override {
    if (!source_) return {};
    auto samples = source_->get_audio_samples(pair, id + offset_);
    const size_t out_pairs = audio_pairs_in_frame(id, system_);
    if (samples.empty() || out_pairs == 0) return samples;
    samples.resize(out_pairs * 2, 0);
    return samples;
  }

  uint32_t get_efm_sample_count(FrameID id) const override {
    return source_ ? source_->get_efm_sample_count(id + offset_) : 0;
  }

  std::vector<uint8_t> get_efm_samples(FrameID id) const override {
    return source_ ? source_->get_efm_samples(id + offset_)
                   : std::vector<uint8_t>{};
  }

  uint32_t get_ac3_symbol_count(FrameID id) const override {
    return source_ ? source_->get_ac3_symbol_count(id + offset_) : 0;
  }

  std::vector<uint8_t> get_ac3_symbols(FrameID id) const override {
    return source_ ? source_->get_ac3_symbols(id + offset_)
                   : std::vector<uint8_t>{};
  }

 private:
  FrameID offset_;
  VideoSystem system_ = VideoSystem::Unknown;
};

// ============================================================================
// PaddedSourceFrameRepresentation
// ============================================================================
// Wraps a VFrameR and prepends `pad_count_` synthetic padding frames so that
// output frame_id 0 maps to the globally earliest VBI frame position, then
// appends `tail_count_` so every aligned output ends on the same frame.
// Padding frames carry is_padding_frame=true in their FrameDescriptor so that
// downstream stages (e.g. stacker) skip them correctly.
class PaddedSourceFrameRepresentation : public VideoFrameRepresentationWrapper,
                                        public Artifact {
 public:
  PaddedSourceFrameRepresentation(
      std::shared_ptr<const VideoFrameRepresentation> source, size_t pad_count,
      size_t tail_count, size_t source_index)
      : VideoFrameRepresentationWrapper(std::move(source)),
        Artifact(ArtifactID("padded_source_" + std::to_string(source_index) +
                            "_pad_" + std::to_string(pad_count) + "_tail_" +
                            std::to_string(tail_count)),
                 Provenance{}),
        pad_count_(pad_count),
        tail_count_(tail_count),
        source_count_(source_ ? source_->frame_count() : 0) {
    if (source_) {
      if (auto params = source_->get_video_parameters()) {
        pad_system_ = params->system;
        pad_height_ = static_cast<size_t>(params->frame_height);
        pad_samples_per_line_ =
            static_cast<size_t>(params->frame_width_nominal);
        // PAL lines 312 and 624 carry extra samples, so a frame is not
        // height x nominal width.
        pad_samples_total_ = frame_line_sample_offset(
            pad_system_, pad_samples_per_line_, pad_height_);
        blanking_level_ = static_cast<sample_type>(params->blanking_level);
      }
    }
  }

  std::string type_name() const override {
    return "padded_source_frame_representation";
  }

  FrameIDRange frame_range() const override {
    const size_t total = frame_count();
    if (total == 0) return FrameIDRange{};
    return FrameIDRange{FrameID{0}, FrameID{total - 1}};
  }

  size_t frame_count() const override {
    return pad_count_ + source_count_ + tail_count_;
  }

  bool has_frame(FrameID id) const override { return id < frame_count(); }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (is_padding(id)) {
      if (id >= frame_count()) return std::nullopt;
      FrameDescriptor desc;
      desc.frame_id = id;
      desc.system = pad_system_;
      desc.height = pad_height_;
      desc.samples_total = pad_samples_total_;
      desc.samples_per_line_nominal = pad_samples_per_line_;
      desc.is_padding_frame = true;
      return desc;
    }
    if (!source_) return std::nullopt;
    auto desc = source_->get_frame_descriptor(id - pad_count_);
    if (desc) desc->frame_id = id;
    return desc;
  }

  const sample_type* get_frame(FrameID id) const override {
    if (is_padding(id)) {
      if (id >= frame_count()) return nullptr;
      ensure_black_frame();
      return black_frame_.empty() ? nullptr : black_frame_.data();
    }
    return source_ ? source_->get_frame(id - pad_count_) : nullptr;
  }

  const sample_type* get_line(FrameID id, size_t line) const override {
    if (is_padding(id)) {
      if (id >= frame_count() || line >= pad_height_) return nullptr;
      ensure_black_frame();
      if (black_frame_.empty()) return nullptr;
      return black_frame_.data() +
             frame_line_sample_offset(pad_system_, pad_samples_per_line_, line);
    }
    return source_ ? source_->get_line(id - pad_count_, line) : nullptr;
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    if (is_padding(id)) {
      if (id >= frame_count()) return {};
      ensure_black_frame();
      return black_frame_;
    }
    return source_ ? source_->get_frame_copy(id - pad_count_)
                   : std::vector<sample_type>{};
  }

  bool has_separate_channels() const override {
    return source_ ? source_->has_separate_channels() : false;
  }

  const sample_type* get_frame_luma(FrameID id) const override {
    if (is_padding(id)) return nullptr;
    return source_ ? source_->get_frame_luma(id - pad_count_) : nullptr;
  }

  const sample_type* get_frame_chroma(FrameID id) const override {
    if (is_padding(id)) return nullptr;
    return source_ ? source_->get_frame_chroma(id - pad_count_) : nullptr;
  }

  const sample_type* get_line_luma(FrameID id, size_t line) const override {
    if (is_padding(id)) return nullptr;
    return source_ ? source_->get_line_luma(id - pad_count_, line) : nullptr;
  }

  const sample_type* get_line_chroma(FrameID id, size_t line) const override {
    if (is_padding(id)) return nullptr;
    return source_ ? source_->get_line_chroma(id - pad_count_, line) : nullptr;
  }

  std::vector<DropoutRun> get_dropout_hints(FrameID id) const override {
    if (is_padding(id)) return {};
    if (!source_) return {};
    auto runs = source_->get_dropout_hints(id - pad_count_);
    for (auto& run : runs) {
      run.frame_id = id;
    }
    return runs;
  }

  // Audio channel pairs must follow the shifted frame IDs; pair count and
  // descriptors forward from the source via the wrapper base. Every output
  // frame must serve exactly audio_pairs_in_frame(id) stereo pairs: padding
  // frames carry cadence-sized silence, and a pad count that breaks the
  // NTSC/PAL-M five-frame audio sequence phase (SMPTE 272M-1994 §14.3:
  // 1602/1601 pairs by sequence position) truncates one trailing pair or
  // appends one trailing silence pair on the shifted real frames. Pad counts
  // that preserve the phase (multiples of 5) and all PAL pad counts are
  // sample-exact.
  std::vector<int32_t> get_audio_samples(size_t pair,
                                         FrameID id) const override {
    if (!source_) return {};
    if (pair >= source_->audio_channel_pair_count()) return {};
    if (id >= frame_count()) return {};
    const size_t out_pairs = audio_pairs_in_frame(id, pad_system_);
    if (is_padding(id)) {
      // Padding frames carry cadence-sized silence.
      return std::vector<int32_t>(out_pairs * 2, 0);
    }
    auto samples = source_->get_audio_samples(pair, id - pad_count_);
    if (samples.empty() || out_pairs == 0) return samples;
    samples.resize(out_pairs * 2, 0);
    return samples;
  }

  uint32_t get_efm_sample_count(FrameID id) const override {
    if (is_padding(id) || !source_) return 0;
    return source_->get_efm_sample_count(id - pad_count_);
  }

  std::vector<uint8_t> get_efm_samples(FrameID id) const override {
    if (is_padding(id) || !source_) return {};
    return source_->get_efm_samples(id - pad_count_);
  }

  uint32_t get_ac3_symbol_count(FrameID id) const override {
    if (is_padding(id) || !source_) return 0;
    return source_->get_ac3_symbol_count(id - pad_count_);
  }

  std::vector<uint8_t> get_ac3_symbols(FrameID id) const override {
    if (is_padding(id) || !source_) return {};
    return source_->get_ac3_symbols(id - pad_count_);
  }

 private:
  // True for head and tail padding, and for ids past the end.
  bool is_padding(FrameID id) const {
    return id < pad_count_ || id >= pad_count_ + source_count_;
  }

  void ensure_black_frame() const {
    if (!black_frame_.empty() || pad_samples_total_ == 0) return;
    black_frame_.assign(pad_samples_total_, blanking_level_);
  }

  size_t pad_count_;
  size_t tail_count_;
  size_t source_count_;
  VideoSystem pad_system_ = VideoSystem::Unknown;
  size_t pad_height_ = 0;
  size_t pad_samples_total_ = 0;
  size_t pad_samples_per_line_ = 0;
  sample_type blanking_level_ = 0;
  mutable std::vector<sample_type> black_frame_;
};

// ============================================================================
// SourceAlignStage
// ============================================================================

SourceAlignStage::SourceAlignStage() {
  set_configuration_status(orc::ConfigurationStatus::Red);
}

// ============================================================================
// SourceAlignStage helpers
// ============================================================================

std::vector<std::pair<size_t, size_t>> SourceAlignStage::parse_alignment_map(
    const std::string& alignment_spec) {
  std::vector<std::pair<size_t, size_t>> result;
  if (alignment_spec.empty()) {
    return result;
  }

  std::istringstream iss(alignment_spec);
  std::string entry;
  while (std::getline(iss, entry, ',')) {
    entry.erase(0, entry.find_first_not_of(" \t"));
    const auto last = entry.find_last_not_of(" \t");
    if (last != std::string::npos) {
      entry.erase(last + 1);
    }
    if (entry.empty()) {
      continue;
    }

    const size_t plus = entry.find('+');
    if (plus == std::string::npos) {
      ORC_LOG_ERROR("Invalid alignment map entry (missing '+'): {}", entry);
      return {};
    }
    try {
      result.push_back({std::stoull(entry.substr(0, plus)),
                        std::stoull(entry.substr(plus + 1))});
    } catch (...) {
      ORC_LOG_ERROR("Invalid alignment map entry (parse error): {}", entry);
      return {};
    }
  }
  return result;
}

// ============================================================================
// execute()
// ============================================================================

std::vector<ArtifactPtr> SourceAlignStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  (void)observation_context;
  if (inputs.empty()) {
    throw DAGExecutionError("SourceAlignStage requires at least 1 input");
  }
  if (inputs.size() > 16) {
    throw DAGExecutionError("SourceAlignStage supports maximum 16 inputs");
  }

  ORC_LOG_DEBUG("SourceAlignStage: Processing {} input(s)", inputs.size());

  std::vector<std::shared_ptr<const VideoFrameRepresentation>> sources;
  for (const auto& input : inputs) {
    auto src = std::dynamic_pointer_cast<const VideoFrameRepresentation>(input);
    if (!src) {
      throw DAGExecutionError(
          "SourceAlignStage input must be VideoFrameRepresentation");
    }
    sources.push_back(src);
  }

  if (!parameters.empty()) {
    set_parameters(parameters);
  }

  std::vector<FrameID> offsets;

  if (alignment_map_.empty()) {
    if (sources.size() == 1) {
      std::lock_guard<std::mutex> lk(execute_mutex_);
      input_sources_ = sources;
      alignment_offsets_ = {FrameID{0}};
      cached_outputs_ = {sources[0]};
      return {inputs[0]};
    }
    throw DAGExecutionError(
        "Alignment map is not configured. Use the Source Alignment tool to "
        "generate the map, or set the alignmentMap parameter manually.");
  }

  const auto entries = parse_alignment_map(alignment_map_);
  if (entries.empty()) {
    throw DAGExecutionError("Invalid alignment map: " + alignment_map_);
  }

  offsets.assign(sources.size(), std::numeric_limits<FrameID>::max());
  for (const auto& [input_id, offset_val] : entries) {
    if (input_id < 1 || input_id > sources.size()) {
      throw DAGExecutionError("Alignment map references invalid input ID: " +
                              std::to_string(input_id));
    }
    offsets[input_id - 1] = static_cast<FrameID>(offset_val);
  }
  ORC_LOG_DEBUG("Using alignment map: {}", alignment_map_);

  // Build outputs locally before acquiring the lock so the lock is held
  // only for the pointer-swap, not during object construction.
  std::vector<ArtifactPtr> outputs;
  std::vector<std::shared_ptr<const VideoFrameRepresentation>> new_cached;

  const FrameID kExcluded = std::numeric_limits<FrameID>::max();
  const bool pad_mode = (alignment_mode_ == "pad_for_alignment");

  // In pad mode every output is padded at the tail to the longest one, so
  // downstream stages see equal-length inputs and keep every picture.
  size_t padded_length = 0;
  if (pad_mode) {
    for (size_t i = 0; i < sources.size(); ++i) {
      if (offsets[i] == kExcluded) continue;
      padded_length =
          std::max(padded_length, static_cast<size_t>(offsets[i]) +
                                      sources[i]->frame_count());
    }
  }

  for (size_t i = 0; i < sources.size(); ++i) {
    if (offsets[i] == kExcluded) {
      new_cached.push_back(nullptr);
      ORC_LOG_DEBUG("  Source {}: EXCLUDED from output", i);
      continue;
    }

    const size_t tail_count =
        pad_mode ? padded_length - static_cast<size_t>(offsets[i]) -
                       sources[i]->frame_count()
                 : 0;

    if (offsets[i] == 0 && tail_count == 0) {
      outputs.push_back(inputs[i]);
      new_cached.push_back(sources[i]);
      ORC_LOG_DEBUG("  Source {}: pass-through (offset=0)", i);
    } else if (pad_mode) {
      auto padded = std::make_shared<PaddedSourceFrameRepresentation>(
          sources[i], static_cast<size_t>(offsets[i]), tail_count, i);
      outputs.push_back(padded);
      new_cached.push_back(padded);
      ORC_LOG_DEBUG(
          "  Source {}: prepended {} and appended {} padding frames, {} total",
          i, offsets[i], tail_count, padded->frame_count());
    } else {
      auto aligned = std::make_shared<AlignedSourceFrameRepresentation>(
          sources[i], offsets[i], i);
      outputs.push_back(aligned);
      new_cached.push_back(aligned);
      ORC_LOG_DEBUG("  Source {}: offset by {} frames, {} remaining", i,
                    offsets[i], aligned->frame_count());
    }
  }

  // Atomically publish the new cached state so concurrent preview calls
  // always observe a consistent snapshot.
  {
    std::lock_guard<std::mutex> lk(execute_mutex_);
    input_sources_ = sources;
    alignment_offsets_ = offsets;
    cached_outputs_ = std::move(new_cached);
  }

  return outputs;
}

// ============================================================================
// Parameters
// ============================================================================

std::vector<ParameterDescriptor> SourceAlignStage::get_parameter_descriptors(
    VideoSystem, SourceType) const {
  return {
      ParameterDescriptor{
          "alignmentMode", "Alignment Mode",
          "How to align sources: 'pad_for_alignment' prepends synthetic "
          "padding frames so all sources start from the earliest available VBI "
          "frame, and pads every output at the tail to the longest one; "
          "'first_common_frame' trims leading frames so all sources start "
          "from the first VBI frame common to all.",
          ParameterType::STRING,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue{std::string("pad_for_alignment")},
                               {"first_common_frame", "pad_for_alignment"},
                               false,
                               std::nullopt}},
      ParameterDescriptor{
          "alignmentMap", "Alignment Map",
          "Alignment map ('1+2, 2+2, 3+1'). Format: input_id+frame_offset. "
          "In 'first_common_frame' mode the offset skips leading frames; in "
          "'pad_for_alignment' mode the offset is the number of padding frames "
          "prepended. Use the Source Alignment tool to generate this value.",
          ParameterType::STRING,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue{std::string("")},
                               {},
                               false,
                               std::nullopt}},
  };
}

std::map<std::string, ParameterValue> SourceAlignStage::get_parameters() const {
  return {{"alignmentMode", ParameterValue{alignment_mode_}},
          {"alignmentMap", ParameterValue{alignment_map_}}};
}

bool SourceAlignStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key == "alignmentMode") {
      if (const auto* s = std::get_if<std::string>(&value)) {
        if (*s != "first_common_frame" && *s != "pad_for_alignment") {
          ORC_LOG_ERROR("SourceAlignStage: unknown alignmentMode '{}'", *s);
          return false;
        }
        alignment_mode_ = *s;
      } else {
        return false;
      }
    } else if (key == "alignmentMap") {
      if (const auto* s = std::get_if<std::string>(&value)) {
        alignment_map_ = *s;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }
  set_configuration_status(alignment_map_.empty()
                               ? orc::ConfigurationStatus::Red
                               : orc::ConfigurationStatus::Green);
  return true;
}

// ============================================================================
// Preview
// ============================================================================

namespace {

PreviewImage render_vfr_grayscale(const VideoFrameRepresentation& vfr,
                                  FrameID fid, bool scale) {
  auto desc = vfr.get_frame_descriptor(fid);
  auto params = vfr.get_video_parameters();
  if (!desc || !params) {
    return PreviewImage{0, 0, {}, {}, {}};
  }
  const size_t H = desc->height;
  const size_t W = static_cast<size_t>(params->frame_width_nominal);
  const int32_t b = params->blanking_level;
  const int32_t w = params->white_level;
  const int32_t range = (w > b) ? (w - b) : 1;
  PreviewImage img;
  img.width = static_cast<uint32_t>(W);
  img.height = static_cast<uint32_t>(H);
  img.rgb_data.reserve(W * H * 3);
  for (size_t line = 0; line < H; ++line) {
    const int16_t* ptr = vfr.get_line(fid, line);
    for (size_t s = 0; s < W; ++s) {
      const int32_t raw = ptr ? static_cast<int32_t>(ptr[s]) : b;
      const uint8_t grey =
          scale ? static_cast<uint8_t>(
                      std::clamp((raw - b) * 255 / range, 0, 255))
                : static_cast<uint8_t>(std::clamp(raw * 255 / 1023, 0, 255));
      img.rgb_data.push_back(grey);
      img.rgb_data.push_back(grey);
      img.rgb_data.push_back(grey);
    }
  }
  return img;
}

}  // namespace

StagePreviewCapability SourceAlignStage::get_preview_capability() const {
  std::lock_guard<std::mutex> lk(execute_mutex_);
  for (const auto& out : cached_outputs_) {
    if (out && out->frame_count() > 0) {
      return PreviewHelpers::make_signal_preview_capability(out);
    }
  }
  return {};
}

std::vector<PreviewOption> SourceAlignStage::get_preview_options() const {
  std::lock_guard<std::mutex> lk(execute_mutex_);
  std::vector<PreviewOption> options;
  for (size_t i = 0; i < cached_outputs_.size(); ++i) {
    const auto& out = cached_outputs_[i];
    if (!out || out->frame_count() == 0) {
      continue;
    }
    const auto params = out->get_video_parameters();
    const uint32_t W =
        params ? static_cast<uint32_t>(params->frame_width_nominal) : 910;
    const uint32_t H =
        params ? static_cast<uint32_t>(params->frame_height) : 525;
    options.push_back({"source_" + std::to_string(i),
                       "Aligned Source " + std::to_string(i), false, W, H,
                       static_cast<uint64_t>(out->frame_count()), 0.75});
  }
  return options;
}

PreviewImage SourceAlignStage::render_preview(const std::string& option_id,
                                              uint64_t index,
                                              PreviewNavigationHint) const {
  const std::string prefix = "source_";
  size_t src_idx = 0;
  if (option_id.compare(0, prefix.size(), prefix) == 0) {
    try {
      src_idx = std::stoull(option_id.substr(prefix.size()));
    } catch (...) {
      throw std::runtime_error("Invalid preview option_id: " + option_id);
    }
  }

  // Take a local copy of the shared_ptr under the lock so rendering can
  // proceed without holding the lock (rendering can be slow).
  std::shared_ptr<const VideoFrameRepresentation> out;
  {
    std::lock_guard<std::mutex> lk(execute_mutex_);
    if (src_idx >= cached_outputs_.size() || !cached_outputs_[src_idx]) {
      throw std::runtime_error("No cached output for preview");
    }
    out = cached_outputs_[src_idx];
  }

  const FrameID fid = static_cast<FrameID>(index);
  if (!out->has_frame(fid)) {
    return PreviewImage{};
  }
  return render_vfr_grayscale(*out, fid, true);
}

}  // namespace orc
