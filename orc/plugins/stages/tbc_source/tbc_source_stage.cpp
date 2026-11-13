/*
 * File:        tbc_source_stage.cpp
 * Module:      orc-stage-plugin-tbc-source
 * Purpose:     Unified TBC source stage — PAL/NTSC/PAL_M TBC file loading
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "tbc_source_stage.h"

#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/error_types.h>
#include <orc/support/dropout_util.h>
#include <orc/support/frame_cache.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/lru_cache.h>
#include <orc/support/pal_line_locked_resample.h>
#include <orc/support/preview_helpers.h>
#include <orc/support/read_only_file.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include "audio-resample/audio_resampler.h"
#include "audio-resample/synchronous_audio_cache.h"
#include "ntsc_tbc_converter.h"
#include "pal_m_tbc_converter.h"
#include "pal_tbc_converter.h"
#include "tbc_level_derivation.h"
#include "tbc_metadata_json_reader.h"
#include "tbc_metadata_reader.h"
#include "tbc_metadata_types.h"

namespace orc {

namespace {

// Build SourceParameters from TBCVideoParams using spec-defined CVBS levels.
SourceParameters build_source_params(const TBCVideoParams& tvp,
                                     int32_t frame_count) {
  SourceParameters sp;
  sp.system = tvp.system;
  sp.number_of_sequential_frames = frame_count;
  sp.is_widescreen = tvp.is_widescreen;
  sp.is_mapped = tvp.is_mapped;
  sp.decoder = tvp.decoder;
  sp.tape_format = tvp.tape_format;
  sp.git_branch = tvp.git_branch;
  sp.git_commit = tvp.git_commit;

  switch (tvp.system) {
    case VideoSystem::PAL:
      // EBU Tech. 3280-E §1.1 level constants.
      sp.frame_width_nominal = kPalSamplesPerLineNominal;
      sp.frame_height = kPalFrameLines;
      sp.sync_tip_level = kPalSyncTip;
      sp.blanking_level = kPalBlanking;
      sp.black_level = kPalBlack;
      sp.white_level = kPalWhite;
      sp.peak_level = kPalPeak;
      sp.active_video_start = kPalActiveVideoStart;
      sp.active_video_end = kPalActiveVideoEnd;
      sp.first_active_frame_line = kPalFirstActiveFrameLine;
      sp.last_active_frame_line = kPalLastActiveFrameLine;
      break;
    case VideoSystem::NTSC:
      // SMPTE 244M-2003 level constants.
      sp.frame_width_nominal = kNtscSamplesPerLine;
      sp.frame_height = kNtscFrameLines;
      sp.sync_tip_level = kNtscSyncTip;
      sp.blanking_level = kNtscBlanking;
      sp.black_level = kNtscBlack;
      sp.white_level = kNtscWhite;
      sp.peak_level = kNtscPeak;
      sp.active_video_start = kNtscActiveVideoStart;
      sp.active_video_end = kNtscActiveVideoEnd;
      sp.first_active_frame_line = kNtscFirstActiveFrameLine;
      sp.last_active_frame_line = kNtscLastActiveFrameLine;
      if (tvp.ntsc_j_black_level_16b.has_value()) {
        // NTSC-J: convert 16-bit TBC domain to CVBS 10-bit.
        const double n =
            static_cast<double>(tvp.ntsc_j_black_level_16b.value() -
                                tvp.blanking_16b) /
            static_cast<double>(tvp.white_16b - tvp.blanking_16b);
        const int32_t cvbs_j = static_cast<int32_t>(
            n * (kNtscWhite - kNtscBlanking) + kNtscBlanking);
        sp.black_level = cvbs_j;
        sp.has_nonstandard_values = true;
      }
      break;
    case VideoSystem::PAL_M:
      // ITU-R BT.1700-1 Annex 1 Part B: PAL_M uses NTSC signal levels.
      sp.frame_width_nominal = kPalMSamplesPerLine;
      sp.frame_height = kPalMFrameLines;
      sp.sync_tip_level = kNtscSyncTip;
      sp.blanking_level = kNtscBlanking;
      sp.black_level = kNtscBlack;
      sp.white_level = kNtscWhite;
      sp.peak_level = kNtscPeak;
      sp.active_video_start = kNtscActiveVideoStart;
      sp.active_video_end = kNtscActiveVideoEnd;
      sp.first_active_frame_line = kNtscFirstActiveFrameLine;
      sp.last_active_frame_line = kNtscLastActiveFrameLine;
      break;
    default:
      break;
  }

  // Compute chroma DC offset in CVBS domain: the raw .tbcc chroma is centred
  // at 32768 (uint16 midpoint); map that to CVBS using the same formula as
  // tbc_to_cvbs so the Y+C combined view can remove the DC before summing.
  if (tvp.blanking_16b > 0 && tvp.white_16b > tvp.blanking_16b &&
      sp.blanking_level >= 0 && sp.white_level > sp.blanking_level) {
    const double n = (32768.0 - static_cast<double>(tvp.blanking_16b)) /
                     static_cast<double>(tvp.white_16b - tvp.blanking_16b);
    const double cvbs =
        n * static_cast<double>(sp.white_level - sp.blanking_level) +
        static_cast<double>(sp.blanking_level);
    sp.chroma_dc_offset = static_cast<int32_t>(std::lround(cvbs));
  }

  return sp;
}

// Resolve display name: "<System> TBC <Composite|YC>"
std::string make_display_name(VideoSystem system, bool is_yc) {
  const std::string sys =
      (system == VideoSystem::Unknown) ? "TBC" : video_system_to_string(system);
  return sys + " TBC " + (is_yc ? "YC" : "Composite");
}

// ---------------------------------------------------------------------------
// TBCDecodedFrameRepresentation
// ---------------------------------------------------------------------------
// Implements VideoFrameRepresentation backed by a pair of ld-decode TBC fields
// assembled into CVBS_U10_4FSC frames.  Currently PAL composite and YC are
// fully implemented.  NTSC and PAL_M are reserved for Phase 5.
class TBCDecodedFrameRepresentation final : public VideoFrameRepresentation,
                                            public Artifact {
 public:
  TBCDecodedFrameRepresentation(
      TBCVideoParams video_params, SourceParameters source_params,
      std::vector<TBCFieldMeta> field_meta,
      std::shared_ptr<const ITBCFieldDropouts> field_dropouts,
      std::shared_ptr<ITBCSourceStageDeps> deps,
      std::string tbc_path,  // composite .tbc (or Y .tbc for YC)
      std::string c_path,    // chroma .tbc for YC mode; empty for composite
      std::string pcm_path, std::string efm_bin_path, std::string ac3_bin_path,
      bool has_audio, double pcm_sample_rate_hz, bool has_efm, bool has_ac3,
      std::string audio_pair_name, ArtifactID artifact_id,
      Provenance provenance)
      : Artifact(std::move(artifact_id), std::move(provenance)),
        video_params_(std::move(video_params)),
        source_params_(std::move(source_params)),
        field_meta_(std::move(field_meta)),
        field_dropouts_(std::move(field_dropouts)),
        deps_(std::move(deps)),
        tbc_path_(std::move(tbc_path)),
        c_path_(std::move(c_path)),
        pcm_path_(std::move(pcm_path)),
        efm_bin_path_(std::move(efm_bin_path)),
        ac3_bin_path_(std::move(ac3_bin_path)),
        has_audio_(has_audio),
        pcm_sample_rate_hz_(pcm_sample_rate_hz),
        has_efm_(has_efm),
        has_ac3_(has_ac3),
        is_yc_(!c_path_.empty()),
        audio_pair_name_(std::move(audio_pair_name)) {
    // Pre-compute the raw audio length fallback and per-frame EFM offsets from
    // field metadata (cheap, metadata-only, no disk I/O).  The audio ingest
    // conversion is deferred to first audio access (see audio_cache()) so it
    // never runs on the source-execute / preview hot path (issue #209).
    compute_audio_total_raw_pairs();
    compute_efm_offsets();
    compute_ac3_offsets();
  }

  // --------------------------------------------------------------------------
  // Artifact
  // --------------------------------------------------------------------------
  std::string type_name() const override {
    return "TBCDecodedFrameRepresentation";
  }

  // --------------------------------------------------------------------------
  // Navigation
  // --------------------------------------------------------------------------
  FrameIDRange frame_range() const override {
    if (frame_count() == 0) return FrameIDRange{0, 0};
    return FrameIDRange{0, static_cast<FrameID>(frame_count() - 1)};
  }

  size_t frame_count() const override {
    return static_cast<size_t>(video_params_.number_of_fields / 2);
  }

  bool has_frame(FrameID id) const override {
    return id < static_cast<FrameID>(frame_count());
  }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    // All descriptor fields come from pre-loaded metadata — no disk read
    // needed. Colour-sequence phase is intentionally not provided here; stages
    // that need it measure it from the burst via the colour_frame_phase
    // observer (see colour_frame_phase_query.h).
    FrameDescriptor desc;
    desc.frame_id = id;
    desc.system = video_params_.system;
    desc.height = static_cast<size_t>(source_params_.frame_height);
    desc.samples_total = static_cast<size_t>(frame_samples_total());
    desc.samples_per_line_nominal =
        static_cast<size_t>(source_params_.frame_width_nominal);
    if (video_params_.ntsc_j_black_level_16b.has_value()) {
      desc.black_level_override = source_params_.black_level;
    }
    // ld-decode marks fields it synthesised (skip/gap fill) with pad=true.
    // A frame counts as padding only when both of its TBC fields are padded —
    // a half-real frame still carries measurable signal.
    const size_t tbc_f1_idx = static_cast<size_t>(id) * 2;
    const size_t tbc_f2_idx = tbc_f1_idx + 1;
    desc.is_padding_frame = tbc_f2_idx < field_meta_.size() &&
                            field_meta_[tbc_f1_idx].is_pad &&
                            field_meta_[tbc_f2_idx].is_pad;
    return desc;
  }

  // --------------------------------------------------------------------------
  // Flat sample access
  // --------------------------------------------------------------------------
  const sample_type* get_frame(FrameID id) const override {
    if (!has_frame(id)) return nullptr;
    auto frame = cached_frame(id);
    const sample_type* data = frame->samples.data();
    return frame_cache_.pin(std::move(frame), data, kSamplesSlot);
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    if (!has_frame(id)) return {};
    return cached_frame(id)->samples;
  }

  // The readers this source's frame cache is sized for, at most.
  size_t max_concurrent_frame_requests() const override {
    return nominal_frame_cache_capacity();
  }

  // --------------------------------------------------------------------------
  // YC channels
  // --------------------------------------------------------------------------
  bool has_separate_channels() const override { return is_yc_; }

  // A Y/C frame's luma is its samples plane.
  const sample_type* get_frame_luma(FrameID id) const override {
    if (!is_yc_ || !has_frame(id)) return nullptr;
    auto frame = cached_frame(id);
    if (frame->samples.empty()) return nullptr;
    const sample_type* data = frame->samples.data();
    return frame_cache_.pin(std::move(frame), data, kSamplesSlot);
  }

  const sample_type* get_frame_chroma(FrameID id) const override {
    if (!is_yc_ || !has_frame(id)) return nullptr;
    auto frame = cached_frame(id);
    if (frame->chroma.empty()) return nullptr;
    const sample_type* data = frame->chroma.data();
    return frame_cache_.pin(std::move(frame), data, kChromaSlot);
  }

  const sample_type* get_line_luma(FrameID id, size_t line) const override {
    return line_in_channel(get_frame_luma(id), line);
  }

  const sample_type* get_line_chroma(FrameID id, size_t line) const override {
    return line_in_channel(get_frame_chroma(id), line);
  }

  // A PAL frame is not fixed-stride: rows 312 and 624 carry two extra samples,
  // so row k >= 313 starts at k * 1135 + 2 (issue #371).
  const sample_type* line_in_channel(const sample_type* frame,
                                     size_t line) const {
    if (!frame) return nullptr;
    if (line >= static_cast<size_t>(source_params_.frame_height)) {
      return nullptr;
    }
    return frame + frame_line_sample_offset(
                       video_params_.system,
                       static_cast<size_t>(source_params_.frame_width_nominal),
                       line);
  }

  // --------------------------------------------------------------------------
  // Hints
  // --------------------------------------------------------------------------
  std::vector<DropoutRun> get_dropout_hints(FrameID id) const override {
    if (!has_frame(id)) return {};
    // The readers of one frame (dropout correction, the observers, the
    // sink) ask more than once, so recent frames are kept.
    // get() copies under the cache's lock; a pointer could be evicted.
    if (auto cached = dropout_hint_cache_.get(id)) return std::move(*cached);
    std::vector<DropoutRun> hints = compute_dropout_hints(id);
    dropout_hint_cache_.put_if_absent(id, hints);
    return hints;
  }

  // The field's dropouts: read on demand, or from the loaded metadata.
  std::vector<DropoutInfo> field_dropout_list(size_t field_index) const {
    const TBCFieldMeta& meta = field_meta_[field_index];
    if (field_dropouts_) {
      return field_dropouts_->field_dropouts(
          meta.field_id >= 0 ? meta.field_id
                             : static_cast<int64_t>(field_index));
    }
    return meta.dropouts;
  }

  std::vector<DropoutRun> compute_dropout_hints(FrameID id) const {
    const VideoSystem sys = video_params_.system;
    const size_t tbc_f1_idx = static_cast<size_t>(id) * 2;
    const size_t tbc_f2_idx = tbc_f1_idx + 1;

    std::vector<DropoutRun> result;

    // PAL/NTSC/PAL_M: even index (TBC field 1, isFirstField) → VFR field 1
    // (top).
    const int32_t f1_vfr = 1;
    const int32_t f2_vfr = 2;

    // Upper bound on a valid field-relative line index. Reject out-of-range
    // lines (including a -1 produced by an upstream underflow) before feeding
    // them to the frame-geometry math — an out-of-range line yields a
    // pathological ~2^64 sample offset that hangs the preview render worker
    // (issue #209).
    const int32_t max_field_lines =
        (sys == VideoSystem::PAL)     ? kPalField1Lines
        : (sys == VideoSystem::NTSC)  ? kNtscField1Lines
        : (sys == VideoSystem::PAL_M) ? kPalMField1Lines
                                      : 0;

    const auto emit_run = [&](const DropoutInfo& d, int32_t vfr_field) {
      // end_sample is exclusive (tbc_metadata_types.h, ld-dropout-correct,
      // TBC Sink); a zero-length entry carries no dropout (issue #371).
      if (d.end_sample <= d.start_sample) return;
      const int32_t line = static_cast<int32_t>(d.line);
      if (line < 0 || line >= max_field_lines) return;
      if (sys == VideoSystem::PAL) {
        // PAL frames are the line-locked TBC fields resampled onto the 4fsc
        // lattice (PalTBCConverter), so the dropout moves with the picture:
        // the lattice samples that cover it, rounded outward.
        const int64_t line_start =
            static_cast<int64_t>(line) * kPalSamplesPerLineNominal;
        const auto span = pal_line_locked::lattice_span(
            vfr_field, line_start + d.start_sample,
            line_start + d.end_sample - 1);
        if (span.last < span.first) return;
        const uint64_t flat_start =
            pal_line_locked::lattice_field_start(vfr_field) +
            static_cast<uint64_t>(span.first);
        const auto count = static_cast<uint32_t>(span.last - span.first + 1);
        result.push_back({id, flat_start, count, 100});
        return;
      }
      const uint64_t flat_start = dropout_util::field_line_to_frame_sample(
          sys, vfr_field, line, static_cast<int32_t>(d.start_sample));
      const uint32_t count = d.end_sample - d.start_sample;
      result.push_back({id, flat_start, count, 100});
    };

    if (tbc_f1_idx < field_meta_.size()) {
      for (const auto& d : field_dropout_list(tbc_f1_idx)) {
        emit_run(d, f1_vfr);
      }
    }

    if (tbc_f2_idx < field_meta_.size()) {
      for (const auto& d : field_dropout_list(tbc_f2_idx)) {
        emit_run(d, f2_vfr);
      }
    }

    return result;
  }

  std::optional<SourceParameters> get_video_parameters() const override {
    return source_params_;
  }

  // --------------------------------------------------------------------------
  // Audio
  // --------------------------------------------------------------------------
  // The PCM sidecar is served as a single channel pair (pair 0). The raw
  // signed-LE 16-bit stereo stream (as written by ld-decode, nominally
  // 44100 Hz) is always converted on ingest — for all systems including PAL —
  // to the only permitted pipeline audio form: 48000 Hz synchronous
  // 24-bit-in-int32 stereo (SMPTE 272M-1994 §1.2/§1.3).  The conversion is
  // lazy: see audio_cache().
  size_t audio_channel_pair_count() const override {
    return has_audio_ ? 1 : 0;
  }

  std::optional<AudioChannelPairDescriptor> get_audio_channel_pair_descriptor(
      size_t pair) const override {
    if (!has_audio_ || pair != 0) return std::nullopt;
    const std::string name =
        audio_pair_name_.empty() ? "Analogue" : audio_pair_name_;
    return AudioChannelPairDescriptor{name, AudioOrigin::ANALOGUE};
  }

  std::vector<int32_t> get_audio_samples(size_t pair,
                                         FrameID id) const override {
    if (pair != 0 || !has_audio_ || !has_frame(id)) return {};
    return audio_cache().frame_block(static_cast<uint64_t>(id));
  }

  // --------------------------------------------------------------------------
  // EFM
  // --------------------------------------------------------------------------
  bool has_efm() const override { return has_efm_; }

  uint32_t get_efm_sample_count(FrameID id) const override {
    if (!has_efm_) return 0;
    const size_t idx = static_cast<size_t>(id);
    if (idx >= efm_frame_byte_counts_.size()) return 0;
    return static_cast<uint32_t>(efm_frame_byte_counts_[idx]);
  }

  std::vector<uint8_t> get_efm_samples(FrameID id) const override {
    if (!has_efm_ || !has_frame(id)) return {};
    const size_t idx = static_cast<size_t>(id);
    if (idx >= efm_frame_offsets_.size()) return {};
    const size_t byte_offset = efm_frame_offsets_[idx];
    const size_t byte_count = efm_frame_byte_counts_[idx];
    if (byte_count == 0) return {};
    return deps_->read_efm_bytes_at(efm_bin_path_, byte_offset, byte_count);
  }

  // --------------------------------------------------------------------------
  // AC3 RF
  // --------------------------------------------------------------------------
  bool has_ac3_rf() const override { return has_ac3_; }

  uint32_t get_ac3_symbol_count(FrameID id) const override {
    if (!has_ac3_) return 0;
    const size_t idx = static_cast<size_t>(id);
    if (idx >= ac3_frame_byte_counts_.size()) return 0;
    return static_cast<uint32_t>(ac3_frame_byte_counts_[idx]);
  }

  std::vector<uint8_t> get_ac3_symbols(FrameID id) const override {
    if (!has_ac3_ || !has_frame(id)) return {};
    const size_t idx = static_cast<size_t>(id);
    if (idx >= ac3_frame_offsets_.size()) return {};
    const size_t byte_offset = ac3_frame_offsets_[idx];
    const size_t byte_count = ac3_frame_byte_counts_[idx];
    if (byte_count == 0) return {};
    return deps_->read_ac3_bytes_at(ac3_bin_path_, byte_offset, byte_count);
  }

  // --------------------------------------------------------------------------
  // Per-line sample access
  // --------------------------------------------------------------------------
  // A line is served from the assembled frame, so it is the same samples
  // get_frame() holds: on PAL that is the 4fsc lattice, not the line-locked
  // line stored in the file. A TBC field is read whole either way, so a
  // reader wanting a few lines a frame costs one frame assembly, kept in the
  // frame cache for the frame's other readers.
  std::vector<sample_type> get_line_samples(FrameID id,
                                            size_t line) const override {
    if (!has_frame(id)) return {};
    if (line >= static_cast<size_t>(source_params_.frame_height)) return {};
    const auto frame = cached_frame(id);
    const auto spl = static_cast<size_t>(source_params_.frame_width_nominal);
    const size_t offset =
        frame_line_sample_offset(video_params_.system, spl, line);
    const size_t count =
        frame_line_sample_count(video_params_.system, spl, line);
    if (offset + count > frame->samples.size()) return {};
    const auto first =
        frame->samples.begin() + static_cast<std::ptrdiff_t>(offset);
    return std::vector<sample_type>(first,
                                    first + static_cast<std::ptrdiff_t>(count));
  }

 private:
  struct CachedFrame {
    std::vector<sample_type> samples;  // composite frame, or YC luma channel
    std::vector<sample_type> chroma;   // YC: chroma channel (empty otherwise)
  };

  // Pins of one frame's planes are kept apart, so a reader holding a frame's
  // luma and chroma pointers keeps both.
  static constexpr unsigned kSamplesSlot = 0;
  static constexpr unsigned kChromaSlot = 1;

  size_t frame_samples_total() const {
    return static_cast<size_t>(frame_samples_from_system(video_params_.system));
  }

  // The assembled frame, assembled once however many threads want it.
  std::shared_ptr<const CachedFrame> cached_frame(FrameID id) const {
    return frame_cache_.get(id, [&] { return assemble_frame(id); });
  }

  CachedFrame assemble_frame(FrameID id) const {
    switch (video_params_.system) {
      case VideoSystem::PAL:
        return assemble_pal_frame(id);
      case VideoSystem::NTSC:
        return assemble_ntsc_frame(id);
      case VideoSystem::PAL_M:
        return assemble_pal_m_frame(id);
      default:
        throw std::runtime_error(
            "TBC source: unsupported video system for frame assembly");
    }
  }

  CachedFrame assemble_pal_frame(FrameID id) const {
    // TBC field ordering (EBU Tech. 3280-E §1.3 / ld-decode PAL convention):
    //   Even field indices (0, 2, 4…) → TBC field 1 (313 lines,
    //   odd-scan/earlier) Odd field indices  (1, 3, 5…) → TBC field 2 (312
    //   lines, even-scan/later)
    const int32_t tbc_f1_idx = static_cast<int32_t>(id) * 2;
    const int32_t tbc_f2_idx = tbc_f1_idx + 1;

    constexpr int32_t kF1Lines = kPalField1Lines;                   // 313
    constexpr int32_t kF2Lines = kPalFrameLines - kPalField1Lines;  // 312
    constexpr int32_t kLineW = kPalSamplesPerLineNominal;           // 1135
    const int32_t stored_field_size = kF1Lines * kLineW;

    std::string err;

    const std::vector<uint16_t> raw_f1 = deps_->read_field_samples(
        tbc_path_, tbc_f1_idx, stored_field_size, kF1Lines * kLineW, err);
    if (raw_f1.empty()) {
      throw std::runtime_error("PAL TBC: failed to read field 1 for frame " +
                               std::to_string(id) + ": " + err);
    }
    const std::vector<uint16_t> raw_f2 = deps_->read_field_samples(
        tbc_path_, tbc_f2_idx, stored_field_size, kF2Lines * kLineW, err);
    if (raw_f2.empty()) {
      throw std::runtime_error("PAL TBC: failed to read field 2 for frame " +
                               std::to_string(id) + ": " + err);
    }

    CachedFrame result;
    result.samples = PalTBCConverter::assemble_frame(
        raw_f1, raw_f2, video_params_.blanking_16b, video_params_.white_16b);

    if (is_yc_) {
      const std::vector<uint16_t> raw_c1 = deps_->read_field_samples(
          c_path_, tbc_f1_idx, stored_field_size, kF1Lines * kLineW, err);
      const std::vector<uint16_t> raw_c2 = deps_->read_field_samples(
          c_path_, tbc_f2_idx, stored_field_size, kF2Lines * kLineW, err);
      if (raw_c1.empty() || raw_c2.empty()) {
        throw std::runtime_error(
            "PAL TBC YC: failed to read chroma field for frame " +
            std::to_string(id) + ": " + err);
      }
      result.chroma = PalTBCConverter::assemble_frame(
          raw_c1, raw_c2, video_params_.blanking_16b, video_params_.white_16b);
    }
    return result;
  }

  CachedFrame assemble_ntsc_frame(FrameID id) const {
    // SMPTE 244M-2003 §4.1: NTSC frame assembly.
    // Both TBC fields stored at 263 lines on disk.
    // TBC field 1 (even index, isFirstField=true) = odd-scan/first temporal,
    //   263 real lines → VFR field 1 (top).
    // TBC field 2 (odd index) = even-scan/second temporal, 262 real lines →
    //   VFR field 2 (bottom); the last stored line is padding and is discarded.
    const int32_t tbc_f1_idx = static_cast<int32_t>(id) * 2;
    const int32_t tbc_f2_idx = tbc_f1_idx + 1;

    constexpr int32_t kF1Lines = kNtscField1Lines;  // 263 = TBC f1 / VFR top
    constexpr int32_t kF2Lines =
        kNtscFrameLines - kNtscField1Lines;  // 262 = TBC f2 / VFR bottom
    constexpr int32_t kLineW = kNtscSamplesPerLine;  // 910
    const int32_t stored_field_size =
        kF1Lines * kLineW;  // 263×910 stored per field

    std::string err;

    // TBC field 1 (263 real lines, odd-scan, VFR top); use all lines.
    const std::vector<uint16_t> raw_f1 = deps_->read_field_samples(
        tbc_path_, tbc_f1_idx, stored_field_size, kF1Lines * kLineW, err);
    if (raw_f1.empty()) {
      throw std::runtime_error("NTSC TBC: failed to read field 1 for frame " +
                               std::to_string(id) + ": " + err);
    }
    // TBC field 2 (262 real lines, even-scan, VFR bottom); discard padding.
    const std::vector<uint16_t> raw_f2 = deps_->read_field_samples(
        tbc_path_, tbc_f2_idx, stored_field_size, kF2Lines * kLineW, err);
    if (raw_f2.empty()) {
      throw std::runtime_error("NTSC TBC: failed to read field 2 for frame " +
                               std::to_string(id) + ": " + err);
    }

    CachedFrame result;
    result.samples = NtscTBCConverter::assemble_frame(
        raw_f1, raw_f2, video_params_.blanking_16b, video_params_.white_16b);

    if (is_yc_) {
      const std::vector<uint16_t> raw_c1 = deps_->read_field_samples(
          c_path_, tbc_f1_idx, stored_field_size, kF1Lines * kLineW, err);
      const std::vector<uint16_t> raw_c2 = deps_->read_field_samples(
          c_path_, tbc_f2_idx, stored_field_size, kF2Lines * kLineW, err);
      if (raw_c1.empty() || raw_c2.empty()) {
        throw std::runtime_error(
            "NTSC TBC YC: failed to read chroma field for frame " +
            std::to_string(id) + ": " + err);
      }
      result.chroma = NtscTBCConverter::assemble_frame(
          raw_c1, raw_c2, video_params_.blanking_16b, video_params_.white_16b);
    }
    return result;
  }

  CachedFrame assemble_pal_m_frame(FrameID id) const {
    // ITU-R BT.1700-1 Annex 1 Part B: PAL_M frame assembly.
    // Identical field ordering to NTSC: TBC field 1 (even index, 263 lines)
    // → VFR field 1 (top). kPalMSamplesPerLine = 909 samples/line.
    const int32_t tbc_f1_idx = static_cast<int32_t>(id) * 2;
    const int32_t tbc_f2_idx = tbc_f1_idx + 1;

    constexpr int32_t kF1Lines = kPalMField1Lines;  // 263 = TBC f1 / VFR top
    constexpr int32_t kF2Lines =
        kPalMFrameLines - kPalMField1Lines;  // 262 = TBC f2 / VFR bottom
    constexpr int32_t kLineW = kPalMSamplesPerLine;  // 909
    const int32_t stored_field_size =
        kF1Lines * kLineW;  // 263×909 stored per field

    std::string err;

    // TBC field 1 (263 real lines, odd-scan, VFR top); use all lines.
    const std::vector<uint16_t> raw_f1 = deps_->read_field_samples(
        tbc_path_, tbc_f1_idx, stored_field_size, kF1Lines * kLineW, err);
    if (raw_f1.empty()) {
      throw std::runtime_error("PAL-M TBC: failed to read field 1 for frame " +
                               std::to_string(id) + ": " + err);
    }
    // TBC field 2 (262 real lines, even-scan, VFR bottom); discard padding.
    const std::vector<uint16_t> raw_f2 = deps_->read_field_samples(
        tbc_path_, tbc_f2_idx, stored_field_size, kF2Lines * kLineW, err);
    if (raw_f2.empty()) {
      throw std::runtime_error("PAL-M TBC: failed to read field 2 for frame " +
                               std::to_string(id) + ": " + err);
    }

    CachedFrame result;
    result.samples = PalMTBCConverter::assemble_frame(
        raw_f1, raw_f2, video_params_.blanking_16b, video_params_.white_16b);

    if (is_yc_) {
      // PAL_M YC: same field geometry as NTSC.
      const std::vector<uint16_t> raw_c1 = deps_->read_field_samples(
          c_path_, tbc_f1_idx, stored_field_size, kF1Lines * kLineW, err);
      const std::vector<uint16_t> raw_c2 = deps_->read_field_samples(
          c_path_, tbc_f2_idx, stored_field_size, kF2Lines * kLineW, err);
      if (raw_c1.empty() || raw_c2.empty()) {
        throw std::runtime_error(
            "PAL-M TBC YC: failed to read chroma field for frame " +
            std::to_string(id) + ": " + err);
      }
      result.chroma = PalMTBCConverter::assemble_frame(
          raw_c1, raw_c2, video_params_.blanking_16b, video_params_.white_16b);
    }
    return result;
  }

  // Sum the per-field audio sample counts from the metadata.  Used only as a
  // fallback for the raw sidecar length when the file size is unavailable;
  // the ingest conversion reads the whole stream regardless of the per-field
  // layout.
  void compute_audio_total_raw_pairs() {
    if (!has_audio_) return;
    size_t cumulative = 0;
    for (const auto& fm : field_meta_) {
      if (fm.audio_sample_count) {
        cumulative += static_cast<size_t>(*fm.audio_sample_count);
      }
    }
    audio_total_raw_pairs_ = cumulative;
  }

  // Ingest conversion of the raw PCM sidecar: widen the 16-bit samples to
  // the 24-bit-in-int32 carrier (<< 8), resample to the synchronous 48000 Hz
  // rate (SMPTE 272M-1994 §1.2), and segment into cadence-sized per-frame
  // blocks (§14.3 audio frame sequence: PAL 1920 pairs constant, NTSC/PAL-M
  // 1602/1601).  Applies to all systems including PAL (44100 → 48000).
  //
  // The cache is created on the first audio access, never in the
  // constructor, so a source execute or a video preview does no audio work
  // (issue #209).  It converts sequentially, a chunk at a time, only as far
  // as the frames asked for, into a scratch file: no whole-stream buffer
  // exists, so memory does not grow with the length of the capture and the
  // first frame is served after one chunk.  Thread-safe: audio accessors are
  // const and may be called concurrently.
  const SynchronousAudioCache& audio_cache() const {
    std::call_once(audio_once_, [this] {
      // The stream length comes from the file itself (authoritative),
      // falling back to the metadata per-field counts.
      const uint64_t total_pairs =
          deps_->get_audio_pair_count(pcm_path_).value_or(
              audio_total_raw_pairs_);
      auto deps = deps_;
      const std::string path = pcm_path_;
      SynchronousAudioCache::SourceReader reader = [deps, path, total_pairs](
                                                       uint64_t first,
                                                       size_t count) {
        if (first >= total_pairs) return std::vector<int32_t>{};
        const auto n =
            static_cast<size_t>(std::min<uint64_t>(count, total_pairs - first));
        std::vector<int16_t> raw =
            deps->read_audio_samples_at(path, static_cast<size_t>(first), n);
        raw.resize(raw.size() / 2 * 2);  // whole pairs only
        return AudioResampler::widen_16_to_24(raw);
      };
      audio_cache_ = std::make_unique<SynchronousAudioCache>(
          std::move(reader), pcm_sample_rate_hz_, video_params_.system,
          static_cast<uint64_t>(frame_count()),
          [deps]() { return deps->make_audio_store(); });
    });
    return *audio_cache_;
  }

  // Pre-compute cumulative per-frame EFM byte offsets from the per-field
  // T-value counts in the TBC metadata.  Each frame's EFM payload is the two
  // constituent fields' T-values concatenated; the raw .efm sidecar stores one
  // byte per T-value in field order, so a frame's byte offset is the running
  // sum of all preceding fields' counts.  Mirrors compute_audio_offsets().
  void compute_efm_offsets() {
    if (!has_efm_) return;
    const size_t fc = frame_count();
    efm_frame_offsets_.resize(fc, 0);
    efm_frame_byte_counts_.resize(fc, 0);

    size_t cumulative = 0;
    for (size_t frame_idx = 0; frame_idx < fc; ++frame_idx) {
      const size_t fld1 = frame_idx * 2;
      const size_t fld2 = fld1 + 1;

      size_t bytes = 0;
      if (fld1 < field_meta_.size()) {
        if (const auto& cnt = field_meta_[fld1].efm_t_value_count) {
          bytes += static_cast<size_t>(*cnt);
        }
      }
      if (fld2 < field_meta_.size()) {
        if (const auto& cnt = field_meta_[fld2].efm_t_value_count) {
          bytes += static_cast<size_t>(*cnt);
        }
      }

      efm_frame_offsets_[frame_idx] = cumulative;
      efm_frame_byte_counts_[frame_idx] = bytes;
      cumulative += bytes;
    }
  }

  // Pre-compute cumulative per-frame AC3 symbol offsets from the per-field
  // symbol counts in the TBC metadata.  The raw .ac3sym sidecar stores one
  // byte per QPSK symbol in field order, so a frame's byte offset is the
  // running sum of all preceding fields' counts.  Mirrors
  // compute_efm_offsets().
  void compute_ac3_offsets() {
    if (!has_ac3_) return;
    const size_t fc = frame_count();
    ac3_frame_offsets_.resize(fc, 0);
    ac3_frame_byte_counts_.resize(fc, 0);

    size_t cumulative = 0;
    for (size_t frame_idx = 0; frame_idx < fc; ++frame_idx) {
      const size_t fld1 = frame_idx * 2;
      const size_t fld2 = fld1 + 1;

      size_t bytes = 0;
      if (fld1 < field_meta_.size()) {
        if (const auto& cnt = field_meta_[fld1].ac3rf_symbol_count) {
          bytes += static_cast<size_t>(*cnt);
        }
      }
      if (fld2 < field_meta_.size()) {
        if (const auto& cnt = field_meta_[fld2].ac3rf_symbol_count) {
          bytes += static_cast<size_t>(*cnt);
        }
      }

      ac3_frame_offsets_[frame_idx] = cumulative;
      ac3_frame_byte_counts_[frame_idx] = bytes;
      cumulative += bytes;
    }
  }

  TBCVideoParams video_params_;
  SourceParameters source_params_;
  std::vector<TBCFieldMeta> field_meta_;
  std::shared_ptr<const ITBCFieldDropouts> field_dropouts_;
  std::shared_ptr<ITBCSourceStageDeps> deps_;
  std::string tbc_path_;
  std::string c_path_;
  std::string pcm_path_;
  std::string efm_bin_path_;
  std::string ac3_bin_path_;
  bool has_audio_ = false;
  // Sidecar input rate for the ingest resample.  44100 Hz (the ld-decode
  // default) unless the metadata pcm_audio_parameters declare otherwise.
  double pcm_sample_rate_hz_ = 44100.0;
  bool has_efm_ = false;
  bool has_ac3_ = false;
  bool is_yc_ = false;
  // Name for the analogue audio channel pair; empty falls back to "Analogue"
  // at the descriptor.
  std::string audio_pair_name_;

  // Fallback raw sidecar length (stereo pairs) from the metadata per-field
  // counts, used when the file size is unavailable.
  size_t audio_total_raw_pairs_ = 0;

  // Pre-computed EFM layout (bytes) — cumulative offsets into the raw .efm
  // sidecar, one entry per frame.
  std::vector<size_t> efm_frame_offsets_;
  std::vector<size_t> efm_frame_byte_counts_;

  // Pre-computed AC3 symbol layout (one byte per symbol, field order).
  std::vector<size_t> ac3_frame_offsets_;
  std::vector<size_t> ac3_frame_byte_counts_;

  // Converted audio (48 kHz 24-bit-in-int32, cadence-sized blocks), created
  // by audio_cache() on the first audio access.
  mutable std::once_flag audio_once_;
  mutable std::unique_ptr<SynchronousAudioCache> audio_cache_;

  // Dropout hints of recently asked frames. Sized above the readers' span.
  static constexpr size_t kDropoutHintCacheFrames = 256;
  mutable LRUCache<FrameID, std::vector<DropoutRun>> dropout_hint_cache_{
      kDropoutHintCacheFrames};

  mutable FrameCache<CachedFrame> frame_cache_;
};

// ---------------------------------------------------------------------------
// Metadata helpers shared by TBCSourceStageDeps
// ---------------------------------------------------------------------------

// Derive the legacy JSON sidecar path from a .db sidecar path.
// "foo.tbc.db" → "foo.tbc.json"
std::string json_path_from_db(const std::string& db_path) {
  constexpr std::string_view kSuffix = ".db";
  if (db_path.size() > kSuffix.size() &&
      db_path.compare(db_path.size() - kSuffix.size(), kSuffix.size(), ".db") ==
          0) {
    return db_path.substr(0, db_path.size() - kSuffix.size()) + ".json";
  }
  return {};
}

// Build TBCVideoParams from any open ITBCMetadataReader.
std::optional<TBCVideoParams> build_tvp_from_reader(
    ITBCMetadataReader& reader, const std::string& path,
    std::string& error_message) {
  const auto sp = reader.read_video_parameters();
  if (!sp) {
    error_message = "No video parameters in '" + path + "'";
    return std::nullopt;
  }
  TBCVideoParams tvp;
  tvp.system = sp->system;
  tvp.decoder = sp->decoder;
  tvp.tape_format = sp->tape_format;
  tvp.git_branch = sp->git_branch;
  tvp.git_commit = sp->git_commit;
  tvp.is_widescreen = sp->is_widescreen;
  tvp.is_mapped = sp->is_mapped;
  // Read actual ld-decode 16-bit domain levels from the metadata.
  // These are the 0 IRE blanking and 100 IRE white levels in the TBC 16-bit
  // domain (CVBS_U10_4FSC × 64); using them gives accurate CVBS_U10_4FSC
  // conversion.  The JSON reader derives blanking from black16bIre for
  // NTSC/PAL_M.
  const auto tbc_levels = reader.read_tbc_domain_levels();
  const bool is_ntsc_like =
      (tvp.system == VideoSystem::NTSC || tvp.system == VideoSystem::PAL_M);
  tvp.blanking_16b = tbc_levels
                         ? tbc_levels->blanking_16b
                         : (is_ntsc_like ? kTbcNtscBlanking : kTbcPalBlanking);
  tvp.white_16b = tbc_levels ? tbc_levels->white_16b
                             : (is_ntsc_like ? kTbcNtscWhite : kTbcPalWhite);
  // NTSC-J: picture black stored at the 0 IRE blanking level instead of the
  // SMPTE 170M 7.5 IRE setup pedestal.  Detected from the metadata levels so
  // SourceParameters/FrameDescriptor carry the black-level override exactly
  // as a CVBS source would (see tbc_level_derivation.h).
  if (tvp.system == VideoSystem::NTSC && tbc_levels &&
      is_ntsc_j_black_level(*tbc_levels)) {
    tvp.ntsc_j_black_level_16b = tbc_levels->black_16b;
  }
  tvp.number_of_fields = sp->number_of_sequential_frames * 2;
  tvp.field_width = sp->frame_width_nominal;
  // Field heights: both stored at max height in the TBC file.
  // PAL: TBC field 1 = 313 lines (odd-scan, isFirstField), TBC field 2 = 312.
  // NTSC/PAL_M: TBC field 1 = 263 lines (isFirstField), TBC field 2 = 262.
  // In all systems, field 1 (isFirstField=true) is the longer stored field.
  const int32_t padded_fh =
      static_cast<int32_t>(calculate_padded_field_height(sp->system));
  tvp.field1_height = padded_fh;
  tvp.field2_height = padded_fh - 1;
  tvp.active_video_start = sp->active_video_start;
  tvp.active_video_end = sp->active_video_end;
  tvp.first_active_frame_line = sp->first_active_frame_line;
  tvp.last_active_frame_line = sp->last_active_frame_line;
  return tvp;
}

// Build the TBCFieldMeta list from any open ITBCMetadataReader, straight
// from its rows in field_id order. |with_dropouts| copies each field's
// dropouts in; otherwise they are read per field on demand.
std::vector<TBCFieldMeta> build_field_meta_from_reader(
    ITBCMetadataReader& reader, bool with_dropouts) {
  std::vector<TBCFieldMeta> result;
  const int32_t count = reader.get_field_record_count();
  if (count > 0) result.reserve(static_cast<size_t>(count));
  reader.for_each_field_metadata([&](const FieldMetadata& fm) {
    TBCFieldMeta meta;
    meta.audio_sample_count = fm.audio_samples;
    meta.efm_t_value_count = fm.efm_t_values;
    meta.ac3rf_symbol_count = fm.ac3rf_symbols;
    meta.file_location = fm.file_location;
    meta.is_pad = fm.is_pad.value_or(false);
    meta.field_id = fm.seq_no;
    if (with_dropouts) meta.dropouts = reader.read_dropouts(FieldID(fm.seq_no));
    result.push_back(std::move(meta));
  });
  return result;
}

// Per-field dropout reads from a .tbc.db: its own connection, so a frame's
// read never waits on another reader of the metadata.
class SqliteFieldDropouts final : public ITBCFieldDropouts {
 public:
  bool open(const std::string& db_path) { return reader_.open(db_path); }

  std::vector<DropoutInfo> field_dropouts(int64_t field_id) const override {
    return reader_.query_field_dropouts(FieldID(field_id));
  }

 private:
  TBCMetadataSqliteReader reader_;
};

// ---------------------------------------------------------------------------
// TBCSourceStageDeps — production filesystem / SQLite implementation
// ---------------------------------------------------------------------------

class TBCSourceStageDeps final : public ITBCSourceStageDeps {
 public:
  bool validate_input_file(const std::string& path,
                           std::string& error_message) const override {
    // An execute checks its inputs here first: files rewritten since they
    // were opened are opened afresh.
    files_.refresh();
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
      error_message = "TBC file not found: '" + path + "'";
      return false;
    }
    if (!fs::is_regular_file(path, ec)) {
      error_message = "TBC path is not a regular file: '" + path + "'";
      return false;
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
      error_message = "TBC file is not readable: '" + path + "'";
      return false;
    }
    return true;
  }

  std::optional<TBCVideoParams> load_video_params(
      const std::string& db_path, std::string& error_message) const override {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Try SQLite (.tbc.db) first.
    if (fs::exists(db_path, ec)) {
      TBCMetadataSqliteReader reader;
      if (!reader.open(db_path)) {
        error_message = "Failed to open TBC metadata: '" + db_path + "'";
        return std::nullopt;
      }
      return build_tvp_from_reader(reader, db_path, error_message);
    }

    // Fall back to legacy JSON (.tbc.json) when the SQLite sidecar is absent.
    const std::string json_path = json_path_from_db(db_path);
    if (!json_path.empty() && fs::exists(json_path, ec)) {
      auto reader = open_json_reader_cached(json_path);
      if (!reader) {
        error_message =
            "Failed to open TBC legacy JSON metadata: '" + json_path + "'";
        return std::nullopt;
      }
      return build_tvp_from_reader(*reader, json_path, error_message);
    }

    error_message = "TBC metadata database not found: '" + db_path + "'";
    return std::nullopt;
  }

  std::vector<TBCFieldMeta> load_all_field_meta(
      const std::string& db_path, std::string& error_message) const override {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Try SQLite (.tbc.db) first.
    if (fs::exists(db_path, ec)) {
      TBCMetadataSqliteReader reader;
      if (!reader.open(db_path)) {
        error_message =
            "Failed to open TBC metadata for field meta: '" + db_path + "'";
        return {};
      }
      // Dropouts are read per field on demand (open_field_dropouts).
      return build_field_meta_from_reader(reader, /*with_dropouts=*/false);
    }

    // Fall back to legacy JSON (.tbc.json). The parsed document already
    // holds every dropout, so they are copied in.
    const std::string json_path = json_path_from_db(db_path);
    if (!json_path.empty() && fs::exists(json_path, ec)) {
      auto reader = open_json_reader_cached(json_path);
      if (!reader) {
        error_message = "Failed to open TBC legacy JSON for field meta: '" +
                        json_path + "'";
        return {};
      }
      return build_field_meta_from_reader(*reader, /*with_dropouts=*/true);
    }

    error_message =
        "Failed to open TBC metadata for field meta: '" + db_path + "'";
    return {};
  }

  std::shared_ptr<const ITBCFieldDropouts> open_field_dropouts(
      const std::string& db_path) const override {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(db_path, ec)) return nullptr;  // legacy JSON: in memory
    auto dropouts = std::make_shared<SqliteFieldDropouts>();
    if (!dropouts->open(db_path)) {
      ORC_LOG_WARN("tbc_source: cannot open '{}' for dropout reads", db_path);
      return nullptr;
    }
    return dropouts;
  }

  std::vector<uint16_t> read_field_samples(
      const std::string& tbc_path, int32_t field_index,
      int32_t stored_samples_per_field, int32_t use_sample_count,
      std::string& error_message) const override {
    return read_field_samples_at(tbc_path, field_index,
                                 stored_samples_per_field, 0, use_sample_count,
                                 error_message);
  }

  std::vector<uint16_t> read_field_samples_at(
      const std::string& tbc_path, int32_t field_index,
      int32_t stored_samples_per_field, int32_t sample_offset,
      int32_t use_sample_count, std::string& error_message) const override {
    const auto file = files_.get(tbc_path, error_message);
    if (!file) return {};
    // Each field in the TBC file is stored at stored_samples_per_field × 2
    // bytes.
    const uint64_t byte_offset =
        (static_cast<uint64_t>(field_index) *
             static_cast<uint64_t>(stored_samples_per_field) +
         static_cast<uint64_t>(sample_offset)) *
        2;
    std::vector<uint16_t> samples(static_cast<size_t>(use_sample_count));
    const uint64_t length = static_cast<uint64_t>(use_sample_count) * 2;
    const int64_t got = file->read_at(byte_offset, samples.data(), length);
    if (got < 0 || static_cast<uint64_t>(got) < length) {
      error_message = std::string(got < 0 ? "Read failed" : "Short read") +
                      " for field " + std::to_string(field_index) +
                      " at offset " + std::to_string(sample_offset) + " in '" +
                      tbc_path + "'";
      return {};
    }
    return samples;
  }

  bool has_audio_file(const std::string& pcm_path) const override {
    namespace fs = std::filesystem;
    std::error_code ec;
    return fs::exists(pcm_path, ec) && fs::is_regular_file(pcm_path, ec);
  }

  std::optional<PcmAudioParameters> load_pcm_audio_parameters(
      const std::string& db_path) const override {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Try SQLite (.tbc.db) first.
    if (fs::exists(db_path, ec)) {
      TBCMetadataSqliteReader reader;
      if (!reader.open(db_path)) return std::nullopt;
      return reader.read_pcm_audio_parameters();
    }

    // Fall back to legacy JSON (.tbc.json).
    const std::string json_path = json_path_from_db(db_path);
    if (!json_path.empty() && fs::exists(json_path, ec)) {
      auto reader = open_json_reader_cached(json_path);
      if (!reader) return std::nullopt;
      return reader->read_pcm_audio_parameters();
    }

    return std::nullopt;
  }

  std::optional<uint64_t> get_audio_pair_count(
      const std::string& pcm_path) const override {
    namespace fs = std::filesystem;
    std::error_code ec;
    const uintmax_t size = fs::file_size(pcm_path, ec);
    if (ec) return std::nullopt;
    // Raw interleaved stereo int16_t: 4 bytes per pair.
    return static_cast<uint64_t>(size / 4);
  }

  std::vector<int16_t> read_audio_samples_at(
      const std::string& pcm_path, size_t stereo_pair_offset,
      size_t stereo_pair_count) const override {
    std::string error;
    const auto file = files_.get(pcm_path, error);
    if (!file) return {};
    std::vector<int16_t> samples(stereo_pair_count * 2);
    const int64_t got = file->read_at(
        static_cast<uint64_t>(stereo_pair_offset) * 4, samples.data(),
        static_cast<uint64_t>(samples.size()) * 2);
    if (got < 0) return {};
    samples.resize(static_cast<size_t>(got) / 2);
    return samples;
  }

  std::unique_ptr<ISynchronousAudioStore> make_audio_store() const override {
    std::string error;
    auto store = make_scratch_file_audio_store(error);
    if (!store) ORC_LOG_ERROR("tbc_source: {}", error);
    return store;
  }

  bool has_efm_file(const std::string& efm_bin_path) const override {
    namespace fs = std::filesystem;
    std::error_code ec;
    return fs::exists(efm_bin_path, ec) &&
           fs::is_regular_file(efm_bin_path, ec);
  }

  std::vector<uint8_t> read_efm_bytes_at(const std::string& efm_bin_path,
                                         size_t efm_byte_offset,
                                         size_t efm_byte_count) const override {
    return read_bytes_at(efm_bin_path, efm_byte_offset, efm_byte_count);
  }

  bool has_ac3_file(const std::string& ac3_bin_path) const override {
    namespace fs = std::filesystem;
    std::error_code ec;
    return fs::exists(ac3_bin_path, ec) &&
           fs::is_regular_file(ac3_bin_path, ec);
  }

  std::vector<uint8_t> read_ac3_bytes_at(const std::string& ac3_bin_path,
                                         size_t ac3_byte_offset,
                                         size_t ac3_byte_count) const override {
    return read_bytes_at(ac3_bin_path, ac3_byte_offset, ac3_byte_count);
  }

 private:
  std::vector<uint8_t> read_bytes_at(const std::string& path, size_t offset,
                                     size_t count) const {
    std::string error;
    const auto file = files_.get(path, error);
    if (!file) return {};
    std::vector<uint8_t> bytes(count);
    const int64_t got = file->read_at(offset, bytes.data(), count);
    if (got < 0) return {};
    bytes.resize(static_cast<size_t>(got));
    return bytes;
  }

  // The TBC, PCM, EFM and AC3 files, each opened once and read at explicit
  // offsets.
  ReadOnlyFileSet files_;

  // Opening a legacy .tbc.json parses the entire document, which is expensive
  // for large captures; video params, per-field meta, and configuration
  // validation all need it. Cache the most recently parsed file (keyed by
  // path, size, and modification time) so each sidecar is parsed once.
  // Returned readers are safe to share across threads: all reads are served
  // from immutable maps populated during open().
  std::shared_ptr<TBCMetadataJsonReader> open_json_reader_cached(
      const std::string& json_path) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    const uintmax_t size = fs::file_size(json_path, ec);
    const fs::file_time_type mtime = fs::last_write_time(json_path, ec);

    std::lock_guard<std::mutex> lock(json_cache_mutex_);
    if (json_cache_reader_ && json_cache_path_ == json_path &&
        json_cache_size_ == size && json_cache_mtime_ == mtime) {
      return json_cache_reader_;
    }

    ORC_LOG_INFO("tbc_source: using legacy JSON metadata: {}", json_path);
    auto reader = std::make_shared<TBCMetadataJsonReader>();
    if (!reader->open(json_path)) {
      return nullptr;
    }
    json_cache_reader_ = std::move(reader);
    json_cache_path_ = json_path;
    json_cache_size_ = size;
    json_cache_mtime_ = mtime;
    return json_cache_reader_;
  }

  mutable std::mutex json_cache_mutex_;
  mutable std::shared_ptr<TBCMetadataJsonReader> json_cache_reader_;
  mutable std::string json_cache_path_;
  mutable uintmax_t json_cache_size_ = 0;
  mutable std::filesystem::file_time_type json_cache_mtime_{};
};

}  // namespace

