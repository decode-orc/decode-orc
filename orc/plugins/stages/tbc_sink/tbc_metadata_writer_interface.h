/*
 * File:        tbc_metadata_writer_interface.h
 * Module:      orc-metadata
 * Purpose:     Interface for TBC Metadata Writer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context_interface.h>
#include <tbc_metadata_types.h>

#include <string>

namespace orc {

/**
 * @brief Interface for writing TBC metadata (SQLite database)
 *
 * Abstracts the TBCMetadataWriter to enable dependency injection and unit
 * testing.
 */
class ITBCMetadataWriter {
 public:
  virtual ~ITBCMetadataWriter() = default;

  // Open/create a metadata database file
  virtual bool open(const std::string& filename) = 0;
  virtual void close() = 0;

  // Write video parameters (creates capture record)
  virtual bool write_video_parameters(const SourceParameters& params) = 0;

  // Describe the layout of the analogue audio .pcm sidecar. Only written when
  // the export carries audio; requires the capture record to exist already.
  virtual bool write_pcm_audio_parameters(const PcmAudioParameters& params) = 0;

  // Write field metadata
  virtual bool write_field_metadata(const FieldMetadata& field) = 0;

  // Write observer data for a field
  // Write observer data for a field.
  // source_field_id: used to look up observations in the context (the
  // representation's field id) db_field_id:     0-based export position written
  // as the field_id column in the database
  virtual bool write_observations(FieldID source_field_id, FieldID db_field_id,
                                  const IObservationContext& context) = 0;
  virtual bool write_dropout(FieldID db_field_id,
                             const DropoutInfo& dropout) = 0;

  // Transaction support for bulk writes
  virtual bool begin_transaction() = 0;
  virtual bool commit_transaction() = 0;
};

}  // namespace orc
