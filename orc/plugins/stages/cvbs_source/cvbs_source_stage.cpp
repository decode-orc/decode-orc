/*
 * File:        cvbs_source_stage.cpp
 * Module:      orc-core
 * Purpose:     CVBS (Composite Video Baseband Signal) source loading stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "cvbs_source_stage.h"

#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/error_types.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/lru_cache.h>
#include <orc/support/preview_helpers.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace orc {

namespace {

// ---------------------------------------------------------------------------
// Sample encoding selection
// ---------------------------------------------------------------------------

// Sentinel value for the sample_encoding parameter: read the encoding from
// the .meta sidecar (the default). Any other allowed value selects that
// encoding manually and makes the .meta sidecar optional (CVBS file format
// spec: the metadata file is optional).
constexpr const char* kSampleEncodingFromMetadata = "From metadata";

// The four TBC-locked 4FSC-domain encodings this stage can normalise.
constexpr const char* kSupportedEncodings[] = {
    "CVBS_U10_4FSC", "CVBS_U16_4FSC", "CVBS_TPG21_4FSC", "CVBS_S16_4FSC"};

bool is_supported_encoding(const std::string& encoding) {
  for (const char* e : kSupportedEncodings) {
    if (encoding == e) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Sidecar path derivation
// ---------------------------------------------------------------------------

// Return the path with the last extension replaced by suffix.
std::string derive_sidecar_path(const std::string& input_path,
                                const std::string& suffix) {
  namespace fs = std::filesystem;
  const fs::path p(input_path);
  return (p.parent_path() / p.stem()).string() + suffix;
}

// ---------------------------------------------------------------------------
// Audio channel-pair state
// ---------------------------------------------------------------------------

// One pipeline audio channel pair backed by a <basename>_audio_<p>.wav
// sidecar (24-bit signed LE stereo PCM at 48000 Hz, CVBS file format spec
// v1.4.0).  An empty wav_path marks a placeholder for a container pair
// number with no file (numbers need not be contiguous); placeholders serve
// cadence-sized silence so pipeline pair indices match container numbers.
struct CVBSAudioChannelPairState {
  int32_t container_number = 0;  // <p> in the _audio_<p>.wav filename
  std::string wav_path;
  AudioChannelPairDescriptor descriptor;  // describes this stage's OUTPUT
};

// Bytes per interleaved 24-bit stereo pair (2 channels × 3 bytes).
constexpr uint64_t kAudioBytesPerPair = 6;

// ---------------------------------------------------------------------------
// Sample encoding normalisation
// ---------------------------------------------------------------------------

// The four sample encodings the CVBS container can declare (file format spec
// §3.1), resolved from the encoding name once so the per-sample conversion
// never touches a string. Matching the name inside the sample loop cost a
// reload of the string's size and data pointer plus up to four literal
// comparisons for every sample — the compiler cannot hoist them, because the
// output writes may alias the object holding the name.
enum class SampleEncoding { kU10, kU16, kTPG21, kS16 };

inline SampleEncoding sample_encoding_from_name(const std::string& encoding) {
  if (encoding == "CVBS_U10_4FSC") return SampleEncoding::kU10;
  if (encoding == "CVBS_TPG21_4FSC") return SampleEncoding::kTPG21;
  if (encoding == "CVBS_S16_4FSC") return SampleEncoding::kS16;
  // "CVBS_U16_4FSC", and any unknown encoding, which it stands in for.
  return SampleEncoding::kU16;
}

// CVBS file format spec §3.1: normalise a raw 16-bit word from any of the
// four declared sample encodings to the CVBS_U10_4FSC domain (int16_t).
// No output clamping — headroom values outside [0, 1023] are preserved.
//
// Converting a whole run of samples goes through normalize_samples() below,
// not this directly: the encoding has to be a compile-time constant for the
// loop to vectorise.
inline int16_t normalize_to_cvbs_u10(uint16_t raw, SampleEncoding encoding,
                                     int32_t blanking_10bit) {
  switch (encoding) {
    case SampleEncoding::kU10:
      // CVBS file format spec §3.1: int16_t stored bitwise as uint16_t.
      return static_cast<int16_t>(raw);
    case SampleEncoding::kTPG21:
      // CVBS file format spec §3.1: signed, device offset 508, ×64 scale.
      return static_cast<int16_t>(
          static_cast<int32_t>(static_cast<int16_t>(raw)) / 64 + 508);
    case SampleEncoding::kS16:
      // CVBS file format spec §3.1: signed, blanking-centred, ×32 scale.
      return static_cast<int16_t>(
          static_cast<int32_t>(static_cast<int16_t>(raw)) / 32 +
          blanking_10bit);
    case SampleEncoding::kU16:
      break;
  }
  // CVBS file format spec §3.1: unsigned value = 10-bit × 64.
  return static_cast<int16_t>(static_cast<int32_t>(raw) / 64);
}

// Convert |count| raw words into the CVBS_U10_4FSC domain.
//
// The encoding is a template parameter so the switch above folds away and each
// instantiation is a flat loop the vectoriser can take — 16 samples per
// instruction rather than a per-sample branch. Selecting the instantiation
// once per call is what makes that possible; dispatching per sample (whether on
// the encoding name or on the enum) leaves the branch inside the loop.
template <SampleEncoding kEncoding>
void normalize_samples_as(const uint16_t* src, size_t count,
                          int32_t blanking_10bit, int16_t* dst) {
  for (size_t i = 0; i < count; ++i) {
    dst[i] = normalize_to_cvbs_u10(src[i], kEncoding, blanking_10bit);
  }
}

inline void normalize_samples(const uint16_t* src, size_t count,
                              SampleEncoding encoding, int32_t blanking_10bit,
                              int16_t* dst) {
  switch (encoding) {
    case SampleEncoding::kU10:
      normalize_samples_as<SampleEncoding::kU10>(src, count, blanking_10bit,
                                                 dst);
      return;
    case SampleEncoding::kTPG21:
      normalize_samples_as<SampleEncoding::kTPG21>(src, count, blanking_10bit,
                                                   dst);
      return;
    case SampleEncoding::kS16:
      normalize_samples_as<SampleEncoding::kS16>(src, count, blanking_10bit,
                                                 dst);
      return;
    case SampleEncoding::kU16:
      break;
  }
  normalize_samples_as<SampleEncoding::kU16>(src, count, blanking_10bit, dst);
}

// ---------------------------------------------------------------------------
// SourceParameters population from spec constants
// ---------------------------------------------------------------------------

SourceParameters build_source_parameters(VideoSystem system,
                                         int32_t frame_count,
                                         int32_t ntsc_j_black_level = -1) {
  SourceParameters sp;
  sp.system = system;
  sp.number_of_sequential_frames = frame_count;
  // Provenance (decoder / git_branch / git_commit) is applied by the caller
  // from the .meta cvbs_file row when present.  The CVBS format carries no
  // is_mapped or tape_format equivalents, so those keep their defaults.

  switch (system) {
    case VideoSystem::PAL:
      // EBU Tech. 3280-E §1.1 level constants.
      sp.frame_width_nominal = kPalSamplesPerLineNominal;  // 1135
      sp.frame_height = kPalFrameLines;                    // 625
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
      sp.frame_width_nominal = kNtscSamplesPerLine;  // 910
      sp.frame_height = kNtscFrameLines;             // 525
      sp.sync_tip_level = kNtscSyncTip;
      sp.blanking_level = kNtscBlanking;
      sp.black_level =
          (ntsc_j_black_level >= 0) ? ntsc_j_black_level : kNtscBlack;
      sp.white_level = kNtscWhite;
      sp.peak_level = kNtscPeak;
      sp.active_video_start = kNtscActiveVideoStart;
      sp.active_video_end = kNtscActiveVideoEnd;
      sp.first_active_frame_line = kNtscFirstActiveFrameLine;
      sp.last_active_frame_line = kNtscLastActiveFrameLine;
      sp.has_nonstandard_values = (ntsc_j_black_level >= 0);
      break;

    case VideoSystem::PAL_M:
      // ITU-R BT.1700-1 Annex 1 Part B: PAL_M uses NTSC signal levels.
      sp.frame_width_nominal = kPalMSamplesPerLine;  // 909
      sp.frame_height = kPalMFrameLines;             // 525
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

  return sp;
}

// ---------------------------------------------------------------------------
// CVBSDecodedFrameRepresentation
// ---------------------------------------------------------------------------

// Immutable, lazily-decoded, cache-keyed-by-FrameID implementation of
// VideoFrameRepresentation for CVBS 4FSC sources.
//
// Inherits from both VideoFrameRepresentation (for consumers that need VFR)
// AND Artifact (so it can be returned as ArtifactPtr from execute()).
class CVBSDecodedFrameRepresentation final : public VideoFrameRepresentation,
                                             public Artifact {
 public:
  CVBSDecodedFrameRepresentation(
      VideoSystem system, int32_t frame_count, int32_t frame_samples,
      int32_t frame_height, int32_t spl_nominal,
      std::shared_ptr<ICVBSSourceStageDeps> deps, std::string input_path,
      std::string sample_encoding, SourceParameters video_params,
      std::optional<int32_t> ntsc_j_black_level,
      // Sidecars
      std::vector<DropoutRun> dropout_runs,
      std::vector<CVBSAudioChannelPairState> audio_pairs, bool has_efm,
      std::string efm_data_path, std::vector<CVBSExtensionFrameRef> efm_table,
      bool has_ac3, std::string ac3_data_path,
      std::vector<CVBSExtensionFrameRef> ac3_table, std::string c_path,
      ArtifactID artifact_id, Provenance provenance)
      : Artifact(std::move(artifact_id), std::move(provenance)),
        system_(system),
        frame_count_(static_cast<size_t>(frame_count)),
        frame_samples_(static_cast<size_t>(frame_samples)),
        frame_height_(static_cast<size_t>(frame_height)),
        spl_nominal_(static_cast<size_t>(spl_nominal)),
        deps_(std::move(deps)),
        input_path_(std::move(input_path)),
        sample_encoding_(std::move(sample_encoding)),
        encoding_(sample_encoding_from_name(sample_encoding_)),
        video_params_(std::move(video_params)),
        ntsc_j_black_level_(ntsc_j_black_level),
        blanking_level_(video_params_.blanking_level),
        dropout_runs_(std::move(dropout_runs)),
        audio_pairs_(std::move(audio_pairs)),
        has_efm_(has_efm),
        efm_data_path_(std::move(efm_data_path)),
        efm_table_(std::move(efm_table)),
        has_ac3_(has_ac3),
        ac3_data_path_(std::move(ac3_data_path)),
        ac3_table_(std::move(ac3_table)),
        c_path_(std::move(c_path)) {}

  // --------------------------------------------------------------------------
  // Artifact
  // --------------------------------------------------------------------------
  std::string type_name() const override {
    return "CVBSDecodedFrameRepresentation";
  }

  // --------------------------------------------------------------------------
  // Navigation
  // --------------------------------------------------------------------------
  FrameIDRange frame_range() const override {
    if (frame_count_ == 0) return FrameIDRange{1, 0};  // empty: last < first
    return FrameIDRange{0, static_cast<FrameID>(frame_count_ - 1)};
  }

  size_t frame_count() const override { return frame_count_; }

  bool has_frame(FrameID id) const override {
    return id < static_cast<FrameID>(frame_count_);
  }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    // All descriptor fields come from pre-loaded metadata — no disk read
    // needed.
    FrameDescriptor desc;
    desc.frame_id = id;
    desc.system = system_;
    desc.height = frame_height_;
    desc.samples_total = frame_samples_;
    desc.samples_per_line_nominal = spl_nominal_;
    if (ntsc_j_black_level_.has_value()) {
      desc.black_level_override = ntsc_j_black_level_;
    }
    return desc;
  }

  // --------------------------------------------------------------------------
  // Flat sample access
  // --------------------------------------------------------------------------

  const sample_type* get_frame(FrameID id) const override {
    if (!has_frame(id)) return nullptr;
    ensure_frame_cached(id);
    const DecodedFrame* df = frame_cache_.get_ptr(id);
    return df ? df->samples.data() : nullptr;
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    if (!has_frame(id)) return {};
    const sample_type* ptr = get_frame(id);
    if (!ptr) return {};
    return std::vector<sample_type>(ptr, ptr + frame_samples_);
  }

  // --------------------------------------------------------------------------
  // Hints
  // --------------------------------------------------------------------------
  std::vector<DropoutRun> get_dropout_hints(FrameID id) const override {
    // dropout_runs_ is sorted by frame_id (loaded with ORDER BY frame_id).
    // Binary search replaces the O(N) linear scan: O(log M + k) per call.
    std::vector<DropoutRun> result;
    auto it = std::lower_bound(
        dropout_runs_.begin(), dropout_runs_.end(), id,
        [](const DropoutRun& run, FrameID fid) { return run.frame_id < fid; });
    while (it != dropout_runs_.end() && it->frame_id == id) {
      result.push_back(*it++);
    }
    return result;
  }

  std::optional<SourceParameters> get_video_parameters() const override {
    return video_params_;
  }

  // --------------------------------------------------------------------------
  // Audio channel pairs
  // --------------------------------------------------------------------------
  // Every <basename>_audio_<p>.wav sidecar is the pipeline channel pair with
  // the same index (CVBS file format spec v1.4.0).  The payload is already
  // in the pipeline audio form (24-bit signed LE stereo at 48000 Hz
  // synchronous), so per-frame reads seek directly to the SMPTE 272M
  // cadence offset; short or absent data is silence-padded to cadence size.
  size_t audio_channel_pair_count() const override {
    return audio_pairs_.size();
  }

  std::optional<AudioChannelPairDescriptor> get_audio_channel_pair_descriptor(
      size_t pair) const override {
    if (pair >= audio_pairs_.size()) return std::nullopt;
    return audio_pairs_[pair].descriptor;
  }

  std::vector<int32_t> get_audio_samples(size_t pair,
                                         FrameID id) const override {
    if (pair >= audio_pairs_.size() || !has_frame(id)) return {};
    const size_t frame_values =
        static_cast<size_t>(audio_pairs_in_frame(id, system_)) * 2;
    const CVBSAudioChannelPairState& state = audio_pairs_[pair];
    // Placeholder pair (container number with no file): cadence silence.
    if (state.wav_path.empty()) return std::vector<int32_t>(frame_values, 0);
    std::vector<int32_t> samples = deps_->read_audio_pairs_at(
        state.wav_path, audio_pair_offset(id, system_),
        audio_pairs_in_frame(id, system_));
    samples.resize(frame_values, 0);  // silence-pad short reads
    return samples;
  }

  // --------------------------------------------------------------------------
  // EFM
  // --------------------------------------------------------------------------
  bool has_efm() const override { return has_efm_; }

  uint32_t get_efm_sample_count(FrameID id) const override {
    if (!has_efm_ || id >= efm_table_.size()) return 0;
    return efm_table_[static_cast<size_t>(id)].count;
  }

  std::vector<uint8_t> get_efm_samples(FrameID id) const override {
    if (!has_efm_ || id >= efm_table_.size()) return {};
    const auto& ref = efm_table_[static_cast<size_t>(id)];
    if (ref.count == 0) return {};
    return deps_->read_efm_bytes_at(efm_data_path_, ref.offset, ref.count);
  }

  // --------------------------------------------------------------------------
  // AC3 RF
  // --------------------------------------------------------------------------
  bool has_ac3_rf() const override { return has_ac3_; }

  uint32_t get_ac3_symbol_count(FrameID id) const override {
    if (!has_ac3_ || id >= ac3_table_.size()) return 0;
    return ac3_table_[static_cast<size_t>(id)].count;
  }

  std::vector<uint8_t> get_ac3_symbols(FrameID id) const override {
    if (!has_ac3_ || id >= ac3_table_.size()) return {};
    const auto& ref = ac3_table_[static_cast<size_t>(id)];
    if (ref.count == 0) return {};
    return deps_->read_ac3_bytes_at(ac3_data_path_, ref.offset, ref.count);
  }

  // --------------------------------------------------------------------------
  // Targeted per-line sample access (bypasses full-frame load)
  // --------------------------------------------------------------------------
  // Uses frame_line_sample_offset to compute the exact byte position of the
  // requested line in the flat CVBS file and reads only that line.
  std::vector<sample_type> get_line_samples(FrameID id,
                                            size_t line) const override {
    if (!has_frame(id)) return {};
    if (line >= frame_height_) return {};

    const size_t line_offset =
        frame_line_sample_offset(system_, spl_nominal_, line);
    const size_t line_count =
        frame_line_sample_count(system_, spl_nominal_, line);
    const size_t word_offset =
        static_cast<size_t>(id) * frame_samples_ + line_offset;

    std::vector<uint16_t> raw_words;
    std::string err;
    if (!deps_->read_input_words_at(input_path_, word_offset, line_count,
                                    raw_words, err)) {
      return {};
    }
    if (raw_words.size() < line_count) return {};

    std::vector<sample_type> result(line_count);
    normalize_samples(raw_words.data(), line_count, encoding_, blanking_level_,
                      result.data());
    return result;
  }

  // --------------------------------------------------------------------------
  // YC (separate luma / chroma) access
  // --------------------------------------------------------------------------
  bool has_separate_channels() const override { return !c_path_.empty(); }

  const sample_type* get_frame_luma(FrameID id) const override {
    return has_separate_channels() ? get_frame(id) : nullptr;
  }

  const sample_type* get_frame_chroma(FrameID id) const override {
    if (c_path_.empty() || !has_frame(id)) return nullptr;
    ensure_c_frame_cached(id);
    const DecodedFrame* df = c_frame_cache_.get_ptr(id);
    return df ? df->samples.data() : nullptr;
  }

  const sample_type* get_line_luma(FrameID id, size_t line) const override {
    const sample_type* frame = get_frame_luma(id);
    if (!frame || line >= frame_height_) return nullptr;
    return frame + frame_line_sample_offset(system_, spl_nominal_, line);
  }

  const sample_type* get_line_chroma(FrameID id, size_t line) const override {
    const sample_type* frame = get_frame_chroma(id);
    if (!frame || line >= frame_height_) return nullptr;
    return frame + frame_line_sample_offset(system_, spl_nominal_, line);
  }

 private:
  struct DecodedFrame {
    std::vector<sample_type> samples;
  };

  // put_if_absent (not put): one representation is shared by every DAG
  // consumer, so several observation workers can race past the contains()
  // check and all decode the same frame. Replacing the cached entry would
  // free the buffer that an earlier thread's get_frame() pointer still refers
  // to — a 1.4 MB mmap'd block, so the fault is immediate and hard.
  void ensure_frame_cached(FrameID id) const {
    if (frame_cache_.contains(id)) return;
    frame_cache_.put_if_absent(id, decode_channel_frame(input_path_, id));
  }

  void ensure_c_frame_cached(FrameID id) const {
    if (c_frame_cache_.contains(id)) return;
    c_frame_cache_.put_if_absent(id, decode_channel_frame(c_path_, id));
  }

  DecodedFrame decode_channel_frame(const std::string& path, FrameID id) const {
    const size_t word_offset = static_cast<size_t>(id) * frame_samples_;
    std::vector<uint16_t> raw_words;
    std::string err;
    if (!deps_->read_input_words_at(path, word_offset, frame_samples_,
                                    raw_words, err)) {
      throw std::runtime_error("CVBS: failed to read frame " +
                               std::to_string(id) + " from '" + path +
                               "': " + err);
    }
    if (raw_words.size() < frame_samples_) {
      throw std::runtime_error("CVBS: short read at frame " +
                               std::to_string(id) + " in '" + path + "'");
    }
    DecodedFrame result;
    result.samples.resize(frame_samples_);
    normalize_samples(raw_words.data(), frame_samples_, encoding_,
                      blanking_level_, result.samples.data());
    return result;
  }

  VideoSystem system_;
  size_t frame_count_;
  size_t frame_samples_;
  size_t frame_height_;
  size_t spl_nominal_;

  std::shared_ptr<ICVBSSourceStageDeps> deps_;
  std::string input_path_;
  std::string sample_encoding_;
  // sample_encoding_ resolved once at construction; the sample loops read this
  // instead of matching the name per sample.
  SampleEncoding encoding_;
  SourceParameters video_params_;
  std::optional<int32_t> ntsc_j_black_level_;
  int32_t blanking_level_;

  static constexpr size_t kFrameCacheSize = 150;
  mutable LRUCache<FrameID, DecodedFrame> frame_cache_{kFrameCacheSize};

  std::vector<DropoutRun> dropout_runs_;

  std::vector<CVBSAudioChannelPairState> audio_pairs_;

  bool has_efm_ = false;
  std::string efm_data_path_;
  std::vector<CVBSExtensionFrameRef> efm_table_;

  bool has_ac3_ = false;
  std::string ac3_data_path_;
  std::vector<CVBSExtensionFrameRef> ac3_table_;

  std::string c_path_;
  mutable LRUCache<FrameID, DecodedFrame> c_frame_cache_{kFrameCacheSize};
};

// ---------------------------------------------------------------------------
// CVBSSourceStageDeps — production filesystem / SQLite implementation
// ---------------------------------------------------------------------------

class CVBSSourceStageDeps final : public ICVBSSourceStageDeps {
 public:
  bool validate_input_file(const std::string& input_path,
                           std::string& error_message) const override {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(input_path, ec)) {
      error_message = "CVBS source file not found: '" + input_path + "'";
      return false;
    }
    if (!fs::is_regular_file(input_path, ec)) {
      error_message =
          "CVBS source path is not a regular file: '" + input_path + "'";
      return false;
    }
    std::ifstream ifs(input_path, std::ios::binary);
    if (!ifs.is_open()) {
      error_message = "CVBS source file is not readable: '" + input_path + "'";
      return false;
    }
    ifs.seekg(0, std::ios::end);
    if (!ifs.good() || ifs.tellg() <= 0) {
      error_message = "CVBS source file is empty: '" + input_path + "'";
      return false;
    }
    return true;
  }

  std::optional<CVBSMetadataRecord> load_metadata(
      const std::string& meta_path, std::string& error_message) const override {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(meta_path, ec)) {
      error_message = "Metadata file not found: '" + meta_path + "'";
      return std::nullopt;
    }

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(meta_path.c_str(), &db, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
      error_message = "Failed to open metadata '" + meta_path +
                      "': " + (db ? sqlite3_errmsg(db) : "unknown error");
      if (db) sqlite3_close(db);
      return std::nullopt;
    }

    // CVBS file format spec v1.6.0 (user_version = 11): sequence_continuous
    // is a mandatory column.
    constexpr const char* kSql =
        "SELECT preset, sample_encoding_preset, signal_state_preset, "
        "signal_type, number_of_sequential_frames, black_level, decoder, "
        "git_branch, git_commit, sequence_continuous "
        "FROM cvbs_file ORDER BY cvbs_file_id LIMIT 1";

    // Fallback for files predating the provenance columns.  Pre-v1.6.0 files
    // that reach this path still load their row, then fail the
    // signal_state_preset acceptance check with a clear error.
    constexpr const char* kSqlNoProvenance =
        "SELECT preset, sample_encoding_preset, signal_state_preset, "
        "signal_type, number_of_sequential_frames, black_level "
        "FROM cvbs_file ORDER BY cvbs_file_id LIMIT 1";

    sqlite3_stmt* stmt = nullptr;
    bool has_provenance_columns = true;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
      has_provenance_columns = false;
      if (sqlite3_prepare_v2(db, kSqlNoProvenance, -1, &stmt, nullptr) !=
          SQLITE_OK) {
        error_message = "Failed to query cvbs_file from '" + meta_path +
                        "': " + sqlite3_errmsg(db);
        sqlite3_close(db);
        return std::nullopt;
      }
    }

    if (sqlite3_step(stmt) != SQLITE_ROW) {
      error_message = "No cvbs_file row in '" + meta_path + "'";
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return std::nullopt;
    }

    auto col_str = [&](int col) -> std::string {
      const unsigned char* v = sqlite3_column_text(stmt, col);
      return v ? reinterpret_cast<const char*>(v) : "";
    };

    CVBSMetadataRecord rec;
    rec.preset = col_str(0);
    rec.sample_encoding_preset = col_str(1);
    rec.signal_state_preset = col_str(2);
    rec.signal_type = col_str(3);

    if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
      rec.number_of_sequential_frames = sqlite3_column_int(stmt, 4);
    }

    if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
      rec.ntsc_j_black_level = sqlite3_column_int(stmt, 5);
    }

    if (has_provenance_columns) {
      rec.decoder = col_str(6);
      rec.git_branch = col_str(7);
      rec.git_commit = col_str(8);
      if (sqlite3_column_type(stmt, 9) != SQLITE_NULL) {
        rec.sequence_continuous = (sqlite3_column_int(stmt, 9) != 0);
      }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rec;
  }

  std::optional<size_t> get_input_word_count(
      const std::string& input_path,
      std::string& error_message) const override {
    std::ifstream ifs(input_path, std::ios::binary);
    if (!ifs.is_open()) {
      error_message = "Failed to open '" + input_path + "'";
      return std::nullopt;
    }
    ifs.seekg(0, std::ios::end);
    const std::streamoff sz = ifs.tellg();
    if (sz <= 0) {
      error_message = "Empty file: '" + input_path + "'";
      return std::nullopt;
    }
    if ((sz % 2) != 0) {
      error_message = "File size not 16-bit aligned: '" + input_path + "'";
      return std::nullopt;
    }
    return static_cast<size_t>(sz / 2);
  }

  bool read_input_words_at(const std::string& input_path, size_t word_offset,
                           size_t word_count, std::vector<uint16_t>& out_words,
                           std::string& error_message) const override {
    std::ifstream ifs(input_path, std::ios::binary);
    if (!ifs.is_open()) {
      error_message = "Failed to open '" + input_path + "'";
      return false;
    }
    ifs.seekg(static_cast<std::streamoff>(word_offset) * 2, std::ios::beg);
    if (!ifs.good()) {
      error_message = "Seek failed in '" + input_path + "'";
      return false;
    }
    out_words.resize(word_count);
    ifs.read(reinterpret_cast<char*>(out_words.data()),
             static_cast<std::streamsize>(word_count * 2));
    if (!ifs.good() && !ifs.eof()) {
      error_message = "Read failed from '" + input_path + "'";
      return false;
    }
    const size_t words_read = static_cast<size_t>(ifs.gcount()) / 2;
    if (words_read < word_count) out_words.resize(words_read);
    return true;
  }

  std::vector<DropoutRun> load_dropout_sidecar(
      const std::string& dropout_meta_path,
      std::string& error_message) const override {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(dropout_meta_path, ec)) return {};

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dropout_meta_path.c_str(), &db, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
      error_message =
          "Failed to open dropout sidecar '" + dropout_meta_path + "'";
      if (db) sqlite3_close(db);
      return {};
    }

    constexpr const char* kSql =
        "SELECT frame_id, sample_start, sample_count, severity "
        "FROM dropout_run ORDER BY frame_id, sample_start";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
      error_message =
          "Failed to query dropout_run from '" + dropout_meta_path + "'";
      sqlite3_close(db);
      return {};
    }

    std::vector<DropoutRun> runs;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      DropoutRun run;
      run.frame_id = static_cast<FrameID>(sqlite3_column_int64(stmt, 0));
      run.sample_start = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
      run.sample_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 2));
      run.severity = static_cast<uint8_t>(sqlite3_column_int(stmt, 3));
      runs.push_back(run);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return runs;
  }

  std::optional<CVBSAudioWavInfo> read_audio_wav_info(
      const std::string& wav_path) const override {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(wav_path, ec)) return std::nullopt;

    // Canonical 44-byte RIFF/WAVE header (the only form the CVBS file
    // format spec v1.4.0 permits: no extended fmt chunks or non-standard
    // RIFF variants).  A malformed header yields zeroed fields, which the
    // stage rejects during validation.
    std::ifstream ifs(wav_path, std::ios::binary);
    if (!ifs.is_open()) return CVBSAudioWavInfo{};
    uint8_t h[44] = {};
    ifs.read(reinterpret_cast<char*>(h), sizeof(h));
    if (static_cast<size_t>(ifs.gcount()) < sizeof(h)) {
      return CVBSAudioWavInfo{};
    }
    const auto le16 = [&h](size_t off) {
      return static_cast<uint16_t>(h[off]) |
             (static_cast<uint16_t>(h[off + 1]) << 8);
    };
    const auto le32 = [&h](size_t off) {
      return static_cast<uint32_t>(h[off]) |
             (static_cast<uint32_t>(h[off + 1]) << 8) |
             (static_cast<uint32_t>(h[off + 2]) << 16) |
             (static_cast<uint32_t>(h[off + 3]) << 24);
    };
    if (std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0 ||
        std::memcmp(h + 12, "fmt ", 4) != 0 ||
        std::memcmp(h + 36, "data", 4) != 0) {
      return CVBSAudioWavInfo{};
    }
    CVBSAudioWavInfo info;
    info.format_tag = le16(20);
    info.channels = le16(22);
    info.sample_rate_hz = le32(24);
    info.bits_per_sample = le16(34);
    info.data_bytes = le32(40);
    return info;
  }

  std::optional<std::vector<CVBSAudioChannelPairRecord>>
  load_audio_channel_pair_table(const std::string& meta_path,
                                std::string& error_message) const override {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(meta_path, ec)) return std::nullopt;

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(meta_path.c_str(), &db, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
      error_message = "Failed to open metadata '" + meta_path + "'";
      if (db) sqlite3_close(db);
      return std::nullopt;
    }

    // CVBS file format spec v1.4.0: one audio_channel_pair row per channel
    // pair file.
    constexpr const char* kSql =
        "SELECT channel_pair, description FROM audio_channel_pair "
        "ORDER BY channel_pair";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
      // Table absent — pre-v1.3.0 metadata, reported by the stage when
      // audio files are present.
      sqlite3_close(db);
      return std::nullopt;
    }

    std::vector<CVBSAudioChannelPairRecord> records;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      CVBSAudioChannelPairRecord rec;
      rec.channel_pair = sqlite3_column_int(stmt, 0);
      if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
        const unsigned char* v = sqlite3_column_text(stmt, 1);
        if (v) rec.description = reinterpret_cast<const char*>(v);
      }
      records.push_back(std::move(rec));
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return records;
  }

  std::vector<int32_t> read_audio_pairs_at(
      const std::string& wav_path, uint64_t stereo_pair_offset,
      size_t stereo_pair_count) const override {
    // WAV PCM: skip the canonical 44-byte RIFF header, then read
    // interleaved 24-bit signed LE stereo pairs (6 bytes each) and
    // sign-extend into the pipeline's 24-bit-in-int32 carrier.
    constexpr uint64_t kWavHeaderBytes = 44;
    std::ifstream ifs(wav_path, std::ios::binary);
    if (!ifs.is_open()) return {};
    ifs.seekg(static_cast<std::streamoff>(
                  kWavHeaderBytes + stereo_pair_offset * kAudioBytesPerPair),
              std::ios::beg);
    if (!ifs.good()) return {};
    std::vector<uint8_t> raw(stereo_pair_count * kAudioBytesPerPair);
    ifs.read(reinterpret_cast<char*>(raw.data()),
             static_cast<std::streamsize>(raw.size()));
    const size_t bytes_read = static_cast<size_t>(ifs.gcount());
    const size_t values_read = bytes_read / 3;
    std::vector<int32_t> samples;
    samples.reserve(values_read);
    for (size_t i = 0; i < values_read; ++i) {
      int32_t v = static_cast<int32_t>(raw[i * 3]) |
                  (static_cast<int32_t>(raw[i * 3 + 1]) << 8) |
                  (static_cast<int32_t>(raw[i * 3 + 2]) << 16);
      if (v & 0x800000) v |= ~0xFFFFFF;  // sign-extend from bit 23
      samples.push_back(v);
    }
    return samples;
  }

  std::optional<std::vector<CVBSExtensionFrameRef>> load_efm_frame_table(
      const std::string& efm_meta_path,
      std::string& error_message) const override {
    return load_extension_frame_table(efm_meta_path, "efm_frame",
                                      error_message);
  }

  std::vector<uint8_t> read_efm_bytes_at(const std::string& efm_data_path,
                                         uint64_t byte_offset,
                                         uint32_t count) const override {
    return read_binary_at(efm_data_path, byte_offset, count);
  }

  std::optional<std::vector<CVBSExtensionFrameRef>> load_ac3_frame_table(
      const std::string& ac3_meta_path,
      std::string& error_message) const override {
    return load_extension_frame_table(ac3_meta_path, "ac3_frame",
                                      error_message);
  }

  std::vector<uint8_t> read_ac3_bytes_at(const std::string& ac3_data_path,
                                         uint64_t byte_offset,
                                         uint32_t count) const override {
    return read_binary_at(ac3_data_path, byte_offset, count);
  }

 private:
  // Shared table-loader for EFM and AC3 sidecars.
  // CVBS EFM extension format §3 / AC3 extension format §3.
  std::optional<std::vector<CVBSExtensionFrameRef>> load_extension_frame_table(
      const std::string& meta_path, const std::string& table_name,
      std::string& error_message) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(meta_path, ec)) return std::nullopt;

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(meta_path.c_str(), &db, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
      error_message = "Failed to open '" + meta_path + "'";
      if (db) sqlite3_close(db);
      return std::nullopt;
    }

    const std::string sql = "SELECT t_value_offset, t_value_count FROM " +
                            table_name + " ORDER BY frame_id";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      error_message =
          "Failed to query " + table_name + " from '" + meta_path + "'";
      sqlite3_close(db);
      return std::nullopt;
    }

    std::vector<CVBSExtensionFrameRef> table;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      CVBSExtensionFrameRef ref;
      ref.offset = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
      ref.count = static_cast<uint32_t>(sqlite3_column_int(stmt, 1));
      table.push_back(ref);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return table;
  }

  std::vector<uint8_t> read_binary_at(const std::string& path,
                                      uint64_t byte_offset,
                                      uint32_t count) const {
    if (count == 0) return {};
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return {};
    ifs.seekg(static_cast<std::streamoff>(byte_offset), std::ios::beg);
    if (!ifs.good()) return {};
    std::vector<uint8_t> buf(count);
    ifs.read(reinterpret_cast<char*>(buf.data()),
             static_cast<std::streamsize>(count));
    const size_t bytes_read = static_cast<size_t>(ifs.gcount());
    if (bytes_read < count) buf.resize(bytes_read);
    return buf;
  }
};

// ---------------------------------------------------------------------------
// Stage identity constants
// ---------------------------------------------------------------------------

struct CVBSStageStaticInfo {
  const char* stage_name;
  const char* initial_display_name;  // set before any file is loaded
  const char* description;
  VideoFormatCompatibility compatible_formats;
  VideoSystem system;
};

constexpr CVBSStageStaticInfo kPALInfo{
    "PAL_CVBS_Source",
    "CVBS Source",
    "PAL CVBS source - loads PAL 4FSC CVBS files",
    VideoFormatCompatibility::PAL_ONLY,
    VideoSystem::PAL,
};

constexpr CVBSStageStaticInfo kNTSCInfo{
    "NTSC_CVBS_Source",
    "CVBS Source",
    "NTSC CVBS source - loads NTSC 4FSC CVBS files",
    VideoFormatCompatibility::NTSC_ONLY,
    VideoSystem::NTSC,
};

constexpr CVBSStageStaticInfo kPALMInfo{
    "PAL_M_CVBS_Source",
    "CVBS Source",
    "PAL-M CVBS source - loads PAL-M 4FSC CVBS files",
    VideoFormatCompatibility::PAL_M_ONLY,
    VideoSystem::PAL_M,
};

// Return the video-system name for logging and display.
const char* system_name(VideoSystem sys) {
  switch (sys) {
    case VideoSystem::PAL:
      return "PAL";
    case VideoSystem::NTSC:
      return "NTSC";
    case VideoSystem::PAL_M:
      return "PAL_M";
    default:
      return "Unknown";
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// FixedFormatCVBSSourceStage
// ---------------------------------------------------------------------------

FixedFormatCVBSSourceStage::FixedFormatCVBSSourceStage(
    const char* stage_name, const char* fixed_display_name,
    const char* description, VideoFormatCompatibility compatible_formats,
    VideoSystem system, std::shared_ptr<ICVBSSourceStageDeps> deps)
    : system_(system),
      stage_name_(stage_name),
      display_name_(fixed_display_name),
      description_(description),
      compatible_formats_(compatible_formats),
      deps_(deps ? std::move(deps) : std::make_shared<CVBSSourceStageDeps>()) {
  set_configuration_status(orc::ConfigurationStatus::Red);
}

std::vector<ArtifactPtr> FixedFormatCVBSSourceStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  std::lock_guard<std::mutex> lock(execute_mutex_);

  if (!inputs.empty()) {
    throw std::runtime_error(std::string(stage_name_) +
                             ": source stage expects no inputs");
  }

  // Detect mode: dual-file YC (y_path + c_path) vs single-file composite.
  auto get_str_param = [&](const char* key) -> std::string {
    auto it = parameters.find(key);
    if (it == parameters.end()) return {};
    const auto* s = std::get_if<std::string>(&it->second);
    return s ? *s : std::string{};
  };

  const std::string y_path = get_str_param("y_path");
  const std::string c_path = get_str_param("c_path");
  const std::string single_path = get_str_param("input_path");
  const std::string manual_encoding = get_str_param("sample_encoding");

  const bool is_yc = !y_path.empty() && !c_path.empty();
  const std::string input_path = is_yc ? y_path : single_path;

  // Metadata mode (default): read the encoding from the .meta sidecar.
  // Manual mode: the user selected an explicit encoding, making the .meta
  // sidecar optional (CVBS file format spec: metadata is optional).
  const bool use_metadata =
      manual_encoding.empty() || manual_encoding == kSampleEncodingFromMetadata;

  if (input_path.empty()) {
    ORC_LOG_DEBUG("{}: no input configured", stage_name_);
    return {};
  }

  const std::string cache_key =
      (is_yc ? (y_path + "|" + c_path) : input_path) + "|" +
      (use_metadata ? std::string("meta") : manual_encoding);
  if (cached_representation_ && cached_input_path_ == cache_key) {
    return {cached_representation_};
  }

  // --- Validate input file(s) ---
  std::string err;
  if (!deps_->validate_input_file(input_path, err)) {
    throw UserDataError(err);
  }
  if (is_yc) {
    std::string c_err;
    if (!deps_->validate_input_file(c_path, c_err)) {
      throw UserDataError(c_err);
    }
  }

  // --- Determine sample encoding and per-capture metadata ---
  std::string encoding;
  std::string signal_type;
  int32_t meta_frame_count = 0;
  std::optional<int32_t> ntsc_j_black_level;
  std::string meta_decoder;
  std::string meta_git_branch;
  std::string meta_git_commit;

  if (use_metadata) {
    // --- Load and validate metadata ---
    const std::string meta_path = derive_sidecar_path(input_path, ".meta");
    std::string meta_err;
    const auto meta_opt = deps_->load_metadata(meta_path, meta_err);
    if (!meta_opt) {
      throw UserDataError("Failed to load CVBS metadata from '" + meta_path +
                          "': " + meta_err);
    }
    const CVBSMetadataRecord& meta = *meta_opt;

    // Signal state: the stage needs standard-rate, time-base-stable samples
    // because its frame geometry is fixed at 4fsc.  The phase-lock axis is a
    // different matter: colour-sequence phase is measured from the burst by
    // the "colour_frame_phase" observer rather than taken on trust from the
    // sidecar, so an unlocked file decodes correctly.  Accept both stable
    // states at the standard rate and reject the rest (including pre-v1.6.0
    // TBC-named presets — the spec forbids them in a v1.6.0 file).
    if (meta.signal_state_preset != "STANDARD_STABLE_LOCKED" &&
        meta.signal_state_preset != "STANDARD_STABLE_UNLOCKED") {
      throw UserDataError(
          "CVBS source '" + input_path + "' has signal_state_preset '" +
          meta.signal_state_preset +
          "'. Only STANDARD_STABLE_LOCKED and STANDARD_STABLE_UNLOCKED files "
          "are accepted: the stage requires time-base-stable samples at the "
          "standard 4fsc rate (CVBS file format spec v1.6.0).");
    }
    // Continuity is declared by sequence_continuous (CVBS file format spec
    // v1.6.0), not by the preset: a discontinuity (typically a disc skip
    // during decode) leaves every frame stable and phase locked.
    if (meta.sequence_continuous.has_value() && !*meta.sequence_continuous) {
      ORC_LOG_WARN(
          "CVBS source '{}' is marked sequence_continuous = FALSE: the "
          "stored content contains at least one discontinuity (typically a "
          "disc skip during decode). Decoding is unaffected, but run disc "
          "mapping before exporting if a continuous sequence is wanted.",
          input_path);
    }

    // Validate that the .meta video standard matches this stage's fixed
    // system.
    if (meta.preset != std::string(system_name(system_))) {
      throw UserDataError("CVBS metadata preset '" + meta.preset + "' in '" +
                          meta_path + "' does not match this stage's system (" +
                          video_system_to_string(system_) + ")");
    }

    // Accept all four declared sample encodings.
    if (!is_supported_encoding(meta.sample_encoding_preset)) {
      throw UserDataError("Unsupported sample_encoding_preset '" +
                          meta.sample_encoding_preset + "' in '" + meta_path +
                          "'");
    }

    encoding = meta.sample_encoding_preset;
    signal_type = meta.signal_type;
    meta_frame_count = meta.number_of_sequential_frames;
    ntsc_j_black_level = meta.ntsc_j_black_level;
    meta_decoder = meta.decoder;
    meta_git_branch = meta.git_branch;
    meta_git_commit = meta.git_commit;
  } else {
    // Manual mode: no .meta sidecar required. The video standard is fixed by
    // the stage choice and the signal is assumed STANDARD_STABLE_LOCKED; the
    // frame count is measured from the payload size.
    if (!is_supported_encoding(manual_encoding)) {
      throw UserDataError("Unsupported sample encoding '" + manual_encoding +
                          "' selected for '" + input_path + "'");
    }
    encoding = manual_encoding;
    signal_type = is_yc ? "yc" : "composite";
  }

  // --- Determine frame geometry ---
  const int32_t frame_samples = frame_samples_from_system(system_);

  if (frame_samples == 0) {
    throw std::runtime_error(std::string(stage_name_) +
                             ": unknown video system");
  }

  std::string wc_err;
  const auto wc_opt = deps_->get_input_word_count(input_path, wc_err);
  if (!wc_opt) {
    throw UserDataError("Failed to measure CVBS payload '" + input_path +
                        "': " + wc_err);
  }
  const size_t total_words = *wc_opt;

  // Prefer number_of_sequential_frames from metadata; fall back to measured.
  int32_t frame_count = 0;
  if (meta_frame_count > 0) {
    frame_count = meta_frame_count;
    // Sanity-check that the file is large enough.
    const size_t expected =
        static_cast<size_t>(frame_count) * static_cast<size_t>(frame_samples);
    if (total_words < expected) {
      throw UserDataError("CVBS payload '" + input_path + "' has " +
                          std::to_string(total_words) +
                          " words but metadata declares " +
                          std::to_string(frame_count) + " frames (" +
                          std::to_string(expected) + " words required)");
    }
  } else {
    frame_count =
        static_cast<int32_t>(total_words / static_cast<size_t>(frame_samples));
  }

  if (frame_count == 0) {
    throw UserDataError("CVBS payload '" + input_path +
                        "' is too short for one complete " +
                        video_system_to_string(system_) + " frame");
  }

  const size_t trailing = total_words - static_cast<size_t>(frame_count) *
                                            static_cast<size_t>(frame_samples);
  if (trailing > 0) {
    ORC_LOG_WARN("{}: {} trailing samples in '{}' (not a complete frame)",
                 stage_name_, trailing, input_path);
  }

  // --- Build SourceParameters from spec constants ---
  const int32_t ntsc_j = ntsc_j_black_level.value_or(-1);
  SourceParameters src_params =
      build_source_parameters(system_, frame_count, ntsc_j);
  src_params.decoder = meta_decoder;
  src_params.git_branch = meta_git_branch;
  src_params.git_commit = meta_git_commit;
  if (is_yc) {
    // CVBS file format spec §3.1: for all YC encoding presets, after
    // normalize_to_cvbs_u10(), chroma zero (DC) maps to 512 in the
    // CVBS_U10_4FSC 10-bit domain.  The Y+C composite view subtracts this
    // offset before adding luma and chroma so the result sits at the
    // correct blanking level.
    src_params.chroma_dc_offset = 512;
  }

  // --- Load sidecars ---

  // Dropout
  std::string do_err;
  const std::string dropout_path =
      derive_sidecar_path(input_path, ".dropouts.meta");
  auto dropout_runs = deps_->load_dropout_sidecar(dropout_path, do_err);

  // Audio: probe <basename>_audio_0.wav … _audio_7.wav (single-digit
  // suffix, CVBS file format spec v1.4.0).  Each existing file becomes the
  // pipeline channel pair with the same index; container numbers need not
  // be contiguous, so absent intermediate numbers become placeholder pairs
  // serving cadence-sized silence.  Every file's RIFF header is validated
  // against the spec (PCM, 2 channels, 48000 Hz, 24-bit) — mismatches are
  // errors.  Per-pair descriptions come from the .meta audio_channel_pair
  // table; existing files without a table row (a spec violation) produce a
  // warning observation and derive a "Channel pair N" name, as does
  // manual-encoding mode, where no metadata is read.  The CVBS metadata
  // carries no origin information, so every pair reports
  // AudioOrigin::UNKNOWN.
  std::optional<std::vector<CVBSAudioChannelPairRecord>> audio_pair_table;
  if (use_metadata) {
    std::string at_err;
    audio_pair_table = deps_->load_audio_channel_pair_table(
        derive_sidecar_path(input_path, ".meta"), at_err);
  }

  // CVBS file format spec v1.4.0: every channel pair file carries exactly
  // audio_pair_offset(frame_count) stereo pairs (equal-length files).
  const uint64_t expected_stream_pairs =
      audio_pair_offset(static_cast<uint64_t>(frame_count), system_);

  std::vector<CVBSAudioChannelPairState> audio_pairs;
  for (int32_t nn = 0; nn < static_cast<int32_t>(kMaxAudioChannelPairs); ++nn) {
    const std::string pair_wav_path = derive_sidecar_path(
        input_path, "_audio_" + std::to_string(nn) + ".wav");
    const auto wav_info = deps_->read_audio_wav_info(pair_wav_path);
    if (!wav_info) continue;  // file absent

    // CVBS file format spec v1.4.0 WAV File Format: PCM, 2 channels,
    // 48000 Hz, 24-bit signed LE — the only permitted audio format.
    if (wav_info->format_tag != 1 || wav_info->channels != 2 ||
        wav_info->sample_rate_hz != kAudioSampleRateHz ||
        wav_info->bits_per_sample != kAudioBitDepth) {
      throw UserDataError(
          "CVBS audio sidecar '" + pair_wav_path +
          "' is not a valid CVBS channel pair file (requires PCM, 2 "
          "channels, 48000 Hz, 24-bit; found format tag " +
          std::to_string(wav_info->format_tag) + ", " +
          std::to_string(wav_info->channels) + " channel(s), " +
          std::to_string(wav_info->sample_rate_hz) + " Hz, " +
          std::to_string(wav_info->bits_per_sample) + "-bit)");
    }

    // Equal-length check: a payload that does not carry exactly one 6-byte
    // stereo pair per cadence position is a spec violation; short frames
    // are silence-padded at read time.
    const uint64_t actual_stream_pairs =
        wav_info->data_bytes / kAudioBytesPerPair;
    if (actual_stream_pairs != expected_stream_pairs ||
        (wav_info->data_bytes % kAudioBytesPerPair) != 0) {
      const std::string message =
          "audio channel pair file '" + pair_wav_path + "' carries " +
          std::to_string(actual_stream_pairs) + " stereo pairs but " +
          std::to_string(expected_stream_pairs) + " are required for " +
          std::to_string(frame_count) +
          " frames (CVBS spec: all pair files are equal-length); short "
          "frames are served as silence";
      observation_context.set(FieldID(0), "cvbs_source",
                              "audio_length_mismatch_" + std::to_string(nn),
                              message);
      ORC_LOG_WARN("{}: {}", stage_name_, message);
    }

    // Fill any container-numbering gap with placeholder (silent) pairs so
    // pipeline pair indices match container pair numbers.
    while (static_cast<int32_t>(audio_pairs.size()) < nn) {
      CVBSAudioChannelPairState placeholder;
      placeholder.container_number = static_cast<int32_t>(audio_pairs.size());
      placeholder.descriptor.name =
          "Channel pair " + std::to_string(placeholder.container_number);
      placeholder.descriptor.origin = AudioOrigin::UNKNOWN;
      audio_pairs.push_back(std::move(placeholder));
    }

    CVBSAudioChannelPairState state;
    state.container_number = nn;
    state.wav_path = pair_wav_path;

    // Container-declared descriptor.  Default (no audio_channel_pair row):
    // name derived from the channel pair number, per the spec's guidance
    // for NULL descriptions.
    state.descriptor.name = "Channel pair " + std::to_string(nn);
    state.descriptor.origin = AudioOrigin::UNKNOWN;
    bool has_row = false;
    if (audio_pair_table) {
      const auto rec =
          std::find_if(audio_pair_table->begin(), audio_pair_table->end(),
                       [nn](const CVBSAudioChannelPairRecord& r) {
                         return r.channel_pair == nn;
                       });
      if (rec != audio_pair_table->end()) {
        has_row = true;
        const std::string description = rec->description.value_or("");
        if (!description.empty()) {
          state.descriptor.name = description;
        }
      }
    }
    if (use_metadata && !has_row) {
      // CVBS file format spec v1.4.0: every channel pair file must have an
      // audio_channel_pair row.
      const std::string message =
          "audio channel pair file '" + pair_wav_path +
          "' has no audio_channel_pair metadata row (spec violation); "
          "using derived name '" +
          state.descriptor.name + "'";
      observation_context.set(
          FieldID(0), "cvbs_source",
          "audio_missing_metadata_row_" + std::to_string(nn), message);
      ORC_LOG_WARN("{}: {}", stage_name_, message);
    }

    audio_pairs.push_back(std::move(state));
  }

  // EFM
  const std::string efm_meta_path =
      derive_sidecar_path(input_path, ".efm.meta");
  const std::string efm_data_path = derive_sidecar_path(input_path, ".efm");
  std::string efm_err;
  const auto efm_table_opt =
      deps_->load_efm_frame_table(efm_meta_path, efm_err);
  const bool has_efm = efm_table_opt.has_value();
  std::vector<CVBSExtensionFrameRef> efm_table =
      has_efm ? *efm_table_opt : std::vector<CVBSExtensionFrameRef>{};

  // AC3
  const std::string ac3_meta_path =
      derive_sidecar_path(input_path, ".ac3.meta");
  const std::string ac3_data_path = derive_sidecar_path(input_path, ".ac3");
  std::string ac3_err;
  const auto ac3_table_opt =
      deps_->load_ac3_frame_table(ac3_meta_path, ac3_err);
  const bool has_ac3 = ac3_table_opt.has_value();
  std::vector<CVBSExtensionFrameRef> ac3_table =
      has_ac3 ? *ac3_table_opt : std::vector<CVBSExtensionFrameRef>{};

  // --- Display name update ---
  const std::string signal_type_display =
      (signal_type == "yc") ? "YC" : "Composite";
  display_name_ =
      video_system_to_string(system_) + " CVBS " + signal_type_display;

  ORC_LOG_INFO(
      "{}: loaded '{}' — {} {} frames, encoding {}, {} audio channel "
      "pair(s), EFM {}, AC3 {}",
      stage_name_, input_path, frame_count, video_system_to_string(system_),
      encoding, audio_pairs.size(), has_efm ? "yes" : "no",
      has_ac3 ? "yes" : "no");

  // --- Frame geometry parameters ---
  const int32_t frame_height_lines = frame_lines_from_system(system_);

  // --- Provenance ---
  Provenance prov;
  prov.stage_name = stage_name_;
  prov.stage_version = version();
  prov.parameters = {
      {"input_path", input_path},
      {"video_system", system_name(system_)},
      {"sample_encoding", encoding},
  };

  auto representation = std::make_shared<CVBSDecodedFrameRepresentation>(
      system_, frame_count, frame_samples, frame_height_lines,
      src_params.frame_width_nominal, deps_, input_path, encoding, src_params,
      ntsc_j_black_level, std::move(dropout_runs), std::move(audio_pairs),
      has_efm, efm_data_path, std::move(efm_table), has_ac3, ac3_data_path,
      std::move(ac3_table), is_yc ? c_path : std::string{},
      ArtifactID(std::string(stage_name_) + ":" + cache_key), std::move(prov));

  cached_representation_ = representation;
  cached_input_path_ = cache_key;
  input_path_ = input_path;
  return {representation};
}

std::vector<ParameterDescriptor>
FixedFormatCVBSSourceStage::get_parameter_descriptors(
    VideoSystem /*project_format*/, SourceType source_type) const {
  std::vector<ParameterDescriptor> desc;

  // A project is either composite or Y/C, never both — only offer the file
  // parameters that match the project's source type. Unknown (no project
  // context) falls back to offering all of them.
  const bool show_composite = (source_type != SourceType::YC);
  const bool show_yc = (source_type != SourceType::Composite);

  if (show_composite) {
    ParameterDescriptor pd;
    pd.name = "input_path";
    pd.display_name = "CVBS File Path";
    pd.description =
        "Path to the CVBS composite data file (.cvbs). "
        "A <basename>.meta sidecar is required unless a sample encoding is "
        "selected manually.";
    pd.type = ParameterType::FILE_PATH;
    pd.constraints.required = false;
    pd.constraints.default_value = std::string("");
    pd.file_extension_hint = ".cvbs";
    desc.push_back(pd);
  }

  if (show_yc) {
    {
      ParameterDescriptor pd;
      pd.name = "y_path";
      pd.display_name = "CVBS Y (Luma) File Path";
      pd.description =
          "Path to the CVBS luma channel file (.cvbsy) for YC sources. "
          "Set together with c_path; a shared <basename>.meta sidecar is "
          "required unless a sample encoding is selected manually.";
      pd.type = ParameterType::FILE_PATH;
      pd.constraints.required = false;
      pd.constraints.default_value = std::string("");
      pd.file_extension_hint = ".cvbsy";
      desc.push_back(pd);
    }

    {
      ParameterDescriptor pd;
      pd.name = "c_path";
      pd.display_name = "CVBS C (Chroma) File Path";
      pd.description =
          "Path to the CVBS chroma channel file (.cvbsc) for YC sources. "
          "Set together with y_path.";
      pd.type = ParameterType::FILE_PATH;
      pd.constraints.required = false;
      pd.constraints.default_value = std::string("");
      pd.file_extension_hint = ".cvbsc";
      desc.push_back(pd);
    }
  }

  {
    ParameterDescriptor pd;
    pd.name = "sample_encoding";
    pd.display_name = "Sample Encoding";
    pd.description =
        "Sample encoding of the CVBS data. 'From metadata' (default) reads "
        "the encoding from the <basename>.meta sidecar. Selecting an "
        "encoding manually makes the sidecar optional, allowing CVBS "
        "sources without metadata to be used; the signal is then assumed "
        "to be TBC-locked and the frame count is measured from the file "
        "size.";
    pd.type = ParameterType::STRING;
    pd.constraints.required = false;
    pd.constraints.default_value = std::string(kSampleEncodingFromMetadata);
    pd.constraints.allowed_strings = {kSampleEncodingFromMetadata};
    for (const char* e : kSupportedEncodings) {
      pd.constraints.allowed_strings.push_back(e);
    }
    desc.push_back(pd);
  }

  return desc;
}