// ---------------------------------------------------------------------------
// TBCSourceStage
// ---------------------------------------------------------------------------

TBCSourceStage::TBCSourceStage(std::shared_ptr<ITBCSourceStageDeps> deps)
    : deps_(std::move(deps)) {
  if (!deps_) {
    deps_ = std::make_shared<TBCSourceStageDeps>();
  }
  set_configuration_status(orc::ConfigurationStatus::Red);
}

TBCSourceStage::SidecarPaths TBCSourceStage::resolve_sidecars(
    const std::string& tbc_path,
    const std::map<std::string, ParameterValue>& params) const {
  namespace fs = std::filesystem;

  auto get_str = [&](const std::string& key) -> std::string {
    const auto it = params.find(key);
    if (it != params.end() && std::holds_alternative<std::string>(it->second)) {
      return std::get<std::string>(it->second);
    }
    return {};
  };

  // For YC sources tbc_path is the .tbcy file, but metadata lives next to the
  // base .tbc file.  Strip the Y/C extension and restore ".tbc" so that the
  // derived db_path resolves to e.g. "foo.tbc.db".
  std::string meta_base = tbc_path;
  if (meta_base.size() > 5) {
    std::string ext = meta_base.substr(meta_base.size() - 5);
    for (auto& c : ext) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext == ".tbcy" || ext == ".tbcc") {
      meta_base = meta_base.substr(0, meta_base.size() - 5) + ".tbc";
    }
  }

  SidecarPaths sp;
  sp.db_path = get_str("db_path");
  if (sp.db_path.empty()) {
    sp.db_path = meta_base + ".db";
  }

  sp.pcm_path = get_str("pcm_path");
  sp.efm_path = get_str("efm_path");
  sp.ac3_path = get_str("ac3rf_path");
  return sp;
}

