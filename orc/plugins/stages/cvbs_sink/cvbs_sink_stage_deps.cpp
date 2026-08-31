/*
 * File:        cvbs_sink_stage_deps.cpp
 * Module:      orc-core
 * Purpose:     CVBSSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "cvbs_sink_stage_deps.h"

#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/observation/colour_frame_phase_query.h>
#include <orc/support/logging.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <utility>
#include <vector>

#include "cvbs_sink_container.h"
#include "cvbs_sink_phase_sequence.h"

namespace orc {

namespace {

// ---------------------------------------------------------------------------
// Video-system helpers
// ---------------------------------------------------------------------------

// Video Standard Preset name for the .meta preset column. Uses the spec
// spelling (PAL_M with an underscore), unlike video_system_to_string().
const char* preset_name(VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
      return "PAL";
    case VideoSystem::NTSC:
      return "NTSC";
    case VideoSystem::PAL_M:
      return "PAL_M";
    default:
      return nullptr;
  }
}

// Spec blanking level for CVBS_S16_4FSC encoding when the representation
// provides no SourceParameters.
int32_t default_blanking_level(VideoSystem system) {
  // EBU Tech. 3280-E §1.1 (PAL) / SMPTE 244M-2003 (NTSC, PAL_M).
  return (system == VideoSystem::PAL) ? kPalBlanking : kNtscBlanking;
}

// ---------------------------------------------------------------------------
// Per-frame sidecar bookkeeping
// ---------------------------------------------------------------------------

// One dropout_run row (dropout extension format).
struct DropoutRow {
  FrameID frame_id = 0;  // output-file frame index
  uint64_t sample_start = 0;
  uint32_t sample_count = 0;
  uint8_t severity = 0;
};

// One efm_frame / ac3_frame row (EFM / AC3 extension formats).
struct ExtensionRow {
  FrameID frame_id = 0;  // output-file frame index
  uint64_t offset = 0;
  uint32_t count = 0;
};

// ---------------------------------------------------------------------------
// SQLite helpers
// ---------------------------------------------------------------------------

// Create a fresh SQLite database at path (removing any existing file) and
// run the given schema SQL. Returns nullptr and sets error_message on
// failure; the caller owns the returned handle.
sqlite3* create_sidecar_db(const std::string& path, const char* schema_sql,
                           std::string& error_message) {
  std::error_code ec;
  std::filesystem::remove(path, ec);

  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      nullptr) != SQLITE_OK) {
    error_message = "Failed to create '" + path +
                    "': " + (db ? sqlite3_errmsg(db) : "unknown error");
    if (db) sqlite3_close(db);
    return nullptr;
  }

  char* exec_err = nullptr;
  if (sqlite3_exec(db, schema_sql, nullptr, nullptr, &exec_err) != SQLITE_OK) {
    error_message = "Failed to initialise '" + path +
                    "': " + (exec_err ? exec_err : "unknown error");
    sqlite3_free(exec_err);
    sqlite3_close(db);
    return nullptr;
  }
  return db;
}

// Write the core <base>.meta metadata database.
// CVBS file format spec v1.6.0: Metadata Schema (PRAGMA user_version = 11).
bool write_core_metadata(
    const std::string& meta_path, VideoSystem system,
    const CVBSSinkWriteConfig& config, uint64_t frames_written,
    const char* signal_state_preset, std::optional<bool> sequence_continuous,
    std::optional<int32_t> black_level, bool has_nonstandard_values,
    const std::vector<CVBSAudioChannelPairMetaRow>& audio_channel_pairs,
    std::string& error_message) {
  sqlite3* db =
      create_sidecar_db(meta_path, kCVBSCoreMetaSchemaSql, error_message);
  if (!db) return false;

  constexpr const char* kInsert =
      "INSERT INTO cvbs_file (cvbs_file_id, preset, sample_encoding_preset, "
      "signal_state_preset, sequence_continuous, signal_type, decoder, "
      "number_of_sequential_frames, black_level, has_nonstandard_values, "
      "capture_notes) "
      "VALUES (1, ?, ?, ?, ?, ?, 'other', ?, ?, ?, ?)";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, kInsert, -1, &stmt, nullptr) != SQLITE_OK) {
    error_message =
        "Failed to prepare metadata insert: " + std::string(sqlite3_errmsg(db));
    sqlite3_close(db);
    return false;
  }

  sqlite3_bind_text(stmt, 1, preset_name(system), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, cvbs_sample_encoding_name(config.sample_encoding),
                    -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, signal_state_preset, -1, SQLITE_STATIC);
  if (sequence_continuous.has_value()) {
    sqlite3_bind_int(stmt, 4, *sequence_continuous ? 1 : 0);
  } else {
    sqlite3_bind_null(stmt, 4);  // continuity unknown
  }
  sqlite3_bind_text(stmt, 5, config.signal_type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(frames_written));
  if (black_level.has_value()) {
    sqlite3_bind_int(stmt, 7, *black_level);
  } else {
    sqlite3_bind_null(stmt, 7);
  }
  if (has_nonstandard_values) {
    sqlite3_bind_int(stmt, 8, 1);
  } else {
    sqlite3_bind_null(stmt, 8);
  }
  if (!config.capture_notes.empty()) {
    sqlite3_bind_text(stmt, 9, config.capture_notes.c_str(), -1,
                      SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 9);
  }

  bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!ok) {
    error_message =
        "Failed to write metadata row: " + std::string(sqlite3_errmsg(db));
  }
  sqlite3_finalize(stmt);

  // Per-pair rows (CVBS file format spec v1.4.0): exactly one
  // audio_channel_pair row per channel pair file.
  if (ok && !audio_channel_pairs.empty()) {
    constexpr const char* kPairInsert =
        "INSERT INTO audio_channel_pair (channel_pair, description) "
        "VALUES (?, ?)";
    sqlite3_stmt* pair_stmt = nullptr;
    if (sqlite3_prepare_v2(db, kPairInsert, -1, &pair_stmt, nullptr) !=
        SQLITE_OK) {
      error_message = "Failed to prepare audio_channel_pair insert: " +
                      std::string(sqlite3_errmsg(db));
      ok = false;
    } else {
      for (const CVBSAudioChannelPairMetaRow& row : audio_channel_pairs) {
        sqlite3_bind_int(pair_stmt, 1, row.channel_pair);
        if (!row.description.empty()) {
          sqlite3_bind_text(pair_stmt, 2, row.description.c_str(), -1,
                            SQLITE_TRANSIENT);
        } else {
          sqlite3_bind_null(pair_stmt, 2);
        }
        if (sqlite3_step(pair_stmt) != SQLITE_DONE) {
          error_message = "Failed to write audio_channel_pair row: " +
                          std::string(sqlite3_errmsg(db));
          ok = false;
          break;
        }
        sqlite3_reset(pair_stmt);
      }
      sqlite3_finalize(pair_stmt);
    }
  }

  sqlite3_close(db);
  return ok;
}

// Write the <base>.dropouts.meta sidecar.
// CVBS dropout extension format (PRAGMA user_version = 5).
bool write_dropout_sidecar(const std::string& path,
                           const std::vector<DropoutRow>& rows,
                           std::string& error_message) {
  constexpr const char* kSchema =
      "PRAGMA user_version = 5;"
      "CREATE TABLE dropout_run ("
      "    cvbs_file_id    INTEGER NOT NULL,"
      "    frame_id        INTEGER NOT NULL"
      "        CHECK (frame_id >= 0),"
      "    sample_start    INTEGER NOT NULL"
      "        CHECK (sample_start >= 0),"
      "    sample_count    INTEGER NOT NULL"
      "        CHECK (sample_count > 0),"
      "    severity        INTEGER NOT NULL"
      "        CHECK (severity >= 0 AND severity <= 100),"
      "    PRIMARY KEY (cvbs_file_id, frame_id, sample_start)"
      ");"
      "CREATE INDEX idx_dropout_run_frame"
      "    ON dropout_run (cvbs_file_id, frame_id);";

  sqlite3* db = create_sidecar_db(path, kSchema, error_message);
  if (!db) return false;

  constexpr const char* kInsert =
      "INSERT OR REPLACE INTO dropout_run (cvbs_file_id, frame_id, "
      "sample_start, sample_count, severity) VALUES (1, ?, ?, ?, ?)";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, kInsert, -1, &stmt, nullptr) != SQLITE_OK) {
    error_message =
        "Failed to prepare dropout insert: " + std::string(sqlite3_errmsg(db));
    sqlite3_close(db);
    return false;
  }

  bool ok = true;
  sqlite3_exec(db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
  for (const DropoutRow& row : rows) {
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(row.frame_id));
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(row.sample_start));
    sqlite3_bind_int(stmt, 3, static_cast<int>(row.sample_count));
    sqlite3_bind_int(stmt, 4,
                     static_cast<int>(std::min<uint8_t>(row.severity, 100)));
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      error_message =
          "Failed to write dropout row: " + std::string(sqlite3_errmsg(db));
      ok = false;
      break;
    }
    sqlite3_reset(stmt);
  }
  sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return ok;
}

// Write an EFM or AC3 frame-index sidecar (<base>.efm.meta / <base>.ac3.meta).
// EFM extension format / AC3 extension format (PRAGMA user_version = 1);
// the two schemas are identical apart from the table and index names.
bool write_extension_sidecar(const std::string& path,
                             const std::string& table_name,
                             const std::vector<ExtensionRow>& rows,
                             std::string& error_message) {
  const std::string schema =
      "PRAGMA user_version = 1;"
      "CREATE TABLE " +
      table_name +
      " ("
      "    cvbs_file_id    INTEGER NOT NULL,"
      "    frame_id        INTEGER NOT NULL"
      "        CHECK (frame_id >= 0),"
      "    t_value_offset  INTEGER NOT NULL"
      "        CHECK (t_value_offset >= 0),"
      "    t_value_count   INTEGER NOT NULL"
      "        CHECK (t_value_count >= 0),"
      "    PRIMARY KEY (cvbs_file_id, frame_id)"
      ");"
      "CREATE INDEX idx_" +
      table_name + "_frame ON " + table_name + " (cvbs_file_id, frame_id);";

  sqlite3* db = create_sidecar_db(path, schema.c_str(), error_message);
  if (!db) return false;

  const std::string insert =
      "INSERT INTO " + table_name +
      " (cvbs_file_id, frame_id, t_value_offset, t_value_count) "
      "VALUES (1, ?, ?, ?)";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, insert.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    error_message = "Failed to prepare " + table_name +
                    " insert: " + std::string(sqlite3_errmsg(db));
    sqlite3_close(db);
    return false;
  }

  bool ok = true;
  sqlite3_exec(db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
  for (const ExtensionRow& row : rows) {
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(row.frame_id));
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(row.offset));
    sqlite3_bind_int(stmt, 3, static_cast<int>(row.count));
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      error_message = "Failed to write " + table_name +
                      " row: " + std::string(sqlite3_errmsg(db));
      ok = false;
      break;
    }
    sqlite3_reset(stmt);
  }
  sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return ok;
}

// ---------------------------------------------------------------------------
// Stale output removal
// ---------------------------------------------------------------------------

// Remove any output files from a previous run so a re-run never leaves a
// stale sidecar describing data that is no longer being written.
void remove_stale_outputs(const std::string& base) {
  static const char* kExtensions[] = {
      ".cvbs", ".cvbsy",    ".cvbsc", ".meta",    ".dropouts.meta",
      ".efm",  ".efm.meta", ".ac3",   ".ac3.meta"};
  std::error_code ec;
  for (const char* ext : kExtensions) {
    std::filesystem::remove(base + ext, ec);
  }
  for (size_t pair = 0; pair < kMaxAudioChannelPairs; ++pair) {
    std::filesystem::remove(cvbs_audio_pair_path(base, pair), ec);
  }
}

}  // namespace

void CVBSSinkStageDeps::init(TriggerProgressCallback progress_callback,
                             std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

CVBSSinkWriteResult CVBSSinkStageDeps::write_cvbs(
    const VideoFrameRepresentation* representation,
    const CVBSSinkWriteConfig& config) {
  if (!representation) {
    return {false, 0, "Input representation is null"};
  }

  const auto frame_rng = representation->frame_range();
  const uint64_t total_frames = frame_rng.count();
  if (total_frames == 0) {
    return {false, 0, "Input representation contains no frames"};
  }

  // --- Determine the video system and blanking level ---
  VideoSystem system = VideoSystem::Unknown;
  int32_t blanking_level = -1;
  std::optional<int32_t> black_level_override;
  bool has_nonstandard_values = false;

  if (const auto params = representation->get_video_parameters()) {
    system = params->system;
    blanking_level = params->blanking_level;
    has_nonstandard_values = params->has_nonstandard_values;
    if (has_nonstandard_values) black_level_override = params->black_level;
  } else {
    for (FrameID fid = frame_rng.first; fid <= frame_rng.last; ++fid) {
      const auto desc = representation->get_frame_descriptor(fid);
      if (!desc.has_value()) continue;
      system = desc->system;
      if (desc->black_level_override.has_value()) {
        black_level_override = desc->black_level_override;
        has_nonstandard_values = true;
      }
      break;
    }
  }

  if (!preset_name(system)) {
    return {false, 0, "Cannot determine the video system of the input"};
  }
  if (blanking_level < 0) blanking_level = default_blanking_level(system);

  // --- Signal type ---
  const bool yc = (config.signal_type == "yc");
  if (!yc && config.signal_type != "composite") {
    return {false, 0, "Unknown signal_type '" + config.signal_type + "'"};
  }
  if (yc && !representation->has_separate_channels()) {
    return {false, 0,
            "signal_type 'yc' requires an input with separate Y/C channels"};
  }

  const std::string base = derive_cvbs_output_base(config.output_base_path);
  if (base.empty()) {
    return {false, 0, "Output path is empty"};
  }

  remove_stale_outputs(base);

  // --- Open payload stream(s) ---
  const std::string primary_path = base + (yc ? ".cvbsy" : ".cvbs");
  std::ofstream primary(primary_path, std::ios::binary | std::ios::trunc);
  if (!primary) {
    return {false, 0, "Failed to open output file: " + primary_path};
  }

  std::ofstream chroma;
  const std::string chroma_path = base + ".cvbsc";
  if (yc) {
    chroma.open(chroma_path, std::ios::binary | std::ios::trunc);
    if (!chroma) {
      return {false, 0, "Failed to open output file: " + chroma_path};
    }
  }

  // --- Extension streams (opened only when the input carries the data) ---
  // Every pipeline audio channel pair is written to <base>_audio_<p>.wav
  // with p = pipeline pair index (single digit, CVBS spec v1.4.0).
  struct AudioChannelPairOutput {
    size_t pair = 0;
    AudioChannelPairDescriptor desc;
    std::string path;
    std::ofstream out;
    uint32_t data_bytes = 0;
  };
  std::vector<AudioChannelPairOutput> pair_outputs;
  const size_t audio_pair_count = std::min(
      representation->audio_channel_pair_count(), kMaxAudioChannelPairs);
  for (size_t pair = 0; pair < audio_pair_count; ++pair) {
    const auto desc = representation->get_audio_channel_pair_descriptor(pair);
    if (!desc) continue;
    AudioChannelPairOutput output;
    output.pair = pair;
    output.desc = *desc;
    output.path = cvbs_audio_pair_path(base, pair);
    pair_outputs.push_back(std::move(output));
  }
  const bool write_efm = representation->has_efm();
  const bool write_ac3 = representation->has_ac3_rf();

  // Audio is gathered frame by frame inside the frame loop, so the WAV data
  // size is only known afterwards; open with a placeholder header and patch
  // it once the loop completes.  All pipeline audio is 48 kHz synchronous
  // 24-bit (SMPTE 272M-1994 §1.2/§1.3) — exact for every system.
  for (AudioChannelPairOutput& pair_out : pair_outputs) {
    pair_out.out.open(pair_out.path, std::ios::binary | std::ios::trunc);
    if (!pair_out.out) {
      return {false, 0, "Failed to open output file: " + pair_out.path};
    }
    const auto header = make_cvbs_audio_wav_header(0);
    pair_out.out.write(reinterpret_cast<const char*>(header.data()),
                       static_cast<std::streamsize>(header.size()));
  }

  const std::string efm_path = base + ".efm";
  std::ofstream efm_out;
  std::vector<ExtensionRow> efm_rows;
  uint64_t efm_offset = 0;
  if (write_efm) {
    efm_out.open(efm_path, std::ios::binary | std::ios::trunc);
    if (!efm_out) {
      return {false, 0, "Failed to open output file: " + efm_path};
    }
  }

  const std::string ac3_path = base + ".ac3";
  std::ofstream ac3_out;
  std::vector<ExtensionRow> ac3_rows;
  uint64_t ac3_offset = 0;
  if (write_ac3) {
    ac3_out.open(ac3_path, std::ios::binary | std::ios::trunc);
    if (!ac3_out) {
      return {false, 0, "Failed to open output file: " + ac3_path};
    }
  }

  ORC_LOG_DEBUG("CVBSSinkDeps: Writing {} frames to {} ({}, {})", total_frames,
                primary_path, config.signal_type,
                cvbs_sample_encoding_name(config.sample_encoding));

  // --- Frame loop ---
  std::vector<DropoutRow> dropout_rows;
  std::vector<uint16_t> encode_buffer;
  uint64_t frames_written = 0;

  // The .meta signal_state_preset and sequence_continuous state what this
  // file is, not what the input claimed to be: any stage upstream may have
  // reordered, dropped or synthesised frames.  Measure the colour sequence of
  // what is actually written and let the result decide (issue #287; CVBS file
  // format spec v1.6.0 splits phase lock from continuity).
  ColourPhaseSequenceCheck phase_check(system);

  // Encode one flat frame plane and append it to the stream.
  const auto write_plane =
      [&](std::ofstream& out,
          const VideoFrameRepresentation::sample_type* samples, size_t count) {
        encode_buffer.resize(count);
        for (size_t i = 0; i < count; ++i) {
          encode_buffer[i] = encode_cvbs_u10_sample(
              samples[i], config.sample_encoding, blanking_level);
        }
        out.write(reinterpret_cast<const char*>(encode_buffer.data()),
                  static_cast<std::streamsize>(count * sizeof(uint16_t)));
      };

  for (FrameID fid = frame_rng.first; fid <= frame_rng.last; ++fid) {
    if (cancel_requested_ && cancel_requested_->load()) {
      return {false, frames_written, "Cancelled by user"};
    }

    if (!representation->has_frame(fid)) {
      continue;
    }

    const auto desc = representation->get_frame_descriptor(fid);
    if (!desc.has_value()) {
      ORC_LOG_WARN("CVBSSinkDeps: No descriptor for frame {}, skipping", fid);
      continue;
    }
    const size_t sample_count = desc->samples_total;

    const auto* primary_data = yc ? representation->get_frame_luma(fid)
                                  : representation->get_frame(fid);
    const auto* chroma_data =
        yc ? representation->get_frame_chroma(fid) : nullptr;

    if (!primary_data || sample_count == 0 || (yc && !chroma_data)) {
      ORC_LOG_WARN("CVBSSinkDeps: Empty frame data for frame {}, skipping",
                   fid);
      continue;
    }

    write_plane(primary, primary_data, sample_count);
    if (yc) write_plane(chroma, chroma_data, sample_count);

    if (!primary || (yc && !chroma)) {
      return {false, frames_written,
              "Write error at frame " + std::to_string(fid)};
    }

    // Frames skipped above shift subsequent frames down, so all sidecar
    // rows use the output-file frame index rather than the source FrameID.
    const FrameID out_frame_id = static_cast<FrameID>(frames_written);

    for (const DropoutRun& run : representation->get_dropout_hints(fid)) {
      if (run.sample_count == 0) continue;
      dropout_rows.push_back(DropoutRow{out_frame_id, run.sample_start,
                                        run.sample_count, run.severity});
    }

    for (AudioChannelPairOutput& pair_out : pair_outputs) {
      // The producer serves exactly audio_pairs_in_frame(fid) stereo pairs;
      // frames without audio get cadence-sized silence so all pair files
      // stay frame-aligned and equal-length by construction.
      const auto audio = representation->get_audio_samples(pair_out.pair, fid);
      const size_t expected_values =
          static_cast<size_t>(audio_pairs_in_frame(fid, system)) * 2;
      const auto bytes = pack_audio_s24le(audio, expected_values);
      pair_out.out.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
      pair_out.data_bytes += static_cast<uint32_t>(bytes.size());
    }

    if (write_efm) {
      const auto efm = representation->get_efm_samples(fid);
      if (!efm.empty()) {
        efm_out.write(reinterpret_cast<const char*>(efm.data()),
                      static_cast<std::streamsize>(efm.size()));
      }
      efm_rows.push_back(ExtensionRow{out_frame_id, efm_offset,
                                      static_cast<uint32_t>(efm.size())});
      efm_offset += efm.size();
    }

    if (write_ac3) {
      const auto ac3 = representation->get_ac3_symbols(fid);
      if (!ac3.empty()) {
        ac3_out.write(reinterpret_cast<const char*>(ac3.data()),
                      static_cast<std::streamsize>(ac3.size()));
      }
      ac3_rows.push_back(ExtensionRow{out_frame_id, ac3_offset,
                                      static_cast<uint32_t>(ac3.size())});
      ac3_offset += ac3.size();
    }

    // Measured after the plane writes: measure_frame_phase() re-fetches the
    // frame, which may invalidate primary_data/chroma_data.
    const auto phase =
        orc::observation::measure_frame_phase(*representation, fid);
    phase_check.observe(phase.field1_phase_id, phase.field2_phase_id);

    ++frames_written;

    if (frames_written % 10 == 0 && progress_callback_) {
      progress_callback_(frames_written, total_frames,
                         "Writing CVBS: frame " +
                             std::to_string(frames_written) + "/" +
                             std::to_string(total_frames));
    }
  }

  primary.close();
  if (yc) chroma.close();
  if (write_efm) efm_out.close();
  if (write_ac3) ac3_out.close();

  if (frames_written == 0) {
    return {false, 0, "No frames could be written"};
  }

  // --- Finalise the WAV headers with the real data sizes ---
  for (AudioChannelPairOutput& pair_out : pair_outputs) {
    const auto header = make_cvbs_audio_wav_header(pair_out.data_bytes);
    pair_out.out.seekp(0, std::ios::beg);
    pair_out.out.write(reinterpret_cast<const char*>(header.data()),
                       static_cast<std::streamsize>(header.size()));
    pair_out.out.close();
    if (!pair_out.out) {
      return {false, frames_written, "Write error in " + pair_out.path};
    }
  }

  // --- Write the .meta core metadata ---
  // One audio_channel_pair row per channel-pair file (CVBS file format spec
  // v1.4.0), channel_pair = pipeline pair index.
  std::string err;
  std::vector<CVBSAudioChannelPairMetaRow> audio_pair_rows;
  for (const AudioChannelPairOutput& pair_out : pair_outputs) {
    CVBSAudioChannelPairMetaRow row;
    row.channel_pair = static_cast<int32_t>(pair_out.pair);
    row.description = pair_out.desc.name;
    audio_pair_rows.push_back(std::move(row));
  }
  if (!write_core_metadata(
          base + ".meta", system, config, frames_written,
          phase_check.signal_state_preset(), phase_check.sequence_continuous(),
          black_level_override, has_nonstandard_values, audio_pair_rows, err)) {
    return {false, frames_written, err};
  }

  // --- Write the extension sidecars ---
  if (!dropout_rows.empty() &&
      !write_dropout_sidecar(base + ".dropouts.meta", dropout_rows, err)) {
    return {false, frames_written, err};
  }
  if (write_efm && !write_extension_sidecar(base + ".efm.meta", "efm_frame",
                                            efm_rows, err)) {
    return {false, frames_written, err};
  }
  if (write_ac3 && !write_extension_sidecar(base + ".ac3.meta", "ac3_frame",
                                            ac3_rows, err)) {
    return {false, frames_written, err};
  }

  ORC_LOG_INFO(
      "CVBSSinkDeps: Wrote {} frames ({} requested) to {} "
      "(dropouts {}, audio channel pairs {}, EFM {}, AC3 {})",
      frames_written, total_frames, primary_path,
      dropout_rows.empty() ? "no" : "yes", audio_pair_rows.size(),
      write_efm ? "yes" : "no", write_ac3 ? "yes" : "no");

  const std::string phase_summary = phase_check.summary();
  if (phase_check.sequence_continuous().value_or(false)) {
    ORC_LOG_INFO("CVBSSinkDeps: {}", phase_summary);
  } else {
    ORC_LOG_WARN("CVBSSinkDeps: {}", phase_summary);
  }

  return {true, frames_written,
          "Success: " + std::to_string(frames_written) + " frames written; " +
              phase_summary};
}

}  // namespace orc
