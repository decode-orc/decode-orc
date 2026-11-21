/*
 * File:        tbc_metadata_reader.h
 * Module:      orc-stage-plugin-tbc-source
 * Purpose:     TBC metadata reader interface and SQLite implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

// Forward-declare the SQLite handle so callers that only need open_from_handle
// do not need to include <sqlite3.h>.
struct sqlite3;

#include <orc/stage/field_id.h>
#include <orc/stage/orc_source_parameters.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tbc_metadata_types.h"

namespace orc {

/**
 * @brief Pure-virtual interface for TBC metadata readers
 *
 * TBCMetadataSqliteReader and TBCMetadataJsonReader both implement this
 * interface so callers can substitute either backend transparently.
 */
class ITBCMetadataReader {
 public:
  virtual ~ITBCMetadataReader() = default;

  virtual bool open(const std::string& filename) = 0;
  virtual void close() = 0;
  virtual bool is_open() const = 0;

  virtual std::optional<SourceParameters> read_video_parameters() = 0;
  virtual std::optional<PcmAudioParameters> read_pcm_audio_parameters() = 0;
  virtual std::optional<TbcDomainLevels> read_tbc_domain_levels() = 0;

  virtual std::optional<FieldMetadata> read_field_metadata(
      FieldID field_id) = 0;
  virtual std::map<FieldID, FieldMetadata> read_all_field_metadata() = 0;
  // Visit every field record in field_id order without building a map.
  virtual void for_each_field_metadata(
      const std::function<void(const FieldMetadata&)>& visit) = 0;
  virtual void read_all_dropouts() = 0;
  virtual void preload_cache() = 0;

  virtual std::optional<VbiData> read_vbi(FieldID field_id) = 0;
  virtual std::optional<VitcData> read_vitc(FieldID field_id) = 0;
  virtual std::optional<ClosedCaptionData> read_closed_caption(
      FieldID field_id) = 0;
  virtual std::optional<DropoutData> read_dropout(FieldID field_id) const = 0;
  virtual std::vector<DropoutInfo> read_dropouts(FieldID field_id) const = 0;

  virtual int32_t get_field_record_count() const = 0;
  virtual bool validate_metadata(std::string* error_message) const = 0;

  bool validate_metadata() const { return validate_metadata(nullptr); }
};

/**
 * @brief SQLite-backed reader for TBC metadata (.tbc.json.db files)
 */
class TBCMetadataSqliteReader : public ITBCMetadataReader {
 public:
  TBCMetadataSqliteReader();
  ~TBCMetadataSqliteReader() override;

  bool open(const std::string& filename) override;

  // Attach an already-open sqlite3 connection (takes ownership).
  // Used by TBCMetadataJsonReader to back a reader with an in-memory database.
  bool open_from_handle(sqlite3* db);

  void close() override;
  bool is_open() const override { return is_open_; }

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

  // The dropouts of one field, read from the database on each call (no
  // cache, and nothing loaded at open): in the drop_outs primary-key order
  // (field_line, startx, endx), the order read_all_dropouts() keeps.
  // Thread-safe.
  std::vector<DropoutInfo> query_field_dropouts(FieldID field_id) const;

  int32_t get_field_record_count() const override;
  using ITBCMetadataReader::validate_metadata;
  bool validate_metadata(std::string* error_message) const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  bool is_open_;
};

// Backward-compatibility alias
using TBCMetadataReader = TBCMetadataSqliteReader;

}  // namespace orc