std::vector<ArtifactPtr> TBCSourceStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  if (!inputs.empty()) {
    throw std::runtime_error("tbc_source stage must have no inputs");
  }

  // Determine mode: composite or YC.
  auto get_str = [&](const std::string& key) -> std::string {
    const auto it = parameters.find(key);
    if (it != parameters.end() &&
        std::holds_alternative<std::string>(it->second)) {
      return std::get<std::string>(it->second);
    }
    return {};
  };

  const std::string y_path = get_str("y_path");
  const std::string c_path = get_str("c_path");
  const std::string comp_path = get_str("input_path");
  const bool is_yc = (!y_path.empty() && !c_path.empty());
  const std::string tbc_path = is_yc ? y_path : comp_path;

  if (tbc_path.empty()) {
    ORC_LOG_DEBUG("tbc_source: No input path configured, returning empty.");
    return {};
  }

  // Cache key: primary TBC path plus the audio pair name (so editing the name
  // re-emits a representation carrying the new descriptor).
  const std::string cache_key = tbc_path + "\x1f" + get_str("pcm_name");
  {
    std::lock_guard<std::mutex> lock(execute_mutex_);
    if (cached_representation_ && cached_input_key_ == cache_key) {
      return {cached_representation_};
    }
  }

  // Validate primary TBC file.
  std::string err;
  if (!deps_->validate_input_file(tbc_path, err)) {
    throw UserDataError("TBC source: " + err);
  }
  if (is_yc && !deps_->validate_input_file(c_path, err)) {
    throw UserDataError("TBC source (chroma): " + err);
  }

  // Resolve sidecar paths.
  const SidecarPaths sc = resolve_sidecars(tbc_path, parameters);

  // Load video parameters from metadata.
  auto tvp_opt = deps_->load_video_params(sc.db_path, err);
  if (!tvp_opt) {
    throw UserDataError("TBC source: failed to load metadata from '" +
                        sc.db_path + "': " + err);
  }
  TBCVideoParams tvp = std::move(*tvp_opt);

  if (tvp.system == VideoSystem::Unknown) {
    throw UserDataError("TBC source: unknown video system in '" + sc.db_path +
                        "'");
  }
  if (tvp.blanking_16b <= 0 || tvp.white_16b <= tvp.blanking_16b) {
    throw UserDataError("TBC source: invalid level values in '" + sc.db_path +
                        "' (blanking=" + std::to_string(tvp.blanking_16b) +
                        ", white=" + std::to_string(tvp.white_16b) + ")");
  }

  // Load all per-field metadata; dropouts are read per field on demand
  // where the metadata allows it.
  auto field_meta = deps_->load_all_field_meta(sc.db_path, err);
  auto field_dropouts = deps_->open_field_dropouts(sc.db_path);

  // Colour-sequence phase is not read from TBC metadata anywhere (it is
  // measured from the burst via the colour_frame_phase observer), so there is
  // no Y/C phase-alignment check here: the luma channel carries no burst, so
  // "Y/C phase alignment" is not a measurable or meaningful signal property.

  // Determine sidecar availability.
  const int32_t frame_count = tvp.number_of_fields / 2;
  bool has_audio = !sc.pcm_path.empty() && deps_->has_audio_file(sc.pcm_path);

  // PCM layout metadata: when the sidecar metadata carries
  // pcm_audio_parameters, validate the declared layout and honour the
  // declared sample rate for the ingest resample.  The supported layout is
  // signed little-endian 16-bit stereo (any sample rate — it is only the
  // resampler input rate).  When absent, assume the ld-decode default of
  // signed-LE 16-bit at 44100 Hz.
  double pcm_sample_rate_hz = 44100.0;
  if (has_audio) {
    const auto pcm_meta = deps_->load_pcm_audio_parameters(sc.db_path);
    if (pcm_meta && pcm_meta->is_valid()) {
      if (!pcm_meta->is_signed || !pcm_meta->is_little_endian ||
          pcm_meta->bits != 16) {
        const std::string message =
            "unsupported .pcm audio layout in metadata (bits=" +
            std::to_string(pcm_meta->bits) + ", " +
            (pcm_meta->is_signed ? "signed" : "unsigned") + ", " +
            (pcm_meta->is_little_endian ? "little" : "big") +
            "-endian); the supported layout is signed little-endian 16-bit — "
            "audio disabled";
        observation_context.set(FieldID(0), "tbc_source",
                                "pcm_audio_unsupported", message);
        ORC_LOG_WARN("tbc_source: {}", message);
        has_audio = false;
      } else {
        pcm_sample_rate_hz = pcm_meta->sample_rate;
      }
    }
  }

  // TBC EFM: the raw .efm sidecar holds one byte per T-value; per-field
  // T-value counts come from the TBC metadata (there is no .efm.meta index —
  // that sidecar is CVBS-only).  EFM is available only when the .efm file
  // exists and the metadata carries at least one field T-value count.
  const bool metadata_has_efm = std::any_of(
      field_meta.begin(), field_meta.end(), [](const TBCFieldMeta& fm) {
        return fm.efm_t_value_count.has_value() && *fm.efm_t_value_count > 0;
      });
  const bool has_efm = !sc.efm_path.empty() &&
                       deps_->has_efm_file(sc.efm_path) && metadata_has_efm;
  // TBC AC3 RF: same model as EFM — the raw .ac3sym sidecar holds one byte
  // per QPSK symbol; per-field symbol counts come from the TBC metadata
  // (there is no .meta index — that sidecar is CVBS-only).  AC3 is available
  // only when the .ac3sym file exists and the metadata carries at least one
  // field symbol count.
  const bool metadata_has_ac3 = std::any_of(
      field_meta.begin(), field_meta.end(), [](const TBCFieldMeta& fm) {
        return fm.ac3rf_symbol_count.has_value() && *fm.ac3rf_symbol_count > 0;
      });
  const bool has_ac3 = !sc.ac3_path.empty() &&
                       deps_->has_ac3_file(sc.ac3_path) && metadata_has_ac3;

  // Build SourceParameters.
  SourceParameters src_params = build_source_params(tvp, frame_count);

  // Create representation.
  auto repr = std::make_shared<TBCDecodedFrameRepresentation>(
      tvp, src_params, std::move(field_meta), std::move(field_dropouts), deps_,
      tbc_path, is_yc ? c_path : std::string{},
      has_audio ? sc.pcm_path : std::string{},
      has_efm ? sc.efm_path : std::string{},
      has_ac3 ? sc.ac3_path : std::string{}, has_audio, pcm_sample_rate_hz,
      has_efm, has_ac3, get_str("pcm_name"), ArtifactID{}, Provenance{});

  // Update display name.
  const std::string new_display = make_display_name(tvp.system, is_yc);
  {
    std::lock_guard<std::mutex> lock(execute_mutex_);
    display_name_ = new_display;
    cached_representation_ = repr;
    cached_input_key_ = cache_key;
  }

  ORC_LOG_INFO("tbc_source: Loaded '{}' — {} ({} frames)", tbc_path,
               new_display, frame_count);
  return {repr};
}

