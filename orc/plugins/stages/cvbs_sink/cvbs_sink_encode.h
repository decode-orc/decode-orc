/*
 * File:        cvbs_sink_encode.h
 * Module:      orc-core
 * Purpose:     Pure sample-encoding helpers for the CVBS sink stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_CVBS_SINK_ENCODE_H
#define ORC_CORE_CVBS_SINK_ENCODE_H

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace orc {

// Output sample encodings supported by the CVBS sink. These are the four
// TBC-locked 4FSC-domain encodings defined in the CVBS file format
// specification (sample-encoding-presets.md); the raw capture presets
// (RAW_S16_28M, RAW_S16_40M) cannot be produced from CVBS_U10_4FSC data.
enum class CVBSSampleEncoding {
  U10_4FSC,    // CVBS_U10_4FSC — int16_t stored bitwise (normative default)
  U16_4FSC,    // CVBS_U16_4FSC — unsigned, 10-bit value × 64
  TPG21_4FSC,  // CVBS_TPG21_4FSC — signed, device offset 508, ×64 scale
  S16_4FSC,    // CVBS_S16_4FSC — signed, blanking-centred, ×32 scale
};

// Preset name strings as they appear in the .meta sample_encoding_preset
// column and in the stage's sample_encoding parameter.
inline const char* cvbs_sample_encoding_name(CVBSSampleEncoding encoding) {
  switch (encoding) {
    case CVBSSampleEncoding::U10_4FSC:
      return "CVBS_U10_4FSC";
    case CVBSSampleEncoding::U16_4FSC:
      return "CVBS_U16_4FSC";
    case CVBSSampleEncoding::TPG21_4FSC:
      return "CVBS_TPG21_4FSC";
    case CVBSSampleEncoding::S16_4FSC:
      return "CVBS_S16_4FSC";
  }
  return "CVBS_U10_4FSC";
}

inline std::optional<CVBSSampleEncoding> parse_cvbs_sample_encoding(
    const std::string& name) {
  if (name == "CVBS_U10_4FSC") return CVBSSampleEncoding::U10_4FSC;
  if (name == "CVBS_U16_4FSC") return CVBSSampleEncoding::U16_4FSC;
  if (name == "CVBS_TPG21_4FSC") return CVBSSampleEncoding::TPG21_4FSC;
  if (name == "CVBS_S16_4FSC") return CVBSSampleEncoding::S16_4FSC;
  return std::nullopt;
}

// Encode one CVBS_U10_4FSC-domain sample (int16_t, 10-bit levels with signed
// headroom) into the 16-bit storage word for the selected encoding. The
// returned word is the little-endian bit pattern to store; signed encodings
// are carried bitwise in the uint16_t.
//
// Inverse of the cvbs_source normalisation (CVBS file format spec §3.1):
//  - CVBS_U10_4FSC: stored bitwise; headroom outside [0, 1023] is preserved.
//  - CVBS_U16_4FSC: value × 64; unsigned container, so the value is clamped
//    to the representable [0, 1023] domain.
//  - CVBS_TPG21_4FSC: (value − 508) × 64; the spec requires a compliant
//    encoder to clamp to the legal [4, 1019] domain.
//  - CVBS_S16_4FSC: (value − blanking) × 32; the spec requires a compliant
//    encoder to clamp to the legal [4, 1019] domain.
inline uint16_t encode_cvbs_u10_sample(int16_t value,
                                       CVBSSampleEncoding encoding,
                                       int32_t blanking_10bit) {
  switch (encoding) {
    case CVBSSampleEncoding::U10_4FSC:
      return static_cast<uint16_t>(value);
    case CVBSSampleEncoding::U16_4FSC: {
      const int32_t clamped = std::clamp<int32_t>(value, 0, 1023);
      return static_cast<uint16_t>(clamped * 64);
    }
    case CVBSSampleEncoding::TPG21_4FSC: {
      const int32_t clamped = std::clamp<int32_t>(value, 4, 1019);
      return static_cast<uint16_t>(static_cast<int16_t>((clamped - 508) * 64));
    }
    case CVBSSampleEncoding::S16_4FSC: {
      const int32_t clamped = std::clamp<int32_t>(value, 4, 1019);
      return static_cast<uint16_t>(
          static_cast<int16_t>((clamped - blanking_10bit) * 32));
    }
  }
  return static_cast<uint16_t>(value);
}

// CVBS file format spec §3.1: for all YC encoding presets, chroma zero (DC)
// maps to 512 in the CVBS_U10_4FSC domain. Used when the representation
// provides no SourceParameters::chroma_dc_offset.
constexpr int32_t kDefaultChromaDcOffset = 512;

// Combine one Y/C sample pair into a single composite (Y+C) sample in the
// CVBS_U10_4FSC domain. Chroma is carried centred on its DC offset, so that
// offset is removed before summing so the result sits at the luma channel's
// blanking level. The sum is saturated to the int16_t container; values
// outside [0, 1023] are kept as headroom, as for any other U10 sample.
inline int16_t combine_yc_sample(int16_t luma, int16_t chroma,
                                 int32_t chroma_dc_offset) {
  const int32_t combined = static_cast<int32_t>(luma) +
                           static_cast<int32_t>(chroma) - chroma_dc_offset;
  return static_cast<int16_t>(
      std::clamp<int32_t>(combined, INT16_MIN, INT16_MAX));
}

// Derive the CVBS output base path (no extension) from a user-supplied
// output path: one trailing payload extension (.cvbs, .cvbsy, .cvbsc — CVBS
// file format spec, File Naming Convention) is stripped when present. The
// sink appends .cvbs/.cvbsy/.cvbsc and the sidecar extensions to this base.
inline std::string derive_cvbs_output_base(const std::string& output_path) {
  static const char* kStrippableExtensions[] = {".cvbs", ".cvbsy", ".cvbsc"};
  for (const char* ext : kStrippableExtensions) {
    const std::string suffix(ext);
    if (output_path.size() > suffix.size() &&
        output_path.compare(output_path.size() - suffix.size(), suffix.size(),
                            suffix) == 0) {
      return output_path.substr(0, output_path.size() - suffix.size());
    }
  }
  return output_path;
}

}  // namespace orc

#endif  // ORC_CORE_CVBS_SINK_ENCODE_H
