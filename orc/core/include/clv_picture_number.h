/*
 * File:        clv_picture_number.h
 * Module:      orc-core
 * Purpose:     Shared IEC 60856/60857 CLV picture number decoding for VBI lines
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <optional>

#include "vbi_bcd.h"

namespace orc {

// ============================================================================
// Legal ranges (IEC 60856-1986 / IEC 60857-1986)
// ============================================================================

// 10.1.6 Programme time code, "FX1DDX2X3": X1 is the hours and occupies one
// 4-bit group, so a legal BCD hours digit runs 0-9. X2X3 are the minutes.
constexpr int32_t kMaxClvHours = 9;
constexpr int32_t kMaxClvMinutes = 59;

// 10.1.10 CLV picture number, "8X1EX3X4X5": X1 runs A through F and carries
// the tens of seconds (0-5), X3 runs 0 through 9 and carries the units.
constexpr int32_t kMaxClvSeconds = 59;

// The widest reading 10.1.10 allows for the picture within the second, given
// as "X4 = 0 through 2 and X5 = 0 through 9" in both the PAL and the NTSC
// standard. Callers that know the disc's video format should pass its frame
// rate instead, which is tighter: a 25 Hz disc never reaches picture 25. This
// is for the few that do not know it.
constexpr int32_t kClvPictureFieldRange = 30;

/**
 * @brief The largest picture number a CLV disc may legally carry
 *
 * The CLV picture number is a running time, so its ceiling is the largest
 * time the code can express: 9:59:59 (see kMaxClvHours) plus the last picture
 * of that second.
 *
 * IEC 60856/60857 - 10.1.10 writes the picture within the second as "X4 = 0
 * through 2 and X5 = 0 through 9", i.e. 0-29, in both the PAL and the NTSC
 * standard. That is the field's width rather than a frame count: a 25 Hz disc
 * never reaches picture 25, so the bound used here is the format's own frame
 * rate and values above it are treated as misreads.
 *
 * @param fps Frames per second of the video format (25 for PAL, 30 for NTSC)
 */
constexpr int32_t max_legal_clv_picture_number(int32_t fps) {
  return ((((kMaxClvHours * 60) + kMaxClvMinutes) * 60) + kMaxClvSeconds) *
             fps +
         (fps - 1);
}

/**
 * @brief The hours and minutes carried by one VBI line
 */
struct ClvTimeCode {
  int32_t hours = 0;
  int32_t minutes = 0;

  friend bool operator==(const ClvTimeCode& a, const ClvTimeCode& b) {
    return a.hours == b.hours && a.minutes == b.minutes;
  }
};

/**
 * @brief A CLV picture number recovered from VBI lines 16, 17 and 18
 */
struct ClvPictureNumber {
  /// The decoded picture number: the running time expressed in frames
  int32_t value = 0;
  /// True when lines 17 and 18 both decoded and agreed on the hours and
  /// minutes, so the redundancy the standard provides was actually available.
  ///
  /// This covers the programme time code only. The seconds and the picture
  /// within the second come from line 16 alone (10.1.10), which the standard
  /// carries on no other line, so no reading of a CLV picture number is fully
  /// protected. What a cross-validated value rules out is the failure that
  /// displaces a frame furthest: one flipped bit in the unprotected hours
  /// digit moves the picture by an hour of running time.
  bool cross_validated = false;
};

/**
 * @brief Decode the CLV programme time code carried by one VBI line
 *
 * IEC 60856/60857-1986 - 10.1.6 Programme time code. The signature test masks
 * out the hours group, which is therefore protected by nothing but its own
 * BCD legality, so callers should cross-validate lines 17 and 18 rather than
 * trust whichever of them answers first.
 *
 * @param vbi The raw 24-bit VBI word from line 17 or line 18
 * @return The hours and minutes, or nullopt when the line carries something
 *         else or holds a value outside the standard's range
 */
inline std::optional<ClvTimeCode> decode_clv_time_code_line(int32_t vbi) {
  if ((vbi & 0xF0FF00) != 0xF0DD00) {
    return std::nullopt;
  }

  int32_t hours = 0;
  int32_t minutes = 0;
  if (!decode_vbi_bcd(static_cast<uint32_t>(vbi & 0x0F0000) >> 16, hours) ||
      !decode_vbi_bcd(static_cast<uint32_t>(vbi) & 0x0000FF, minutes)) {
    return std::nullopt;
  }

  if (hours < 0 || hours > kMaxClvHours || minutes < 0 ||
      minutes > kMaxClvMinutes) {
    return std::nullopt;
  }

  return ClvTimeCode{hours, minutes};
}