// ---------------------------------------------------------------------------
// Parameter descriptors
// ---------------------------------------------------------------------------

std::vector<ParameterDescriptor> TBCSourceStage::get_parameter_descriptors(
    VideoSystem /*project_format*/, SourceType source_type) const {
  std::vector<ParameterDescriptor> descs;

  // A project is either composite or Y/C, never both, so only the file
  // parameters that match its source type are offered. Unknown (no project
  // context) offers all of them.
  const bool show_composite = (source_type != SourceType::YC);
  const bool show_yc = (source_type != SourceType::Composite);

  auto make_path = [](const std::string& name, const std::string& display,
                      const std::string& desc_text,
                      const std::string& ext) -> ParameterDescriptor {
    ParameterDescriptor d;
    d.name = name;
    d.display_name = display;
    d.description = desc_text;
    d.type = ParameterType::FILE_PATH;
    d.constraints.required = false;
    d.constraints.default_value = std::string{};
    d.file_extension_hint = ext;
    return d;
  };

  if (show_composite) {
    descs.push_back(make_path(
        "input_path", "TBC File Path",
        "Path to the composite .tbc file from ld-decode (YC: leave empty and "
        "use y_path/c_path instead)",
        ".tbc"));
  }
  if (show_yc) {
    descs.push_back(make_path("y_path", "Luma TBC File Path (YC)",
                              "Path to the luma .tbcy file for YC sources",
                              ".tbcy"));
    descs.push_back(make_path("c_path", "Chroma TBC File Path (YC)",
                              "Path to the chroma .tbcc file for YC sources",
                              ".tbcc"));
  }
  descs.push_back(make_path(
      "pcm_path", "PCM Audio File Path",
      "Path to the analogue audio .pcm sidecar (raw signed 16-bit stereo PCM "
      "as written by ld-decode; always converted to 48 kHz 24-bit "
      "frame-locked audio on ingest)",
      ".pcm"));

  {
    ParameterDescriptor d;
    d.name = "pcm_name";
    d.display_name = "Audio Channel Pair Name";
    d.description =
        "Human-readable name for the analogue audio channel pair. Surfaces in "
        "the CVBS container and as the embedded stream title in the video "
        "sink. Empty uses \"Analogue\".";
    d.type = ParameterType::STRING;
    d.constraints.required = false;
    d.constraints.default_value = std::string("Analogue");
    descs.push_back(d);
  }

  descs.push_back(make_path("efm_path", "EFM Data File Path",
                            "Path to the EFM t-value .efm sidecar", ".efm"));
  descs.push_back(make_path("ac3rf_path", "AC3 RF Symbols File Path",
                            "Path to the AC3 RF symbols .ac3sym sidecar",
                            ".ac3sym"));
  return descs;
}

