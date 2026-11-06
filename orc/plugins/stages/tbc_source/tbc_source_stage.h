/*
 * File:        tbc_source_stage.h
 * Module:      orc-stage-plugin-tbc-source
 * Purpose:     Unified TBC source stage — PAL/NTSC/PAL_M TBC file loading
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef TBC_SOURCE_STAGE_H
#define TBC_SOURCE_STAGE_H

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/dropout/dropout_run.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/orc_source_parameters.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "tbc_metadata_types.h"

namespace orc {

// ---------------------------------------------------------------------------
// TBCVideoParams — video parameters extracted from .tbc.json.db
// ---------------------------------------------------------------------------
// These are the TBC-domain values; level fields are 16-bit unsigned TBC units.
struct TBCVideoParams {
  VideoSystem system = VideoSystem::Unknown;
  int32_t number_of_fields = 0;  // total fields in the TBC file
  int32_t field_width = 0;       // nominal samples per line
  // PAL: field1_height=312 (TBC field 1), field2_height=313 (TBC field 2)
  // NTSC/PAL_M: field1_height=263 (TBC field 1, VFR top), field2_height=262
  //   (TBC field 2, VFR bottom; both stored at 263 lines in the TBC file)
  int32_t field1_height = 0;
  int32_t field2_height = 0;
  int32_t blanking_16b = 0;  // ld-decode 16-bit domain blanking (0 IRE)
  int32_t white_16b = 0;     // ld-decode 16-bit domain white (100 IRE)
  std::string decoder;
  std::string tape_format;
  std::string git_branch;
  std::string git_commit;
  bool is_widescreen = false;
  bool is_mapped = false;
  int32_t active_video_start = -1;
  int32_t active_video_end = -1;
  int32_t first_active_frame_line = -1;
  int32_t last_active_frame_line = -1;
  // NTSC-J: non-standard black level in TBC 16-bit domain (nullopt = standard)
  std::optional<int32_t> ntsc_j_black_level_16b;
};

// ---------------------------------------------------------------------------
// TBCFieldMeta — per-field metadata from .tbc.json.db
// ---------------------------------------------------------------------------
struct TBCFieldMeta {
  std::optional<int32_t> audio_sample_count;  // stereo pairs for this field
  std::optional<int32_t> efm_t_value_count;
  std::optional<int32_t> ac3rf_symbol_count;
  std::optional<int64_t> file_location;  // byte offset in .tbc (informational)
  std::vector<DropoutInfo> dropouts;     // field-local TBC dropout regions
  bool is_pad = false;  // decoder marked this field as padding (no signal)
};

// ---------------------------------------------------------------------------
// ITBCSourceStageDeps — dependency injection interface
// ---------------------------------------------------------------------------
// All external I/O is accessed through this interface so that unit tests can
// inject synthetic data without touching the filesystem.
class ITBCSourceStageDeps {
 public:
  virtual ~ITBCSourceStageDeps() = default;

  // Validate that a path exists and is a readable regular file.
  virtual bool validate_input_file(const std::string& path,
                                   std::string& error_message) const = 0;

  // Open .tbc.json.db and read the video_parameters table.
  virtual std::optional<TBCVideoParams> load_video_params(
      const std::string& db_path, std::string& error_message) const = 0;

  // Read all per-field metadata rows from .tbc.json.db.
  // Returns a vector indexed by sequential field number (0-based).
  virtual std::vector<TBCFieldMeta> load_all_field_meta(
      const std::string& db_path, std::string& error_message) const = 0;

  // Read use_sample_count raw uint16_t samples for one TBC field.
  // field_index is 0-based (0 = first field in the file).
  // stored_samples_per_field is the number of uint16_t words stored per field
  // in the TBC file; used to compute the byte offset.  May be larger than
  // use_sample_count when TBC field 1 (312 lines) is shorter than the stored
  // field size (313 × width for PAL).
  // Returns empty vector on error.
  virtual std::vector<uint16_t> read_field_samples(
      const std::string& tbc_path, int32_t field_index,
      int32_t stored_samples_per_field, int32_t use_sample_count,
      std::string& error_message) const = 0;

  // Targeted variant: read use_sample_count samples starting at sample_offset
  // within the field's stored area.  Enables per-line reads without loading
  // the entire field — critical for analysis sinks that only need a few lines
  // per frame across a long recording.
  // sample_offset is relative to the start of the field (0 = first sample of
  // line 0).  stored_samples_per_field is used only to compute the field's
  // start byte offset in the file.
  // Returns empty vector on error.
  virtual std::vector<uint16_t> read_field_samples_at(
      const std::string& tbc_path, int32_t field_index,
      int32_t stored_samples_per_field, int32_t sample_offset,
      int32_t use_sample_count, std::string& error_message) const = 0;

  // Audio: returns true when the PCM sidecar exists.
  virtual bool has_audio_file(const std::string& pcm_path) const = 0;

  // PCM layout metadata (pcm_audio_parameters table, or the legacy JSON
  // pcmAudioParameters block) from the metadata sidecar. Returns nullopt when
  // the metadata carries no PCM parameters.
  virtual std::optional<PcmAudioParameters> load_pcm_audio_parameters(
      const std::string& db_path) const = 0;

  // Total number of stereo pairs in the raw PCM sidecar (file size / 4).
  // Returns nullopt when the file is absent or unreadable.
  virtual std::optional<uint64_t> get_audio_pair_count(
      const std::string& pcm_path) const = 0;

  // Read stereo_pair_count interleaved signed little-endian int16_t stereo
  // pairs starting at stereo_pair_offset from the raw PCM sidecar (no WAV
  // header).
  virtual std::vector<int16_t> read_audio_samples_at(
      const std::string& pcm_path, size_t stereo_pair_offset,
      size_t stereo_pair_count) const = 0;

  // EFM: returns true when the raw .efm T-value sidecar exists.  TBC captures
  // store one byte per EFM T-value in the .efm file in field order; there is
  // no .efm.meta index sidecar (that is a CVBS-only construct) — per-field
  // T-value counts come from the TBC metadata (efm_t_values) instead.
  virtual bool has_efm_file(const std::string& efm_bin_path) const = 0;

  // Read efm_byte_count raw EFM T-value bytes starting at efm_byte_offset from
  // the .efm sidecar.  Offsets/counts are computed by the caller from the
  // per-field T-value counts.  Returns empty vector on error.
  virtual std::vector<uint8_t> read_efm_bytes_at(
      const std::string& efm_bin_path, size_t efm_byte_offset,
      size_t efm_byte_count) const = 0;

  // AC3 RF symbols: same structure as EFM.  TBC captures store one byte per
  // QPSK symbol in the .ac3sym file in field order; there is no .meta index
  // sidecar (that is a CVBS-only construct) — per-field symbol counts come
  // from the TBC metadata (ac3_symbols) instead.
  virtual bool has_ac3_file(const std::string& ac3_bin_path) const = 0;

  // Read ac3_byte_count raw AC3 symbol bytes starting at ac3_byte_offset from
  // the .ac3sym sidecar.  Offsets/counts are computed by the caller from the
  // per-field symbol counts.  Returns empty vector on error.
  virtual std::vector<uint8_t> read_ac3_bytes_at(
      const std::string& ac3_bin_path, size_t ac3_byte_offset,
      size_t ac3_byte_count) const = 0;
};

// ---------------------------------------------------------------------------
// TBCSourceStage
// ---------------------------------------------------------------------------
// Unified TBC source stage.  Reads the video system from .tbc.json.db at
// execute() time and dispatches to the appropriate converter.
//
// Stage ID: "tbc_source"
// Display name (resolved at load time): "<VideoSystem> TBC <Composite|YC>"
//   e.g., "PAL TBC Composite", "NTSC TBC YC"
//
// Parameters:
//   input_path       — composite .tbc file  (required for composite mode)
//   y_path           — luma .tbc file       (required for YC mode)
//   c_path           — chroma .tbc file     (required for YC mode)
//   db_path          — .tbc.json.db         (optional; auto-derived from
//                      input_path)
//   pcm_path         — audio .pcm sidecar   (optional; raw signed-LE 16-bit
//                      stereo, always converted on ingest to the 48 kHz
//                      24-bit synchronous pipeline form)
//   efm_path         — EFM .efm sidecar     (optional)
//   ac3rf_path       — AC3 .ac3sym sidecar  (optional)
//
// Signal type is auto-detected: if y_path and c_path are both set, the stage
// operates in YC mode; otherwise composite.
class TBCSourceStage : public DAGStage,
                       public ParameterizedStage,
                       public IStagePreviewCapability {
 public:
  explicit TBCSourceStage(std::shared_ptr<ITBCSourceStageDeps> deps = nullptr);
  ~TBCSourceStage() override = default;

  void set_deps_override(std::shared_ptr<ITBCSourceStageDeps> deps) {
    deps_ = std::move(deps);
  }

  // DAGStage interface
  std::string version() const override { return "3.1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::SOURCE,
                        "tbc_source",
                        display_name_,
                        "TBC file source — supports PAL, NTSC, and PAL-M "
                        "composite and YC signals from ld-decode",
                        0,
                        0,
                        1,
                        UINT32_MAX,
                        VideoFormatCompatibility::ALL};
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

 private:
  // Resolve all sidecar paths from the composite input_path or y_path.
  struct SidecarPaths {
    std::string db_path;
    std::string pcm_path;
    std::string efm_path;
    std::string ac3_path;
  };
  SidecarPaths resolve_sidecars(
      const std::string& tbc_path,
      const std::map<std::string, ParameterValue>& params) const;

  mutable std::mutex execute_mutex_;
  mutable std::string cached_input_key_;
  mutable ArtifactPtr cached_representation_;

  std::string display_name_{"TBC Source"};
  std::map<std::string, ParameterValue> parameters_;

  std::shared_ptr<ITBCSourceStageDeps> deps_;
};

}  // namespace orc

#endif  // TBC_SOURCE_STAGE_H