/**
 * @brief Decode the CLV programme time code from VBI lines 17 and 18
 *
 * IEC 60856/60857-1986 - 10.1.6 Programme time code. Lines 17 and 18 carry
 * redundant copies, so they are cross-validated the same way the CAV picture
 * number is: if both decode but disagree, one of them is corrupt and neither
 * can be trusted in isolation, and nullopt is returned. If only one decodes,
 * its value is returned but flagged as not cross-validated.
 *
 * @param vbi17            The raw 24-bit VBI word from line 17
 * @param vbi18            The raw 24-bit VBI word from line 18
 * @param cross_validated  Receives whether both lines decoded and agreed
 * @return The hours and minutes, or nullopt
 */
inline std::optional<ClvTimeCode> decode_clv_time_code(int32_t vbi17,
                                                       int32_t vbi18,
                                                       bool& cross_validated) {
  cross_validated = false;

  const auto tc17 = decode_clv_time_code_line(vbi17);
  const auto tc18 = decode_clv_time_code_line(vbi18);

  if (tc17 && tc18) {
    if (!(*tc17 == *tc18)) {
      return std::nullopt;
    }
    cross_validated = true;
    return tc17;
  }

  if (tc17) return tc17;
  if (tc18) return tc18;

  return std::nullopt;
}

/**
 * @brief Decode the seconds and picture-within-second from VBI line 16
 *
 * IEC 60856/60857-1986 - 10.1.10 CLV picture number. The standard puts this
 * code on line 16 (or 279/329) only, so there is no second copy to check it
 * against; the ranges below are the whole of the available error detection.
 *
 * @param vbi16   The raw 24-bit VBI word from line 16
 * @param fps     Frames per second of the video format
 * @param seconds Receives the seconds of the running time
 * @param picture Receives the picture within that second
 * @return true when the line carried a CLV picture number in range
 */
inline bool decode_clv_seconds_picture(int32_t vbi16, int32_t fps,
                                       int32_t& seconds, int32_t& picture) {
  if ((vbi16 & 0xF0F000) != 0x80E000) {
    return false;
  }

  const uint32_t tens = static_cast<uint32_t>(vbi16 & 0x0F0000) >> 16;
  if (tens < 0xA || tens > 0xF) {
    return false;
  }

  int32_t units = 0;
  int32_t picture_no = 0;
  if (!decode_vbi_bcd(static_cast<uint32_t>(vbi16 & 0x000F00) >> 8, units) ||
      !decode_vbi_bcd(static_cast<uint32_t>(vbi16) & 0x0000FF, picture_no)) {
    return false;
  }

  const int32_t sec = (10 * static_cast<int32_t>(tens - 0xA)) + units;
  if (sec < 0 || sec > kMaxClvSeconds || picture_no < 0 || picture_no >= fps) {
    return false;
  }

  seconds = sec;
  picture = picture_no;
  return true;
}

/**
 * @brief Decode the CLV picture number from VBI lines 16, 17 and 18
 *
 * Combines the programme time code (10.1.6, lines 17 and 18) with the seconds
 * and picture within the second (10.1.10, line 16) into a running time
 * expressed in frames, which numbers the disc's pictures in the same way a
 * CAV picture number does.
 *
 * @param vbi16 The raw 24-bit VBI word from line 16
 * @param vbi17 The raw 24-bit VBI word from line 17
 * @param vbi18 The raw 24-bit VBI word from line 18
 * @param fps   Frames per second of the video format (25 PAL, 30 NTSC)
 * @return The picture number and its cross-validation status, or nullopt
 */
inline std::optional<ClvPictureNumber> decode_clv_picture_number(int32_t vbi16,
                                                                 int32_t vbi17,
                                                                 int32_t vbi18,
                                                                 int32_t fps) {
  bool cross_validated = false;
  const auto time_code = decode_clv_time_code(vbi17, vbi18, cross_validated);
  if (!time_code) {
    return std::nullopt;
  }

  int32_t seconds = 0;
  int32_t picture = 0;
  if (!decode_clv_seconds_picture(vbi16, fps, seconds, picture)) {
    return std::nullopt;
  }

  const int32_t total_seconds =
      (time_code->hours * 3600) + (time_code->minutes * 60) + seconds;
  const int32_t value = (total_seconds * fps) + picture;

  // A running time of zero is the very first picture of the programme, which
  // the mapper reads from the lead-in code instead; rejecting it here keeps
  // the long-standing behaviour of treating a decoded zero as "no picture
  // number" rather than as picture zero.
  if (value <= 0) {
    return std::nullopt;
  }

  // The range checks on each field above already confine the result to this
  // bound. Stating it once more at the boundary makes the contract the header
  // offers its callers explicit — nothing above the standard's maximum ever
  // leaves this function — so a later change to one of those field checks
  // cannot quietly widen it.
  if (value > max_legal_clv_picture_number(fps)) {
    return std::nullopt;
  }

  return ClvPictureNumber{value, cross_validated};
}

}  // namespace orc
