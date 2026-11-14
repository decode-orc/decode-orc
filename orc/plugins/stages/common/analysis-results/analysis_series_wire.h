/*
 * File:        analysis_series_wire.h
 * Module:      decode-orc stage plugins and orc-view_core
 * Purpose:     The bytes the dropout, SNR and burst level analysis sinks send
 *              their views: one record per analysed frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// The whole of what the three analysis sinks and their windows in
// orc-view_core share. The host passes these bytes through untouched
// (IStageToolSession::request / IStageToolRequestHandler), so only the two
// halves here need to agree; a change to the layout changes kVersion.
//
// Layout, little-endian: u32 kMagic, u32 kVersion, u32 kind, i32
// total_frames, then the kind's header and u32 count followed by count
// records. Reals are IEEE 754 doubles, flags one byte each.
//
// Standard library only: the stages include it beside the stage tier and
// the view beside the view tier, and neither tier's types may cross.

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace orc::analysis_wire {

constexpr char kDropoutContractId[] =
    "decode-orc.stage-tools.dropout-analysis.v1";
constexpr char kSnrContractId[] = "decode-orc.stage-tools.snr-analysis.v1";
constexpr char kBurstLevelContractId[] =
    "decode-orc.stage-tools.burst-level-analysis.v1";

/// The one verb: the per-frame results of the last trigger. No payload.
constexpr char kResultsVerb[] = "results";

/// The error a stage answers kResultsVerb with before its first trigger.
constexpr char kNotTriggeredMessage[] =
    "This stage has not been triggered yet, so there are no results to "
    "show.\n\nRight-click the stage and choose Trigger Stage to produce them.";

constexpr uint32_t kMagic = 0x4f415357;  // "WSAO"
constexpr uint32_t kVersion = 1;

enum class Kind : uint32_t { Dropout = 1, Snr = 2, BurstLevel = 3 };

/// The video system the levels are measured against.
enum class System : uint32_t { PAL = 0, NTSC = 1, PAL_M = 2, Unknown = 3 };

/// The levels of the analysed source, for showing burst amplitudes in the
/// project's unit. blanking_level and white_level are -1 when unknown.
struct VideoLevels {
  System system = System::Unknown;
  int32_t blanking_level = -1;
  int32_t white_level = -1;
};

/// One analysed frame's dropouts.
struct DropoutFrame {
  int32_t frame_number = 0;  ///< 1-based
  int32_t dropout_count = 0;
  int64_t dropout_length_samples = 0;
  bool has_data = false;  ///< the frame had one or more dropouts
};

/// One analysed frame's SNR figures (dB).
struct SnrFrame {
  int32_t frame_number = 0;  ///< 1-based
  double white_snr = 0.0;
  double black_psnr = 0.0;
  bool has_white_snr = false;
  bool has_black_psnr = false;
  bool has_data = false;
};

/// One analysed frame's median burst amplitude (10-bit sample units).
struct BurstLevelFrame {
  int32_t frame_number = 0;  ///< 1-based
  double median_burst_10bit = 0.0;
  bool has_data = false;
};

struct DropoutResults {
  int32_t total_frames = 0;
  std::vector<DropoutFrame> frames;
};

struct SnrResults {
  int32_t total_frames = 0;
  std::vector<SnrFrame> frames;
};

struct BurstLevelResults {
  int32_t total_frames = 0;
  VideoLevels levels;
  std::vector<BurstLevelFrame> frames;
};

namespace detail {

class Writer {
 public:
  void u8(uint8_t v) { bytes_.push_back(v); }
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      bytes_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
  }
  void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
  void u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      bytes_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
  }
  void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
  void real(double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    u64(bits);
  }
  void flag(bool v) { u8(v ? 1 : 0); }
  std::vector<uint8_t> take() { return std::move(bytes_); }

 private:
  std::vector<uint8_t> bytes_;
};

class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  bool u8(uint8_t* v) {
    if (pos_ + 1 > bytes_.size()) return false;
    *v = bytes_[pos_++];
    return true;
  }
  bool u32(uint32_t* v) {
    if (pos_ + 4 > bytes_.size()) return false;
    uint32_t out = 0;
    for (int i = 0; i < 4; ++i) {
      out |= static_cast<uint32_t>(bytes_[pos_++]) << (8 * i);
    }
    *v = out;
    return true;
  }
  bool i32(int32_t* v) {
    uint32_t raw = 0;
    if (!u32(&raw)) return false;
    *v = static_cast<int32_t>(raw);
    return true;
  }
  bool u64(uint64_t* v) {
    if (pos_ + 8 > bytes_.size()) return false;
    uint64_t out = 0;
    for (int i = 0; i < 8; ++i) {
      out |= static_cast<uint64_t>(bytes_[pos_++]) << (8 * i);
    }
    *v = out;
    return true;
  }
  bool i64(int64_t* v) {
    uint64_t raw = 0;
    if (!u64(&raw)) return false;
    *v = static_cast<int64_t>(raw);
    return true;
  }
  bool real(double* v) {
    uint64_t bits = 0;
    if (!u64(&bits)) return false;
    std::memcpy(v, &bits, sizeof bits);
    return true;
  }
  bool flag(bool* v) {
    uint8_t raw = 0;
    if (!u8(&raw) || raw > 1) return false;
    *v = raw == 1;
    return true;
  }
  /// True when @p count records of @p record_size bytes can follow.
  bool can_hold(uint32_t count, size_t record_size) const {
    return static_cast<uint64_t>(count) * record_size <= bytes_.size() - pos_;
  }
  bool at_end() const { return pos_ == bytes_.size(); }

 private:
  const std::vector<uint8_t>& bytes_;
  size_t pos_ = 0;
};

inline void write_header(Writer* w, Kind kind, int32_t total_frames) {
  w->u32(kMagic);
  w->u32(kVersion);
  w->u32(static_cast<uint32_t>(kind));
  w->i32(total_frames);
}

inline bool read_header(Reader* r, Kind expected, int32_t* total_frames,
                        std::string* error) {
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t kind = 0;
  if (!r->u32(&magic) || magic != kMagic) {
    *error = "The stage's reply is not analysis results";
    return false;
  }
  if (!r->u32(&version) || version != kVersion) {
    *error = "The stage sends results in a format this view does not read";
    return false;
  }
  if (!r->u32(&kind) || kind != static_cast<uint32_t>(expected)) {
    *error = "The stage sent results of another analysis";
    return false;
  }
  if (!r->i32(total_frames)) {
    *error = "The stage's results are cut short";
    return false;
  }
  return true;
}

inline bool finish(const Reader& r, bool ok, std::string* error) {
  if (!ok) {
    *error = "The stage's results are cut short";
    return false;
  }
  if (!r.at_end()) {
    *error = "The stage's results have bytes left over";
    return false;
  }
  return true;
}

}  // namespace detail

inline std::vector<uint8_t> encode(const DropoutResults& results) {
  detail::Writer w;
  detail::write_header(&w, Kind::Dropout, results.total_frames);
  w.u32(static_cast<uint32_t>(results.frames.size()));
  for (const auto& f : results.frames) {
    w.i32(f.frame_number);
    w.i32(f.dropout_count);
    w.i64(f.dropout_length_samples);
    w.flag(f.has_data);
  }
  return w.take();
}

inline std::vector<uint8_t> encode(const SnrResults& results) {
  detail::Writer w;
  detail::write_header(&w, Kind::Snr, results.total_frames);
  w.u32(static_cast<uint32_t>(results.frames.size()));
  for (const auto& f : results.frames) {
    w.i32(f.frame_number);
    w.real(f.white_snr);
    w.real(f.black_psnr);
    w.flag(f.has_white_snr);
    w.flag(f.has_black_psnr);
    w.flag(f.has_data);
  }
  return w.take();
}

inline std::vector<uint8_t> encode(const BurstLevelResults& results) {
  detail::Writer w;
  detail::write_header(&w, Kind::BurstLevel, results.total_frames);
  w.u32(static_cast<uint32_t>(results.levels.system));
  w.i32(results.levels.blanking_level);
  w.i32(results.levels.white_level);
  w.u32(static_cast<uint32_t>(results.frames.size()));
  for (const auto& f : results.frames) {
    w.i32(f.frame_number);
    w.real(f.median_burst_10bit);
    w.flag(f.has_data);
  }
  return w.take();
}

inline bool decode(const std::vector<uint8_t>& bytes, DropoutResults* out,
                   std::string* error) {
  detail::Reader r(bytes);
  DropoutResults results;
  if (!detail::read_header(&r, Kind::Dropout, &results.total_frames, error)) {
    return false;
  }
  uint32_t count = 0;
  bool ok = r.u32(&count) && r.can_hold(count, 4 + 4 + 8 + 1);
  if (ok) results.frames.resize(count);
  for (uint32_t i = 0; ok && i < count; ++i) {
    auto& f = results.frames[i];
    ok = r.i32(&f.frame_number) && r.i32(&f.dropout_count) &&
         r.i64(&f.dropout_length_samples) && r.flag(&f.has_data);
  }
  if (!detail::finish(r, ok, error)) return false;
  *out = std::move(results);
  return true;
}

inline bool decode(const std::vector<uint8_t>& bytes, SnrResults* out,
                   std::string* error) {
  detail::Reader r(bytes);
  SnrResults results;
  if (!detail::read_header(&r, Kind::Snr, &results.total_frames, error)) {
    return false;
  }
  uint32_t count = 0;
  bool ok = r.u32(&count) && r.can_hold(count, 4 + 8 + 8 + 3);
  if (ok) results.frames.resize(count);
  for (uint32_t i = 0; ok && i < count; ++i) {
    auto& f = results.frames[i];
    ok = r.i32(&f.frame_number) && r.real(&f.white_snr) &&
         r.real(&f.black_psnr) && r.flag(&f.has_white_snr) &&
         r.flag(&f.has_black_psnr) && r.flag(&f.has_data);
  }
  if (!detail::finish(r, ok, error)) return false;
  *out = std::move(results);
  return true;
}

inline bool decode(const std::vector<uint8_t>& bytes, BurstLevelResults* out,
                   std::string* error) {
  detail::Reader r(bytes);
  BurstLevelResults results;
  if (!detail::read_header(&r, Kind::BurstLevel, &results.total_frames,
                           error)) {
    return false;
  }
  uint32_t system = 0;
  uint32_t count = 0;
  bool ok = r.u32(&system) &&
            system <= static_cast<uint32_t>(System::Unknown) &&
            r.i32(&results.levels.blanking_level) &&
            r.i32(&results.levels.white_level) && r.u32(&count) &&
            r.can_hold(count, 4 + 8 + 1);
  results.levels.system = static_cast<System>(system);
  if (ok) results.frames.resize(count);
  for (uint32_t i = 0; ok && i < count; ++i) {
    auto& f = results.frames[i];
    ok = r.i32(&f.frame_number) && r.real(&f.median_burst_10bit) &&
         r.flag(&f.has_data);
  }
  if (!detail::finish(r, ok, error)) return false;
  *out = std::move(results);
  return true;
}

}  // namespace orc::analysis_wire
