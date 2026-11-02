/*
 * File:        nabts_playlist.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     A broadcast automation playlist read out of a datacast
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_PLAYLIST_H
#define ORC_NABTS_PLAYLIST_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace orc {

// Events a payload must hold before it is read as a playlist: a run of a few
// is as likely a coincidence in other data.
constexpr size_t kNabtsPlaylistMinEvents = 16;

/**
 * @brief One event of a broadcast automation playlist
 *
 * A station's automation sent its playlist down a stream of the VBI, every
 * event of the list sent again as the list changed. Each event is a record
 * of its own:
 *
 *  - its date and key, digits around an '@' and ended by NUL: "010203@140561"
 *    is the 2nd of January 2003 and event 61 of the block at 14:05, the
 *    events of a block numbered in tens so that one inserted between two
 *    takes a number between them;
 *  - a type byte, then a house number in twelve characters;
 *  - two time codes, hours, minutes, seconds and frames in BCD: the event's
 *    duration, and a second time code that has so far always equalled it;
 *  - a byte 'F', then a text field padded to its end with 0xAA: the title in
 *    38 characters, a code in 8 (the commercial's or promotion's), and a
 *    one-character flag where there is one ('*').
 *
 * The record's opening, shortly before the date, carries the moment it was
 * sent: FD FC, two bytes, then month, day, hour, minute and second.
 */
struct NabtsPlaylistEvent {
  /// The date and key as sent: six digits each.
  std::string date;
  std::string key;
  char type = ' ';
  std::string house_number;
  /// "hh:mm:ss:ff".
  std::string duration;
  std::string second_time;
  std::string title;
  std::string code;
  std::string flag;
  /// When the first and last copies of the event were sent, "MM-DD
  /// hh:mm:ss", where their records say; empty where they do not.
  std::string first_sent;
  std::string last_sent;
  /// Copies of the event, the same in every field, the payload held.
  uint32_t sends = 0;
};

/**
 * @brief The playlist events @p payload holds
 *
 * Each event once, with how many times it was sent, in the order of the
 * schedule: by date, then key. Empty unless the payload holds at least
 * kNabtsPlaylistMinEvents of them. A character of the text field that did
 * not arrive printable reads as '?'. Pure.
 */
std::vector<NabtsPlaylistEvent> nabts_playlist_events(
    const std::vector<uint8_t>& payload);

/// The column titles of a playlist's table, in the order
/// nabts_playlist_row() gives a row's values.
std::vector<std::string> nabts_playlist_columns();
std::vector<std::string> nabts_playlist_row(const NabtsPlaylistEvent& event);

/// One line saying what a playlist holds: its events, the sends of them,
/// and the dates and blocks they cover.
std::string nabts_playlist_summary(
    const std::vector<NabtsPlaylistEvent>& events);

}  // namespace orc

#endif  // ORC_NABTS_PLAYLIST_H
