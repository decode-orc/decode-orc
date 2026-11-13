/*
 * File:        tbc_metadata_writer.h
 * Module:      orc-metadata
 * Purpose:     TBC Metadata Writer (SQLite)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context_interface.h>
#include <tbc_metadata_types.h>

#include <memory>
#include <string>
#include <vector>

#include "tbc_metadata_writer_interface.h"

namespace orc {

/**
 * @brief Writer for TBC metadata (SQLite database)
 *
 * Creates ld-decode compatible SQLite databases with capture metadata,
 * field records, and observer data (VBI, VITC, closed captions, VITS metrics).
 */
class TBCMetadataWriter : public ITBCMetadataWriter {
 public:
  TBCMetadataWriter();
  ~TBCMetadataWriter() override;

  // Open/create a metadata database file
  bool open(const std::string& filename) override;
  void close() override;

  bool is_open() const { return is_open_; }

  // Write video parameters (creates capture record)
  bool write_video_parameters(const SourceParameters& params) override;

  // Write PCM audio parameters (optional)
  bool write_pcm_audio_parameters(const PcmAudioParameters& params) override;

  // Write field metadata
  bool write_field_metadata(const FieldMetadata& field) override;

  // Update field metadata (for fields already written)
  bool update_field_median_burst_ire(FieldID field_id, double median_burst_ire);
  bool update_field_phase_id(FieldID field_id, int32_t field_phase_id);
  bool update_field_is_first_field(FieldID field_id, bool is_first_field);

  // Write observer data for a field
  bool write_vbi(FieldID field_id, const VbiData& vbi);
  bool write_vitc(FieldID field_id, const VitcData& vitc);
  bool write_closed_caption(FieldID field_id, const ClosedCaptionData& cc);
  bool write_vits_metrics(FieldID field_id, const VitsMetrics& metrics);
  bool write_dropout(FieldID field_id, const DropoutInfo& dropout) override;

  // Write all observations from an IObservationContext
  bool write_observations(FieldID source_field_id, FieldID db_field_id,
                          const IObservationContext& context) override;

  // Transaction support for bulk writes
  bool begin_transaction() override;
  bool commit_transaction() override;
  bool rollback_transaction();

 private:
  class Impl;  // Forward declaration for pimpl
  std::unique_ptr<Impl> impl_;
  bool is_open_;
  int capture_id_;  // ID of the capture record
};

}  // namespace orc
