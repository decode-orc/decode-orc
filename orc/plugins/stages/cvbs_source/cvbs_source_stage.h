/*
 * File:        cvbs_source_stage.h
 * Module:      orc-core
 * Purpose:     CVBS (Composite Video Baseband Signal) source loading stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef CVBS_SOURCE_STAGE_H
#define CVBS_SOURCE_STAGE_H

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/dropout/dropout_run.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace orc {

// Per-frame EFM / AC3 byte-range descriptor.
struct CVBSExtensionFrameRef {
  uint64_t offset = 0;  // byte offset into the binary data file
  uint32_t count = 0;   // number of bytes for this frame
};

// Extended per-capture metadata read from <basename>.meta.
struct CVBSMetadataRecord {
  std::string preset;                  // video_standard_preset (PAL/NTSC/PAL_M)
  std::string sample_encoding_preset;  // CVBS_U10_4FSC / CVBS_U16_4FSC / etc.
  std::string signal_state_preset;     // STANDARD_TBC_LOCKED/_UNLOCKED
  std::string signal_type;             // composite / yc
  int32_t number_of_sequential_frames = 0;
  // NTSC-J only: explicit black level stored in the 10-bit domain.
  std::optional<int32_t> ntsc_j_black_level;
  // Upstream decode provenance (cvbs_file.decoder / git_branch / git_commit,
  // CVBS file format spec).  Empty when the columns are NULL or absent.
  std::string decoder;
  std::string git_branch;
  std::string git_commit;
};

// One row of the .meta audio_channel_pair table (CVBS file format spec
// v1.4.0).
struct CVBSAudioChannelPairRecord {
  int32_t channel_pair = 0;  // 0–7, matches the _audio_<p>.wav suffix
  std::optional<std::string> description;  // human-readable; nullopt = NULL
};

// RIFF/WAVE fmt-chunk properties and data-chunk size of an audio channel
// pair sidecar, read for validation against the CVBS file format spec
// v1.4.0 requirements (PCM, 2 channels, 48000 Hz, 24-bit signed LE).
struct CVBSAudioWavInfo {
  uint16_t format_tag = 0;       // wFormatTag; 1 = PCM
  uint16_t channels = 0;         // nChannels
  uint32_t sample_rate_hz = 0;   // nSamplesPerSec
  uint16_t bits_per_sample = 0;  // wBitsPerSample
  uint64_t data_bytes = 0;       // data chunk payload size
};

// Dependency injection interface for the CVBS source stage.
// All external I/O is accessed through this interface, allowing full mocking
// in unit tests.
class ICVBSSourceStageDeps {
 public:
  virtual ~ICVBSSourceStageDeps() = default;

  // Validate that input_path exists and is a readable regular file.
  virtual bool validate_input_file(const std::string& input_path,
                                   std::string& error_message) const = 0;

  // Open <basename>.meta and read the cvbs_file row.
  virtual std::optional<CVBSMetadataRecord> load_metadata(
      const std::string& meta_path, std::string& error_message) const = 0;

  // Return the total number of 16-bit words in the CVBS data file.
  virtual std::optional<size_t> get_input_word_count(
      const std::string& input_path, std::string& error_message) const = 0;

  // Read exactly word_count 16-bit words starting at word_offset.
  virtual bool read_input_words_at(const std::string& input_path,
                                   size_t word_offset, size_t word_count,
                                   std::vector<uint16_t>& out_words,
                                   std::string& error_message) const = 0;

  // Load all DropoutRun rows from <basename>.dropouts.meta.
  // Returns an empty vector (no error) when the file is absent.
  virtual std::vector<DropoutRun> load_dropout_sidecar(
      const std::string& dropout_meta_path,
      std::string& error_message) const = 0;

  // Read the WAV's RIFF fmt-chunk properties and data-chunk size for
  // validation.  Returns nullopt when the file is absent (no error) — this
  // doubles as the per-file existence probe for the
  // <basename>_audio_0.wav … _audio_7.wav enumeration.  A present but
  // malformed header returns zeroed fields, which then fail validation.
  virtual std::optional<CVBSAudioWavInfo> read_audio_wav_info(
      const std::string& wav_path) const = 0;

  // Load all rows of the audio_channel_pair table from <basename>.meta
  // (CVBS file format spec v1.4.0).  Returns nullopt when the metadata file
  // or the table is absent (no error; missing rows for existing audio files
  // are reported by the stage as a spec-violation warning).
  virtual std::optional<std::vector<CVBSAudioChannelPairRecord>>
  load_audio_channel_pair_table(const std::string& meta_path,
                                std::string& error_message) const = 0;

  // Read stereo_pair_count interleaved 24-bit signed LE stereo pairs
  // starting at stereo_pair_offset from the WAV data (header already
  // skipped), sign-extended into the pipeline's 24-bit-in-int32 carrier.
  // Short files return fewer values; callers pad with silence.
  virtual std::vector<int32_t> read_audio_pairs_at(
      const std::string& wav_path, uint64_t stereo_pair_offset,
      size_t stereo_pair_count) const = 0;

  // Load the efm_frame index table from <basename>.efm.meta.
  // Returns nullopt when either sidecar file is absent.
  virtual std::optional<std::vector<CVBSExtensionFrameRef>>
  load_efm_frame_table(const std::string& efm_meta_path,
                       std::string& error_message) const = 0;

  // Read count bytes at byte_offset from the <basename>.efm binary file.
  virtual std::vector<uint8_t> read_efm_bytes_at(
      const std::string& efm_data_path, uint64_t byte_offset,
      uint32_t count) const = 0;

  // Load the ac3_frame index table from <basename>.ac3.meta.
  // Returns nullopt when either sidecar file is absent.
  virtual std::optional<std::vector<CVBSExtensionFrameRef>>
  load_ac3_frame_table(const std::string& ac3_meta_path,
                       std::string& error_message) const = 0;

  // Read count bytes at byte_offset from the <basename>.ac3 binary file.
  virtual std::vector<uint8_t> read_ac3_bytes_at(
      const std::string& ac3_data_path, uint64_t byte_offset,
      uint32_t count) const = 0;
};

// Fixed-video-standard CVBS source stage base class.
//
// Each concrete subclass hard-wires a single video system and signal type.
// The stage loads the CVBS data file and its sidecars at execute() time and
// returns a CVBSDecodedFrameRepresentation satisfying VideoFrameRepresentation.
//
// Signal state: STANDARD_TBC_LOCKED and STANDARD_TBC_UNLOCKED are accepted;
// files in any other state are rejected with a clear UserDataError before any
// sample data is read, because the stage's frame geometry assumes
// time-base-corrected samples at the standard 4fsc rate.  Burst lock is not
// required: colour-sequence phase is measured from the burst downstream, never
// taken from the sidecar.  An unlocked file is accepted with a logged warning.
//
// Sample encoding: CVBS_U10_4FSC, CVBS_U16_4FSC, CVBS_TPG21_4FSC, and
// CVBS_S16_4FSC are all normalised to CVBS_U10_4FSC (int16_t 10-bit domain)
// without output clamping so that headroom is preserved.
//
// Parameters:
//   input_path       – path to the CVBS composite data file (.cvbs)
//   y_path           – path to the luma channel file (.cvbsy) for YC mode
//   c_path           – path to the chroma channel file (.cvbsc) for YC mode
//   sample_encoding  – "From metadata" (default) or an explicit encoding
//
// Audio: every <basename>_audio_0.wav … _audio_7.wav sidecar (single-digit
// suffix, CVBS file format spec v1.4.0) becomes the pipeline audio channel
// pair with the same index; container numbers need not be contiguous, and
// numbering is preserved by serving silence for absent intermediate pairs.
// Each file's RIFF header is validated against the spec (PCM, 2 channels,
// 48000 Hz, 24-bit); mismatches are errors.  Per-pair descriptions come
// from the .meta audio_channel_pair table; existing files without a table
// row (a spec violation) produce a warning observation and derive a
// "Channel pair N" name.  Payloads are already in the pipeline audio form,
// so per-frame reads seek directly by the SMPTE 272M cadence offset.
//
// YC mode is active when both y_path and c_path are non-empty; composite mode
// uses input_path.  The two modes are mutually exclusive; the parameter
// descriptors only offer the file fields matching the project's source type.
//
// Metadata: the CVBS file format spec declares the .meta sidecar optional.
// With sample_encoding at its default the sidecar is required and provides
// the encoding, frame count, signal state, and NTSC-J black level.  When an
// encoding is selected manually the sidecar is ignored: the video standard
// comes from the stage's fixed system, the signal is assumed TBC-locked, and
// the frame count is measured from the payload size.
class FixedFormatCVBSSourceStage : public DAGStage,
                                   public ParameterizedStage,
                                   public IStagePreviewCapability {
 public:
  explicit FixedFormatCVBSSourceStage(
      const char* stage_name, const char* fixed_display_name,
      const char* description, VideoFormatCompatibility compatible_formats,
      VideoSystem system, std::shared_ptr<ICVBSSourceStageDeps> deps = nullptr);
  ~FixedFormatCVBSSourceStage() override = default;

  void set_deps_override(std::shared_ptr<ICVBSSourceStageDeps> deps) {
    deps_ = std::move(deps);
  }

  // DAGStage interface
  std::string version() const override { return "2.2.0"; }
  ORC_STAGE_INSTRUCTIONS_MD

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::SOURCE,
                        stage_name_,
                        display_name_,
                        description_,
                        0,
                        0,
                        1,
                        UINT32_MAX,
                        compatible_formats_};
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }

  // ParameterizedStage interface
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;

  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

 protected:
  VideoSystem system_;

 private:
  const char* stage_name_;
  std::string display_name_;  // updated at load time with signal_type
  const char* description_;
  VideoFormatCompatibility compatible_formats_;

  std::string input_path_;       // composite / single-channel mode
  std::string y_path_;           // YC mode: luma path
  std::string c_path_;           // YC mode: chroma path
  std::string sample_encoding_;  // "From metadata" (default) or explicit

  mutable std::mutex execute_mutex_;
  mutable std::string cached_input_path_;
  mutable ArtifactPtr cached_representation_;

  std::shared_ptr<ICVBSSourceStageDeps> deps_;
};

class PALCVBSSourceStage final : public FixedFormatCVBSSourceStage {
 public:
  explicit PALCVBSSourceStage(
      std::shared_ptr<ICVBSSourceStageDeps> deps = nullptr);
  ~PALCVBSSourceStage() override = default;
};

class NTSCCVBSSourceStage final : public FixedFormatCVBSSourceStage {
 public:
  explicit NTSCCVBSSourceStage(
      std::shared_ptr<ICVBSSourceStageDeps> deps = nullptr);
  ~NTSCCVBSSourceStage() override = default;
};

class PALMCVBSSourceStage final : public FixedFormatCVBSSourceStage {
 public:
  explicit PALMCVBSSourceStage(
      std::shared_ptr<ICVBSSourceStageDeps> deps = nullptr);
  ~PALMCVBSSourceStage() override = default;
};

}  // namespace orc

#endif  // CVBS_SOURCE_STAGE_H
