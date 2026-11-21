/*
 * File:        stage_config_wire.h
 * Module:      decode-orc stage plugins and orc-view_core
 * Purpose:     What the Mask Line and video sink stages tell their
 *              configuration dialogs: the input's signal and the hardware
 *              encoders this build can use
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// The whole of what the two stages and their dialogs in orc-view_core share.
// The host passes these bytes through untouched, so only the two halves here
// need to agree; a change to the layout changes kVersion.
//
// Layout, little-endian: u32 kMagic, u32 kVersion, u32 kind, then the kind's
// fields. Strings are a u32 byte count followed by that many UTF-8 bytes.
//
// Standard library only: the stages include it beside the stage tier and
// the view beside the view tier, and neither tier's types may cross.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace orc::config_wire {

constexpr char kMaskLineContractId[] =
    "decode-orc.stage-tools.mask-line-config.v1";
constexpr char kFfmpegPresetContractId[] =
    "decode-orc.stage-tools.ffmpeg-preset.v1";

/// Mask Line, a job (it reads the node's input): the video system and levels
/// of what the stage masks. No payload.
constexpr char kSignalVerb[] = "signal";

/// Video sink, a request: the hardware encoder types (the stage's
/// hardware_encoder values, other than "none") whose encoders this build of
/// FFmpeg has. No payload.
constexpr char kHardwareEncodersVerb[] = "hardware_encoders";

constexpr uint32_t kMagic = 0x57474643;  // "CFGW"
constexpr uint32_t kVersion = 1;

enum class Kind : uint32_t { Signal = 1, HardwareEncoders = 2 };

/// The video system of the masked input.
enum class System : uint32_t { PAL = 0, NTSC = 1, PAL_M = 2, Unknown = 3 };

/// The input's system and levels (10-bit samples). blanking_level and
/// white_level are -1 when the input does not say.
struct Signal {
  System system = System::Unknown;
  int32_t blanking_level = -1;
  int32_t white_level = -1;
};

/// Hardware encoder types, e.g. "vaapi", "nvenc", in a fixed order.
struct HardwareEncoders {
  std::vector<std::string> types;
};

namespace detail {

class Writer {
 public:
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      bytes_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
  }
  void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
  void text(const std::string& s) {
    u32(static_cast<uint32_t>(s.size()));
    bytes_.insert(bytes_.end(), s.begin(), s.end());
  }
  std::vector<uint8_t> take() { return std::move(bytes_); }

 private:
  std::vector<uint8_t> bytes_;
};

class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  bool u32(uint32_t* v) {
    if (bytes_.size() - pos_ < 4) return false;
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
  bool text(std::string* s) {
    uint32_t size = 0;
    if (!u32(&size) || bytes_.size() - pos_ < size) return false;
    s->assign(bytes_.begin() + static_cast<std::ptrdiff_t>(pos_),
              bytes_.begin() + static_cast<std::ptrdiff_t>(pos_ + size));
    pos_ += size;
    return true;
  }
  size_t remaining() const { return bytes_.size() - pos_; }
  bool at_end() const { return pos_ == bytes_.size(); }

 private:
  const std::vector<uint8_t>& bytes_;
  size_t pos_ = 0;
};

inline void write_header(Writer* w, Kind kind) {
  w->u32(kMagic);
  w->u32(kVersion);
  w->u32(static_cast<uint32_t>(kind));
}

inline bool read_header(Reader* r, Kind expected, std::string* error) {
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t kind = 0;
  if (!r->u32(&magic) || magic != kMagic) {
    *error = "The stage's reply is not configuration data";
    return false;
  }
  if (!r->u32(&version) || version != kVersion) {
    *error = "The stage replies in a format this dialog does not read";
    return false;
  }
  if (!r->u32(&kind) || kind != static_cast<uint32_t>(expected)) {
    *error = "The stage sent another kind of reply";
    return false;
  }
  return true;
}

inline bool finish(const Reader& r, bool ok, std::string* error) {
  if (!ok) {
    *error = "The stage's reply is cut short";
    return false;
  }
  if (!r.at_end()) {
    *error = "The stage's reply has bytes left over";
    return false;
  }
  return true;
}

}  // namespace detail

inline std::vector<uint8_t> encode(const Signal& signal) {
  detail::Writer w;
  detail::write_header(&w, Kind::Signal);
  w.u32(static_cast<uint32_t>(signal.system));
  w.i32(signal.blanking_level);
  w.i32(signal.white_level);
  return w.take();
}

inline std::vector<uint8_t> encode(const HardwareEncoders& encoders) {
  detail::Writer w;
  detail::write_header(&w, Kind::HardwareEncoders);
  w.u32(static_cast<uint32_t>(encoders.types.size()));
  for (const auto& type : encoders.types) {
    w.text(type);
  }
  return w.take();
}

inline bool decode(const std::vector<uint8_t>& bytes, Signal* out,
                   std::string* error) {
  detail::Reader r(bytes);
  if (!detail::read_header(&r, Kind::Signal, error)) {
    return false;
  }
  Signal signal;
  uint32_t system = 0;
  bool ok = r.u32(&system) && r.i32(&signal.blanking_level) &&
            r.i32(&signal.white_level);
  if (ok) {
    signal.system = system <= static_cast<uint32_t>(System::Unknown)
                        ? static_cast<System>(system)
                        : System::Unknown;
  }
  if (!detail::finish(r, ok, error)) return false;
  *out = signal;
  return true;
}

inline bool decode(const std::vector<uint8_t>& bytes, HardwareEncoders* out,
                   std::string* error) {
  detail::Reader r(bytes);
  if (!detail::read_header(&r, Kind::HardwareEncoders, error)) {
    return false;
  }
  HardwareEncoders encoders;
  uint32_t count = 0;
  // Each entry is at least its four-byte length.
  bool ok = r.u32(&count) && count <= r.remaining() / 4;
  for (uint32_t i = 0; ok && i < count; ++i) {
    std::string type;
    ok = r.text(&type);
    if (ok) encoders.types.push_back(std::move(type));
  }
  if (!detail::finish(r, ok, error)) return false;
  *out = std::move(encoders);
  return true;
}

}  // namespace orc::config_wire
