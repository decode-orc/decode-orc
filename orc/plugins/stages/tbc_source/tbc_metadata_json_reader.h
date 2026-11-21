/*
 * File:        tbc_metadata_json_reader.h
 * Module:      orc-stage-plugin-tbc-source
 * Purpose:     JSON-backed TBC metadata reader
 *
 * Reads legacy .tbc.json metadata produced by older ld-decode / vhs-decode
 * versions directly into C++ data structures via LdDecodeMetaData.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "tbc_metadata_reader.h"

namespace orc {

/**
 * @brief ITBCMetadataReader backed by a legacy .tbc.json file.
 *
 * On open() the JSON is parsed directly by LdDecodeMetaData into C++ data
 * structures stored in in-memory maps.  All reads are served from those maps.
 */
class TBCMetadataJsonReader : public ITBCMetadataReader {
 public:
  TBCMetadataJsonReader();
  ~TBCMetadataJsonReader() override;

  bool open(const std::string& json_path) override;
  void close() override;
  bool is_open() const override;

  std::optional<SourceParameters> read_video_parameters() override;
  std::optional<TbcDomainLevels> read_tbc_domain_levels() override;
  std::optional<PcmAudioParameters> read_pcm_audio_parameters() override;

  std::optional<FieldMetadata> read_field_metadata(FieldID field_id) override;
  std::map<FieldID, FieldMetadata> read_all_field_metadata() override;
  void for_each_field_metadata(
      const std::function<void(const FieldMetadata&)>& visit) override;
  void read_all_dropouts() override;
  void preload_cache() override;

  std::optional<VbiData> read_vbi(FieldID field_id) override;
  std::optional<VitcData> read_vitc(FieldID field_id) override;
  std::optional<ClosedCaptionData> read_closed_caption(
      FieldID field_id) override;
  std::optional<DropoutData> read_dropout(FieldID field_id) const override;
  std::vector<DropoutInfo> read_dropouts(FieldID field_id) const override;

  int32_t get_field_record_count() const override;
  using ITBCMetadataReader::validate_metadata;
  bool validate_metadata(std::string* error_message) const override;

 private:
  bool is_open_ = false;
  std::optional<SourceParameters> source_params_;
  std::optional<TbcDomainLevels> tbc_domain_levels_;
  std::optional<PcmAudioParameters> audio_params_;
  std::map<FieldID, FieldMetadata> field_cache_;
  std::map<FieldID, VbiData> vbi_cache_;
  std::map<FieldID, VitcData> vitc_cache_;
  std::map<FieldID, ClosedCaptionData> cc_cache_;
  std::map<FieldID, std::vector<DropoutInfo>> dropout_cache_;
};

}  // namespace orc
