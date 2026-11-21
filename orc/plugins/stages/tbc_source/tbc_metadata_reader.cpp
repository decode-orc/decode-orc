/*
 * File:        tbc_metadata_reader.cpp
 * Module:      orc-stage-plugin-tbc-source
 * Purpose:     TBC SQLite metadata reader implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "tbc_metadata_reader.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/logging.h>
#include <sqlite3.h>

#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>

#include "tbc_level_derivation.h"

// Windows compatibility for strcasecmp
#ifdef _WIN32
#define strcasecmp _stricmp
#endif

namespace orc {

// ============================================================================
// TBCMetadataSqliteReader::Impl (Private implementation using SQLite)
// ============================================================================

class TBCMetadataSqliteReader::Impl {
 public:
  sqlite3* db = nullptr;
  int capture_id = 1;

  mutable std::mutex cache_mutex_;
  std::map<FieldID, FieldMetadata> metadata_cache_;
  std::map<FieldID, std::vector<DropoutInfo>> dropout_cache_;
  bool cache_loaded_ = false;

  // query_field_dropouts(): one statement, reused under its own mutex.
  mutable std::mutex field_dropouts_mutex_;
  mutable sqlite3_stmt* field_dropouts_stmt_ = nullptr;

  ~Impl() {
    if (field_dropouts_stmt_) sqlite3_finalize(field_dropouts_stmt_);
    if (db) {
      sqlite3_close(db);
      db = nullptr;
    }
  }

  int get_int(sqlite3_stmt* stmt, int col, int default_val = -1) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return default_val;
    return sqlite3_column_int(stmt, col);
  }

  std::optional<int> get_optional_int(sqlite3_stmt* stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_int(stmt, col);
  }

  std::optional<int64_t> get_optional_int64(sqlite3_stmt* stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_int64(stmt, col);
  }

  double get_double(sqlite3_stmt* stmt, int col, double default_val = -1.0) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return default_val;
    return sqlite3_column_double(stmt, col);
  }

  std::optional<double> get_optional_double(sqlite3_stmt* stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_double(stmt, col);
  }

  bool get_bool(sqlite3_stmt* stmt, int col, bool default_val = false) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return default_val;
    return sqlite3_column_int(stmt, col) != 0;
  }

  std::optional<bool> get_optional_bool(sqlite3_stmt* stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_int(stmt, col) != 0;
  }

  std::string get_string(sqlite3_stmt* stmt, int col,
                         const std::string& default_val = "") {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return default_val;
    const char* text =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return text ? std::string(text) : default_val;
  }
};

// ============================================================================
// TBCMetadataSqliteReader implementation
// ============================================================================

TBCMetadataSqliteReader::TBCMetadataSqliteReader()
    : impl_(std::make_unique<Impl>()), is_open_(false) {}

TBCMetadataSqliteReader::~TBCMetadataSqliteReader() { close(); }

bool TBCMetadataSqliteReader::open(const std::string& filename) {
  close();

  int rc = sqlite3_open_v2(filename.c_str(), &impl_->db, SQLITE_OPEN_READONLY,
                           nullptr);

  if (rc != SQLITE_OK) {
    if (impl_->db) {
      sqlite3_close(impl_->db);
      impl_->db = nullptr;
    }
    return false;
  }

  is_open_ = true;
  return true;
}

bool TBCMetadataSqliteReader::open_from_handle(sqlite3* db) {
  close();
  if (!db) return false;
  impl_->db = db;
  is_open_ = true;
  return true;
}

void TBCMetadataSqliteReader::close() {
  {
    std::lock_guard<std::mutex> lock(impl_->field_dropouts_mutex_);
    if (impl_->field_dropouts_stmt_) {
      sqlite3_finalize(impl_->field_dropouts_stmt_);
      impl_->field_dropouts_stmt_ = nullptr;
    }
  }
  if (impl_->db) {
    sqlite3_close(impl_->db);
    impl_->db = nullptr;
  }
  is_open_ = false;
}

std::optional<SourceParameters>
TBCMetadataSqliteReader::read_video_parameters() {
  if (!is_open_) {
    ORC_LOG_DEBUG("read_video_parameters: Metadata database is not open");
    return std::nullopt;
  }

  SourceParameters params;
  const char* sql_with_blanking =
      "SELECT system, video_sample_rate, active_video_start, active_video_end, "
      "field_width, field_height, number_of_sequential_fields, "
      "colour_burst_start, colour_burst_end, is_mapped, is_subcarrier_locked, "
      "is_widescreen, blanking_16b_ire, black_16b_ire, white_16b_ire, decoder, "
      "git_branch, git_commit "
      "FROM capture WHERE capture_id = ?";

  const char* sql_without_blanking =
      "SELECT system, video_sample_rate, active_video_start, active_video_end, "
      "field_width, field_height, number_of_sequential_fields, "
      "colour_burst_start, colour_burst_end, is_mapped, is_subcarrier_locked, "
      "is_widescreen, black_16b_ire, white_16b_ire, decoder, git_branch, "
      "git_commit "
      "FROM capture WHERE capture_id = ?";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(impl_->db, sql_with_blanking, -1, &stmt, nullptr);
  bool has_blanking_column = true;

  if (rc != SQLITE_OK) {
    const char* err_msg = sqlite3_errmsg(impl_->db);
    if (std::string(err_msg).find("blanking_16b_ire") != std::string::npos) {
      ORC_LOG_WARN(
          "read_video_parameters: blanking_16b_ire column not found, using "
          "fallback query");
      has_blanking_column = false;
      rc = sqlite3_prepare_v2(impl_->db, sql_without_blanking, -1, &stmt,
                              nullptr);
      if (rc != SQLITE_OK) {
        ORC_LOG_ERROR(
            "read_video_parameters: Failed to prepare fallback SQL: {}",
            sqlite3_errmsg(impl_->db));
        return std::nullopt;
      }
    } else {
      ORC_LOG_ERROR("read_video_parameters: Failed to prepare SQL: {}",
                    err_msg);
      return std::nullopt;
    }
  }

  sqlite3_bind_int(stmt, 1, impl_->capture_id);

  int step_rc = sqlite3_step(stmt);
  if (step_rc == SQLITE_ROW) {
    params.system = video_system_from_string(impl_->get_string(stmt, 0));
    params.active_video_start = impl_->get_int(stmt, 2);
    params.active_video_end = impl_->get_int(stmt, 3);
    const int32_t db_sequential_fields = impl_->get_int(stmt, 6);
    params.number_of_sequential_frames =
        (db_sequential_fields > 0) ? db_sequential_fields / 2 : -1;
    params.is_mapped = impl_->get_bool(stmt, 9);
    params.is_widescreen = impl_->get_bool(stmt, 11);

    int col_offset = 12;
    if (has_blanking_column) {
      col_offset += 3;
      params.decoder = impl_->get_string(stmt, col_offset);
      params.git_branch = impl_->get_string(stmt, col_offset + 1);
      params.git_commit = impl_->get_string(stmt, col_offset + 2);
    } else {
      col_offset += 2;
      params.decoder = impl_->get_string(stmt, col_offset);
      params.git_branch = impl_->get_string(stmt, col_offset + 1);
      params.git_commit = impl_->get_string(stmt, col_offset + 2);
    }

    // EBU Tech. 3280-E §1.1 (PAL) / SMPTE 244M-2003 §4.1 (NTSC) /
    // ITU-R BT.1700-1 Annex 1 Part B (PAL_M).
    switch (params.system) {
      case VideoSystem::PAL:
        params.frame_width_nominal = kPalSamplesPerLineNominal;
        params.frame_height = kPalFrameLines;
        break;
      case VideoSystem::PAL_M:
        params.frame_width_nominal = kPalMSamplesPerLine;
        params.frame_height = kPalMFrameLines;
        break;
      default:
        params.frame_width_nominal = kNtscSamplesPerLine;
        params.frame_height = kNtscFrameLines;
        break;
    }

    if (params.system == VideoSystem::PAL) {
      params.first_active_frame_line = 44;
      params.last_active_frame_line = 620;
    } else {
      params.first_active_frame_line = 40;
      params.last_active_frame_line = 525;
    }

    sqlite3_finalize(stmt);
    ORC_LOG_DEBUG(
        "read_video_parameters: Successfully read video parameters from "
        "capture_id {}",
        impl_->capture_id);
    return params;
  } else if (step_rc == SQLITE_DONE) {
    ORC_LOG_ERROR(
        "read_video_parameters: No capture record found for capture_id {}",
        impl_->capture_id);
  } else {
    ORC_LOG_ERROR("read_video_parameters: SQL execution error: {}",
                  sqlite3_errmsg(impl_->db));
  }

  sqlite3_finalize(stmt);
  return std::nullopt;
}

std::optional<TbcDomainLevels>
TBCMetadataSqliteReader::read_tbc_domain_levels() {
  if (!is_open_) return std::nullopt;

  const char* sql_with_blanking =
      "SELECT blanking_16b_ire, white_16b_ire, black_16b_ire FROM capture "
      "WHERE capture_id = ?";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(impl_->db, sql_with_blanking, -1, &stmt, nullptr);
  bool has_blanking_column = true;

  if (rc != SQLITE_OK) {
    const char* sql_without_blanking =
        "SELECT black_16b_ire, white_16b_ire, system FROM capture WHERE "
        "capture_id = ?";
    has_blanking_column = false;
    rc =
        sqlite3_prepare_v2(impl_->db, sql_without_blanking, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return std::nullopt;
  }

  sqlite3_bind_int(stmt, 1, impl_->capture_id);

  TbcDomainLevels levels;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    if (has_blanking_column) {
      levels.blanking_16b = sqlite3_column_int(stmt, 0);
      levels.white_16b = sqlite3_column_int(stmt, 1);
      if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
        levels.black_16b = sqlite3_column_int(stmt, 2);
      }
    } else {
      // Pre-blanking-column schema: only picture black and white are stored.
      // Derive the 0 IRE blanking (setup subtraction / NTSC-J detection) the
      // same way as the JSON reader — see tbc_level_derivation.h.
      const VideoSystem system =
          video_system_from_string(impl_->get_string(stmt, 2));
      levels = derive_tbc_domain_levels(system, sqlite3_column_int(stmt, 0),
                                        sqlite3_column_int(stmt, 1));
    }
  } else {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  sqlite3_finalize(stmt);

  if (!levels.is_valid()) {
    ORC_LOG_WARN(
        "read_tbc_domain_levels: invalid levels (blanking={}, white={}); "
        "caller will use defaults",
        levels.blanking_16b, levels.white_16b);
    return std::nullopt;
  }

  return levels;
}

std::optional<PcmAudioParameters>
TBCMetadataSqliteReader::read_pcm_audio_parameters() {
  if (!is_open_) return std::nullopt;

  PcmAudioParameters params;
  const char* sql =
      "SELECT sample_rate, bits, is_signed, is_little_endian "
      "FROM pcm_audio_parameters WHERE capture_id = ?";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return std::nullopt;

  sqlite3_bind_int(stmt, 1, impl_->capture_id);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    params.sample_rate = impl_->get_double(stmt, 0);
    params.bits = impl_->get_int(stmt, 1);
    params.is_signed = impl_->get_bool(stmt, 2);
    params.is_little_endian = impl_->get_bool(stmt, 3);
    sqlite3_finalize(stmt);
    return params;
  }

  sqlite3_finalize(stmt);
  return std::nullopt;
}

void TBCMetadataSqliteReader::preload_cache() {
  if (!is_open_) return;

  std::lock_guard<std::mutex> lock(impl_->cache_mutex_);
  if (!impl_->cache_loaded_) {
    ORC_LOG_DEBUG("Preloading metadata cache from database");
    impl_->metadata_cache_ = read_all_field_metadata();
    read_all_dropouts();
    impl_->cache_loaded_ = true;
    ORC_LOG_DEBUG("Preloaded {} field metadata records and dropouts",
                  impl_->metadata_cache_.size());
  }
}

std::optional<FieldMetadata> TBCMetadataSqliteReader::read_field_metadata(
    FieldID field_id) {
  if (!is_open_ || !field_id.is_valid()) return std::nullopt;

  {
    std::lock_guard<std::mutex> lock(impl_->cache_mutex_);
    if (!impl_->cache_loaded_) {
      impl_->metadata_cache_ = read_all_field_metadata();
      read_all_dropouts();
      impl_->cache_loaded_ = true;
    }
    auto it = impl_->metadata_cache_.find(field_id);
    if (it != impl_->metadata_cache_.end()) return it->second;
  }

  return std::nullopt;
}

std::map<FieldID, FieldMetadata>
TBCMetadataSqliteReader::read_all_field_metadata() {
  std::map<FieldID, FieldMetadata> result;
  for_each_field_metadata([&result](const FieldMetadata& metadata) {
    result[FieldID(metadata.seq_no)] = metadata;
  });
  return result;
}

void TBCMetadataSqliteReader::for_each_field_metadata(
    const std::function<void(const FieldMetadata&)>& visit) {
  if (!is_open_) return;

  const char* sql =
      "SELECT field_id, is_first_field, sync_conf, median_burst_ire, "
      "audio_samples, pad, disk_loc, file_loc, decode_faults, efm_t_values, "
      "ac3_symbols "
      "FROM field_record WHERE capture_id = ? ORDER BY field_id";
  const char* sql_legacy =
      "SELECT field_id, is_first_field, sync_conf, median_burst_ire, "
      "audio_samples, pad, disk_loc, file_loc, decode_faults, efm_t_values "
      "FROM field_record WHERE capture_id = ? ORDER BY field_id";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  bool has_ac3_symbols = (rc == SQLITE_OK);

  if (!has_ac3_symbols) {
    rc = sqlite3_prepare_v2(impl_->db, sql_legacy, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return;
  }

  sqlite3_bind_int(stmt, 1, impl_->capture_id);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    FieldMetadata metadata;
    metadata.seq_no = impl_->get_int(stmt, 0);
    metadata.is_first_field = impl_->get_optional_bool(stmt, 1);
    metadata.sync_confidence = impl_->get_optional_int(stmt, 2);
    metadata.median_burst_ire = impl_->get_optional_double(stmt, 3);
    metadata.audio_samples = impl_->get_optional_int(stmt, 4);
    metadata.is_pad = impl_->get_optional_bool(stmt, 5);
    metadata.disk_location = impl_->get_optional_double(stmt, 6);
    metadata.file_location = impl_->get_optional_int64(stmt, 7);
    metadata.decode_faults = impl_->get_optional_int(stmt, 8);
    metadata.efm_t_values = impl_->get_optional_int(stmt, 9);
    if (has_ac3_symbols) {
      metadata.ac3rf_symbols = impl_->get_optional_int(stmt, 10);
    }
    visit(metadata);
  }

  sqlite3_finalize(stmt);
}

std::optional<VbiData> TBCMetadataSqliteReader::read_vbi(FieldID field_id) {
  if (!is_open_ || !field_id.is_valid()) return std::nullopt;

  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT * FROM vbi WHERE capture_id = ? AND field_id = ?";
  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return std::nullopt;

  sqlite3_bind_int(stmt, 1, impl_->capture_id);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(field_id.value()));

  VbiData vbi;
  vbi.in_use = false;
  vbi.vbi_data = {0, 0, 0};

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    int col_count = sqlite3_column_count(stmt);
    auto get_col = [&](const char* const names[]) -> std::optional<int> {
      for (int c = 0; c < col_count; ++c) {
        const char* cname = sqlite3_column_name(stmt, c);
        if (!cname) continue;
        for (int i = 0; names[i] != nullptr; ++i) {
          if (strcasecmp(cname, names[i]) == 0) return c;
        }
      }
      return std::nullopt;
    };

    const char* line16_cols[] = {"l16",   "line16", "line_16",
                                 "vbi16", "vbi_16", nullptr};
    const char* line17_cols[] = {"l17",   "line17", "line_17",
                                 "vbi17", "vbi_17", nullptr};
    const char* line18_cols[] = {"l18",   "line18", "line_18",
                                 "vbi18", "vbi_18", nullptr};

    auto c16 = get_col(line16_cols);
    auto c17 = get_col(line17_cols);
    auto c18 = get_col(line18_cols);

    bool found_any = false;
    if (c16.has_value() && sqlite3_column_type(stmt, *c16) != SQLITE_NULL) {
      vbi.vbi_data[0] = sqlite3_column_int(stmt, *c16);
      found_any = true;
    }
    if (c17.has_value() && sqlite3_column_type(stmt, *c17) != SQLITE_NULL) {
      vbi.vbi_data[1] = sqlite3_column_int(stmt, *c17);
      found_any = true;
    }
    if (c18.has_value() && sqlite3_column_type(stmt, *c18) != SQLITE_NULL) {
      vbi.vbi_data[2] = sqlite3_column_int(stmt, *c18);
      found_any = true;
    }
    vbi.in_use = found_any;
  }

  sqlite3_finalize(stmt);
  return vbi.in_use ? std::optional<VbiData>{vbi} : std::nullopt;
}

std::optional<VitcData> TBCMetadataSqliteReader::read_vitc(FieldID field_id) {
  if (!is_open_ || !field_id.is_valid()) return std::nullopt;
  return std::nullopt;
}

std::optional<ClosedCaptionData> TBCMetadataSqliteReader::read_closed_caption(
    FieldID field_id) {
  if (!is_open_ || !field_id.is_valid()) return std::nullopt;

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT data0, data1 FROM closed_caption WHERE capture_id = ? AND "
      "field_id = ?";

  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return std::nullopt;

  sqlite3_bind_int(stmt, 1, impl_->capture_id);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(field_id.value()));

  ClosedCaptionData cc;
  cc.in_use = false;

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    cc.in_use = true;
    cc.data0 = sqlite3_column_int(stmt, 0);
    cc.data1 = sqlite3_column_int(stmt, 1);
  }

  sqlite3_finalize(stmt);
  return cc.in_use ? std::optional<ClosedCaptionData>{cc} : std::nullopt;
}

std::vector<DropoutInfo> TBCMetadataSqliteReader::read_dropouts(
    FieldID field_id) const {
  if (!is_open_ || !field_id.is_valid()) return {};

  std::lock_guard<std::mutex> lock(impl_->cache_mutex_);
  auto it = impl_->dropout_cache_.find(field_id);
  if (it != impl_->dropout_cache_.end()) return it->second;
  return {};
}

void TBCMetadataSqliteReader::read_all_dropouts() {
  if (!is_open_) return;

  impl_->dropout_cache_.clear();

  const char* sql =
      "SELECT field_id, startx, endx, field_line "
      "FROM drop_outs WHERE capture_id = ? ORDER BY field_id";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return;

  sqlite3_bind_int(stmt, 1, impl_->capture_id);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    int64_t field_id_val = sqlite3_column_int64(stmt, 0);
    FieldID field_id(field_id_val);

    DropoutInfo dropout;
    dropout.start_sample = static_cast<uint32_t>(impl_->get_int(stmt, 1));
    dropout.end_sample = static_cast<uint32_t>(impl_->get_int(stmt, 2));
    // TBC database uses 1-based line numbers; convert to 0-based. Guard against
    // the unsigned underflow that a value of 0 (or out-of-range) would produce
    // (0 - 1 == 0xFFFFFFFF), which fed a pathological offset into the preview
    // geometry loops and hung the render worker (issue #209).
    const int64_t field_line = impl_->get_int(stmt, 3);
    if (field_line < 1) continue;
    dropout.line = static_cast<uint32_t>(field_line - 1);

    impl_->dropout_cache_[field_id].push_back(dropout);
  }

  sqlite3_finalize(stmt);
}

std::vector<DropoutInfo> TBCMetadataSqliteReader::query_field_dropouts(
    FieldID field_id) const {
  std::vector<DropoutInfo> result;
  if (!is_open_ || !field_id.is_valid()) return result;

  std::lock_guard<std::mutex> lock(impl_->field_dropouts_mutex_);
  if (!impl_->field_dropouts_stmt_) {
    const char* sql =
        "SELECT startx, endx, field_line FROM drop_outs "
        "WHERE capture_id = ? AND field_id = ? "
        "ORDER BY field_line, startx, endx";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &impl_->field_dropouts_stmt_,
                           nullptr) != SQLITE_OK) {
      impl_->field_dropouts_stmt_ = nullptr;
      return result;
    }
  }
  sqlite3_stmt* stmt = impl_->field_dropouts_stmt_;
  sqlite3_reset(stmt);
  sqlite3_bind_int(stmt, 1, impl_->capture_id);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(field_id.value()));
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    // As read_all_dropouts(): 1-based lines, and a line below 1 is skipped.
    const int64_t field_line = impl_->get_int(stmt, 2);
    if (field_line < 1) continue;
    DropoutInfo dropout;
    dropout.start_sample = static_cast<uint32_t>(impl_->get_int(stmt, 0));
    dropout.end_sample = static_cast<uint32_t>(impl_->get_int(stmt, 1));
    dropout.line = static_cast<uint32_t>(field_line - 1);
    result.push_back(dropout);
  }
  return result;
}

std::optional<DropoutData> TBCMetadataSqliteReader::read_dropout(
    FieldID field_id) const {
  auto dropouts = read_dropouts(field_id);
  if (dropouts.empty()) return std::nullopt;
  DropoutData data;
  data.dropouts = dropouts;
  return data;
}

int32_t TBCMetadataSqliteReader::get_field_record_count() const {
  if (!is_open_) return -1;

  const char* sql = "SELECT COUNT(*) FROM field_record WHERE capture_id = ?";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_int(stmt, 1, impl_->capture_id);

  int32_t count = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }

  sqlite3_finalize(stmt);
  return count;
}

bool TBCMetadataSqliteReader::validate_metadata(
    std::string* error_message) const {
  if (!is_open_) {
    if (error_message) {
      *error_message = "Metadata database is not open";
    }
    ORC_LOG_ERROR("validate_metadata: {}",
                  error_message ? *error_message : "Unknown error");
    return false;
  }

  auto params_opt =
      const_cast<TBCMetadataSqliteReader*>(this)->read_video_parameters();
  if (!params_opt) {
    if (error_message) {
      *error_message = "Failed to read video parameters from metadata";
    }
    ORC_LOG_ERROR("validate_metadata: {} - check debug logs for details",
                  error_message ? *error_message : "Unknown error");
    return false;
  }

  const auto& params = *params_opt;

  if (params.number_of_sequential_frames <= 0) {
    if (error_message) {
      *error_message =
          "Metadata does not specify valid number_of_sequential_frames (" +
          std::to_string(params.number_of_sequential_frames) + ")";
    }
    ORC_LOG_ERROR("validate_metadata: {}", *error_message);
    return false;
  }

  int32_t field_record_count = get_field_record_count();
  if (field_record_count < 0) {
    if (error_message) {
      *error_message = "Failed to count field records in database";
    }
    ORC_LOG_ERROR("validate_metadata: {}", *error_message);
    return false;
  }

  const int32_t expected_field_count = params.number_of_sequential_frames * 2;
  if (field_record_count != expected_field_count) {
    if (error_message) {
      *error_message =
          "Metadata inconsistency: capture table specifies " +
          std::to_string(expected_field_count) +
          " fields, but field_record table contains " +
          std::to_string(field_record_count) +
          " records. This TBC file has inconsistent metadata, likely from a "
          "buggy ld-decode version or interrupted capture.";
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
