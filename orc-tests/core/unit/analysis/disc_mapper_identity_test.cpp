/*
 * File:        disc_mapper_identity_test.cpp
 * Module:      analysis
 * Purpose:     Unit tests for the Disc Mapper's disc identity statistics
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <cstdint>
#include <string>
#include <variant>

#include "../../../../orc/core/analysis/disc_mapper/disc_mapper_analysis.h"

namespace {

using orc::FieldID;
using orc::FieldMappingDecision;
using orc::FrameID;
using orc::FrameIDRange;
using orc::ObservationContext;
using orc::VideoSystem;

// Publishes on both fields of |frame| what the biphase observer publishes
// for a decoded programme status word: a 12-inch side-2 disc with CX, no
// teletext, in stereo; |cx| false for a field whose CX bit misread.
void set_status(ObservationContext& obs, FrameID frame, bool cx = true) {
  for (uint64_t field = 0; field < 2; ++field) {
    const FieldID fid(frame * 2 + field);
    obs.set(fid, "vbi", "programme_status_cx_enabled", int32_t{cx ? 1 : 0});
    obs.set(fid, "vbi", "programme_status_is_12_inch", int32_t{1});
    obs.set(fid, "vbi", "programme_status_is_side_1", int32_t{0});
    obs.set(fid, "vbi", "programme_status_has_teletext", int32_t{0});
    obs.set(fid, "vbi", "programme_status_is_digital", int32_t{0});
    obs.set(fid, "vbi", "programme_status_is_fm_multiplex", int32_t{0});
    obs.set(fid, "vbi", "programme_status_is_programme_dump", int32_t{0});
    obs.set(fid, "vbi", "programme_status_parity_valid", int32_t{1});
    obs.set(fid, "vbi", "programme_status_sound_mode", int32_t{0});
  }
}

void set_flag(ObservationContext& obs, FrameID frame, const char* key) {
  obs.set(FieldID(frame * 2), "vbi", key, int32_t{1});
}

FieldMappingDecision cav_mapping(int32_t first, int32_t last) {
  FieldMappingDecision decision;
  decision.success = true;
  decision.is_cav = true;
  decision.is_pal = true;
  decision.stats.first_picture = first;
  decision.stats.last_picture = last;
  return decision;
}

template <typename T>
T stat(const std::map<std::string, orc::StatisticValue>& stats,
       const std::string& key) {
  return std::get<T>(stats.at(key));
}

}  // namespace

// Ten frames: a lead-in frame carrying the user code, eight pictures with
// the status word (one field pair misreading CX), and a lead-out frame. The
// statistics are the disc metadata document's vote, and the pictures are
// the mapping's.
TEST(DiscMapperIdentity, ReportsWhatTheDiscSaysOfItself) {
  ObservationContext obs;
  set_flag(obs, 0, "lead_in");
  obs.set(FieldID(0), "vbi", "user_code", std::string("1234"));
  for (FrameID frame = 1; frame <= 8; ++frame) {
    set_status(obs, frame, /*cx=*/frame != 4);
    obs.set(FieldID(frame * 2), "vbi", "picture_number",
            static_cast<int32_t>(frame));
  }
  obs.set(FieldID(10), "vbi", "amendment2_status_copy_permitted", int32_t{1});
  set_flag(obs, 9, "lead_out");

  const auto stats = orc::disc_identity_statistics(
      obs, FrameIDRange{0, 9}, VideoSystem::PAL, cav_mapping(1, 8));

  EXPECT_EQ(stat<std::string>(stats, "userCode"), "1234");
  EXPECT_EQ(stat<int>(stats, "discSide"), 2);
  EXPECT_EQ(stat<int>(stats, "discSizeInches"), 12);
  EXPECT_TRUE(stat<bool>(stats, "cx"));
  EXPECT_FALSE(stat<bool>(stats, "teletext"));
  EXPECT_FALSE(stat<bool>(stats, "digitalVideo"));
  EXPECT_FALSE(stat<bool>(stats, "fmMultiplex"));
  EXPECT_FALSE(stat<bool>(stats, "programmeDump"));
  EXPECT_EQ(stat<std::string>(stats, "soundMode"), "stereo");
  EXPECT_TRUE(stat<bool>(stats, "amendment2"));
  EXPECT_TRUE(stat<bool>(stats, "amendment2CopyPermitted"));
  // 14 of the 16 status fields agree with the vote.
  EXPECT_DOUBLE_EQ(stat<double>(stats, "statusAgreement"), 0.875);
  EXPECT_EQ(stat<int>(stats, "firstPicture"), 1);
  EXPECT_EQ(stat<int>(stats, "lastPicture"), 8);
  EXPECT_TRUE(stat<bool>(stats, "leadInSeen"));
  EXPECT_TRUE(stat<bool>(stats, "leadOutSeen"));
}

// A capture that stops before the end of the side never sees the lead-out,
// and values never read are left out rather than guessed.
TEST(DiscMapperIdentity, LeavesOutWhatWasNeverSeen) {
  ObservationContext obs;
  set_flag(obs, 0, "lead_in");
  for (FrameID frame = 1; frame <= 5; ++frame) {
    obs.set(FieldID(frame * 2), "vbi", "picture_number",
            static_cast<int32_t>(frame + 100));
  }

  const auto stats = orc::disc_identity_statistics(
      obs, FrameIDRange{0, 5}, VideoSystem::PAL, cav_mapping(101, 105));

  EXPECT_TRUE(stat<bool>(stats, "leadInSeen"));
  EXPECT_FALSE(stat<bool>(stats, "leadOutSeen"));
  EXPECT_EQ(stat<int>(stats, "lastPicture"), 105);
  EXPECT_FALSE(stat<bool>(stats, "amendment2"));
  for (const char* key : {"userCode", "discSide", "cx", "soundMode",
                          "statusAgreement", "amendment2CopyPermitted"}) {
    EXPECT_FALSE(stats.count(key)) << key;
  }
}

// Pictures come from a CAV mapping only: a CLV mapping's numbers are time
// codes counted as frames, not picture numbers.
TEST(DiscMapperIdentity, GivesNoPicturesForAClvMapping) {
  ObservationContext obs;
  FieldMappingDecision decision = cav_mapping(1, 100);
  decision.is_cav = false;

  const auto stats = orc::disc_identity_statistics(obs, FrameIDRange{0, 3},
                                                   VideoSystem::PAL, decision);

  EXPECT_FALSE(stats.count("firstPicture"));
  EXPECT_FALSE(stats.count("lastPicture"));
}
