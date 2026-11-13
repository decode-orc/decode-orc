/*
 * File:        tbc_metadata_writer.cpp
 * Module:      orc-metadata
 * Purpose:     TBC Metadata Writer implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/logging.h>
#include <sqlite3.h>
#include <tbc_metadata_writer.h>

#include <filesystem>
#include <stdexcept>

#include "tbc_sink_levels.h"

namespace orc {

// TBCMetadataWriter::Impl (Private implementation using SQLite)
class TBCMetadataWriter::Impl {
 public:
  sqlite3* db = nullptr;
  int capture_id = -1;

  ~Impl() {
    if (db) {
      sqlite3_close(db);
    }
  }

  bool exec_sql(const std::string& sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
      std::string error = err_msg ? err_msg : "Unknown error";
      sqlite3_free(err_msg);
      ORC_LOG_ERROR("SQL error: {}", error);
      return false;
    }
    return true;
  }

  bool create_schema() {
    // Set schema version to match ld-decode's expected user_version
    if (!exec_sql("PRAGMA user_version = 1;")) {
      return false;
    }

    // Create tables matching the ld-decode schema exactly (including CHECK
    // constraints)
    const char* schema_sql = R"(
            CREATE TABLE IF NOT EXISTS capture (
                capture_id INTEGER PRIMARY KEY,
                system TEXT NOT NULL
                    CHECK (system IN ('NTSC','PAL','PAL_M')),
                decoder TEXT NOT NULL
                    CHECK (decoder IN ('ld-decode','vhs-decode')),
                git_branch TEXT,
                git_commit TEXT,
                video_sample_rate REAL,
                active_video_start INTEGER,
                active_video_end INTEGER,
                field_width INTEGER,
                field_height INTEGER,
                number_of_sequential_fields INTEGER,
                colour_burst_start INTEGER,
                colour_burst_end INTEGER,
                is_mapped INTEGER
                    CHECK (is_mapped IN (0,1)),
                is_subcarrier_locked INTEGER
                    CHECK (is_subcarrier_locked IN (0,1)),
                is_widescreen INTEGER
                    CHECK (is_widescreen IN (0,1)),
                white_16b_ire INTEGER,
                black_16b_ire INTEGER,
                blanking_16b_ire INTEGER,
                capture_notes TEXT
            );
            
            CREATE TABLE IF NOT EXISTS pcm_audio_parameters (
                capture_id INTEGER PRIMARY KEY
                    REFERENCES capture(capture_id) ON DELETE CASCADE,
                bits INTEGER,
                is_signed INTEGER
                    CHECK (is_signed IN (0,1)),
                is_little_endian INTEGER
                    CHECK (is_little_endian IN (0,1)),
                sample_rate REAL
            );
            
            CREATE TABLE IF NOT EXISTS field_record (
                capture_id INTEGER NOT NULL
                    REFERENCES capture(capture_id) ON DELETE CASCADE,
                field_id INTEGER NOT NULL,
                audio_samples INTEGER,
                decode_faults INTEGER,
                disk_loc REAL,
                efm_t_values INTEGER,
                field_phase_id INTEGER,
                file_loc INTEGER,
                is_first_field INTEGER
                    CHECK (is_first_field IN (0,1)),
                median_burst_ire REAL,
                pad INTEGER
                    CHECK (pad IN (0,1)),
                sync_conf INTEGER,
                ntsc_is_fm_code_data_valid INTEGER
                    CHECK (ntsc_is_fm_code_data_valid IN (0,1)),
                ntsc_fm_code_data INTEGER,
                ntsc_field_flag INTEGER
                    CHECK (ntsc_field_flag IN (0,1)),
                ntsc_is_video_id_data_valid INTEGER
                    CHECK (ntsc_is_video_id_data_valid IN (0,1)),
                ntsc_video_id_data INTEGER,
                ntsc_white_flag INTEGER
                    CHECK (ntsc_white_flag IN (0,1)),
                ac3_symbols INTEGER,
                PRIMARY KEY (capture_id, field_id)
            );
            
            CREATE TABLE IF NOT EXISTS vits_metrics (
                capture_id INTEGER NOT NULL,
                field_id INTEGER NOT NULL,
                b_psnr REAL,
                w_snr REAL,
                FOREIGN KEY (capture_id, field_id)
                    REFERENCES field_record(capture_id, field_id)
                    ON DELETE CASCADE,
                PRIMARY KEY (capture_id, field_id)
            );
            
            CREATE TABLE IF NOT EXISTS vbi (
                capture_id INTEGER NOT NULL,
                field_id INTEGER NOT NULL,
                vbi0 INTEGER NOT NULL,
                vbi1 INTEGER NOT NULL,
                vbi2 INTEGER NOT NULL,
                FOREIGN KEY (capture_id, field_id)
                    REFERENCES field_record(capture_id, field_id)
                    ON DELETE CASCADE,
                PRIMARY KEY (capture_id, field_id)
            );
            
            CREATE TABLE IF NOT EXISTS drop_outs (
                capture_id INTEGER NOT NULL,
                field_id INTEGER NOT NULL,
                field_line INTEGER NOT NULL,
                startx INTEGER NOT NULL,
                endx INTEGER NOT NULL,
                PRIMARY KEY (capture_id, field_id, field_line, startx, endx),
                FOREIGN KEY (capture_id, field_id)
                    REFERENCES field_record(capture_id, field_id)
                    ON DELETE CASCADE
            );
            
            CREATE TABLE IF NOT EXISTS vitc (
                capture_id INTEGER NOT NULL,
                field_id INTEGER NOT NULL,
                vitc0 INTEGER NOT NULL,
                vitc1 INTEGER NOT NULL,
                vitc2 INTEGER NOT NULL,
                vitc3 INTEGER NOT NULL,
                vitc4 INTEGER NOT NULL,
                vitc5 INTEGER NOT NULL,
                vitc6 INTEGER NOT NULL,
                vitc7 INTEGER NOT NULL,
                FOREIGN KEY (capture_id, field_id)
                    REFERENCES field_record(capture_id, field_id)
                    ON DELETE CASCADE,
                PRIMARY KEY (capture_id, field_id)
            );
            
            CREATE TABLE IF NOT EXISTS closed_caption (
                capture_id INTEGER NOT NULL,
                field_id INTEGER NOT NULL,
                data0 INTEGER,
                data1 INTEGER,
                FOREIGN KEY (capture_id, field_id)
                    REFERENCES field_record(capture_id, field_id)
                    ON DELETE CASCADE,
                PRIMARY KEY (capture_id, field_id)
            );
        )";

    return exec_sql(schema_sql);
  }
};

// TBCMetadataWriter implementation

TBCMetadataWriter::TBCMetadataWriter()
    : impl_(std::make_unique<Impl>()), is_open_(false), capture_id_(-1) {}

TBCMetadataWriter::~TBCMetadataWriter() { close(); }

bool TBCMetadataWriter::open(const std::string& filename) {
  if (is_open_) {
    close();
  }

  // Delete existing database file to ensure clean state
  std::error_code ec;
  std::filesystem::remove(filename, ec);
  // Ignore error if file doesn't exist

  int rc = sqlite3_open(filename.c_str(), &impl_->db);
  if (rc != SQLITE_OK) {
    ORC_LOG_ERROR("Failed to open metadata database: {}", filename);
    return false;
  }

  is_open_ = true;

  // Create schema
  if (!impl_->create_schema()) {
    close();
    return false;
  }

  return true;
}

void TBCMetadataWriter::close() {
  if (impl_->db) {
    sqlite3_close(impl_->db);
    impl_->db = nullptr;
  }
  is_open_ = false;
  capture_id_ = -1;
}

bool TBCMetadataWriter::write_video_parameters(const SourceParameters& params) {
  if (!is_open_) return false;

  sqlite3_stmt* stmt = nullptr;
  const char* sql = R"(
        INSERT INTO capture (
            system, decoder, git_branch, git_commit,
            video_sample_rate, active_video_start, active_video_end,
            field_width, field_height, number_of_sequential_fields,
            colour_burst_start, colour_burst_end,
            is_mapped, is_subcarrier_locked, is_widescreen,
            white_16b_ire, black_16b_ire, blanking_16b_ire, capture_notes
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    ORC_LOG_ERROR("Failed to prepare capture insert");
    return false;
  }

  // Compute values for DB columns that mirror deprecated SourceParameters
  // fields. The DB schema is retained for round-trip compatibility with
  // ld-decode tools.
  const int32_t db_field_width = params.frame_width_nominal;
  const int32_t db_field_height =
      static_cast<int32_t>(calculate_padded_field_height(params.system));
  const int32_t db_sequential_fields = params.number_of_sequential_frames * 2;
  const auto [db_burst_start, db_burst_end] = colour_burst_range(params.system);
  const double db_sample_rate = sample_rate_from_system(params.system);

  std::string system_str = video_system_to_string(params.system);
  sqlite3_bind_text(stmt, 1, system_str.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, params.decoder.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, params.git_branch.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, params.git_commit.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt, 5, db_sample_rate);
  sqlite3_bind_int(stmt, 6, params.active_video_start);
  sqlite3_bind_int(stmt, 7, params.active_video_end);
  sqlite3_bind_int(stmt, 8, db_field_width);
  sqlite3_bind_int(stmt, 9, db_field_height);
  sqlite3_bind_int(stmt, 10, db_sequential_fields);
  sqlite3_bind_int(stmt, 11, db_burst_start);
  sqlite3_bind_int(stmt, 12, db_burst_end);
  sqlite3_bind_int(stmt, 13, params.is_mapped ? 1 : 0);
  sqlite3_bind_int(stmt, 14,
                   1);  // is_subcarrier_locked: always true (deprecated field)
  sqlite3_bind_int(stmt, 15, params.is_widescreen ? 1 : 0);
  // ld-decode 16-bit TBC domain levels (CVBS_U10_4FSC × 64), taken from the
  // levels the pipeline reports rather than the spec constants so a non-spec
  // black level — NTSC-J picture black at 0 IRE, a video_params override —
  // survives the export instead of being re-labelled as standard setup
  // (issue #257).  See tbc_sink_levels.h.
  const TbcCaptureLevels db_levels = tbc_capture_levels(params);
  sqlite3_bind_int(stmt, 16, db_levels.white_16b);
  sqlite3_bind_int(stmt, 17, db_levels.black_16b);
  sqlite3_bind_int(stmt, 18, db_levels.blanking_16b);
  sqlite3_bind_text(stmt, 19, "", -1, SQLITE_TRANSIENT);  // capture_notes

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    ORC_LOG_ERROR("Failed to insert capture record");
    return false;
  }

  capture_id_ = static_cast<int>(sqlite3_last_insert_rowid(impl_->db));
  impl_->capture_id = capture_id_;
  return true;
}

bool TBCMetadataWriter::write_pcm_audio_parameters(
    const PcmAudioParameters& params) {
  if (!is_open_ || capture_id_ < 0) return false;

  sqlite3_stmt* stmt = nullptr;
  const char* sql = R"(
        INSERT INTO pcm_audio_parameters (
            capture_id, bits, is_signed, is_little_endian, sample_rate
        ) VALUES (?, ?, ?, ?, ?)
    )";

  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return false;

  sqlite3_bind_int64(stmt, 1, capture_id_);
  sqlite3_bind_int(stmt, 2, params.bits);
  sqlite3_bind_int(stmt, 3, params.is_signed ? 1 : 0);
  sqlite3_bind_int(stmt, 4, params.is_little_endian ? 1 : 0);
  sqlite3_bind_double(stmt, 5, params.sample_rate);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool TBCMetadataWriter::write_field_metadata(const FieldMetadata& field) {
  if (!is_open_ || capture_id_ < 0) return false;

  sqlite3_stmt* stmt = nullptr;
  const char* sql = R"(
        INSERT INTO field_record (
            capture_id, field_id, audio_samples, decode_faults,
            disk_loc, efm_t_values, field_phase_id, file_loc,
            is_first_field, median_burst_ire, pad, sync_conf,
            ntsc_is_fm_code_data_valid, ntsc_fm_code_data, ntsc_field_flag,
            ntsc_is_video_id_data_valid, ntsc_video_id_data, ntsc_white_flag,
            ac3_symbols
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return false;

  // field_id is 0-based in database (seq_no - 1)
  int field_id = field.seq_no - 1;

  sqlite3_bind_int64(stmt, 1, capture_id_);
  sqlite3_bind_int(stmt, 2, field_id);

  // Only write fields that have values (from hints/observers)
  if (field.audio_samples.has_value()) {
    sqlite3_bind_int(stmt, 3, field.audio_samples.value());
  } else {
    sqlite3_bind_null(stmt, 3);
  }

  if (field.decode_faults.has_value()) {
    sqlite3_bind_int(stmt, 4, field.decode_faults.value());
  } else {
    sqlite3_bind_null(stmt, 4);
  }

  if (field.disk_location.has_value()) {
    sqlite3_bind_double(stmt, 5, field.disk_location.value());
  } else {
    sqlite3_bind_null(stmt, 5);
  }

  if (field.efm_t_values.has_value()) {
    sqlite3_bind_int(stmt, 6, field.efm_t_values.value());
  } else {
    sqlite3_bind_null(stmt, 6);
  }

  // field_phase_id comes from PALPhaseObserver
  if (field.field_phase_id.has_value()) {
    sqlite3_bind_int(stmt, 7, field.field_phase_id.value());
  } else {
    sqlite3_bind_null(stmt, 7);
  }

  if (field.file_location.has_value()) {
    sqlite3_bind_int64(stmt, 8, field.file_location.value());
  } else {
    sqlite3_bind_null(stmt, 8);
  }

  // is_first_field comes from FieldParityObserver
  if (field.is_first_field.has_value()) {
    sqlite3_bind_int(stmt, 9, field.is_first_field.value() ? 1 : 0);
  } else {
    sqlite3_bind_null(stmt, 9);
  }

  // median_burst_ire comes from BurstLevelObserver
  if (field.median_burst_ire.has_value()) {
    sqlite3_bind_double(stmt, 10, field.median_burst_ire.value());
  } else {
    sqlite3_bind_null(stmt, 10);
  }

  if (field.is_pad.has_value()) {
    sqlite3_bind_int(stmt, 11, field.is_pad.value() ? 1 : 0);
  } else {
    sqlite3_bind_null(stmt, 11);
  }

  if (field.sync_confidence.has_value()) {
    sqlite3_bind_int(stmt, 12, field.sync_confidence.value());
  } else {
    sqlite3_bind_null(stmt, 12);
  }

  // NTSC-specific fields: must be NULL for non-NTSC content (PAL, PAL_M)
  if (field.ntsc.in_use) {
    sqlite3_bind_int(stmt, 13, field.ntsc.is_fm_code_data_valid ? 1 : 0);
    if (field.ntsc.is_fm_code_data_valid) {
      sqlite3_bind_int(stmt, 14, field.ntsc.fm_code_data);
    } else {
      sqlite3_bind_null(stmt, 14);
    }
    sqlite3_bind_int(stmt, 15, field.ntsc.field_flag ? 1 : 0);
    sqlite3_bind_int(stmt, 16, field.ntsc.is_video_id_data_valid ? 1 : 0);
    if (field.ntsc.is_video_id_data_valid) {
      sqlite3_bind_int(stmt, 17, field.ntsc.video_id_data);
    } else {
      sqlite3_bind_null(stmt, 17);
    }
    sqlite3_bind_int(stmt, 18, field.ntsc.white_flag ? 1 : 0);
  } else {
    // Non-NTSC: all NTSC-specific fields must be NULL per schema spec
    sqlite3_bind_null(stmt, 13);
    sqlite3_bind_null(stmt, 14);
    sqlite3_bind_null(stmt, 15);
    sqlite3_bind_null(stmt, 16);
    sqlite3_bind_null(stmt, 17);
    sqlite3_bind_null(stmt, 18);
  }

  if (field.ac3rf_symbols.has_value()) {
    sqlite3_bind_int(stmt, 19, field.ac3rf_symbols.value());
  } else {
    sqlite3_bind_null(stmt, 19);
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool TBCMetadataWriter::update_field_median_burst_ire(FieldID field_id,
                                                      double median_burst_ire) {
  if (!is_open_ || capture_id_ < 0) return false;

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "UPDATE field_record SET median_burst_ire = ? WHERE capture_id = ? AND "
      "field_id = ?";

  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return false;

  sqlite3_bind_double(stmt, 1, median_burst_ire);
  sqlite3_bind_int64(stmt, 2, capture_id_);
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(field_id.value()));

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool TBCMetadataWriter::update_field_phase_id(FieldID field_id,
                                              int32_t field_phase_id) {
  if (!is_open_ || capture_id_ < 0) return false;

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "UPDATE field_record SET field_phase_id = ? WHERE capture_id = ? AND "
      "field_id = ?";

  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return false;

  sqlite3_bind_int(stmt, 1, field_phase_id);
  sqlite3_bind_int64(stmt, 2, capture_id_);
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(field_id.value()));

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool TBCMetadataWriter::update_field_is_first_field(FieldID field_id,
                                                    bool is_first_field) {
  if (!is_open_ || capture_id_ < 0) return false;

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "UPDATE field_record SET is_first_field = ? WHERE capture_id = ? AND "
      "field_id = ?";

  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return false;

  sqlite3_bind_int(stmt, 1, is_first_field ? 1 : 0);
  sqlite3_bind_int64(stmt, 2, capture_id_);
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(field_id.value()));

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool TBCMetadataWriter::write_vbi(FieldID field_id, const VbiData& vbi) {
  if (!is_open_ || capture_id_ < 0 || !vbi.in_use) return false;

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO vbi (capture_id, field_id, vbi0, vbi1, vbi2) VALUES (?, ?, "
      "?, ?, ?)";

  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return false;

  sqlite3_bind_int64(stmt, 1, capture_id_);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(field_id.value()));
  sqlite3_bind_int(stmt, 3, vbi.vbi_data[0]);
  sqlite3_bind_int(stmt, 4, vbi.vbi_data[1]);
  sqlite3_bind_int(stmt, 5, vbi.vbi_data[2]);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool TBCMetadataWriter::write_vitc(FieldID field_id, const VitcData& vitc) {
  if (!is_open_ || capture_id_ < 0 || !vitc.in_use) return false;

  sqlite3_stmt* stmt = nullptr;
  const char* sql = R"(
        INSERT INTO vitc (capture_id, field_id, vitc0, vitc1, vitc2, vitc3, vitc4, vitc5, vitc6, vitc7)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return false;

  sqlite3_bind_int64(stmt, 1, capture_id_);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(field_id.value()));
  for (int i = 0; i < 8; ++i) {
    sqlite3_bind_int(stmt, 3 + i, vitc.vitc_data[i]);
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool TBCMetadataWriter::write_closed_caption(FieldID field_id,
                                             const ClosedCaptionData& cc) {
  if (!is_open_ || capture_id_ < 0 || !cc.in_use) return false;

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO closed_caption (capture_id, field_id, data0, data1) VALUES "
      "(?, ?, ?, ?)";

  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return false;

  sqlite3_bind_int64(stmt, 1, capture_id_);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(field_id.value()));
  sqlite3_bind_int(stmt, 3, cc.data0);
  sqlite3_bind_int(stmt, 4, cc.data1);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool TBCMetadataWriter::write_vits_metrics(FieldID field_id,
                                           const VitsMetrics& metrics) {
  if (!is_open_ || capture_id_ < 0 || !metrics.in_use) return false;

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO vits_metrics (capture_id, field_id, b_psnr, w_snr) VALUES "
      "(?, ?, ?, ?)";

  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return false;

  sqlite3_bind_int64(stmt, 1, capture_id_);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(field_id.value()));
  sqlite3_bind_double(stmt, 3, metrics.black_psnr);
  sqlite3_bind_double(stmt, 4, metrics.white_snr);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool TBCMetadataWriter::write_dropout(FieldID field_id,
                                      const DropoutInfo& dropout) {
  if (!is_open_ || capture_id_ < 0) return false;

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO drop_outs (capture_id, field_id, field_line, startx, endx) "
      "VALUES (?, ?, ?, ?, ?)";

  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return false;

  sqlite3_bind_int64(stmt, 1, capture_id_);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(field_id.value()));
  // Convert from 0-based (internal) to 1-based (database) line numbering
  sqlite3_bind_int(stmt, 3, static_cast<int>(dropout.line) + 1);
  sqlite3_bind_int(stmt, 4, static_cast<int>(dropout.start_sample));
  sqlite3_bind_int(stmt, 5, static_cast<int>(dropout.end_sample));

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool TBCMetadataWriter::write_observations(FieldID source_field_id,
                                           FieldID db_field_id,
                                           const IObservationContext& context) {
  if (!is_open_ || capture_id_ < 0) return false;

  bool any_written = false;

  // Extract and write VBI data (from BiphaseObserver)
  // The biphase observer stores the raw VBI words in "biphase" namespace
  auto vbi0_obs = context.get(source_field_id, "biphase", "vbi_line_16");
  auto vbi1_obs = context.get(source_field_id, "biphase", "vbi_line_17");
  auto vbi2_obs = context.get(source_field_id, "biphase", "vbi_line_18");

  if (vbi0_obs && vbi1_obs && vbi2_obs &&
      std::holds_alternative<int32_t>(*vbi0_obs) &&
      std::holds_alternative<int32_t>(*vbi1_obs) &&
      std::holds_alternative<int32_t>(*vbi2_obs)) {
    VbiData vbi;
    vbi.in_use = true;
    vbi.vbi_data[0] = std::get<int32_t>(*vbi0_obs);
    vbi.vbi_data[1] = std::get<int32_t>(*vbi1_obs);
    vbi.vbi_data[2] = std::get<int32_t>(*vbi2_obs);
    any_written |= write_vbi(db_field_id, vbi);
  }

  // Extract and write VITC data (from VitcObserver)
  // Check if vitc data is present - the observer stores timecode components
  auto vitc_present_obs = context.get(source_field_id, "vitc", "present");
  if (vitc_present_obs && std::holds_alternative<bool>(*vitc_present_obs) &&
      std::get<bool>(*vitc_present_obs)) {
    // VITC stores the raw BCD bytes
    auto v0_obs = context.get(source_field_id, "vitc", "vitc0");
    auto v1_obs = context.get(source_field_id, "vitc", "vitc1");
    auto v2_obs = context.get(source_field_id, "vitc", "vitc2");
    auto v3_obs = context.get(source_field_id, "vitc", "vitc3");
    auto v4_obs = context.get(source_field_id, "vitc", "vitc4");
    auto v5_obs = context.get(source_field_id, "vitc", "vitc5");
    auto v6_obs = context.get(source_field_id, "vitc", "vitc6");
    auto v7_obs = context.get(source_field_id, "vitc", "vitc7");
    if (v0_obs && v1_obs && v2_obs && v3_obs && v4_obs && v5_obs && v6_obs &&
        v7_obs && std::holds_alternative<int32_t>(*v0_obs) &&
        std::holds_alternative<int32_t>(*v1_obs) &&
        std::holds_alternative<int32_t>(*v2_obs) &&
        std::holds_alternative<int32_t>(*v3_obs) &&
        std::holds_alternative<int32_t>(*v4_obs) &&
        std::holds_alternative<int32_t>(*v5_obs) &&
        std::holds_alternative<int32_t>(*v6_obs) &&
        std::holds_alternative<int32_t>(*v7_obs)) {
      VitcData vitc;
      vitc.in_use = true;
      vitc.vitc_data[0] = std::get<int32_t>(*v0_obs);
      vitc.vitc_data[1] = std::get<int32_t>(*v1_obs);
      vitc.vitc_data[2] = std::get<int32_t>(*v2_obs);
      vitc.vitc_data[3] = std::get<int32_t>(*v3_obs);
      vitc.vitc_data[4] = std::get<int32_t>(*v4_obs);
      vitc.vitc_data[5] = std::get<int32_t>(*v5_obs);
      vitc.vitc_data[6] = std::get<int32_t>(*v6_obs);
      vitc.vitc_data[7] = std::get<int32_t>(*v7_obs);
      any_written |= write_vitc(db_field_id, vitc);
    }
  }

  // Extract and write closed caption data (from ClosedCaptionObserver)
  auto cc_present_obs =
      context.get(source_field_id, "closed_caption", "present");
  if (cc_present_obs && std::holds_alternative<bool>(*cc_present_obs) &&
      std::get<bool>(*cc_present_obs)) {
    auto data0_obs = context.get(source_field_id, "closed_caption", "data0");
    auto data1_obs = context.get(source_field_id, "closed_caption", "data1");

    if (data0_obs && data1_obs && std::holds_alternative<int32_t>(*data0_obs) &&
        std::holds_alternative<int32_t>(*data1_obs)) {
      ClosedCaptionData cc;
      cc.in_use = true;
      cc.data0 = std::get<int32_t>(*data0_obs);
      cc.data1 = std::get<int32_t>(*data1_obs);
      any_written |= write_closed_caption(db_field_id, cc);
    }
  }

  // Extract and write VITS metrics (from WhiteSNRObserver and
  // BlackPSNRObserver)
  auto white_snr_obs = context.get(source_field_id, "white_snr", "snr_db");
  auto black_psnr_obs = context.get(source_field_id, "black_psnr", "psnr_db");

  if (white_snr_obs || black_psnr_obs) {
    VitsMetrics metrics;
    metrics.in_use = true;
    metrics.white_snr =
        (white_snr_obs && std::holds_alternative<double>(*white_snr_obs))
            ? std::get<double>(*white_snr_obs)
            : 0.0;
    metrics.black_psnr =
        (black_psnr_obs && std::holds_alternative<double>(*black_psnr_obs))
            ? std::get<double>(*black_psnr_obs)
            : 0.0;
    any_written |= write_vits_metrics(db_field_id, metrics);
  }

  return any_written;
}

bool TBCMetadataWriter::begin_transaction() {
  return is_open_ && impl_->exec_sql("BEGIN TRANSACTION");
}

bool TBCMetadataWriter::commit_transaction() {
  return is_open_ && impl_->exec_sql("COMMIT");
}

bool TBCMetadataWriter::rollback_transaction() {
  return is_open_ && impl_->exec_sql("ROLLBACK");
}

}  // namespace orc
