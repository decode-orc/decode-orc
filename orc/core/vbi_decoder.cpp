/*
 * File:        vbi_decoder.cpp
 * Module:      orc-core
 * Purpose:     VBI decoding API implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "vbi_decoder.h"

#include <cav_picture_number.h>
#include <clv_picture_number.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/support/logging.h>

#include <cstdio>

namespace {

// Helper function to check IEC 60857 parity (x51/x52/x53)
bool check_parity(uint32_t x4, uint32_t x5) {
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

// IEC 60856/60857-1986 - 10.1.6 Programme time code. Lines 17 and 18 carry
// redundant copies, so they are cross-validated through the same helper the
// biphase observer and the disc mapper use: two readable lines that disagree
// yield nothing rather than whichever line was looked at last. The hours
// digit sits outside the pattern the line is matched on, so the second copy
// is the only thing that can catch a flipped bit in it.
bool read_clv_hours_minutes(int32_t vbi_line_17, int32_t vbi_line_18,
                            int32_t& hours, int32_t& minutes) {
  hours = -1;
  minutes = -1;

  bool cross_validated = false;
  const auto time_code =
      orc::decode_clv_time_code(vbi_line_17, vbi_line_18, cross_validated);
  if (!time_code) {
    return false;
  }

  hours = time_code->hours;
  minutes = time_code->minutes;
  return true;
}

// IEC 60856/60857-1986 - 10.1.10 CLV picture number. This decoder is reached
// from the VBI dialog, which is not told the disc's video format, so the
// picture within the second is held to the standard's own field range rather
// than to a frame rate.
bool read_clv_seconds_picture(int32_t vbi_line_16, int32_t& seconds,
                              int32_t& picture) {
  seconds = -1;
  picture = -1;
  return orc::decode_clv_seconds_picture(
      vbi_line_16, orc::kClvPictureFieldRange, seconds, picture);
}

}  // namespace

namespace orc {

std::optional<VBIFieldInfo> VBIDecoder::decode_vbi(
    const ObservationContext& observation_context, FieldID field_id) {
  // Try to get VBI observations from the biphase namespace
  auto vbi_16_opt = observation_context.get(field_id, "biphase", "vbi_line_16");
  auto vbi_17_opt = observation_context.get(field_id, "biphase", "vbi_line_17");
  auto vbi_18_opt = observation_context.get(field_id, "biphase", "vbi_line_18");

  if (!vbi_16_opt || !vbi_17_opt || !vbi_18_opt) {
    ORC_LOG_DEBUG("VBIDecoder: No VBI data found for field {}",
                  field_id.value());
    VBIFieldInfo info;
    info.field_id = field_id;
    info.has_vbi_data = false;
    info.vbi_data = {0, 0, 0};
    info.error_message = "No VBI data available";
    return info;
  }

  // Extract int32_t values from variant
  int32_t vbi_16 = std::get<int32_t>(*vbi_16_opt);
  int32_t vbi_17 = std::get<int32_t>(*vbi_17_opt);
  int32_t vbi_18 = std::get<int32_t>(*vbi_18_opt);

  ORC_LOG_DEBUG("VBIDecoder: Field {} raw VBI lines: {:#08x}, {:#08x}, {:#08x}",
                field_id.value(), vbi_16, vbi_17, vbi_18);

  return parse_vbi_data(field_id, vbi_16, vbi_17, vbi_18, observation_context);
}

VBIFieldInfo VBIDecoder::parse_vbi_data(
    FieldID field_id, int32_t vbi_line_16, int32_t vbi_line_17,
    int32_t vbi_line_18, const ObservationContext& observation_context) {
  VBIFieldInfo info;
  info.field_id = field_id;
  info.has_vbi_data = true;
  info.vbi_data = {vbi_line_16, vbi_line_17, vbi_line_18};

  (void)observation_context;

  // ------------------------------------------------------------------------
  // Decode from raw VBI lines (IEC 60857)
  // ------------------------------------------------------------------------

  // Picture numbers (CAV) - lines 17/18. Cross-validated through the same
  // helper the biphase observer and the disc mapper use, so the dialog shows
  // the picture number the rest of the pipeline works from: two readable lines
  // that disagree report no picture number at all.
  const std::optional<int32_t> cav_picture_number =
      [&]() -> std::optional<int32_t> {
    if (auto pn = orc::decode_cav_picture_number(vbi_line_17, vbi_line_18)) {
      return pn->value;
    }
    return std::nullopt;
  }();

  // Chapter numbers - lines 17/18
  if ((vbi_line_17 & 0xF00FFF) == 0x800DDD) {
    int32_t chapter;
    if (orc::decode_vbi_bcd((vbi_line_17 & 0x07F000) >> 12, chapter)) {
      info.chapter_number = chapter;
    }
  }

  if ((vbi_line_18 & 0xF00FFF) == 0x800DDD) {
    int32_t chapter;
    if (orc::decode_vbi_bcd((vbi_line_18 & 0x07F000) >> 12, chapter)) {
      info.chapter_number = chapter;
    }
  }

  // CLV programme time code (hours/minutes) - lines 17/18
  CLVTimecode clv_tc{-1, -1, -1, -1};
  read_clv_hours_minutes(vbi_line_17, vbi_line_18, clv_tc.hours,
                         clv_tc.minutes);

  // CLV picture number (seconds/picture) - line 16
  bool has_clv_picture_number = read_clv_seconds_picture(
      vbi_line_16, clv_tc.seconds, clv_tc.picture_number);

  if (clv_tc.hours != -1 && clv_tc.minutes != -1 && clv_tc.seconds != -1 &&
      clv_tc.picture_number != -1) {
    info.clv_timecode = clv_tc;
  }

  if (!has_clv_picture_number && cav_picture_number.has_value()) {
    info.picture_number = cav_picture_number;
  }

  // Lead-in / lead-out / picture stop codes
  if (vbi_line_17 == 0x88FFFF || vbi_line_18 == 0x88FFFF) {
    info.lead_in = true;
  }

  if (vbi_line_17 == 0x80EEEE || vbi_line_18 == 0x80EEEE) {
    info.lead_out = true;
  }

  if (vbi_line_16 == 0x82CFFF || vbi_line_17 == 0x82CFFF) {
    info.stop_code_present = true;
  }

  // Programme status (line 16)
  if ((vbi_line_16 & 0xFFF000) == 0x8DC000 ||
      (vbi_line_16 & 0xFFF000) == 0x8BA000) {
    ProgrammeStatus prog_status;

    bool cx_enabled = ((vbi_line_16 & 0x0FF000) == 0x0DC000);
    prog_status.cx_enabled = cx_enabled;

    uint32_t x3 = (vbi_line_16 & 0x000F00) >> 8;
    uint32_t x4 = (vbi_line_16 & 0x0000F0) >> 4;
    uint32_t x5 = (vbi_line_16 & 0x00000F);

    prog_status.parity_valid = check_parity(x4, x5);

    prog_status.is_12_inch = ((x3 & 0x08) == 0);
    prog_status.is_side_1 = ((x3 & 0x04) == 0);
    prog_status.has_teletext = ((x3 & 0x02) != 0);
    prog_status.is_digital = ((x4 & 0x04) != 0);

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

    prog_status.is_programme_dump = is_programme_dump;
    prog_status.is_fm_multiplex = is_fm_multiplex;
    prog_status.sound_mode = static_cast<VbiSoundMode>(sound_mode);

    info.programme_status = prog_status;
  }

  // Amendment 2 status (line 16)
  if ((vbi_line_16 & 0xFFF000) == 0x8DC000 ||
      (vbi_line_16 & 0xFFF000) == 0x8BA000) {
    Amendment2Status am2_status;

    uint32_t x3 = (vbi_line_16 & 0x000F00) >> 8;
    uint32_t x4 = (vbi_line_16 & 0x0000F0) >> 4;

    am2_status.copy_permitted = ((x3 & 0x01) != 0);

    uint32_t am2_audio_status = (x4 & 0x0F);
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

    am2_status.is_video_standard = is_video_standard;
    am2_status.sound_mode = static_cast<VbiSoundMode>(am2_sound_mode);

    info.amendment2_status = am2_status;
  }

  // User code (line 16)
  if ((vbi_line_16 & 0xF0F000) == 0x80D000) {
    uint32_t x1 = (vbi_line_16 & 0x0F0000) >> 16;
    uint32_t x3x4x5 = (vbi_line_16 & 0x000FFF);

    if (x1 <= 7) {
      char user_code_str[8];
      std::snprintf(user_code_str, sizeof(user_code_str), "%01X%03X", x1,
                    x3x4x5);
      info.user_code = std::string(user_code_str);
    }
  }

  ORC_LOG_DEBUG(
      "VBIDecoder: Parsed VBI for field {} - lines: {:#08x}, {:#08x}, {:#08x}",
      field_id.value(), vbi_line_16, vbi_line_17, vbi_line_18);

  return info;
}

VBIFieldInfo VBIDecoder::merge_frame_vbi(const VBIFieldInfo& field1_info,
                                         const VBIFieldInfo& field2_info) {
  VBIFieldInfo merged;

  // Use first field ID as base
  merged.field_id = field1_info.field_id;

  // Has VBI data if either field has it
  merged.has_vbi_data = field1_info.has_vbi_data || field2_info.has_vbi_data;

  ORC_LOG_DEBUG(
      "VBIDecoder: Frame merge raw VBI field1: {:#08x}, {:#08x}, {:#08x} | "
      "field2: {:#08x}, {:#08x}, {:#08x}",
      field1_info.vbi_data[0], field1_info.vbi_data[1], field1_info.vbi_data[2],
      field2_info.vbi_data[0], field2_info.vbi_data[1],
      field2_info.vbi_data[2]);

  // Raw VBI data - prefer first field, use second as fallback
  merged.vbi_data =
      field1_info.has_vbi_data ? field1_info.vbi_data : field2_info.vbi_data;

  // Picture number - use whichever field has it (prefer first)
  if (field1_info.picture_number.has_value()) {
    merged.picture_number = field1_info.picture_number;
  } else if (field2_info.picture_number.has_value()) {
    merged.picture_number = field2_info.picture_number;
  }

  // CLV timecode - merge components from both fields (raw VBI lines)
  CLVTimecode merged_tc{-1, -1, -1, -1};

  int32_t hours = -1, minutes = -1;
  int32_t seconds = -1, picture = -1;

  if (!field1_info.vbi_data.empty()) {
    read_clv_hours_minutes(field1_info.vbi_data[1], field1_info.vbi_data[2],
                           hours, minutes);
    read_clv_seconds_picture(field1_info.vbi_data[0], seconds, picture);
  }

  if (hours == -1 || minutes == -1) {
    if (!field2_info.vbi_data.empty()) {
      read_clv_hours_minutes(field2_info.vbi_data[1], field2_info.vbi_data[2],
                             hours, minutes);
    }
  }

  if (seconds == -1 || picture == -1) {
    if (!field2_info.vbi_data.empty()) {
      read_clv_seconds_picture(field2_info.vbi_data[0], seconds, picture);
    }
  }

  if (hours != -1 && minutes != -1 && seconds != -1 && picture != -1) {
    merged_tc.hours = hours;
    merged_tc.minutes = minutes;
    merged_tc.seconds = seconds;
    merged_tc.picture_number = picture;
    merged.clv_timecode = merged_tc;
  }

  // Chapter number - use whichever field has it (prefer first)
  if (field1_info.chapter_number.has_value()) {
    merged.chapter_number = field1_info.chapter_number;
  } else if (field2_info.chapter_number.has_value()) {
    merged.chapter_number = field2_info.chapter_number;
  }

  // User code - prefer first field
  if (field1_info.user_code.has_value()) {
    merged.user_code = field1_info.user_code;
  } else if (field2_info.user_code.has_value()) {
    merged.user_code = field2_info.user_code;
  }

  // Control codes - OR together from both fields
  merged.lead_in = field1_info.lead_in || field2_info.lead_in;
  merged.lead_out = field1_info.lead_out || field2_info.lead_out;
  merged.stop_code_present =
      field1_info.stop_code_present || field2_info.stop_code_present;

  // Programme status - merge across both fields
  if (field1_info.programme_status.has_value() ||
      field2_info.programme_status.has_value()) {
    ProgrammeStatus merged_status{};
    const ProgrammeStatus* ps1 = field1_info.programme_status.has_value()
                                     ? &field1_info.programme_status.value()
                                     : nullptr;
    const ProgrammeStatus* ps2 = field2_info.programme_status.has_value()
                                     ? &field2_info.programme_status.value()
                                     : nullptr;

    if (ps1 && ps1->cx_enabled) merged_status.cx_enabled = true;
    if (ps2 && ps2->cx_enabled) merged_status.cx_enabled = true;

    if (ps1 && ps1->is_12_inch) merged_status.is_12_inch = true;
    if (ps2 && ps2->is_12_inch) merged_status.is_12_inch = true;

    if (ps1 && ps1->is_side_1) merged_status.is_side_1 = true;
    if (ps2 && ps2->is_side_1) merged_status.is_side_1 = true;

    if (ps1 && ps1->has_teletext) merged_status.has_teletext = true;
    if (ps2 && ps2->has_teletext) merged_status.has_teletext = true;

    if (ps1 && ps1->is_digital) merged_status.is_digital = true;
    if (ps2 && ps2->is_digital) merged_status.is_digital = true;

    if (ps1 && ps1->is_fm_multiplex) merged_status.is_fm_multiplex = true;
    if (ps2 && ps2->is_fm_multiplex) merged_status.is_fm_multiplex = true;

    if (ps1 && ps1->is_programme_dump) merged_status.is_programme_dump = true;
    if (ps2 && ps2->is_programme_dump) merged_status.is_programme_dump = true;

    if (ps1 && ps1->parity_valid) merged_status.parity_valid = true;
    if (ps2 && ps2->parity_valid) merged_status.parity_valid = true;

    if (ps1 && ps1->sound_mode != VbiSoundMode::FUTURE_USE) {
      merged_status.sound_mode = ps1->sound_mode;
    } else if (ps2 && ps2->sound_mode != VbiSoundMode::FUTURE_USE) {
      merged_status.sound_mode = ps2->sound_mode;
    }

    merged.programme_status = merged_status;
  }

  // Amendment 2 status - merge across both fields
  if (field1_info.amendment2_status.has_value() ||
      field2_info.amendment2_status.has_value()) {
    Amendment2Status merged_status{};
    const Amendment2Status* a1 = field1_info.amendment2_status.has_value()
                                     ? &field1_info.amendment2_status.value()
                                     : nullptr;
    const Amendment2Status* a2 = field2_info.amendment2_status.has_value()
                                     ? &field2_info.amendment2_status.value()
                                     : nullptr;

    if (a1 && a1->copy_permitted) merged_status.copy_permitted = true;
    if (a2 && a2->copy_permitted) merged_status.copy_permitted = true;

    if (a1 && a1->is_video_standard) merged_status.is_video_standard = true;
    if (a2 && a2->is_video_standard) merged_status.is_video_standard = true;

    if (a1 && a1->sound_mode != VbiSoundMode::FUTURE_USE) {
      merged_status.sound_mode = a1->sound_mode;
    } else if (a2 && a2->sound_mode != VbiSoundMode::FUTURE_USE) {
      merged_status.sound_mode = a2->sound_mode;
    }

    merged.amendment2_status = merged_status;
  }

  ORC_LOG_DEBUG("VBIDecoder: Merged frame VBI from fields {} and {}",
                field1_info.field_id.value(), field2_info.field_id.value());

  return merged;
}

}  // namespace orc
