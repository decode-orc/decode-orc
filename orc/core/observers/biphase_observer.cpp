/*
 * File:        biphase_observer.cpp
 * Module:      orc-core
 * Purpose:     Biphase VBI data extraction observer implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <biphase_observer.h>
#include <cav_picture_number.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>
#include <orc/support/vbi_types.h>
#include <orc/support/vbi_utilities.h>

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace orc {

// Decode Manchester/biphase encoded VBI data from a video line
static int32_t decode_manchester(const int16_t* line_data, size_t sample_count,
                                 int16_t zero_crossing, size_t active_start,
                                 double sample_rate) {
  if (!line_data || sample_count == 0) {
    return 0;
  }

  // Get transition map
  auto transition_map =
      vbi_utils::get_transition_map(line_data, sample_count, zero_crossing);

  // Calculate samples for 1.5us (cell window is 2us, we jump 1.5us)
  double jump_samples = (sample_rate / 1000000.0) * 1.5;

  // Find first transition
  size_t x = active_start;
  while (x < transition_map.size() && !transition_map[x]) {
    x++;
  }

  if (x >= transition_map.size()) {
    return 0;  // No data
  }

  // First transition is always 01 in Manchester code
  int32_t result = 1;
  int decode_count = 1;

  // Decode remaining bits
  while (x < transition_map.size() && decode_count < 24) {
    x += static_cast<size_t>(jump_samples);
    if (x >= transition_map.size()) {
      break;
    }

    // Find next transition from current position
    uint8_t start_state = transition_map[x];
    while (x < transition_map.size() && transition_map[x] == start_state) {
      x++;
    }

    if (x >= transition_map.size()) {
      break;
    }

    // Check transition direction
    if (!transition_map[x - 1] && transition_map[x]) {
      // 01 transition = 1
      result = (result << 1) | 1;
    } else {
      // 10 transition = 0
      result = result << 1;
    }
    decode_count++;
  }

  // Must have exactly 24 bits
  if (decode_count != 24) {
    return (decode_count == 0) ? 0 : -1;  // 0 = blank, -1 = error
  }

  return result;
}

// Helper function to check IEC 60857 parity (x51/x52/x53)
static bool check_parity(uint32_t x4, uint32_t x5) {
  // X51 is parity with X41, X42 and X44
  // X52 is parity with X41, X43 and X44
  // X53 is parity with X42, X43 and X44
  int x51 = (x5 & 0x8) ? 1 : 0;
  int x52 = (x5 & 0x4) ? 1 : 0;
  int x53 = (x5 & 0x2) ? 1 : 0;

  int x41 = (x4 & 0x8) ? 1 : 0;
  int x42 = (x4 & 0x4) ? 1 : 0;
  int x43 = (x4 & 0x2) ? 1 : 0;
  int x44 = (x4 & 0x1) ? 1 : 0;

  int x51count = x41 + x42 + x44;
  int x52count = x41 + x43 + x44;
  int x53count = x42 + x43 + x44;

  bool x51p = (((x51count % 2) == 0) && (x51 == 0)) ||
              (((x51count % 2) != 0) && (x51 != 0));
  bool x52p = (((x52count % 2) == 0) && (x52 == 0)) ||
              (((x52count % 2) != 0) && (x52 != 0));
  bool x53p = (((x53count % 2) == 0) && (x53 == 0)) ||
              (((x53count % 2) != 0) && (x53 != 0));

  return x51p && x52p && x53p;
}

// Interpret the decoded VBI data according to IEC 60857 LaserDisc standard
static void interpret_vbi_data(int32_t vbi16, int32_t vbi17, int32_t vbi18,
                               FieldID field_id, IObservationContext& context) {
  // IEC 60857-1986 - 10.1.3 Picture numbers (CAV discs)
  // ---------------------------- Lines 17 and 18 carry redundant copies of the
  // picture number, so decode_cav_picture_number() cross-validates them: two
  // readable lines that disagree yield nothing rather than whichever line was
  // looked at last. cross_validated is published alongside the number so that
  // consumers with sequence context (the disc mapper) can tell a value backed
  // by both lines from one resting on a single unprotected BCD field.

  const std::optional<int32_t> cav_pn_line_17 =
      decode_cav_picture_number_line(vbi17);
  const std::optional<int32_t> cav_pn_line_18 =
      decode_cav_picture_number_line(vbi18);
  const std::optional<CavPictureNumber> cav_picture_number =
      decode_cav_picture_number(vbi17, vbi18);
  if (cav_picture_number) {
    ORC_LOG_DEBUG("BiphaseObserver: CAV picture number {} ({})",
                  cav_picture_number->value,
                  cav_picture_number->cross_validated
                      ? "lines 17 and 18 agree"
                      : "one readable line only");
  } else if (cav_pn_line_17 && cav_pn_line_18) {
    ORC_LOG_DEBUG(
        "BiphaseObserver: CAV picture number rejected, lines 17 and 18 "
        "disagree ({} vs {})",
        *cav_pn_line_17, *cav_pn_line_18);
  }

  // IEC 60857-1986 - 10.1.5 Chapter numbers
  // ---------------------------------------- Check for chapter number on lines
  // 17 and 18

  if ((vbi17 & 0xF00FFF) == 0x800DDD) {
    int32_t chapter;
    if (decode_vbi_bcd((vbi17 & 0x07F000) >> 12, chapter)) {
      context.set(field_id, "vbi", "chapter_number", chapter);
      ORC_LOG_DEBUG("BiphaseObserver: Chapter number {} from line 17", chapter);
    }
  }

  if ((vbi18 & 0xF00FFF) == 0x800DDD) {
    int32_t chapter;
    if (decode_vbi_bcd((vbi18 & 0x07F000) >> 12, chapter)) {
      context.set(field_id, "vbi", "chapter_number", chapter);
      ORC_LOG_DEBUG("BiphaseObserver: Chapter number {} from line 18", chapter);
    }
  }

  // IEC 60857-1986 - 10.1.6 Programme time code (CLV hours and minutes)
  // ------------- Check for CLV programme time code on lines 17 and 18 Both
  // lines should carry redundant data - verify they match

  CLVTimecode clv_tc{-1, -1, -1, -1};
  // Decode line 17 hours/minutes
  if ((vbi17 & 0xF0FF00) == 0xF0DD00) {
    int32_t hour17 = -1, minute17 = -1;
    if (decode_vbi_bcd((vbi17 & 0x0F0000) >> 16, hour17) &&
        decode_vbi_bcd(vbi17 & 0x0000FF, minute17)) {
      clv_tc.hours = hour17;
      clv_tc.minutes = minute17;
      ORC_LOG_DEBUG("BiphaseObserver: CLV hours={} minutes={} from line 17",
                    hour17, minute17);
    }
  }

  // Decode line 18 hours/minutes (overwrites line 17 if present)
  if ((vbi18 & 0xF0FF00) == 0xF0DD00) {
    int32_t hour18 = -1, minute18 = -1;
    if (decode_vbi_bcd((vbi18 & 0x0F0000) >> 16, hour18) &&
        decode_vbi_bcd(vbi18 & 0x0000FF, minute18)) {
      clv_tc.hours = hour18;
      clv_tc.minutes = minute18;
      ORC_LOG_DEBUG("BiphaseObserver: CLV hours={} minutes={} from line 18",
                    hour18, minute18);
    }
  }

  // IEC 60857-1986 - 10.1.10 CLV picture number (seconds and frame within
  // second) --- Check for CLV picture number on line 16 Both second and picture
  // number must be valid

  bool has_clv_picture_number = false;
  if ((vbi16 & 0xF0F000) == 0x80E000) {
    int32_t sec_digit, pic_no;

    // First digit of second is A-F (representing 0-5 tens of seconds)
    uint32_t tens = (vbi16 & 0x0F0000) >> 16;

    if (tens >= 0xA && tens <= 0xF &&
        decode_vbi_bcd((vbi16 & 0x000F00) >> 8, sec_digit) &&
        decode_vbi_bcd(vbi16 & 0x0000FF, pic_no)) {
      int32_t seconds = (10 * static_cast<int32_t>(tens - 0xA)) + sec_digit;

      // Validate range: seconds 0-59, picture 0-29 (PAL) or 0-24 (NTSC)
      // Be permissive and accept 0-29 for both formats
      if (seconds >= 0 && seconds <= 59 && pic_no >= 0 && pic_no <= 29) {
        clv_tc.seconds = seconds;
        clv_tc.picture_number = pic_no;
        has_clv_picture_number = true;
        ORC_LOG_DEBUG("BiphaseObserver: CLV seconds={} picture={} from line 16",
                      seconds, pic_no);
      } else {
        ORC_LOG_DEBUG(
            "BiphaseObserver: Invalid CLV seconds/picture range: seconds={} "
            "picture={}",
            seconds, pic_no);
      }
    }
  }

  // Only store CLV timecode if ALL fields are present and valid
  if (clv_tc.hours != -1 && clv_tc.minutes != -1 && clv_tc.seconds != -1 &&
      clv_tc.picture_number != -1) {
    context.set(field_id, "vbi", "clv_timecode_hours", clv_tc.hours);
    context.set(field_id, "vbi", "clv_timecode_minutes", clv_tc.minutes);
    context.set(field_id, "vbi", "clv_timecode_seconds", clv_tc.seconds);
    context.set(field_id, "vbi", "clv_timecode_picture", clv_tc.picture_number);
    ORC_LOG_DEBUG(
        "BiphaseObserver: Complete CLV timecode validated: {}:{}:{}.{}",
        clv_tc.hours, clv_tc.minutes, clv_tc.seconds, clv_tc.picture_number);
  }

  // Only set CAV picture number if CLV picture number not detected
  if (!has_clv_picture_number && cav_picture_number.has_value()) {
    context.set(field_id, "vbi", "picture_number", cav_picture_number->value);
    context.set(field_id, "vbi", "picture_number_cross_validated",
                cav_picture_number->cross_validated ? 1 : 0);
  }

  // IEC 60857-1986 - 10.1.1 Lead-in
  // ------------------------------------------------
  if (vbi17 == 0x88FFFF || vbi18 == 0x88FFFF) {
    context.set(field_id, "vbi", "lead_in", static_cast<int32_t>(1));
    ORC_LOG_DEBUG("BiphaseObserver: Lead-in detected");
  }

  // IEC 60857-1986 - 10.1.2 Lead-out
  // -----------------------------------------------
  if (vbi17 == 0x80EEEE || vbi18 == 0x80EEEE) {
    context.set(field_id, "vbi", "lead_out", static_cast<int32_t>(1));
    ORC_LOG_DEBUG("BiphaseObserver: Lead-out detected");
  }

  // IEC 60857-1986 - 10.1.4 Picture stop code
  // -------------------------------------- Check for picture stop code on lines
  // 16 and 17
  if (vbi16 == 0x82CFFF || vbi17 == 0x82CFFF) {
    context.set(field_id, "vbi", "stop_code_present", static_cast<int32_t>(1));
    ORC_LOG_DEBUG("BiphaseObserver: Picture stop code detected");
  }

  // IEC 60857-1986 - 10.1.7 Constant linear velocity code
  // -------------------------- CLV indicator on line 17
  if (vbi17 == 0x87FFFF) {
    ORC_LOG_DEBUG("BiphaseObserver: CLV indicator code detected");
  }

  // IEC 60857-1986 - 10.1.8 Programme status code
  // ----------------------------------
  if ((vbi16 & 0xFFF000) == 0x8DC000 || (vbi16 & 0xFFF000) == 0x8BA000) {
    // CX on or off?
    bool cx_enabled = ((vbi16 & 0x0FF000) == 0x0DC000);
    context.set(field_id, "vbi", "programme_status_cx_enabled",
                cx_enabled ? 1 : 0);

    // Extract x3, x4, x5 parameters
    uint32_t x3 = (vbi16 & 0x000F00) >> 8;
    uint32_t x4 = (vbi16 & 0x0000F0) >> 4;
    uint32_t x5 = (vbi16 & 0x00000F);

    // Check parity
    bool parity_valid = check_parity(x4, x5);
    context.set(field_id, "vbi", "programme_status_parity_valid",
                parity_valid ? 1 : 0);

    // Disc size (x31): 1=8 inch, 0=12 inch
    bool is_12_inch = ((x3 & 0x08) == 0);
    context.set(field_id, "vbi", "programme_status_is_12_inch",
                is_12_inch ? 1 : 0);

    // Disc side (x32): 1=side 2, 0=side 1
    bool is_side_1 = ((x3 & 0x04) == 0);
    context.set(field_id, "vbi", "programme_status_is_side_1",
                is_side_1 ? 1 : 0);

    // Teletext (x33): 1=present, 0=not present
    bool has_teletext = ((x3 & 0x02) != 0);
    context.set(field_id, "vbi", "programme_status_has_teletext",
                has_teletext ? 1 : 0);

    // Digital video (x42): 1=digital, 0=analogue
    bool is_digital = ((x4 & 0x04) != 0);
    context.set(field_id, "vbi", "programme_status_is_digital",
                is_digital ? 1 : 0);

    // The audio channel status is given by x41, x34, x43 and x44 combined
    uint32_t audio_status = 0;
    if ((x4 & 0x08) == 0x08) audio_status += 8;  // X41
    if ((x3 & 0x01) == 0x01) audio_status += 4;  // X34
    if ((x4 & 0x02) == 0x02) audio_status += 2;  // X43
    if ((x4 & 0x01) == 0x01) audio_status += 1;  // X44

    bool is_programme_dump = false;
    bool is_fm_multiplex = false;
    int sound_mode = static_cast<int>(VbiSoundMode::STEREO);

    switch (audio_status) {
      case 0:
        sound_mode = static_cast<int>(VbiSoundMode::STEREO);
        break;
      case 1:
        sound_mode = static_cast<int>(VbiSoundMode::MONO);
        break;
      case 2:
        sound_mode = static_cast<int>(VbiSoundMode::FUTURE_USE);
        break;
      case 3:
        sound_mode = static_cast<int>(VbiSoundMode::BILINGUAL);
        break;
      case 4:
        is_fm_multiplex = true;
        sound_mode = static_cast<int>(VbiSoundMode::STEREO_STEREO);
        break;
      case 5:
        is_fm_multiplex = true;
        sound_mode = static_cast<int>(VbiSoundMode::STEREO_BILINGUAL);
        break;
      case 6:
        is_fm_multiplex = true;
        sound_mode = static_cast<int>(VbiSoundMode::CROSS_CHANNEL_STEREO);
        break;
      case 7:
        is_fm_multiplex = true;
        sound_mode = static_cast<int>(VbiSoundMode::BILINGUAL_BILINGUAL);
        break;
      case 8:
      case 9:
        is_programme_dump = true;
        sound_mode = static_cast<int>(VbiSoundMode::MONO_DUMP);
        break;
      case 10:
        is_programme_dump = true;
        sound_mode = static_cast<int>(VbiSoundMode::FUTURE_USE);
        break;
      case 11:
        is_programme_dump = true;
        sound_mode = static_cast<int>(VbiSoundMode::MONO_DUMP);
        break;
      case 12:
      case 13:
        is_programme_dump = true;
        is_fm_multiplex = true;
        sound_mode = static_cast<int>(VbiSoundMode::STEREO_DUMP);
        break;
      case 14:
      case 15:
        is_programme_dump = true;
        is_fm_multiplex = true;
        sound_mode = static_cast<int>(VbiSoundMode::BILINGUAL_DUMP);
        break;
      default:
        sound_mode = static_cast<int>(VbiSoundMode::STEREO);
        break;
    }

    context.set(field_id, "vbi", "programme_status_is_programme_dump",
                is_programme_dump ? 1 : 0);
    context.set(field_id, "vbi", "programme_status_is_fm_multiplex",
                is_fm_multiplex ? 1 : 0);
    context.set(field_id, "vbi", "programme_status_sound_mode", sound_mode);

    ORC_LOG_DEBUG(
        "BiphaseObserver: Programme status - CX={}, size={}, side={}, "
        "digital={}, audio_status={}",
        cx_enabled, is_12_inch ? 12 : 8, is_side_1 ? 1 : 2, is_digital,
        audio_status);
  }

  // IEC 60857-1986 - 10.1.8 Amendment 2 status code
  // -------------------------------- Amendment 2 uses the same status code
  // location as programme status (line 16) Decode Amendment 2 in all cases
  // (parallel to programme status)
  if ((vbi16 & 0xFFF000) == 0x8DC000 || (vbi16 & 0xFFF000) == 0x8BA000) {
    // Extract x3, x4, x5 parameters
    uint32_t x3 = (vbi16 & 0x000F00) >> 8;
    uint32_t x4 = (vbi16 & 0x0000F0) >> 4;

    // Copy permitted flag (x34): 1=copy permitted, 0=copy prohibited
    bool copy_permitted = ((x3 & 0x01) != 0);
    context.set(field_id, "vbi", "amendment2_status_copy_permitted",
                copy_permitted ? 1 : 0);

    // For Amendment 2, the audio status is in x41, x42, x43, x44 (all 4 bits of
    // x4)
    uint32_t am2_audio_status = (x4 & 0x0F);

    // Decode Amendment 2 audio status
    bool is_video_standard = true;
    int am2_sound_mode = static_cast<int>(VbiSoundMode::STEREO);

    switch (am2_audio_status) {
      case 0:
        is_video_standard = true;
        am2_sound_mode = static_cast<int>(VbiSoundMode::STEREO);
        break;
      case 1:
        is_video_standard = true;
        am2_sound_mode = static_cast<int>(VbiSoundMode::MONO);
        break;
      case 2:
        is_video_standard = false;
        am2_sound_mode = static_cast<int>(VbiSoundMode::FUTURE_USE);
        break;
      case 3:
        is_video_standard = true;
        am2_sound_mode = static_cast<int>(VbiSoundMode::BILINGUAL);
        break;
      case 4:
      case 5:
      case 6:
      case 7:
        is_video_standard = false;
        am2_sound_mode = static_cast<int>(VbiSoundMode::FUTURE_USE);
        break;
      case 8:
        is_video_standard = true;
        am2_sound_mode = static_cast<int>(VbiSoundMode::MONO_DUMP);
        break;
      case 9:
      case 10:
      case 11:
      case 12:
      case 13:
      case 14:
      case 15:
        is_video_standard = false;
        am2_sound_mode = static_cast<int>(VbiSoundMode::FUTURE_USE);
        break;
      default:
        is_video_standard = false;
        am2_sound_mode = static_cast<int>(VbiSoundMode::STEREO);
        break;
    }

    context.set(field_id, "vbi", "amendment2_status_is_video_standard",
                is_video_standard ? 1 : 0);
    context.set(field_id, "vbi", "amendment2_status_sound_mode",
                am2_sound_mode);

    ORC_LOG_DEBUG(
        "BiphaseObserver: Amendment 2 status - copy_permitted={}, "
        "video_standard={}, sound_mode={}",
        copy_permitted, is_video_standard, am2_sound_mode);
  }

  // IEC 60857-1986 - 10.1.9 Users code
  // ----------------------------------------------
  if ((vbi16 & 0xF0F000) == 0x80D000) {
    uint32_t x1 = (vbi16 & 0x0F0000) >> 16;
    uint32_t x3x4x5 = (vbi16 & 0x000FFF);

    if (x1 <= 7) {
      // Format as hex string
      char user_code_str[8];
      snprintf(user_code_str, sizeof(user_code_str), "%01X%03X", x1, x3x4x5);
      context.set(field_id, "vbi", "user_code", std::string(user_code_str));
      ORC_LOG_DEBUG("BiphaseObserver: User code = {}", user_code_str);
    } else {
      ORC_LOG_DEBUG("BiphaseObserver: Invalid user code (X1 > 7)");
    }
  }
}

void BiphaseObserver::process_frame(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    IObservationContext& context) {
  auto vp_opt = representation.get_video_parameters();
  if (!vp_opt.has_value()) {
    ORC_LOG_TRACE("BiphaseObserver: No video parameters for frame {}",
                  frame_id);
    return;
  }
  const auto& vp = vp_opt.value();

  // 4FSC sample rate is fully determined by the video system.
  double sample_rate = sample_rate_from_system(vp.system);
  size_t active_start = static_cast<size_t>(vp.active_video_start);
  size_t line_width = static_cast<size_t>(vp.frame_width_nominal);
  size_t f1_lines = field1_lines(vp.system);

  // CVBS_U10_4FSC zero-crossing: midpoint between blanking and white.
  int16_t zero_crossing =
      static_cast<int16_t>((vp.white_level + vp.blanking_level) / 2);

  for (size_t field_idx = 0; field_idx < 2; ++field_idx) {
    FieldID derived_fid(frame_id * 2 + field_idx);
    size_t line_offset = (field_idx == 0) ? 0 : f1_lines;
    size_t field_height = (field_idx == 0)
                              ? f1_lines
                              : static_cast<size_t>(vp.frame_height) - f1_lines;

    // Decode lines 16, 17, 18 (VBI lines; 0-based: positions 15, 16, 17)
    std::array<int32_t, 3> vbi_data = {0, 0, 0};
    int lines_decoded = 0;

    for (int line_offset_vbi = 0; line_offset_vbi < 3; ++line_offset_vbi) {
      size_t field_line = 15 + static_cast<size_t>(line_offset_vbi);
      if (field_line >= field_height) {
        vbi_data[line_offset_vbi] = -1;
        continue;
      }

      const int16_t* line_data =
          representation.has_separate_channels()
              ? representation.get_line_luma(frame_id, line_offset + field_line)
              : representation.get_line(frame_id, line_offset + field_line);
      if (!line_data) {
        vbi_data[line_offset_vbi] = -1;
        continue;
      }

      vbi_data[line_offset_vbi] = decode_manchester(
          line_data, line_width, zero_crossing, active_start, sample_rate);

      if (vbi_data[line_offset_vbi] != 0 && vbi_data[line_offset_vbi] != -1) {
        lines_decoded++;
      }
    }

    // Store the decoded VBI data in observation context
    if (lines_decoded > 0) {
      context.set(derived_fid, "biphase", "vbi_line_16", vbi_data[0]);
      context.set(derived_fid, "biphase", "vbi_line_17", vbi_data[1]);
      context.set(derived_fid, "biphase", "vbi_line_18", vbi_data[2]);

      ORC_LOG_DEBUG(
          "BiphaseObserver: Decoded {} VBI lines for field {}: {:08x} {:08x} "
          "{:08x}",
          lines_decoded, derived_fid.value(), vbi_data[0], vbi_data[1],
          vbi_data[2]);

      // Interpret the VBI data according to IEC 60857 standard
      interpret_vbi_data(vbi_data[0], vbi_data[1], vbi_data[2], derived_fid,
                         context);
    } else {
      ORC_LOG_TRACE("BiphaseObserver: No biphase data decoded for field {}",
                    derived_fid.value());
    }
  }
}

}  // namespace orc
