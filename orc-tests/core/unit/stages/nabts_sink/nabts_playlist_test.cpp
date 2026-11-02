/*
 * File:        nabts_playlist_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the automation playlist read out of a datacast
 *
 * Covers: an event's fields — date and key, type, house number, the BCD time
 * codes, title, code and flag — the moment its record was sent, events sent
 * again counted once, the schedule's order, and too few events not read as
 * a playlist.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_playlist.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::NabtsPlaylistEvent;

void append(std::vector<uint8_t>& out, const std::string& text) {
  out.insert(out.end(), text.begin(), text.end());
}

/// One event's record: its opening, sent at 01-02 14:05:|second|, then the
/// fields, the text padded to fifty bytes with 0xAA, and four check bytes.
void event(std::vector<uint8_t>& out, const std::string& key,
           const std::string& house, uint8_t seconds, const std::string& title,
           const std::string& code, const std::string& flag = "",
           uint8_t sent_second = 13) {
  out.insert(out.end(), {0xFD, 0xFC, 0xFF, 0x02, 0x01, 0x02, 0x0E, 0x05,
                         sent_second, 0x00, 0x00, 0x00, 0x1B, 0x00, 0x16});
  append(out, "010203@" + key);
  out.push_back(0x00);
  out.push_back('I');
  std::string h = house;
  h.resize(12, ' ');
  append(out, h);
  for (int copy = 0; copy < 2; ++copy) {
    out.insert(out.end(), {0x00, 0x00, seconds, 0x00});
  }
  out.push_back('F');
  std::string text = title;
  text.resize(38, ' ');
  std::string c = code;
  c.resize(8, ' ');
  text += c + flag;
  append(out, text);
  out.insert(out.end(), 50 - text.size(), 0xAA);
  out.insert(out.end(), {0x12, 0x34, 0x56, 0x78});
}

/// |events| events, numbered in tens within blocks of nine: 211010 to
/// 211090, then 211110 and on.
std::vector<uint8_t> playlist(size_t events) {
  std::vector<uint8_t> out = {0x00, 0x00, 0x30, 0x00};
  for (size_t i = 0; i < events; ++i) {
    const std::string key =
        std::to_string(2110 + i / 9) + std::to_string(10 * (i % 9 + 1));
    event(out, key, "PGCE" + std::to_string(i), 0x15, "CTER & GA-VT-CREST",
          "02AF" + std::to_string(i));
  }
  return out;
}

TEST(NabtsPlaylistTest, AnEventReadsAsItsFields) {
  std::vector<uint8_t> payload = playlist(20);
  // The first event sent again, a second later, and one with a flag.
  event(payload, "211010", "PGCE0", 0x15, "CTER & GA-VT-CREST", "02AF0", "",
        14);
  event(payload, "211061", "ZYRA1093", 0x30, "LATE SHOW (CONT'D)", "", "*");
  const std::vector<NabtsPlaylistEvent> events =
      orc::nabts_playlist_events(payload);
  ASSERT_EQ(events.size(), 21U);
  const NabtsPlaylistEvent& first = events[0];
  EXPECT_EQ(first.date, "010203");
  EXPECT_EQ(first.key, "211010");
  EXPECT_EQ(first.type, 'I');
  EXPECT_EQ(first.house_number, "PGCE0");
  EXPECT_EQ(first.duration, "00:00:15:00");
  EXPECT_EQ(first.second_time, "00:00:15:00");
  EXPECT_EQ(first.title, "CTER & GA-VT-CREST");
  EXPECT_EQ(first.code, "02AF0");
  EXPECT_EQ(first.flag, "");
  EXPECT_EQ(first.sends, 2U);
  EXPECT_EQ(first.first_sent, "01-02 14:05:13");
  EXPECT_EQ(first.last_sent, "01-02 14:05:14");

  // Inserted between events 60 and 70 of its block, as its key says.
  const NabtsPlaylistEvent* inserted = nullptr;
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].key == "211061") {
      inserted = &events[i];
      EXPECT_EQ(events[i - 1].key, "211060");
      EXPECT_EQ(events[i + 1].key, "211070");
    }
  }
  ASSERT_NE(inserted, nullptr);
  EXPECT_EQ(inserted->flag, "*");
  EXPECT_EQ(inserted->code, "");
  EXPECT_EQ(inserted->duration, "00:00:30:00");

  const std::vector<std::string> row = orc::nabts_playlist_row(first);
  EXPECT_EQ(row.size(), orc::nabts_playlist_columns().size());
  EXPECT_NE(orc::nabts_playlist_summary(events).find("21 events in 3 blocks"),
            std::string::npos);
}

TEST(NabtsPlaylistTest, AFewEventsAreNotAPlaylist) {
  EXPECT_TRUE(
      orc::nabts_playlist_events(playlist(orc::kNabtsPlaylistMinEvents - 1))
          .empty());
  EXPECT_TRUE(orc::nabts_playlist_events({}).empty());
}

}  // namespace
}  // namespace orc_unit_test