std::map<std::string, ParameterValue> TBCSourceStage::get_parameters() const {
  return parameters_;
}

bool TBCSourceStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& kv : params) {
    if (!std::holds_alternative<std::string>(kv.second)) return false;
  }
  parameters_ = params;

  const auto get_str = [&](const std::string& key) -> std::string {
    const auto it = params.find(key);
    if (it != params.end() && std::holds_alternative<std::string>(it->second)) {
      return std::get<std::string>(it->second);
    }
    return {};
  };

  const std::string comp_path = get_str("input_path");
  const std::string y_path = get_str("y_path");
  const std::string c_path = get_str("c_path");
  const bool is_yc = (!y_path.empty() && !c_path.empty());

  if (comp_path.empty() && !is_yc) {
    set_configuration_status(orc::ConfigurationStatus::Red);
    return true;
  }

  const std::string tbc_path = is_yc ? y_path : comp_path;
  std::string err;
  if (!deps_->validate_input_file(tbc_path, err)) {
    ORC_LOG_WARN("tbc_source: source file not accessible: {}", err);
    // The configured path does not point to a usable source file, so the
    // stage cannot produce any output; report Red rather than Yellow.
    set_configuration_status(orc::ConfigurationStatus::Red);
    return true;
  }

  const std::string explicit_db = get_str("db_path");
  const std::string db_path = explicit_db.empty()
                                  ? resolve_sidecars(tbc_path, params).db_path
                                  : explicit_db;

  std::string meta_err;
  const auto tvp_opt = deps_->load_video_params(db_path, meta_err);
  if (!tvp_opt) {
    ORC_LOG_WARN("tbc_source: metadata not accessible: {}", meta_err);
    set_configuration_status(orc::ConfigurationStatus::Yellow);
    return true;
  }

  set_configuration_status(orc::ConfigurationStatus::Green);
  return true;
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

StagePreviewCapability TBCSourceStage::get_preview_capability() const {
  auto vfr = std::dynamic_pointer_cast<const VideoFrameRepresentation>(
      cached_representation_);
  return PreviewHelpers::make_signal_preview_capability(vfr);
}

}  // namespace orc
