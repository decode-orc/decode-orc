/*
 * File:        nabts_playlist.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     A broadcast automation playlist read out of a datacast
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_playlist.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace orc {
namespace {

constexpr size_t kDigits = 6;
constexpr uint8_t kAt = '@';
// From the date's first digit: the key's NUL, the type byte, the house
// number, the two time codes, and the byte before the text.
constexpr size_t kNulAt = 2 * kDigits + 1;
constexpr size_t kTypeAt = kNulAt + 1;
constexpr size_t kHouseAt = kTypeAt + 1;
constexpr size_t kHouseChars = 12;
constexpr size_t kTimeAt = kHouseAt + kHouseChars;
constexpr size_t kTimeBytes = 4;
constexpr size_t kTextMarkAt = kTimeAt + 2 * kTimeBytes;
constexpr uint8_t kTextMark = 'F';
constexpr size_t kTextAt = kTextMarkAt + 1;
// The text field: the title, the code, the flag, padded with 0xAA.
constexpr size_t kTitleChars = 38;
constexpr size_t kCodeChars = 8;
constexpr size_t kTextMaxChars = kTitleChars + kCodeChars + 1;
constexpr uint8_t kTextPad = 0xAA;
// The record's opening, which carries when it was sent: FD FC, two bytes,
// then month, day, hour, minute and second; looked for this far before the
// date.
constexpr uint8_t kOpening0 = 0xFD;
constexpr uint8_t kOpening1 = 0xFC;
constexpr size_t kOpeningSearch = 48;
constexpr size_t kSentAt = 4;

bool is_digit(uint8_t byte) { return byte >= '0' && byte <= '9'; }
bool is_printable(uint8_t byte) { return byte >= 0x20 && byte < 0x7F; }

bool digits_at(const std::vector<uint8_t>& bytes, size_t at, size_t count) {
  if (at + count > bytes.size()) {
    return false;
  }
  for (size_t i = at; i < at + count; ++i) {
    if (!is_digit(bytes[i])) {
      return false;
    }
  }
  return true;
}

/// A time code of four BCD bytes as "hh:mm:ss:ff", or empty where a nibble
/// is not a decimal digit.
std::string time_code(const std::vector<uint8_t>& bytes, size_t at) {
  std::string out;
  for (size_t i = 0; i < kTimeBytes; ++i) {
    const uint8_t byte = bytes[at + i];
    if ((byte >> 4U) > 9 || (byte & 0xFU) > 9) {
      return {};
    }
    out += fmt::format("{}{:02X}", i == 0 ? "" : ":", byte);
  }
  return out;
}

std::string trimmed(std::string text) {
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
  size_t from = 0;
  while (from < text.size() && text[from] == ' ') {
    ++from;
  }
  return text.substr(from);
}

/// When the record the event at |date_at| opens with says it was sent, or
/// empty: the last opening before the date whose fields are a date and time.
std::string sent_before(const std::vector<uint8_t>& bytes, size_t date_at) {
  const size_t from = date_at > kOpeningSearch ? date_at - kOpeningSearch : 0;
  for (size_t i = date_at; i-- > from;) {
    if (bytes[i] != kOpening0 || i + 1 >= bytes.size() ||
        bytes[i + 1] != kOpening1 || i + kSentAt + 5 >= date_at) {
      continue;
    }
    const size_t at = i + kSentAt;
    const unsigned month = bytes[at];
    const unsigned day = bytes[at + 1];
    const unsigned hour = bytes[at + 2];
    const unsigned minute = bytes[at + 3];
    const unsigned second = bytes[at + 4];
    if (month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour < 24 &&
        minute < 60 && second < 60) {
      return fmt::format("{:02}-{:02} {:02}:{:02}:{:02}", month, day, hour,
                         minute, second);
    }
    return {};
  }
  return {};
}

}  // namespace

std::vector<NabtsPlaylistEvent> nabts_playlist_events(
    const std::vector<uint8_t>& payload) {
  using Key =
      std::tuple<std::string, std::string, char, std::string, std::string,
                 std::string, std::string, std::string, std::string>;
  std::map<Key, NabtsPlaylistEvent> events;
  for (size_t at = 0; at + kTextAt < payload.size(); ++at) {
    if (payload[at + kDigits] != kAt || !digits_at(payload, at, kDigits) ||
        !digits_at(payload, at + kDigits + 1, kDigits) ||
        payload[at + kNulAt] != 0 || payload[at + kTextMarkAt] != kTextMark) {
      continue;
    }
    NabtsPlaylistEvent event;
    event.date.assign(
        payload.begin() + static_cast<std::ptrdiff_t>(at),
        payload.begin() + static_cast<std::ptrdiff_t>(at + kDigits));
    event.key.assign(
        payload.begin() + static_cast<std::ptrdiff_t>(at + kDigits + 1),
        payload.begin() + static_cast<std::ptrdiff_t>(at + kNulAt));
    event.type = static_cast<char>(
        is_printable(payload[at + kTypeAt]) ? payload[at + kTypeAt] : '?');
    std::string house;
    for (size_t i = at + kHouseAt; i < at + kTimeAt; ++i) {
      house += static_cast<char>(is_printable(payload[i]) ? payload[i] : '?');
    }
    event.house_number = trimmed(house);
    event.duration = time_code(payload, at + kTimeAt);
    event.second_time = time_code(payload, at + kTimeAt + kTimeBytes);
    std::string text;
    for (size_t i = at + kTextAt;
         i < payload.size() && text.size() < kTextMaxChars &&
         payload[i] != kTextPad;
         ++i) {
      text += static_cast<char>(is_printable(payload[i]) ? payload[i] : '?');
    }
    event.title = trimmed(text.substr(0, std::min(text.size(), kTitleChars)));
    if (text.size() > kTitleChars) {
      event.code = trimmed(text.substr(
          kTitleChars, std::min(text.size() - kTitleChars, kCodeChars)));
    }
    if (text.size() > kTitleChars + kCodeChars) {
      event.flag = trimmed(text.substr(kTitleChars + kCodeChars));
    }
    const std::string sent = sent_before(payload, at);
    const Key key{event.date,         event.key,      event.type,
                  event.house_number, event.duration, event.second_time,
                  event.title,        event.code,     event.flag};
    NabtsPlaylistEvent& held = events[key];
    if (held.sends == 0) {
      held = event;
    }
    ++held.sends;
    if (!sent.empty()) {
      if (held.first_sent.empty()) {
        held.first_sent = sent;
      }
      held.last_sent = sent;
    }
    at += kTextAt - 1;  // past this event's fixed fields
  }
  if (events.size() < kNabtsPlaylistMinEvents) {
    return {};
  }
  std::vector<NabtsPlaylistEvent> out;
  out.reserve(events.size());
  for (auto& [key, event] : events) {
    out.push_back(std::move(event));
  }
  // The schedule's order: a date as sent is month, day, year.
  std::stable_sort(
      out.begin(), out.end(),
      [](const NabtsPlaylistEvent& a, const NabtsPlaylistEvent& b) {
        const std::string da = a.date.substr(4, 2) + a.date.substr(0, 4);
        const std::string db = b.date.substr(4, 2) + b.date.substr(0, 4);
        return std::tie(da, a.key) < std::tie(db, b.key);
      });
  return out;
}

std::vector<std::string> nabts_playlist_columns() {
  return {"Date",  "Key",  "Type", "House number", "Duration",   "Second time",
          "Title", "Code", "Flag", "Sends",        "First sent", "Last sent"};
}

std::vector<std::string> nabts_playlist_row(const NabtsPlaylistEvent& event) {
  return {event.date,
          event.key,
          std::string(1, event.type),
          event.house_number,
          event.duration,
          event.second_time,
          event.title,
          event.code,
          event.flag,
          std::to_string(event.sends),
          event.first_sent,
          event.last_sent};
}

std::string nabts_playlist_summary(
    const std::vector<NabtsPlaylistEvent>& events) {
  if (events.empty()) {
    return {};
  }
  uint64_t sends = 0;
  std::set<std::string> dates;
  std::set<std::string> blocks;
  for (const NabtsPlaylistEvent& event : events) {
    sends += event.sends;
    dates.insert(event.date);
    blocks.insert(event.date + event.key.substr(0, 4));
  }
  return fmt::format(
      "An automation playlist: {} events in {} blocks over {} date{}, sent "
      "{} times in all as the list changed (date and key, house number, "
      "duration, title and code, as a table)",
      events.size(), blocks.size(), dates.size(), dates.size() == 1 ? "" : "s",
      sends);
}

}  // namespace orc