std::map<std::string, ParameterValue>
FixedFormatCVBSSourceStage::get_parameters() const {
  return {{"input_path", input_path_},
          {"y_path", y_path_},
          {"c_path", c_path_},
          {"sample_encoding", sample_encoding_}};
}

bool FixedFormatCVBSSourceStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key == "input_path") {
      input_path_ = std::get<std::string>(value);
    } else if (key == "y_path") {
      y_path_ = std::get<std::string>(value);
    } else if (key == "c_path") {
      c_path_ = std::get<std::string>(value);
    } else if (key == "sample_encoding") {
      sample_encoding_ = std::get<std::string>(value);
    } else {
      ORC_LOG_WARN("{}: unknown parameter '{}'", stage_name_, key);
      return false;
    }
  }

  // Determine the primary path for validation (y_path in YC mode, else
  // input_path).
  const bool is_yc = !y_path_.empty() && !c_path_.empty();
  const std::string& primary = is_yc ? y_path_ : input_path_;

  if (primary.empty()) {
    set_configuration_status(orc::ConfigurationStatus::Red);
    return true;
  }

  // Validate the primary source file and its metadata so the status dot
  // reflects a format mismatch immediately, rather than only at execute() time.
  std::string err;
  if (!deps_->validate_input_file(primary, err)) {
    ORC_LOG_WARN("{}: source file not accessible: {}", stage_name_, err);
    // The configured path does not point to a usable source file, so the
    // stage cannot produce any output; report Red rather than Yellow.
    set_configuration_status(orc::ConfigurationStatus::Red);
    return true;
  }

  // Manual sample encoding makes the .meta sidecar optional; the stage can
  // run on the payload file alone.
  const bool use_metadata = sample_encoding_.empty() ||
                            sample_encoding_ == kSampleEncodingFromMetadata;
  if (!use_metadata) {
    set_configuration_status(orc::ConfigurationStatus::Green);
    return true;
  }

  namespace fs = std::filesystem;
  const fs::path p(primary);
  const std::string meta_path = (p.parent_path() / p.stem()).string() + ".meta";

  std::string meta_err;
  const auto meta_opt = deps_->load_metadata(meta_path, meta_err);
  if (!meta_opt) {
    ORC_LOG_WARN("{}: metadata not accessible: {}", stage_name_, meta_err);
    set_configuration_status(orc::ConfigurationStatus::Yellow);
    return true;
  }

  if (meta_opt->preset != std::string(system_name(system_))) {
    ORC_LOG_WARN("{}: source file format '{}' does not match stage system '{}'",
                 stage_name_, meta_opt->preset, system_name(system_));
    set_configuration_status(orc::ConfigurationStatus::Red);
    return true;
  }

  set_configuration_status(orc::ConfigurationStatus::Green);
  return true;
}

