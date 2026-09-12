/*
 * File:        cav_picture_number.h
 * Module:      orc-core
 * Purpose:     Shared IEC 60857 CAV picture number decoding for VBI lines
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <optional>

#include "vbi_bcd.h"

namespace orc {

/**
 * @brief The largest picture number a CAV disc may legally carry
 *
 * IEC 60857-1986 (NTSC) - 10.1.3: "The maximum available picture number is
 * 79999." IEC 60856-1986 (PAL) - 10.1.3 gives 99999 for the same field. A
 * value above the format's limit did not come off a disc that follows the
 * standard, so it is a misread however cleanly its BCD decoded.
 *
 * Note that decode_cav_picture_number_line() masks the field to 0x07FFFF,
 * because IEC 60857 - 10.1.4 reserves the top bit of X1 for the picture stop
 * indication, so what it returns is already inside the NTSC limit. These
 * constants state the standard's bound for callers that validate a picture
 * number reaching them by some other route.
 */
constexpr int32_t kMaxCavPictureNumberNtsc = 79999;
constexpr int32_t kMaxCavPictureNumberPal = 99999;

/**
 * @brief A CAV picture number recovered from VBI lines 17 and 18
 */
struct CavPictureNumber {
  /// The decoded picture number (0-79999)
  int32_t value = 0;
  /// True when both lines decoded and agreed, so the redundancy the standard
  /// provides was actually available. False when only one line was readable:
  /// the value then rests on a single unprotected BCD field, in which case one
  /// flipped bit produces a wrong picture number that still decodes cleanly
  /// (a `4` reading as `6`, for instance, shifts the number by 20000). Callers
  /// with sequence context should sanity-check such a value before trusting
  /// it; callers without one should treat it as provisional.
  bool cross_validated = false;
};

/**
 * @brief Decode the CAV picture number carried by one VBI line
 *
 * IEC 60857-1986 - 10.1.3 Picture numbers (CAV discs). The top bit of the
 * marker can be used for the stop code, so the picture number field is masked
 * to 0x07FFFF, giving the standard's 0-79999 range.
 *
 * @param vbi The raw 24-bit VBI word from line 17 or line 18
 * @return The picture number, or nullopt when the line carries something else
 */
inline std::optional<int32_t> decode_cav_picture_number_line(int32_t vbi) {
  if ((vbi & 0xF00000) != 0xF00000) {
    return std::nullopt;
  }
  int32_t picture_number;
  if (!decode_vbi_bcd(static_cast<uint32_t>(vbi) & 0x07FFFF, picture_number)) {
    return std::nullopt;
  }
  return picture_number;
}

/**
 * @brief Decode the CAV picture number from VBI lines 17 and 18
 *
 * IEC 60857-1986 - 10.1.3 Picture numbers (CAV discs). Lines 17 and 18 carry
 * redundant copies of the picture number, so they are cross-validated: if both
 * decode but disagree, one of them is corrupt and neither can be trusted in
 * isolation, and nullopt is returned so the caller can fall back on the other
 * field of the frame. If only one decodes, its value is returned but flagged
 * as not cross-validated.
 *
 * @param vbi17 The raw 24-bit VBI word from line 17
 * @param vbi18 The raw 24-bit VBI word from line 18
 * @return The picture number and its cross-validation status, or nullopt
 */
inline std::optional<CavPictureNumber> decode_cav_picture_number(
    int32_t vbi17, int32_t vbi18) {
  const auto pn17 = decode_cav_picture_number_line(vbi17);
  const auto pn18 = decode_cav_picture_number_line(vbi18);

  if (pn17 && pn18) {
    if (*pn17 != *pn18) {
      return std::nullopt;
    }
    return CavPictureNumber{*pn17, /*cross_validated=*/true};
  }

  if (pn17) return CavPictureNumber{*pn17, /*cross_validated=*/false};
  if (pn18) return CavPictureNumber{*pn18, /*cross_validated=*/false};

  return std::nullopt;
}

}  // namespace orc
