/*
 * File:        cc_xds_decoder.cpp
 * Module:      orc-core
 * Purpose:     Assemble and read the Extended Data Service packets of line 21
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "cc_xds_decoder.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace orc {

namespace {

// CTA-608-E §8: control codes 01h-0Eh start (odd) or continue (even) a packet
// of a class, 0Fh ends one; caption and text control codes are 10h-1Fh.
constexpr uint8_t kXdsFirstCode = 0x01;
constexpr uint8_t kXdsLastClassCode = 0x0E;
constexpr uint8_t kXdsEnd = 0x0F;
constexpr uint8_t kCaptionFirstControl = 0x10;
constexpr uint8_t kCaptionLastControl = 0x1F;
// A packet carries at most 32 informational characters [CTA-608-E §8].
constexpr size_t kXdsMaxCharacters = 32;
// Packets held open at once, waiting for their continue code.
constexpr size_t kXdsMaxOpen = 16;
// Distinct values the summary keeps per type.
constexpr size_t kSummaryMaxValues = 32;

constexpr uint8_t kClassCurrent = 0x01;
constexpr uint8_t kClassFuture = 0x03;
constexpr uint8_t kClassChannel = 0x05;
constexpr uint8_t kClassMisc = 0x07;
constexpr uint8_t kClassPublic = 0x09;
constexpr uint8_t kClassPrivate = 0x0D;

std::string text_of(const std::vector<uint8_t>& data) {
  std::string out;
  for (const uint8_t byte : data) {
    if (byte >= 0x20 && byte < 0x7F) {
      out += static_cast<char>(byte);
    }
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  while (!out.empty() && out.front() == ' ') {
    out.erase(out.begin());
  }
  return out;
}

std::string hex_of(const std::vector<uint8_t>& data) {
  std::string out;
  char buffer[4];
  for (const uint8_t byte : data) {
    std::snprintf(buffer, sizeof(buffer), "%02X", byte);
    if (!out.empty()) {
      out += ' ';
    }
    out += buffer;
  }
  return out;
}

bool is_programme_class(uint8_t xds_class) {
  return xds_class == kClassCurrent || xds_class == kClassFuture;
}

bool carries_text(uint8_t xds_class, uint8_t type) {
  if (is_programme_class(xds_class)) {
    // Programme name, and the eight programme description rows.
    return type == 0x03 || (type >= 0x10 && type <= 0x17);
  }
  if (xds_class == kClassChannel) {
    return type == 0x01 || type == 0x02;  // network name, call letters
  }
  if (xds_class == kClassPublic) {
    return type == 0x01 || type == 0x02;  // weather code and message
  }
  return false;
}

// CTA-608-E: the time of day is six characters — minute, hour (bit 5 daylight
// saving), date, month, day of the week, year since 1990 — each with bit 6 set,
// in UTC.
std::string time_of_day_text(const std::vector<uint8_t>& data) {
  if (data.size() < 6) {
    return {};
  }
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d UTC",
                1990 + (data[5] & 0x3F), data[3] & 0x0F, data[2] & 0x1F,
                data[1] & 0x1F, data[0] & 0x3F);
  return buffer;
}

// A programme's identification number is its scheduled start: minute, hour,
// date and month, in UTC [CTA-608-E §9.5.1.1, Table 15]. The month's T bit
// marks a programme the station routinely tape delays, and all four
// characters at their all-ones value end the current programme.
std::string programme_start_text(const std::vector<uint8_t>& data) {
  if (data.size() < 4) {
    return {};
  }
  if (std::all_of(data.begin(), data.begin() + 4,
                  [](uint8_t byte) { return byte == 0x7F; })) {
    return "end of programme";
  }
  char buffer[56];
  std::snprintf(buffer, sizeof(buffer), "starts %02d-%02d %02d:%02d UTC%s",
                data[3] & 0x0F, data[2] & 0x1F, data[1] & 0x1F, data[0] & 0x3F,
                (data[3] & 0x10) != 0 ? ", tape delayed" : "");
  return buffer;
}

// Hours and minutes as h:mm, from a minute and an hour character.
std::string hours_minutes(uint8_t minute, uint8_t hour) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%02d:%02d", hour & 0x3F,
                minute & 0x3F);
  return buffer;
}

// The programme's length, then the time it has run, in minutes and hours,
// then seconds [CTA-608-E §9.5.1.2, Table 16].
std::string length_text(const std::vector<uint8_t>& data) {
  if (data.size() < 2) {
    return {};
  }
  std::string out = hours_minutes(data[0], data[1]);
  if (data.size() >= 4) {
    out += ", " + hours_minutes(data[2], data[3]);
    if (data.size() >= 5) {
      char seconds[8];
      std::snprintf(seconds, sizeof(seconds), ":%02d", data[4] & 0x3F);
      out += seconds;
    }
    out += " elapsed";
  }
  return out;
}

// The keywords a programme type is sent as, one character each, 20h to 7Fh
// [CTA-608-E §9.5.1.4, Table 17].
constexpr std::array<const char*, 96> kProgrammeTypes = {
    "Education",     "Entertainment", "Movie",      "News",
    "Religious",     "Sports",        "Other",      "Action",
    "Advertisement", "Animated",      "Anthology",  "Automobile",
    "Awards",        "Baseball",      "Basketball", "Bulletin",
    "Business",      "Classical",     "College",    "Combat",
    "Comedy",        "Commentary",    "Concert",    "Consumer",
    "Contemporary",  "Crime",         "Dance",      "Documentary",
    "Drama",         "Elementary",    "Erotica",    "Exercise",
    "Fantasy",       "Farm",          "Fashion",    "Fiction",
    "Food",          "Football",      "Foreign",    "Fund Raiser",
    "Game/Quiz",     "Garden",        "Golf",       "Government",
    "Health",        "High School",   "History",    "Hobby",
    "Hockey",        "Home",          "Horror",     "Information",
    "Instruction",   "International", "Interview",  "Language",
    "Legal",         "Live",          "Local",      "Math",
    "Medical",       "Meeting",       "Military",   "Miniseries",
    "Music",         "Mystery",       "National",   "Nature",
    "Police",        "Politics",      "Premier",    "Prerecorded",
    "Product",       "Professional",  "Public",     "Racing",
    "Reading",       "Repair",        "Repeat",     "Review",
    "Romance",       "Science",       "Series",     "Service",
    "Shopping",      "Soap Opera",    "Special",    "Suspense",
    "Talk",          "Technical",     "Tennis",     "Travel",
    "Variety",       "Video",         "Weather",    "Western"};

std::string programme_type_text(const std::vector<uint8_t>& data) {
  std::string out;
  for (const uint8_t code : data) {
    if (code < 0x20) {
      continue;
    }
    if (!out.empty()) {
      out += ", ";
    }
    out += kProgrammeTypes[static_cast<size_t>((code & 0x7F) - 0x20)];
  }
  return out;
}

// Content advisory [CTA-608-E §9.5.1.5, Tables 18-23]: bits a1 a0 of the first
// character name the rating system, with a2 (first character's bit 5) and a3
// (the second's bit 3) where they are 1 1. The MPA rating is in the first
// character's low three bits, every other system's in the second's.
std::string content_advisory_text(const std::vector<uint8_t>& data) {
  if (data.size() < 2) {
    return {};
  }
  const uint8_t first = data[0];
  const uint8_t second = data[1];
  const unsigned system = (first >> 3U) & 0x03U;
  if (system == 0 || system == 2) {
    static constexpr std::array<const char*, 8> kMpa = {
        "N/A", "G", "PG", "PG-13", "R", "NC-17", "X", "Not Rated"};
    return std::string("MPA ") + kMpa[first & 0x07U];
  }
  const unsigned level = second & 0x07U;
  if (system == 1) {
    // U.S. TV Parental Guidelines [Table 21]: fantasy violence goes with
    // TV-Y7 alone; violence, sexual situations and adult language with TV-PG,
    // TV-14 and TV-MA; suggestive dialogue with TV-PG and TV-14.
    static constexpr std::array<const char*, 8> kTv = {
        "TV None", "TV-Y",  "TV-Y7", "TV-G",
        "TV-PG",   "TV-14", "TV-MA", "TV None"};
    std::string flags;
    const auto add = [&flags](const char* flag) {
      flags += flags.empty() ? flag : std::string(", ") + flag;
    };
    if (level == 2 && (second & 0x20U) != 0) {
      add("fantasy violence");
    }
    if (level >= 4 && level <= 6) {
      if ((second & 0x20U) != 0) {
        add("violence");
      }
      if ((second & 0x10U) != 0) {
        add("sexual situations");
      }
      if ((second & 0x08U) != 0) {
        add("adult language");
      }
      if (level != 6 && (first & 0x20U) != 0) {
        add("suggestive dialogue");
      }
    }
    return flags.empty() ? std::string(kTv[level])
                         : std::string(kTv[level]) + " (" + flags + ")";
  }
  const bool a2 = (first & 0x20U) != 0;
  const bool a3 = (second & 0x08U) != 0;
  if (!a3 && !a2) {
    // Canadian English Language [Table 22]; level 7 is invalid.
    static constexpr std::array<const char*, 8> kEnglish = {
        "E", "C", "C8+", "G", "PG", "14+", "18+", "(invalid)"};
    return std::string("Canadian English ") + kEnglish[level];
  }
  if (!a3 && a2) {
    // Canadian French Language [Table 23]; levels 6 and 7 are invalid.
    static constexpr std::array<const char*, 8> kFrench = {
        "E",        "G",        "8 ans +",   "13 ans +",
        "16 ans +", "18 ans +", "(invalid)", "(invalid)"};
    return std::string("Canadian French ") + kFrench[level];
  }
  return {};  // a system reserved for other countries
}

// Language of an audio or caption service [CTA-608-E Table 25].
constexpr std::array<const char*, 8> kLanguages = {
    "unknown language", "English", "Spanish",        "French",
    "German",           "Italian", "other language", "no language"};

// The main and second audio programmes [CTA-608-E §9.5.1.6, Tables 24-26].
std::string audio_services_text(const std::vector<uint8_t>& data) {
  if (data.size() < 2) {
    return {};
  }
  static constexpr std::array<const char*, 8> kMain = {"unknown",
                                                       "mono",
                                                       "simulated stereo",
                                                       "true stereo",
                                                       "stereo surround",
                                                       "data service",
                                                       "other",
                                                       "none"};
  static constexpr std::array<const char*, 8> kSecond = {"unknown",
                                                         "mono",
                                                         "video descriptions",
                                                         "non-program audio",
                                                         "special effects",
                                                         "data service",
                                                         "other",
                                                         "none"};
  return std::string("main ") + kLanguages[(data[0] >> 3U) & 0x07U] + ", " +
         kMain[data[0] & 0x07U] + "; second " +
         kLanguages[(data[1] >> 3U) & 0x07U] + ", " + kSecond[data[1] & 0x07U];
}

// The caption and text services a programme carries, a character each: its
// field (F), data channel (C) and whether it is text (T) [CTA-608-E §9.5.1.7,
// Tables 27 and 28], as the stage's service parameter names them.
std::string caption_services_text(const std::vector<uint8_t>& data) {
  static constexpr std::array<const char*, 8> kServices = {
      "CC1", "TEXT1", "CC2", "TEXT2", "CC3", "TEXT3", "CC4", "TEXT4"};
  std::string out;
  for (const uint8_t code : data) {
    if (!out.empty()) {
      out += ", ";
    }
    out += std::string(kServices[code & 0x07U]) + " " +
           kLanguages[(code >> 3U) & 0x07U];
  }
  return out;
}

// Copy generation and analogue protection [CTA-608-E §9.5.1.8, Tables 29-31].
std::string copy_control_text(const std::vector<uint8_t>& data) {
  if (data.empty()) {
    return {};
  }
  static constexpr std::array<const char*, 4> kCgms = {
      "copying permitted", "no more copies", "one copy permitted",
      "no copying"};
  static constexpr std::array<const char*, 4> kAps = {
      "no APS", "PSP on, split burst off", "PSP on, 2-line split burst",
      "PSP on, 4-line split burst"};
  std::string out = std::string(kCgms[(data[0] >> 3U) & 0x03U]) + ", " +
                    kAps[(data[0] >> 1U) & 0x03U];
  if ((data[0] & 0x01U) != 0) {
    out += ", analog source";
  }
  if (data.size() >= 2 && (data[1] & 0x01U) != 0) {
    out += ", redistribution controlled";
  }
  return out;
}

// The hours and minutes a station routinely tape delays its network's
// programmes [CTA-608-E §9.5.3.3, Table 34].
std::string tape_delay_text(const std::vector<uint8_t>& data) {
  if (data.size() < 2) {
    return {};
  }
  return hours_minutes(data[0], static_cast<uint8_t>(data[1] & 0x1F));
}

// The 16-bit Transmission Signal Identifier, four bits a character, the low
// four first [CTA-608-E §9.5.3.4, Table 35].
std::string transmission_signal_id_text(const std::vector<uint8_t>& data) {
  if (data.size() < 4) {
    return {};
  }
  unsigned id = 0;
  for (size_t i = 0; i < 4; ++i) {
    id |= static_cast<unsigned>(data[i] & 0x0FU) << (4 * i);
  }
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%04Xh (%u)", id, id);
  return buffer;
}

// The local time zone: hours from UTC in bits 0-4, daylight saving observed in
// bit 5.
std::string time_zone_text(const std::vector<uint8_t>& data) {
  if (data.empty()) {
    return {};
  }
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "UTC-%d%s", data[0] & 0x1F,
                (data[0] & 0x20) != 0 ? ", daylight saving observed" : "");
  return buffer;
}

}  // namespace

bool CCXdsDecoder::accept(uint8_t byte1, uint8_t byte2, uint64_t field_index) {
  byte1 = static_cast<uint8_t>(byte1 & 0x7F);
  byte2 = static_cast<uint8_t>(byte2 & 0x7F);

  if (byte1 >= kXdsFirstCode && byte1 <= kXdsLastClassCode) {
    const bool start = (byte1 & 1U) != 0;
    const uint8_t xds_class = start ? byte1 : static_cast<uint8_t>(byte1 - 1);
    current_ = {xds_class, byte2};
    in_packet_ = true;
    if (start) {
      if (open_.size() >= kXdsMaxOpen && open_.count(current_) == 0) {
        open_.erase(open_.begin());
        ++packets_bad_;
      }
      Open packet;
      packet.packet.xds_class = xds_class;
      packet.packet.type = byte2;
      packet.sum = static_cast<unsigned>(byte1) + byte2;
      open_[current_] = std::move(packet);
    }
    return true;
  }

  if (byte1 == kXdsEnd) {
    const auto it = open_.find(current_);
    if (in_packet_ && it != open_.end()) {
      // The checksum makes the seven-bit sum of the packet zero.
      const unsigned sum = it->second.sum + kXdsEnd + byte2;
      if ((sum & 0x7FU) == 0) {
        ++packets_good_;
        it->second.packet.field_index = field_index;
        if (callback_) {
          callback_(it->second.packet);
        }
      } else {
        ++packets_bad_;
      }
      open_.erase(it);
    }
    in_packet_ = false;
    return true;
  }

  if (byte1 >= kCaptionFirstControl && byte1 <= kCaptionLastControl) {
    // A caption or text service takes the line back; the packet waits for its
    // continue code.
    in_packet_ = false;
    return false;
  }

  if (byte1 == 0 && byte2 == 0) {
    return false;  // padding, which belongs to no one
  }

  if (!in_packet_) {
    return false;
  }
  const auto it = open_.find(current_);
  if (it != open_.end()) {
    Open& packet = it->second;
    packet.sum += static_cast<unsigned>(byte1) + byte2;
    for (const uint8_t byte : {byte1, byte2}) {
      if (byte != 0) {
        packet.packet.data.push_back(byte);
      }
    }
    if (packet.packet.data.size() > kXdsMaxCharacters) {
      ++packets_bad_;
      open_.erase(it);
    }
  }
  return true;
}

std::string cc_xds_type_name(uint8_t xds_class, uint8_t type) {
  if (is_programme_class(xds_class)) {
    const std::string which =
        xds_class == kClassCurrent ? "Current programme: " : "Next programme: ";
    switch (type) {
      case 0x01:
        return which + "start";
      case 0x02:
        return which + "length";
      case 0x03:
        return which + "name";
      case 0x04:
        return which + "type";
      case 0x05:
        return which + "content advisory";
      case 0x06:
        return which + "audio services";
      case 0x07:
        return which + "caption services";
      case 0x08:
        return which + "copy generation (CGMS-A)";
      case 0x09:
        return which + "aspect ratio";
      default:
        break;
    }
    if (type >= 0x10 && type <= 0x17) {
      return which + "description row " + std::to_string(type - 0x0F);
    }
    return {};
  }
  if (xds_class == kClassChannel) {
    switch (type) {
      case 0x01:
        return "Channel: network name";
      case 0x02:
        return "Channel: call letters";
      case 0x03:
        return "Channel: tape delay";
      case 0x04:
        return "Channel: transmission signal ID";
      default:
        return {};
    }
  }
  if (xds_class == kClassMisc) {
    switch (type) {
      case 0x01:
        return "Time of day";
      case 0x02:
        return "Impulse capture ID";
      case 0x03:
        return "Supplemental data location";
      case 0x04:
        return "Local time zone";
      default:
        return {};
    }
  }
  if (xds_class == kClassPublic) {
    switch (type) {
      case 0x01:
        return "Weather: code";
      case 0x02:
        return "Weather: message";
      default:
        return {};
    }
  }
  if (xds_class == kClassPrivate) {
    return "Private data";
  }
  return {};
}

std::string cc_xds_value_text(const CCXdsPacket& packet) {
  if (carries_text(packet.xds_class, packet.type)) {
    return text_of(packet.data);
  }
  if (packet.xds_class == kClassMisc && packet.type == 0x01) {
    const std::string time = time_of_day_text(packet.data);
    if (!time.empty()) {
      return time;
    }
  }
  if (packet.xds_class == kClassMisc && packet.type == 0x04) {
    const std::string zone = time_zone_text(packet.data);
    if (!zone.empty()) {
      return zone;
    }
  }
  std::string value;
  if (is_programme_class(packet.xds_class)) {
    switch (packet.type) {
      case 0x01:
        value = programme_start_text(packet.data);
        break;
      case 0x02:
        value = length_text(packet.data);
        break;
      case 0x04:
        value = programme_type_text(packet.data);
        break;
      case 0x05:
        value = content_advisory_text(packet.data);
        break;
      case 0x06:
        value = audio_services_text(packet.data);
        break;
      case 0x07:
        value = caption_services_text(packet.data);
        break;
      case 0x08:
        value = copy_control_text(packet.data);
        break;
      default:
        break;
    }
  } else if (packet.xds_class == kClassChannel) {
    if (packet.type == 0x03) {
      value = tape_delay_text(packet.data);
    } else if (packet.type == 0x04) {
      value = transmission_signal_id_text(packet.data);
    }
  }
  return value.empty() ? hex_of(packet.data) : value;
}

void CCXdsSummary::add(const CCXdsPacket& packet) {
  const std::string value = cc_xds_value_text(packet);
  Values& values = types_[{packet.xds_class, packet.type}];
  ++values.packets;
  const auto it = values.counts.find(value);
  if (it != values.counts.end()) {
    ++it->second;
  } else if (values.counts.size() < kSummaryMaxValues) {
    values.counts.emplace(value, 1);
    values.order.push_back(value);
  }
  if (packet.xds_class == kClassMisc && packet.type == 0x01 &&
      packet.data.size() >= 6) {
    if (first_time_.empty()) {
      first_time_ = value;
    }
    last_time_ = value;
  }
}

std::vector<std::string> CCXdsSummary::lines() const {
  std::vector<std::string> out;
  for (const auto& [key, values] : types_) {
    std::string name = cc_xds_type_name(key.first, key.second);
    if (name.empty()) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "Class %02Xh type %02Xh", key.first,
                    key.second);
      name = buffer;
    }
    std::string line = name + ": ";
    if (key.first == kClassMisc && key.second == 0x01 && !first_time_.empty()) {
      line += first_time_ == last_time_ ? first_time_
                                        : first_time_ + " to " + last_time_;
    } else {
      // The values in the order they first arrived, each with how often.
      std::string listed;
      for (const std::string& value : values.order) {
        if (!listed.empty()) {
          listed += "; ";
        }
        listed +=
            "\"" + value + "\" x" + std::to_string(values.counts.at(value));
      }
      line += listed;
    }
    line += " (" + std::to_string(values.packets) + " packets)";
    out.push_back(std::move(line));
  }
  return out;
}

}  // namespace orc