StagePreviewCapability FixedFormatCVBSSourceStage::get_preview_capability()
    const {
  return PreviewHelpers::make_signal_preview_capability(
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(
          cached_representation_));
}

// ---------------------------------------------------------------------------
// Concrete stage constructors
// ---------------------------------------------------------------------------

PALCVBSSourceStage::PALCVBSSourceStage(
    std::shared_ptr<ICVBSSourceStageDeps> deps)
    : FixedFormatCVBSSourceStage(
          kPALInfo.stage_name, kPALInfo.initial_display_name,
          kPALInfo.description, kPALInfo.compatible_formats, kPALInfo.system,
          std::move(deps)) {}

NTSCCVBSSourceStage::NTSCCVBSSourceStage(
    std::shared_ptr<ICVBSSourceStageDeps> deps)
    : FixedFormatCVBSSourceStage(
          kNTSCInfo.stage_name, kNTSCInfo.initial_display_name,
          kNTSCInfo.description, kNTSCInfo.compatible_formats, kNTSCInfo.system,
          std::move(deps)) {}

PALMCVBSSourceStage::PALMCVBSSourceStage(
    std::shared_ptr<ICVBSSourceStageDeps> deps)
    : FixedFormatCVBSSourceStage(
          kPALMInfo.stage_name, kPALMInfo.initial_display_name,
          kPALMInfo.description, kPALMInfo.compatible_formats, kPALMInfo.system,
          std::move(deps)) {}

}  // namespace orc
