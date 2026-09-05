/*
 * File:        disc_metadata_collect.cpp
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     Read LaserDisc VBI observations into DiscMetadataFrame records
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "disc_metadata_collect.h"

#include <orc/stage/field_id.h>

#include <string>

namespace orc {
namespace {

std::optional<int32_t> get_int(const IObservationContext& ctx, FieldID fid,
                               const char* ns, const char* key) {
  auto v = ctx.get(fid, ns, key);
  if (v && std::holds_alternative<int32_t>(*v)) return std::get<int32_t>(*v);
  return std::nullopt;
}

bool get_flag(const IObservationContext& ctx, FieldID fid, const char* key) {
  auto v = get_int(ctx, fid, "vbi", key);
  return v.has_value() && *v != 0;
}

std::optional<std::string> get_string(const IObservationContext& ctx,
                                      FieldID fid, const char* ns,
                                      const char* key) {
  auto v = ctx.get(fid, ns, key);
  if (v && std::holds_alternative<std::string>(*v)) {
    return std::get<std::string>(*v);
  }
  return std::nullopt;
}

// The biphase observer publishes the four CLV components separately and only
// when the whole timecode validated, so all four must be present.
std::optional<CLVTimecode> get_clv(const IObservationContext& ctx,
                                   FieldID fid) {
  auto h = get_int(ctx, fid, "vbi", "clv_timecode_hours");
  auto m = get_int(ctx, fid, "vbi", "clv_timecode_minutes");
  auto s = get_int(ctx, fid, "vbi", "clv_timecode_seconds");
  auto p = get_int(ctx, fid, "vbi", "clv_timecode_picture");
  if (!h || !m || !s || !p) return std::nullopt;
  CLVTimecode tc{};
  tc.hours = *h;
  tc.minutes = *m;
  tc.seconds = *s;
  tc.picture_number = *p;
  return tc;
}

std::optional<ProgrammeStatus> get_programme_status(
    const IObservationContext& ctx, FieldID fid) {
  auto cx = get_int(ctx, fid, "vbi", "programme_status_cx_enabled");
  if (!cx) return std::nullopt;  // the whole word is set together or not at all
  ProgrammeStatus ps;
  ps.cx_enabled = *cx != 0;
  auto b = [&](const char* key, bool fallback) {
    auto v = get_int(ctx, fid, "vbi", key);
    return v ? (*v != 0) : fallback;
  };
  ps.is_12_inch = b("programme_status_is_12_inch", true);
  ps.is_side_1 = b("programme_status_is_side_1", true);
  ps.has_teletext = b("programme_status_has_teletext", false);
  ps.is_digital = b("programme_status_is_digital", false);
  ps.is_fm_multiplex = b("programme_status_is_fm_multiplex", false);
  ps.is_programme_dump = b("programme_status_is_programme_dump", false);
  ps.parity_valid = b("programme_status_parity_valid", false);
  auto mode = get_int(ctx, fid, "vbi", "programme_status_sound_mode");
  ps.sound_mode = static_cast<VbiSoundMode>(mode.value_or(0));
  return ps;
}

std::optional<Amendment2Status> get_amendment2(const IObservationContext& ctx,
                                               FieldID fid) {
  auto copy = get_int(ctx, fid, "vbi", "amendment2_status_copy_permitted");
  if (!copy) return std::nullopt;
  Amendment2Status a2;
  a2.copy_permitted = *copy != 0;
  auto std_v = get_int(ctx, fid, "vbi", "amendment2_status_is_video_standard");
  a2.is_video_standard = std_v.value_or(0) != 0;
  auto mode = get_int(ctx, fid, "vbi", "amendment2_status_sound_mode");
  a2.sound_mode = static_cast<VbiSoundMode>(mode.value_or(0));
  return a2;
}

}  // namespace

std::vector<DiscMetadataFrame> collect_disc_metadata_frames(
    const IObservationContext& context, uint64_t start_field_index,
    uint64_t frame_count) {
  std::vector<DiscMetadataFrame> frames(static_cast<size_t>(frame_count));

  for (uint64_t n = 0; n < frame_count; ++n) {
    DiscMetadataFrame& f = frames[static_cast<size_t>(n)];

    for (uint64_t field = 0; field < 2; ++field) {
      const FieldID fid(
          static_cast<uint32_t>(start_field_index + n * 2 + field));
      const size_t slot = static_cast<size_t>(field);

      // Raw biphase words, 3 per field: f1 16/17/18 then f2 16/17/18.
      static const char* kRawKeys[3] = {"vbi_line_16", "vbi_line_17",
                                        "vbi_line_18"};
      for (size_t line = 0; line < 3; ++line) {
        auto raw = get_int(context, fid, "biphase", kRawKeys[line]);
        if (raw) f.raw_vbi[slot * 3 + line] = *raw;
      }

      // Frame-level values: take whichever field carries them.
      if (!f.picture_number) {
        f.picture_number = get_int(context, fid, "vbi", "picture_number");
      }
      if (!f.clv_timecode) f.clv_timecode = get_clv(context, fid);
      if (!f.chapter_number) {
        f.chapter_number = get_int(context, fid, "vbi", "chapter_number");
      }
      if (!f.user_code) {
        f.user_code = get_string(context, fid, "vbi", "user_code");
      }
      f.lead_in = f.lead_in || get_flag(context, fid, "lead_in");
      f.lead_out = f.lead_out || get_flag(context, fid, "lead_out");
      f.stop_code = f.stop_code || get_flag(context, fid, "stop_code_present");

      // Per-field: the status word rides on line 16 of every field, so
      // keeping both doubles the sample count the disc-level vote sees.
      f.programme_status[slot] = get_programme_status(context, fid);
      f.amendment2_status[slot] = get_amendment2(context, fid);
    }
  }

  return frames;
}

}  // namespace orc
