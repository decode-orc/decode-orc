/*
 * File:        tbc_metadata_json_reader.cpp
 * Module:      orc-stage-plugin-tbc-source
 * Purpose:     JSON-backed TBC metadata reader
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "tbc_metadata_json_reader.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/logging.h>

#include <cmath>

#include "lddecodemetadata.h"
#include "tbc_level_derivation.h"

namespace orc {

TBCMetadataJsonReader::TBCMetadataJsonReader() = default;

TBCMetadataJsonReader::~TBCMetadataJsonReader() { close(); }

bool TBCMetadataJsonReader::open(const std::string& json_path) {
  close();

  LdDecodeMetaData meta;
  if (!meta.read(json_path)) {
    ORC_LOG_ERROR("TBCMetadataJsonReader: failed to read JSON metadata from {}",
                  json_path);
    return false;
  }

  // ---- Source (video) parameters ----
  const auto& vp = meta.getVideoParameters();
  SourceParameters sp;

  switch (vp.system) {
    case LdVideoSystem::PAL:
      sp.system = VideoSystem::PAL;
      break;
    case LdVideoSystem::NTSC:
      sp.system = VideoSystem::NTSC;
      break;
    case LdVideoSystem::PAL_M:
      sp.system = VideoSystem::PAL_M;
      break;
  }

  // Read legacy JSON fields for backwards-compatibility; do NOT populate
  // deprecated struct fields.  Canonical values are derived from sp.system.

  // Active video region (horizontal sample positions — not deprecated).
  sp.active_video_start = vp.activeVideoStart;
  sp.active_video_end = vp.activeVideoEnd;

  // Number of frames: legacy JSON stores field count; convert to frames.
  sp.number_of_sequential_frames =
      (vp.numberOfSequentialFields > 0) ? vp.numberOfSequentialFields / 2 : -1;

  // Source metadata.
  sp.is_mapped = vp.isMapped;
  // is_subcarrier_locked skipped — deprecated field; DB-only round-trip value.
  sp.is_widescreen = vp.isWidescreen;
  sp.git_branch = vp.gitBranch;
  sp.git_commit = vp.gitCommit;
  sp.tape_format = vp.tapeFormat;
  sp.decoder = "ld-decode";

  // Canonical CVBS_U10_4FSC fields derived from video system.
  // Legacy per-disc values (vp.sampleRate, vp.fieldWidth, etc.) are ignored;
  // spec-defined constants are always authoritative.
  // EBU Tech. 3280-E §1.1 (PAL) / SMPTE 244M-2003 §4.1 (NTSC) /
  // ITU-R BT.1700-1 Annex 1 Part B (PAL_M).
  switch (sp.system) {
    case VideoSystem::PAL:
      sp.frame_width_nominal = kPalSamplesPerLineNominal;  // 1135
      sp.frame_height = kPalFrameLines;                    // 625
      break;
    case VideoSystem::PAL_M:
      sp.frame_width_nominal = kPalMSamplesPerLine;  // 909
      sp.frame_height = kPalMFrameLines;             // 525
      break;
    default:                                         // NTSC
      sp.frame_width_nominal = kNtscSamplesPerLine;  // 910
      sp.frame_height = kNtscFrameLines;             // 525
      break;
  }

  // Active line boundaries — mirrors TBCMetadataSqliteReader.
  // For PAL: first/last_active_frame_line from legacy-tools/lddecodemetadata.
  // For NTSC/PAL_M: hardcoded to match ld-chroma-decoder baseline.
  if (sp.system == VideoSystem::PAL) {
    sp.first_active_frame_line = 44;
    sp.last_active_frame_line = 620;
  } else {  // NTSC and PAL_M
    sp.first_active_frame_line = 40;
    sp.last_active_frame_line = 525;
  }

  source_params_ = sp;

  // ---- ld-decode 16-bit domain levels ----
  // ld-decode stores white16bIre as the 100 IRE white level and black16bIre
  // as the picture black level.  The 0 IRE blanking reference that
  // tbc_to_cvbs() needs is derived by derive_tbc_domain_levels(): for PAL
  // black IS blanking; for NTSC/PAL_M the 7.5 IRE setup pedestal is
  // subtracted, unless the levels indicate an NTSC-J capture (black stored
  // at 0 IRE, no setup) — see tbc_level_derivation.h.
  if (vp.white16bIre > vp.black16bIre && vp.black16bIre >= 0) {
    tbc_domain_levels_ =
        derive_tbc_domain_levels(sp.system, vp.black16bIre, vp.white16bIre);
  }

  // ---- PCM audio parameters ----
  const auto& ap = meta.getPcmAudioParameters();
  if (ap.sampleRate > 0) {
    PcmAudioParameters orc_ap;
    orc_ap.sample_rate = ap.sampleRate;
    orc_ap.bits = ap.bits;
    orc_ap.is_signed = ap.isSigned;
    orc_ap.is_little_endian = ap.isLittleEndian;
    audio_params_ = orc_ap;
  }

  // ---- Per-field data ----
  // Field IDs are 0-based (fieldNum 1 → FieldID 0), matching the SQLite reader.
  int32_t num_fields = meta.getNumberOfFields();
  for (int32_t field_num = 1; field_num <= num_fields; ++field_num) {
    const auto& f = meta.getField(field_num);
    FieldID fid(static_cast<FieldID::value_type>(field_num - 1));

    // Core field record — mirrors read_all_field_metadata() SQLite query
    // columns
    FieldMetadata fm;
    fm.seq_no = static_cast<int32_t>(fid.value());
    fm.is_first_field = f.isFirstField;
    fm.sync_confidence = f.syncConf;
    fm.median_burst_ire = f.medianBurstIRE;
    fm.is_pad = f.pad;
    if (f.audioSamples > 0) fm.audio_samples = f.audioSamples;
    if (f.decodeFaults > 0) fm.decode_faults = f.decodeFaults;
    if (f.diskLoc > 0.0) fm.disk_location = f.diskLoc;
    if (f.efmTValues > 0) fm.efm_t_values = f.efmTValues;
    if (f.ac3Symbols > 0) fm.ac3rf_symbols = f.ac3Symbols;
    if (f.fileLoc > 0) fm.file_location = f.fileLoc;
    field_cache_[fid] = fm;

    // VBI
    if (f.vbi.inUse) {
      VbiData orc_vbi;
      orc_vbi.in_use = true;
      orc_vbi.vbi_data = f.vbi.vbiData;
      vbi_cache_[fid] = orc_vbi;
    }

    // VITC
    if (f.vitc.inUse) {
      VitcData orc_vitc;
      orc_vitc.in_use = true;
      orc_vitc.vitc_data = f.vitc.vitcData;
      vitc_cache_[fid] = orc_vitc;
    }

    // Closed caption
    if (f.closedCaption.inUse) {
      ClosedCaptionData orc_cc;
      orc_cc.in_use = true;
      orc_cc.data0 = f.closedCaption.data0;
      orc_cc.data1 = f.closedCaption.data1;
      cc_cache_[fid] = orc_cc;
    }

    // Dropouts (legacy JSON uses 1-based line numbers; convert to 0-based to
    // match SQLite reader)
    const auto& dos = f.dropOuts;
    if (dos.size() > 0) {
      std::vector<DropoutInfo> orc_dos;
      orc_dos.reserve(static_cast<size_t>(dos.size()));
      for (int32_t i = 0; i < dos.size(); ++i) {
        // fieldLine is 1-based. Guard against the unsigned underflow that a
        // fieldLine of 0 (or out-of-range value from an older/interrupted
        // ld-decode) would otherwise produce (0 - 1 == 0xFFFFFFFF), which fed
        // a pathological ~2^64 sample offset into the preview geometry loops
        // and hung the render worker (issue #209).
        const auto field_line = dos.fieldLine(i);
        if (field_line < 1) continue;
        DropoutInfo di;
        di.line = static_cast<uint32_t>(field_line - 1);
        di.start_sample = static_cast<uint32_t>(dos.startx(i));
        di.end_sample = static_cast<uint32_t>(dos.endx(i));
        orc_dos.push_back(di);
      }
      dropout_cache_[fid] = std::move(orc_dos);
    }
  }

  is_open_ = true;
  ORC_LOG_INFO("TBCMetadataJsonReader: loaded {} fields from {}", num_fields,
               json_path);
  return true;
}

void TBCMetadataJsonReader::close() {
  is_open_ = false;
  source_params_.reset();
  tbc_domain_levels_.reset();
  audio_params_.reset();
  field_cache_.clear();
  vbi_cache_.clear();
  vitc_cache_.clear();
  cc_cache_.clear();
  dropout_cache_.clear();
}

bool TBCMetadataJsonReader::is_open() const { return is_open_; }

std::optional<SourceParameters> TBCMetadataJsonReader::read_video_parameters() {
  return source_params_;
}

std::optional<TbcDomainLevels> TBCMetadataJsonReader::read_tbc_domain_levels() {
  return tbc_domain_levels_;
}

std::optional<PcmAudioParameters>
TBCMetadataJsonReader::read_pcm_audio_parameters() {
  return audio_params_;
}

std::optional<FieldMetadata> TBCMetadataJsonReader::read_field_metadata(
    FieldID field_id) {
  if (!is_open_ || !field_id.is_valid()) {
    return std::nullopt;
  }
  auto it = field_cache_.find(field_id);
  if (it != field_cache_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::map<FieldID, FieldMetadata>
TBCMetadataJsonReader::read_all_field_metadata() {
  return field_cache_;
}

void TBCMetadataJsonReader::for_each_field_metadata(
    const std::function<void(const FieldMetadata&)>& visit) {
  for (const auto& entry : field_cache_) visit(entry.second);
}

void TBCMetadataJsonReader::read_all_dropouts() {
  // All dropouts are already loaded eagerly in open(); nothing to do.
}

void TBCMetadataJsonReader::preload_cache() {
  // All data is loaded eagerly in open(); nothing to do.
}

std::optional<VbiData> TBCMetadataJsonReader::read_vbi(FieldID field_id) {
  if (!is_open_ || !field_id.is_valid()) {
    return std::nullopt;
  }
  auto it = vbi_cache_.find(field_id);
  if (it != vbi_cache_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<VitcData> TBCMetadataJsonReader::read_vitc(FieldID field_id) {
  if (!is_open_ || !field_id.is_valid()) {
    return std::nullopt;
  }
  auto it = vitc_cache_.find(field_id);
  if (it != vitc_cache_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<ClosedCaptionData> TBCMetadataJsonReader::read_closed_caption(
    FieldID field_id) {
  if (!is_open_ || !field_id.is_valid()) {
    return std::nullopt;
  }
  auto it = cc_cache_.find(field_id);
  if (it != cc_cache_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<DropoutData> TBCMetadataJsonReader::read_dropout(
    FieldID field_id) const {
  auto dropouts = read_dropouts(field_id);
  if (dropouts.empty()) {
    return std::nullopt;
  }
  DropoutData data;
  data.dropouts = std::move(dropouts);
  return data;
}

std::vector<DropoutInfo> TBCMetadataJsonReader::read_dropouts(
    FieldID field_id) const {
  if (!is_open_ || !field_id.is_valid()) {
    return {};
  }
  auto it = dropout_cache_.find(field_id);
  if (it != dropout_cache_.end()) {
    return it->second;
  }
  return {};
}

int32_t TBCMetadataJsonReader::get_field_record_count() const {
  return static_cast<int32_t>(field_cache_.size());
}

bool TBCMetadataJsonReader::validate_metadata(
    std::string* error_message) const {
  if (!is_open_) {
    if (error_message) {
      *error_message = "Metadata is not open";
    }
    ORC_LOG_ERROR("validate_metadata: {}",
                  error_message ? *error_message : "not open");
    return false;
  }

  if (!source_params_) {
    if (error_message) {
      *error_message = "Failed to read video parameters from metadata";
    }
    ORC_LOG_ERROR("validate_metadata: {} - check debug logs for details",
                  error_message ? *error_message : "Unknown error");
    return false;
  }

  const auto& params = *source_params_;

  if (params.number_of_sequential_frames <= 0) {
    if (error_message) {
      *error_message =
          "Metadata does not specify valid number_of_sequential_frames (" +
          std::to_string(params.number_of_sequential_frames) + ")";
    }
    ORC_LOG_ERROR(
        "validate_metadata: {}",
        error_message ? *error_message : "invalid number_of_sequential_frames");
    return false;
  }

  // Legacy JSON stores per-field records; compare against 2x frames.
  int32_t field_count = get_field_record_count();
  const int32_t expected_field_count = params.number_of_sequential_frames * 2;
  if (field_count != expected_field_count) {
    if (error_message) {
      *error_message =
          "Metadata inconsistency: video parameters specifies " +
          std::to_string(expected_field_count) + " fields, but " +
          std::to_string(field_count) +
          " field records loaded. "
          "This TBC file has inconsistent metadata, likely from a buggy "
          "ld-decode version or interrupted capture.";
    }
    return false;
  }

  if (params.frame_width_nominal <= 0) {
    if (error_message) {
      *error_message = "Invalid frame_width_nominal: " +
                       std::to_string(params.frame_width_nominal);
    }
    return false;
  }

  if (params.system == VideoSystem::Unknown) {
    if (error_message) {
      *error_message = "Unknown or unsupported video system";
    }
    return false;
  }

  return true;
}

}  // namespace orc
