/*
 * File:        picture_truth.cpp
 * Module:      orc-videosynth-support
 * Purpose:     The component pictures of a still: videosynth's picture truth
 *              (.truth with its .truth.meta sidecar) and a video sink's raw
 *              YUV444P16 decode, both on the 4fsc active-window grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "picture_truth.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <sqlite3.h>

#include <cstring>
#include <fstream>
#include <memory>
#include <utility>

namespace orc_videosynth {

namespace {

// The video sink's YUV444P16 coding (outputwriter.cpp): Y' 16..235 and
// Cb/Cr 16..240 scaled to 16 bits, Cb = (B'-Y') / (1 - Kb) and
// Cr = (R'-Y') / (1 - Kr), and U = kB (B'-Y'), V = kR (R'-Y').
constexpr double kYZero = 16.0 * 256.0;
constexpr double kYScale = 219.0 * 256.0;
constexpr double kCZero = 128.0 * 256.0;
constexpr double kCScale = 112.0 * 256.0;
constexpr double kOneMinusKb = 1.0 - 0.114;
constexpr double kOneMinusKr = 1.0 - 0.299;
constexpr double kB = 0.49211104112248356308804691718185;
constexpr double kR = 0.87728321993817866838972487283129;

struct SqliteClose {
  void operator()(sqlite3* db) const { sqlite3_close(db); }
};
struct StatementFinalize {
  void operator()(sqlite3_stmt* stmt) const { sqlite3_finalize(stmt); }
};

std::optional<orc::VideoSystem> parse_system(const std::string& name) {
  if (name == "PAL") return orc::VideoSystem::PAL;
  if (name == "NTSC") return orc::VideoSystem::NTSC;
  if (name == "PAL_M") return orc::VideoSystem::PAL_M;
  if (name == "SECAM") return orc::VideoSystem::SECAM;
  return std::nullopt;
}

std::string column_text(sqlite3_stmt* stmt, int column) {
  const auto* text = sqlite3_column_text(stmt, column);
  return text == nullptr ? std::string()
                         : std::string(reinterpret_cast<const char*>(text));
}

}  // namespace

std::optional<TruthGeometry> truth_geometry(const TruthMeta& meta,
                                            std::string* error) {
  auto fail = [&](const std::string& why) -> std::optional<TruthGeometry> {
    if (error != nullptr) *error = why;
    return std::nullopt;
  };
  const auto system = parse_system(meta.video_standard);
  if (!system) return fail("unknown video_standard");
  const bool common = meta.sample_format == "float32le" && meta.units == "mV" &&
                      meta.y_reference == "blanking";
  // SECAM gives Y, D'R and D'B (YDRDB); the others Y, U and V on the
  // carrier axes, with the PAL V-switch removed.
  const bool planes =
      *system == orc::VideoSystem::SECAM
          ? meta.plane_order == "YDRDB" &&
                meta.colour_difference_units == "nominal_deviation"
          : meta.plane_order == "YUV" && meta.u_axis == "sin" &&
                meta.v_axis == "cos" && meta.v_switch == "removed";
  if (!common || !planes) {
    return fail("the sidecar states a format this reader does not know");
  }
  TruthGeometry g = meta.geometry;
  g.system = *system;
  if (g.samples_per_line == 0 || g.rows == 0 || !(g.white_mv > g.black_mv)) {
    return fail("the sidecar states an empty picture or no level range");
  }
  if (*system == orc::VideoSystem::SECAM &&
      (g.dr_per_r_minus_y == 0.0 || g.db_per_b_minus_y == 0.0)) {
    return fail("the sidecar states no D'R or D'B scale");
  }
  return g;
}

std::optional<TruthFile> TruthFile::from_bytes(const TruthGeometry& geometry,
                                               const std::vector<uint8_t>& raw,
                                               std::string* error) {
  const size_t expected = geometry.frame_count * 3 * geometry.rows *
                          geometry.samples_per_line * sizeof(float);
  if (raw.size() != expected) {
    if (error != nullptr) {
      *error = ".truth holds " + std::to_string(raw.size()) +
               " bytes, not the " + std::to_string(expected) +
               " its sidecar states";
    }
    return std::nullopt;
  }
  std::vector<float> samples(raw.size() / sizeof(float));
  for (size_t i = 0; i < samples.size(); ++i) {
    const uint32_t word = static_cast<uint32_t>(raw[4 * i]) |
                          (static_cast<uint32_t>(raw[(4 * i) + 1]) << 8) |
                          (static_cast<uint32_t>(raw[(4 * i) + 2]) << 16) |
                          (static_cast<uint32_t>(raw[(4 * i) + 3]) << 24);
    std::memcpy(&samples[i], &word, sizeof(float));
  }
  return TruthFile(geometry, std::move(samples));
}

namespace {

// The secam_frame table's dr_line_parity, by frame_index; nullopt, with
// |error| set, unless it gives every one of |frames| frames.
std::optional<std::vector<int>> read_dr_line_parity(sqlite3* db, size_t frames,
                                                    std::string* error) {
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT frame_index, dr_line_parity FROM secam_frame "
                         "ORDER BY frame_index",
                         -1, &raw_stmt, nullptr) != SQLITE_OK) {
    *error = std::string(".truth.meta has no secam_frame table: ") +
             sqlite3_errmsg(db);
    return std::nullopt;
  }
  const std::unique_ptr<sqlite3_stmt, StatementFinalize> stmt(raw_stmt);
  std::vector<int> parity;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    if (sqlite3_column_int64(stmt.get(), 0) !=
        static_cast<sqlite3_int64>(parity.size())) {
      break;
    }
    parity.push_back(sqlite3_column_int(stmt.get(), 1));
  }
  if (parity.size() != frames) {
    *error = "secam_frame gives the line parity of " +
             std::to_string(parity.size()) + " frames, not " +
             std::to_string(frames);
    return std::nullopt;
  }
  return parity;
}

}  // namespace

std::optional<TruthFile> TruthFile::read(const std::filesystem::path& base,
                                         std::string* error) {
  const std::filesystem::path data_path = base.string() + ".truth";
  const std::filesystem::path meta_path = base.string() + ".truth.meta";
  auto fail = [&](const std::string& why) -> std::optional<TruthFile> {
    if (error != nullptr) *error = base.string() + ": " + why;
    return std::nullopt;
  };

  sqlite3* raw_db = nullptr;
  if (sqlite3_open_v2(meta_path.c_str(), &raw_db, SQLITE_OPEN_READONLY,
                      nullptr) != SQLITE_OK) {
    sqlite3_close(raw_db);
    return fail(".truth.meta cannot be opened");
  }
  const std::unique_ptr<sqlite3, SqliteClose> db(raw_db);
  sqlite3_stmt* raw_stmt = nullptr;
  const char* query =
      "SELECT video_standard, sample_rate_hz, frame_count, samples_per_line, "
      "rows, first_sample, first_line_field1, first_line_field2, upper_field, "
      "half_picture_line, half_picture_end_sample, plane_order, "
      "sample_format, units, y_reference, black_mv, white_mv, u_axis, "
      "v_axis, v_switch, colour_difference_units, dr_per_r_minus_y, "
      "db_per_b_minus_y FROM truth_file";
  if (sqlite3_prepare_v2(db.get(), query, -1, &raw_stmt, nullptr) !=
      SQLITE_OK) {
    return fail(std::string(".truth.meta has no truth_file row: ") +
                sqlite3_errmsg(db.get()));
  }
  const std::unique_ptr<sqlite3_stmt, StatementFinalize> stmt(raw_stmt);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return fail(".truth.meta has no truth_file row");
  }

  TruthMeta meta;
  sqlite3_stmt* row = stmt.get();
  meta.video_standard = column_text(row, 0);
  TruthGeometry& g = meta.geometry;
  g.sample_rate_hz = sqlite3_column_double(row, 1);
  g.frame_count = static_cast<size_t>(sqlite3_column_int64(row, 2));
  g.samples_per_line = static_cast<size_t>(sqlite3_column_int64(row, 3));
  g.rows = static_cast<size_t>(sqlite3_column_int64(row, 4));
  g.first_sample = sqlite3_column_int(row, 5);
  g.first_line_field1 = sqlite3_column_int(row, 6);
  g.first_line_field2 = sqlite3_column_int(row, 7);
  g.upper_field = sqlite3_column_int(row, 8);
  g.half_picture_line = sqlite3_column_int(row, 9);
  g.half_picture_end_sample = sqlite3_column_int(row, 10);
  meta.plane_order = column_text(row, 11);
  meta.sample_format = column_text(row, 12);
  meta.units = column_text(row, 13);
  meta.y_reference = column_text(row, 14);
  g.black_mv = sqlite3_column_double(row, 15);
  g.white_mv = sqlite3_column_double(row, 16);
  meta.u_axis = column_text(row, 17);
  meta.v_axis = column_text(row, 18);
  meta.v_switch = column_text(row, 19);
  meta.colour_difference_units = column_text(row, 20);
  g.dr_per_r_minus_y = sqlite3_column_double(row, 21);
  g.db_per_b_minus_y = sqlite3_column_double(row, 22);
  std::string why;
  const auto geometry = truth_geometry(meta, &why);
  if (!geometry) return fail(why);
  std::vector<int> dr_line_parity;
  if (geometry->system == orc::VideoSystem::SECAM) {
    auto parity = read_dr_line_parity(db.get(), geometry->frame_count, &why);
    if (!parity) return fail(why);
    dr_line_parity = std::move(*parity);
  }

  std::error_code ec;
  const auto bytes = std::filesystem::file_size(data_path, ec);
  if (ec) return fail(".truth cannot be read");
  const size_t expected = geometry->frame_count * 3 * geometry->rows *
                          geometry->samples_per_line * sizeof(float);
  if (bytes != expected) {
    return fail(".truth holds " + std::to_string(bytes) + " bytes, not the " +
                std::to_string(expected) + " its sidecar states");
  }
  std::ifstream in(data_path, std::ios::binary);
  std::vector<uint8_t> raw(bytes);
  in.read(reinterpret_cast<char*>(raw.data()),
          static_cast<std::streamsize>(raw.size()));
  if (!in) return fail(".truth cannot be read");
  auto truth = from_bytes(*geometry, raw, &why);
  if (!truth) return fail(why);
  return TruthFile(*geometry, std::move(truth->samples_),
                   std::move(dr_line_parity));
}

TruthFile::TruthFile(TruthGeometry geometry, std::vector<float> samples,
                     std::vector<int> dr_line_parity)
    : geometry_(geometry),
      samples_(std::move(samples)),
      dr_line_parity_(std::move(dr_line_parity)) {}

ComponentPicture TruthFile::frame(size_t frame) const {
  const TruthGeometry& g = geometry_;
  ComponentPicture picture(g.samples_per_line, g.rows);
  const size_t plane = g.samples_per_line * g.rows;
  const float* base = samples_.data() + (frame * 3 * plane);
  const double range = g.white_mv - g.black_mv;
  if (g.system == orc::VideoSystem::SECAM) {
    // Planes Y, D'R, D'B: D'R and D'B are already on Y's 0-to-1 scale, as
    // multiples of R'-Y' and B'-Y'.
    const double u_per_db = kB / g.db_per_b_minus_y;
    const double v_per_dr = kR / g.dr_per_r_minus_y;
    for (size_t n = 0; n < plane; ++n) {
      picture.y[n] = (static_cast<double>(base[n]) - g.black_mv) / range;
      picture.v[n] = static_cast<double>(base[plane + n]) * v_per_dr;
      picture.u[n] = static_cast<double>(base[(2 * plane) + n]) * u_per_db;
    }
    return picture;
  }
  for (size_t n = 0; n < plane; ++n) {
    picture.y[n] = (static_cast<double>(base[n]) - g.black_mv) / range;
    picture.u[n] = static_cast<double>(base[plane + n]) / range;
    picture.v[n] = static_cast<double>(base[(2 * plane) + n]) / range;
  }
  return picture;
}

int truth_row_frame_line(const TruthGeometry& geometry, size_t row) {
  const bool upper = row % 2 == 0;
  const int field = upper ? geometry.upper_field : 3 - geometry.upper_field;
  const int first =
      field == 1 ? geometry.first_line_field1 : geometry.first_line_field2;
  return first + static_cast<int>(row / 2);
}

std::optional<bool> TruthFile::row_transmits_dr(size_t frame,
                                                size_t row) const {
  if (frame >= dr_line_parity_.size()) return std::nullopt;
  const bool odd_line = truth_row_frame_line(geometry_, row) % 2 == 1;
  return odd_line == (dr_line_parity_[frame] == 1);
}

std::optional<int> TruthFile::secam_sequence_frame(size_t frame) const {
  if (frame >= dr_line_parity_.size()) return std::nullopt;
  return dr_line_parity_[frame] == 1 ? 1 : 2;
}

std::vector<uint8_t> TruthFile::picture_mask() const {
  const TruthGeometry& g = geometry_;
  std::vector<uint8_t> mask(g.samples_per_line * g.rows, 1);
  if (g.half_picture_line == 0 || g.half_picture_end_sample == 0) return mask;
  // The half-picture line's frame row: its field's rows run from that
  // field's first line, interleaved with the other field's.
  const int field1_offset = g.half_picture_line - g.first_line_field1;
  const int field2_offset = g.half_picture_line - g.first_line_field2;
  const bool in_field1 = field1_offset >= 0 &&
                         static_cast<size_t>(field1_offset) < (g.rows + 1) / 2;
  const int field_row = in_field1 ? field1_offset : field2_offset;
  const int parity = ((in_field1 ? 1 : 2) == g.upper_field) ? 0 : 1;
  const size_t row = static_cast<size_t>((2 * field_row) + parity);
  if (row >= g.rows) return mask;
  const int end = g.half_picture_end_sample - g.first_sample;
  for (size_t x = 0; x < g.samples_per_line; ++x) {
    if (static_cast<int>(x) >= end) mask[(row * g.samples_per_line) + x] = 0;
  }
  return mask;
}

DecodeOrigin decode_origin(orc::VideoSystem system) {
  // The truth is 0H-relative: truth sample j sits first_sample + j + 0.5
  // samples after 0H (+ 0.633 on NTSC; videosynth docs/reference/output.md).
  // The video sink crops to the cvbs_source active window, and its output
  // sample c sits at active_video_start + c plus the same fraction: on NTSC
  // and PAL_M the stored grid is orthogonal with 0H 57/90 sample before
  // sample 0 (CVBS spec v1.8.1, Preset NTSC); on PAL the lattice puts 0H at
  // -0.5 + d on each line, and Video Sink takes d out of the decoded picture
  // (PalOutputRaster), leaving every row where field 1 row 0 has it, at
  // -0.5. The fractions cancel, so the origin is the whole-sample crop
  // start, and decoded_picture() adds the truth's first_sample. The first
  // decoded row is the first truth row (PAL and SECAM: field 1 line 23).
  // SECAM is stored on the PAL lattice and shifted the same way.
  if (orc::is_625_line(system)) {
    return {orc::kPalActiveVideoStart, 0};
  }
  return {orc::kNtscActiveVideoStart, 0};
}

ComponentPicture decoded_picture(const DecodedFile& decoded, size_t frame,
                                 size_t decoded_width,
                                 const DecodeOrigin& origin,
                                 const TruthGeometry& geometry,
                                 std::vector<uint8_t>* covered) {
  ComponentPicture picture(geometry.samples_per_line, geometry.rows);
  if (covered != nullptr) covered->assign(picture.y.size(), 0);
  if (decoded_width == 0) return picture;
  const size_t decoded_rows = decoded.plane_samples() / decoded_width;
  const uint16_t* py = decoded.plane(frame, 0);
  const uint16_t* pu = decoded.plane(frame, 1);
  const uint16_t* pv = decoded.plane(frame, 2);
  const double u_scale = kOneMinusKb * kB / kCScale;
  const double v_scale = kOneMinusKr * kR / kCScale;
  for (size_t row = 0; row < geometry.rows; ++row) {
    const long decoded_row = static_cast<long>(row) - origin.first_row;
    if (decoded_row < 0 || static_cast<size_t>(decoded_row) >= decoded_rows) {
      continue;
    }
    for (size_t x = 0; x < geometry.samples_per_line; ++x) {
      const long column =
          static_cast<long>(x) + geometry.first_sample - origin.first_sample;
      if (column < 0 || static_cast<size_t>(column) >= decoded_width) continue;
      const size_t from = (static_cast<size_t>(decoded_row) * decoded_width) +
                          static_cast<size_t>(column);
      const size_t to = picture.index(x, row);
      picture.y[to] = (static_cast<double>(py[from]) - kYZero) / kYScale;
      picture.u[to] = (static_cast<double>(pu[from]) - kCZero) * u_scale;
      picture.v[to] = (static_cast<double>(pv[from]) - kCZero) * v_scale;
      if (covered != nullptr) (*covered)[to] = 1;
    }
  }
  return picture;
}

}  // namespace orc_videosynth
